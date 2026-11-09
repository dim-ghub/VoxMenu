// Shared by the menu (ui.cc) and the HUD overlays and automation window
// (hud.cc): scale, colours, fonts and drawing helpers. Render thread only.

#ifndef VOX_MENU_UI_INTERNAL_H_
#define VOX_MENU_UI_INTERNAL_H_

#include <string>
#include <vector>

#include "config.h"
#include "imgui.h"
#include "overlay.h"

namespace nac::ui {

extern float g_px;     // swapchain pixels per logical unit
extern bool g_srgb;    // target stores linear values
extern float g_alpha;  // fade for animations

// Logical units to swapchain pixels.
float S(float logical);
// sRGB colours, converted for the target.
ImU32 C(int r, int g, int b, float a = 1.0f);
ImVec4 C4(int r, int g, int b, float a = 1.0f);
ImU32 White(float a = 1.0f);
ImU32 Muted();
ImU32 Panel();
ImU32 Highlight();
ImU32 ButtonBg();
ImU32 ButtonHover();
ImU32 Primary();
ImU32 PrimaryHover();
ImU32 Divider();
// How opaque menu backgrounds are: Roblox's menu look (0.82), more opaque
// as Roblox's Background Transparency setting is lowered, solid at 0.
float MenuOpacity();
// HUD overlay backgrounds: solid, like Roblox's top-bar buttons, unless the
// HUD tab sets its own opacity.
float OverlayOpacity(const Config& cfg);
// The automation window: as see-through as Roblox's menu (MenuOpacity()),
// unless the HUD tab sets its own.
float WindowOpacity(const Config& cfg);
// Seconds since the library loaded.
float Now();

struct Fonts {
  ImFont* title = nullptr;    // row labels (Medium 18)
  ImFont* value = nullptr;    // picker values, field text (Regular 18)
  ImFont* small = nullptr;    // subtitles (Regular 13)
  ImFont* tiny = nullptr;     // slider end labels (Regular 12.5)
  ImFont* tab = nullptr;      // tab labels (Medium 20)
  ImFont* header = nullptr;   // section headers (Bold 25)
  ImFont* button = nullptr;   // bottom buttons (Bold 18)
  ImFont* hint = nullptr;     // key hints (Bold 10)
  ImFont* hud = nullptr;      // HUD overlays (Bold, baked large, drawn scaled)
  ImFont* ui = nullptr;       // automation window (Regular 15)
  ImFont* ui_bold = nullptr;  // automation window headings (Bold 15)
  float px = 0;
  std::string family;
};
extern Fonts g_fonts;

extern ImDrawList* g_dl;  // where Text() and friends draw
void Text(ImFont* font, ImVec2 pos, ImU32 col, const char* text);
ImVec2 Measure(ImFont* font, const char* text);
void TextCentered(ImFont* font, ImVec2 center, ImU32 col, const char* text);
// A Roblox icon centred on c; false when the icon font lacks it.
bool Icon(ImDrawList* dl, const char* name, ImVec2 c, float size, ImU32 col);

// Whether a point (swapchain pixels) is on the open menu's panel.
bool OverMenu(ImVec2 p);

// --- hud.cc --------------------------------------------------------------------

// Once per presented frame, drawn or not (for the FPS counter).
void CountFrame();
// Whether any overlay, the automation window or a status badge is showing.
bool HudVisible(const Config& cfg);
// Once: pills a version without joining showed side by side (they merged
// when 8 px apart) become joined, so they look as they did.
void AdoptSideBySide(Config& cfg);
// The headset's stats: the FPS / ping / server overlays that are on, as one
// pill at `origin`. Returns its width (0 when none are on).
float DrawStatsPill(ImDrawList* dl, const Config& cfg, ImVec2 origin, float u);
// The FPS, ping, server and keystroke overlays.
void DrawHud(const Config& cfg);
// The layout editor over the overlays: drag to move, drag a corner (or
// scroll) to resize, with snapping. False once the player leaves it.
bool LayoutEditor(Config& cfg);
void ResetLayout(Config& cfg);

// The floating automation window and the click-point markers. Adds the
// areas that take the mouse to `hot` (swapchain pixels).
void DrawTools(Config& cfg, std::vector<ImVec4>* hot);
// Picking a click point: the next click anywhere adds it.
bool Picking();
void CancelPick();
// The badge shown while the autoclicker, recording or a macro runs.
void DrawStatusBadge(const Config& cfg);

}  // namespace nac::ui

#endif  // VOX_MENU_UI_INTERNAL_H_
