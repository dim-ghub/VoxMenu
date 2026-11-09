#include "netstats.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "app.h"
#include "gamestate.h"
#include "nightcap.h"

namespace nac::net {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
Stats g_stats;
std::once_flag g_started;
std::atomic<bool> g_want_ping{false};
std::atomic<bool> g_want_location{false};

bool ParseIPv4(const std::string& ip, in_addr* out) { return inet_pton(AF_INET, ip.c_str(), out) == 1; }

// The remote end of the game's connected UDP socket: the one socket of this
// process talking to a public address (Roblox's servers speak UDP).
std::string AddressFromSockets() {
  std::set<std::string> inodes;
  if (DIR* fds = opendir("/proc/self/fd")) {
    while (dirent* e = readdir(fds)) {
      char link[300], target[128];
      std::snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
      const ssize_t n = readlink(link, target, sizeof(target) - 1);
      if (n <= 0) continue;
      target[n] = 0;
      if (std::strncmp(target, "socket:[", 8) == 0) inodes.insert(std::string(target + 8, n - 9));
    }
    closedir(fds);
  }
  std::ifstream udp("/proc/self/net/udp");
  std::string line;
  std::getline(udp, line);  // header
  while (std::getline(udp, line)) {
    std::istringstream in(line);
    std::string slot, local, remote, state, queues, timer, retr, uid, timeout, inode;
    in >> slot >> local >> remote >> state >> queues >> timer >> retr >> uid >> timeout >> inode;
    if (!inodes.count(inode)) continue;
    const size_t colon = remote.find(':');
    if (colon == std::string::npos) continue;
    in_addr addr{};
    addr.s_addr = static_cast<uint32_t>(std::strtoul(remote.substr(0, colon).c_str(), nullptr, 16));
    if (addr.s_addr == 0) continue;  // not connected
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    if (IsPublicAddress(ip)) return ip;
  }
  return "";
}

// Round trip of an ICMP echo, in ms; -1 when unavailable.
int PingIcmp(const in_addr& to) {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
  if (fd < 0) return -1;  // ping_group_range doesn't include us
  icmphdr req{};
  req.type = ICMP_ECHO;
  req.un.echo.sequence = htons(1);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr = to;
  const Clock::time_point sent = Clock::now();
  int ms = -1;
  if (sendto(fd, &req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == sizeof(req)) {
    pollfd p{fd, POLLIN, 0};
    char buf[256];
    while (poll(&p, 1, 1000) > 0) {
      const ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n >= static_cast<ssize_t>(sizeof(icmphdr)) && reinterpret_cast<icmphdr*>(buf)->type == ICMP_ECHOREPLY) {
        ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count());
        break;
      }
    }
  }
  close(fd);
  return ms;
}

// Time to a TCP answer (accepted or refused, both take one round trip).
int PingTcp(const in_addr& to) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr = to;
  dst.sin_port = htons(443);
  const Clock::time_point sent = Clock::now();
  int ms = -1;
  const int r = connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
  if (r == 0 || errno == EINPROGRESS) {
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, 1000) > 0) {
      int err = 0;
      socklen_t len = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
      if (err == 0 || err == ECONNREFUSED) {
        ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent).count());
      }
    }
  }
  close(fd);
  return ms;
}

// GET http://ip-api.com/json/<ip>; the body, or "".
std::string FetchLocation(const std::string& ip) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo("ip-api.com", "80", &hints, &res) != 0 || res == nullptr) return "";
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  std::string body;
  if (fd >= 0) {
    timeval tv{4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
      const std::string req = "GET /json/" + ip +
                              "?fields=status,country,countryCode,regionName,city HTTP/1.0\r\n"
                              "Host: ip-api.com\r\nUser-Agent: vox-menu\r\nConnection: close\r\n\r\n";
      if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(req.size())) {
        std::string response;
        char buf[2048];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0 && response.size() < 65536) response.append(buf, n);
        const size_t split = response.find("\r\n\r\n");
        if (split != std::string::npos) body = response.substr(split + 4);
      }
    }
    close(fd);
  }
  freeaddrinfo(res);
  return body;
}

std::string JsonString(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = json.find(needle);
  if (at == std::string::npos) return "";
  const size_t start = at + needle.size();
  const size_t end = json.find('"', start);
  return end == std::string::npos ? "" : json.substr(start, end - start);
}

void Worker() {
  std::string address;
  std::string located;  // the address `location` belongs to
  Clock::time_point lookup_failed{};
  while (true) {
    std::string now_address = ServerAddress();
    if (now_address.empty() || !IsPublicAddress(now_address)) {
      const std::string from_sockets = AddressFromSockets();
      if (!from_sockets.empty()) now_address = from_sockets;
    }
    if (now_address != address) {
      address = now_address;
      std::lock_guard<std::mutex> lock(g_mutex);
      g_stats = Stats{};
      g_stats.address = address;
    }
    in_addr addr{};
    if (!address.empty() && ParseIPv4(address, &addr)) {
      if (g_want_ping) {
        // The game's own packets give its real ping; probes are the fallback.
        int ms = nightcap::GamePingMs();
        if (ms < 0 && !nightcap::PacketObserverAttached()) ms = PingIcmp(addr);
        if (ms < 0 && !nightcap::PacketObserverAttached()) ms = PingTcp(addr);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stats.ping_ms = ms;
      }
      if (g_want_location && located != address && IsPublicAddress(address) &&
          Clock::now() - lookup_failed > std::chrono::seconds(30)) {
        const std::string location = ParseLocation(FetchLocation(address));
        if (location.empty()) {
          lookup_failed = Clock::now();
          Log("could not look up where the game server is");
        } else {
          located = address;
          std::lock_guard<std::mutex> lock(g_mutex);
          g_stats.location = location;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(address.empty() ? 1000 : 2000));
  }
}

}  // namespace

bool IsPublicAddress(const std::string& ip) {
  in_addr addr{};
  if (!ParseIPv4(ip, &addr)) return false;
  const uint32_t a = ntohl(addr.s_addr);
  auto in = [&](uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
  return !(a == 0 || in(0x0A000000, 8) || in(0x7F000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) ||
           in(0xA9FE0000, 16) || in(0x64400000, 10) || in(0xE0000000, 4));
}

std::string ParseLocation(const std::string& json) {
  if (JsonString(json, "status") != "success") return "";
  std::string out;
  for (const char* key : {"city", "regionName", "countryCode"}) {
    const std::string part = JsonString(json, key);
    if (part.empty() || out.find(part) != std::string::npos) continue;  // "Singapore, Singapore"
    out += (out.empty() ? "" : ", ") + part;
  }
  return out;
}

Stats Get(bool ping, bool location) {
  g_want_ping = ping;
  g_want_location = location;
  std::call_once(g_started, [] { std::thread(Worker).detach(); });
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_stats;
}

}  // namespace nac::net
