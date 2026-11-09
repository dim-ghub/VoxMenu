#include "gamestate.h"

#include <sys/stat.h>

#include <unistd.h>

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace nac {
namespace {

std::atomic<bool> g_in_game{false};
std::atomic<bool> g_direct{false};
std::atomic<std::uint64_t> g_endpoint{0};
std::once_flag g_started;
std::mutex g_server_mutex;
std::string g_server;  // the current server's address
ServerInfo g_last;
std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> g_history;

void SetServer(const std::string& address) {
  std::lock_guard<std::mutex> lock(g_server_mutex);
  g_server = address;
  if (!address.empty()) g_last.address = address;
}

void Joined(const ServerInfo& info) {
  std::lock_guard<std::mutex> lock(g_server_mutex);
  g_server = info.address;
  g_last = info;
  g_history.emplace_back(info.job, std::chrono::steady_clock::now());
  if (g_history.size() > 200) g_history.erase(g_history.begin());
}

// The dotted IPv4 address starting at text[at], or "".
std::string AddressAt(std::string_view text, size_t at) {
  size_t end = at;
  while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) ++end;
  const std::string ip(text.substr(at, end - at));
  return std::count(ip.begin(), ip.end(), '.') == 3 ? ip : "";
}

std::string LatestLog() {
  const char* state = std::getenv("XDG_STATE_HOME");
  const char* home = std::getenv("HOME");
  std::string base = state && *state ? state : std::string(home ? home : "") + "/.local/state";
  return base + "/mocktail/logs/latest.log";
}

void Watch() {
  const std::string link = LatestLog();
  std::string path;
  std::streamoff offset = 0;
  std::string partial;
  while (true) {
    // latest.log points at this session's log once Nightcap has started it.
    char target[4096];
    const ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
    std::string current = link;
    if (n > 0) {
      target[n] = 0;
      current = target[0] == '/' ? target : link.substr(0, link.rfind('/') + 1) + target;
    }
    if (current != path) {
      path = current;
      offset = 0;
      partial.clear();
      if (!g_direct) {
        g_in_game = false;
        SetServer("");
      }
    }
    std::ifstream file;
    if (!g_direct) file.open(path);
    if (file) {
      file.seekg(0, std::ios::end);
      const std::streamoff size = file.tellg();
      if (size < offset) offset = 0;  // truncated
      if (size > offset) {
        file.seekg(offset);
        std::string chunk(static_cast<size_t>(size - offset), '\0');
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        offset = size;
        partial += chunk;
        size_t start = 0, end;
        while ((end = partial.find('\n', start)) != std::string::npos) {
          partial[end] = 0;
          OnLogLine(partial.c_str() + start);
          start = end + 1;
        }
        partial.erase(0, start);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
}

}  // namespace

void OnLogLine(const char* text) {
  // Most lines are none of these; skip them without a copy.
  const bool join = std::strstr(text, "Joining game '") != nullptr;
  const char* udmux = join ? nullptr : std::strstr(text, "UDMUX Address = ");
  const bool leave = !join && udmux == nullptr &&
                     (std::strstr(text, "onGameLeaveBegin()") != nullptr ||
                      std::strstr(text, "leave game dispatched") != nullptr);
  ServerInfo joined;
  if (join && ParseJoinLine(text, &joined)) {
    g_in_game = true;
    g_endpoint = 0;
    Joined(joined);
  } else if (udmux != nullptr) {
    const std::string rest = udmux + 16;
    const std::string ip = AddressAt(rest, 0);
    if (ip.empty()) return;
    SetServer(ip);
    const size_t port_at = rest.find("Port = ");
    in_addr addr{};
    if (port_at != std::string::npos && inet_pton(AF_INET, ip.c_str(), &addr) == 1) {
      g_endpoint = EndpointKey(addr.s_addr, static_cast<std::uint16_t>(std::atoi(rest.c_str() + port_at + 7)));
    }
  } else if (leave) {
    g_in_game = false;
    g_endpoint = 0;
    SetServer("");
  }
}

void SetDirectLogFeed(bool direct) {
  g_direct = direct;
  InGame();  // the watcher still follows which log file is current
}

std::uint64_t GameEndpointKey() { return g_endpoint.load(); }

bool ParseJoinLine(const std::string& line, ServerInfo* out) {
  // "... ! Joining game '<job>' place <id> at <ip>": the address is the
  // server itself, often behind a UDMUX relay that a later line names.
  const std::string mark = "Joining game '";
  const size_t at = line.find(mark);
  if (at == std::string::npos) return false;
  const size_t job_start = at + mark.size();
  const size_t job_end = line.find('\'', job_start);
  if (job_end == std::string::npos) return false;
  ServerInfo info;
  info.job = line.substr(job_start, job_end - job_start);
  const size_t place = line.find(" place ", job_end);
  if (place != std::string::npos) {
    size_t end = place + 7;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
    info.place = line.substr(place + 7, end - place - 7);
  }
  const size_t ip = line.find(" at ", job_end);
  if (ip != std::string::npos) info.address = AddressAt(line, ip + 4);
  *out = info;
  return !info.job.empty();
}

ServerInfo LastServer() {
  InGame();
  std::lock_guard<std::mutex> lock(g_server_mutex);
  return g_last;
}

std::vector<std::string> RecentServers(int seconds) {
  InGame();
  const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(seconds);
  std::lock_guard<std::mutex> lock(g_server_mutex);
  std::vector<std::string> out;
  for (const auto& [job, when] : g_history) {
    if (when >= cutoff) out.push_back(job);
  }
  return out;
}

std::string ServerAddress() {
  InGame();  // starts the watcher
  std::lock_guard<std::mutex> lock(g_server_mutex);
  return g_server;
}

bool InGame() {
  std::call_once(g_started, [] { std::thread(Watch).detach(); });
  return g_in_game.load();
}

}  // namespace nac
