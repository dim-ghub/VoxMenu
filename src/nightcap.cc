#include "nightcap.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <vector>

#include "app.h"
#include "exe_symbols.h"
#include "gamestate.h"

namespace nac::nightcap {
namespace {

using LogObserver = void (*)(int priority, const char* tag, const char* message);
using DatagramObserver = void (*)(bool outgoing, const sockaddr* peer, socklen_t peer_length, ssize_t bytes);

bool g_attached = false;  // main thread
std::atomic<DatagramObserver>* g_datagram_slot = nullptr;
std::atomic<DatagramObserver> g_next_observer{nullptr};  // Nightcap's own, called after ours
std::atomic<bool> g_packets{false};

std::mutex g_ping_mutex;
AckPing g_ping;
std::uint64_t g_ping_target = 0;

std::int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void OnLog(int, const char*, const char* message) {
  if (message != nullptr) OnLogLine(message);
}

void OnDatagram(bool outgoing, const sockaddr* peer, socklen_t peer_length, ssize_t bytes) {
  if (peer != nullptr && peer->sa_family == AF_INET && peer_length >= static_cast<socklen_t>(sizeof(sockaddr_in))) {
    const std::uint64_t target = GameEndpointKey();
    const auto* in = reinterpret_cast<const sockaddr_in*>(peer);
    if (target != 0 && EndpointKey(in->sin_addr.s_addr, ntohs(in->sin_port)) == target) {
      std::lock_guard<std::mutex> lock(g_ping_mutex);
      if (target != g_ping_target) {  // a new server
        g_ping.Reset();
        g_ping_target = target;
      }
      if (outgoing) g_ping.OnSend(NowUs());
      else g_ping.OnReceive(NowUs(), static_cast<std::size_t>(bytes));
    }
  }
  if (DatagramObserver next = g_next_observer.load(std::memory_order_acquire)) next(outgoing, peer, peer_length, bytes);
}

// The broker's listening socket: the one of this process's sockets bound to
// "...external-launch-v1.sock" (so a second Nightcap's is never picked).
std::string BrokerSocket() {
  const std::string suffix = ".external-launch-v1.sock";
  std::set<std::string> ours;
  if (DIR* fds = opendir("/proc/self/fd")) {
    while (dirent* e = readdir(fds)) {
      char link[300], target[128];
      std::snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
      const ssize_t n = readlink(link, target, sizeof(target) - 1);
      if (n > 9 && std::strncmp(target, "socket:[", 8) == 0) ours.insert(std::string(target + 8, n - 9));
    }
    closedir(fds);
  }
  std::ifstream unix_sockets("/proc/self/net/unix");
  std::string line;
  while (std::getline(unix_sockets, line)) {
    std::istringstream in(line);
    std::string num, refs, proto, flags, type, state, inode, path;
    if (!(in >> num >> refs >> proto >> flags >> type >> state >> inode >> path)) continue;
    if (ours.count(inode) && path.size() > suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
      return path;
    }
  }
  // Not visible (a sandbox without /proc/net): only if there's just one.
  glob_t g{};
  const std::string uid = std::to_string(getuid());
  std::string found;
  if (glob(("/run/user/" + uid + "/mocktail.*" + suffix).c_str(), 0, nullptr, &g) == 0 && g.gl_pathc == 1) {
    found = g.gl_pathv[0];
  }
  globfree(&g);
  return found;
}

bool AllOf(const std::string& s, const char* allowed_extra) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [&](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || std::strchr(allowed_extra, c) != nullptr;
  });
}

}  // namespace

void Attach() {
  if (!g_attached) {
    g_attached = true;
    if (auto add = reinterpret_cast<bool (*)(LogObserver)>(dlsym(RTLD_DEFAULT, "mocktail_android_log_add_observer"));
        add != nullptr && add(OnLog)) {
      SetDirectLogFeed(true);
      Log("reading Roblox's log through Nightcap");
    } else {
      Log("Nightcap's log observer not found; reading the session log file instead");
    }
    void* slot = nullptr;
    FindExeObjects(
        [](std::string_view name, std::size_t size) {
          return name == "_ZN12_GLOBAL__N_1L19g_datagram_observerE" && size == sizeof(void*) ? 0 : -1;
        },
        &slot, 1);
    g_datagram_slot = static_cast<std::atomic<DatagramObserver>*>(slot);
    if (g_datagram_slot == nullptr) Log("Nightcap's packet observer not found; ping uses ICMP");
  }
  // Nightcap sets its own observer when its server-info overlay starts,
  // possibly after us: stay in front of whatever is there.
  if (g_datagram_slot != nullptr) {
    DatagramObserver current = g_datagram_slot->load(std::memory_order_acquire);
    if (current != OnDatagram) {
      g_next_observer.store(current, std::memory_order_release);
      if (g_datagram_slot->compare_exchange_strong(current, OnDatagram, std::memory_order_acq_rel)) {
        if (!g_packets.exchange(true)) Log("timing the game's own packets for ping");
      }
    }
  }
}

