// In-game state and current server, parsed from Roblox's log lines.

#ifndef VOX_MENU_GAMESTATE_H_
#define VOX_MENU_GAMESTATE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace nac {

bool InGame();
// The game server's public address, or "" when the log hasn't named one.
std::string ServerAddress();

// The server joined last; still known after leaving it (to rejoin).
struct ServerInfo {
  std::string place;    // place ID
  std::string job;      // server (job) ID
  std::string address;  // as in ServerAddress()
};
ServerInfo LastServer();
// Servers joined in the last `seconds`, newest last (to avoid hopping back).
std::vector<std::string> RecentServers(int seconds);

// One Roblox log line, from any thread.
void OnLogLine(const char* line);
// Lines come through OnLogLine: stop reading the session log file.
void SetDirectLogFeed(bool direct);

// The game's UDP endpoint (from the UDMUX line) as EndpointKey(), 0 if none.
std::uint64_t GameEndpointKey();
inline std::uint64_t EndpointKey(std::uint32_t ip_network_order, std::uint16_t port) {
  return (static_cast<std::uint64_t>(ip_network_order) << 16) | port;
}

// Exposed for tests: picks a "Joining game '<job>' place <id> at <ip>" line apart.
bool ParseJoinLine(const std::string& line, ServerInfo* out);

}  // namespace nac

#endif  // VOX_MENU_GAMESTATE_H_
