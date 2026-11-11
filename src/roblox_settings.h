// Roblox's own saved settings (GlobalBasicSettings_*.xml in Nightcap's
// Roblox data), for matching the overlay to them.

#ifndef VOX_MENU_ROBLOX_SETTINGS_H_
#define VOX_MENU_ROBLOX_SETTINGS_H_

#include <string>

namespace nac {

// Roblox's Background Transparency setting (PreferredTransparency): 1 is the
// normal see-through look, 0 fully opaque. Re-read when Roblox saves it.
float RobloxPreferredTransparency();

// Exposed for tests: the value in a settings file's text, or `fallback`.
float ParsePreferredTransparency(const std::string& xml, float fallback);

}  // namespace nac

#endif  // VOX_MENU_ROBLOX_SETTINGS_H_
