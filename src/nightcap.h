// Direct hooks into Nightcap's own host interfaces, from inside its process:
//
//   * Roblox's log lines as they are logged, through Nightcap's log
//     observer API (mocktail_android_log_add_observer), instead of polling
//     the session log file.
//   * The game's UDP packets, through Nightcap's datagram observer, chained
//     in front of Nightcap's own server-info overlay. Timing the server's
//     ACKs gives the real in-game ping (Nightcap's method).
//   * Joining a server through Nightcap's external-launch broker: the
//     running Nightcap leaves the current game and joins the new one in
//     place, as when you click Join on the website.
//
// Each piece is optional: with an older or stripped Nightcap the callers
// fall back (log file, ICMP ping, starting a second Nightcap with the link).

#ifndef VOX_MENU_NIGHTCAP_H_
#define VOX_MENU_NIGHTCAP_H_

#include <cstdint>
#include <string>

namespace nac::nightcap {

// Main thread, once per event pump. Cheap after the first call.
void Attach();

// Round trip measured from the game's own packets, or -1 until known.
int GamePingMs();
bool PacketObserverAttached();

// A fresh join attempt ID (a random UUID), as Roblox's own Join buttons send.
// Nightcap ignores a launch identical to the one that started the current
// game, so every join carries a new one: a rejoin really rejoins.
std::string NewJoinAttemptId();

// Asks the running Nightcap to join this server now. False, with *error,
// when its launch broker can't be reached.
bool QueueJoin(const std::string& place, const std::string& job, std::string* error);

// Exposed for tests: Nightcap's ACK-timing estimator (Apache-2.0, adapted).
class AckPing {
 public:
  void OnSend(std::int64_t now_us);
  void OnReceive(std::int64_t now_us, std::size_t bytes);
  int EstimateMs(std::int64_t now_us) const;  // -1 when unsure
  void Reset();

 private:
  static constexpr std::size_t kAckBytes = 54;  // RakNet ACK datagrams
  static constexpr int kBinMs = 2, kBins = 200, kWindowSeconds = 3, kMinimumPeak = 12;
  static constexpr std::size_t kSendHistory = 128;
  struct Second {
    std::int64_t index = -1;
    std::uint16_t bins[kBins] = {};
  };
  std::int64_t sends_[kSendHistory] = {};
  std::size_t send_count_ = 0, send_next_ = 0;
  Second seconds_[kWindowSeconds];
};

}  // namespace nac::nightcap

#endif  // VOX_MENU_NIGHTCAP_H_
