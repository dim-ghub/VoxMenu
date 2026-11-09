// Network numbers for the HUD: the game server's address, its round-trip
// time, and roughly where it is.
//
// The address comes from Roblox's log (gamestate.h), or else from the
// game's connected UDP socket. Ping is timed from the game's own packets
// (nightcap.h); without that, an ICMP echo or a TCP handshake. The location is looked up once per server
// at ip-api.com, only while the server-location overlay is on.

#ifndef VOX_MENU_NETSTATS_H_
#define VOX_MENU_NETSTATS_H_

#include <string>

namespace nac::net {

struct Stats {
  std::string address;   // "" when not connected
  int ping_ms = -1;      // -1: unknown
  std::string location;  // "City, Region, CC", or "" until known
};

// Starts the background worker on first use. `ping` and `location` say
// which numbers are wanted (each costs a little network traffic).
Stats Get(bool ping, bool location);

// Exposed for tests.
bool IsPublicAddress(const std::string& ip);
std::string ParseLocation(const std::string& json);

}  // namespace nac::net

#endif  // VOX_MENU_NETSTATS_H_