bool PacketObserverAttached() { return g_packets.load(); }

int GamePingMs() {
  if (!g_packets) return -1;
  std::lock_guard<std::mutex> lock(g_ping_mutex);
  return g_ping_target == GameEndpointKey() ? g_ping.EstimateMs(NowUs()) : -1;
}

std::string NewJoinAttemptId() {
  static std::mutex mutex;
  static std::mt19937_64 rng{std::random_device{}()};
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t a = rng(), b = rng();
  char id[40];
  // Version 4, variant 10xx.
  std::snprintf(id, sizeof(id), "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xffff), static_cast<unsigned>(a & 0xfff),
                static_cast<unsigned>(0x8000 | ((b >> 48) & 0x3fff)),
                static_cast<unsigned long long>(b & 0xffffffffffffULL));
  return id;
}

bool QueueJoin(const std::string& place, const std::string& job, std::string* error) {
  if (!AllOf(place, "") || !AllOf(job, "-")) {
    *error = "not a server";
    return false;
  }
  const std::string path = BrokerSocket();
  if (path.empty()) {
    *error = "Nightcap's launch broker isn't running";
    return false;
  }
  sockaddr_un address{};
  if (path.size() >= sizeof(address.sun_path)) {
    *error = "socket path too long";
    return false;
  }
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *error = "no socket";
    return false;
  }
  timeval timeout{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  // MOCKTAIL-EXTERNAL-LAUNCH/1: one packet of prefix + launch JSON, answered
  // by "MTL1" and a status byte.
  const std::string packet =
      "MOCKTAIL-EXTERNAL-LAUNCH/1\n{\"placeId\":" + place + ",\"gameInstanceId\":\"" + job +
      "\",\"joinAttemptId\":\"" + NewJoinAttemptId() + "\"}";
  unsigned char reply[5] = {};
  bool ok = false;
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    *error = "can't reach Nightcap's launch broker";
  } else if (send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(packet.size())) {
    *error = "couldn't send the join";
  } else if (recv(fd, reply, sizeof(reply), 0) != sizeof(reply) || std::memcmp(reply, "MTL1", 4) != 0) {
    *error = "Nightcap didn't answer";
  } else {
    static const char* const kReplies[] = {"accepted", "rejected the request", "has too many joins queued",
                                           "denied the request", "is shutting down"};
    ok = reply[4] == 0;
    if (!ok) *error = std::string("Nightcap ") + (reply[4] < 5 ? kReplies[reply[4]] : "sent an unknown reply");
  }
  close(fd);
  return ok;
}

// --- AckPing ------------------------------------------------------------------
// Adapted from Nightcap's AckPingEstimator (src/runtime/server_info.cc,
// Apache-2.0): every ACK from the server is matched against recent sends,
// and the delay that comes up most often is the round trip.

void AckPing::OnSend(std::int64_t now_us) {
  sends_[send_next_] = now_us;
  send_next_ = (send_next_ + 1) % kSendHistory;
  send_count_ = std::min(send_count_ + 1, kSendHistory);
}

void AckPing::OnReceive(std::int64_t now_us, std::size_t bytes) {
  if (bytes != kAckBytes) return;
  constexpr std::int64_t kBinUs = kBinMs * 1000;
  const std::int64_t index = now_us / 1000000;
  Second& second = seconds_[index % kWindowSeconds];
  if (second.index != index) {
    second = Second{};
    second.index = index;
  }
  for (std::size_t slot = 0; slot < send_count_; ++slot) {
    const std::int64_t delay = now_us - sends_[slot];
    if (delay <= 0 || delay >= kBins * kBinUs) continue;
    std::uint16_t& bin = second.bins[delay / kBinUs];
    if (bin != UINT16_MAX) ++bin;
  }
}

int AckPing::EstimateMs(std::int64_t now_us) const {
  const std::int64_t current = now_us / 1000000;
  int totals[kBins] = {};
  for (const Second& second : seconds_) {
    if (second.index < 0 || second.index > current || second.index <= current - kWindowSeconds) continue;
    for (int bin = 0; bin < kBins; ++bin) totals[bin] += second.bins[bin];
  }
  int best = 0, best_total = 0;
  for (int bin = 1; bin + 1 < kBins; ++bin) {
    const int total = totals[bin - 1] + totals[bin] + totals[bin + 1];
    if (total > best_total) {
      best = bin;
      best_total = total;
    }
  }
  if (best_total < kMinimumPeak) return -1;
  double weighted = 0;
  for (int bin = best - 1; bin <= best + 1; ++bin) weighted += totals[bin] * (bin * kBinMs + kBinMs / 2.0);
  return static_cast<int>(weighted / best_total + 0.5);
}

void AckPing::Reset() { *this = AckPing{}; }

}  // namespace nac::nightcap
