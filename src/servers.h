// Rejoin and server hop.

#ifndef VOX_MENU_SERVERS_H_
#define VOX_MENU_SERVERS_H_

#include <string>
#include <vector>

namespace nac::servers {

// From any thread; the work happens in the background.
void Rejoin();
// mode: "random", "smallest" (fewest players) or "ping" (best reported ping).
void Hop(const std::string& mode);
// Puts a link friends can open to join this server on the clipboard.
void CopyInvite();

// What the last action did, for the menu ("" when nothing happened yet).
std::string StatusText();
bool Busy();

// Main thread: text waiting to go on the clipboard (SDL wants that thread).
bool TakeClipboard(std::string* text);

// Exposed for tests.
struct Listed {
  std::string id;
  int playing = 0, max_players = 0;
  double ping = -1;
};
std::vector<Listed> ParseServerList(const std::string& json);
// The server to hop to, or "" when there is none.
std::string PickServer(const std::vector<Listed>& servers, const std::string& current,
                       const std::vector<std::string>& recent, const std::string& mode, unsigned seed);
std::string JoinUri(const std::string& place, const std::string& job);

}  // namespace nac::servers

#endif  // VOX_MENU_SERVERS_H_
