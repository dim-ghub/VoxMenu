// The in-game menu, drawn with Dear ImGui in the style of Roblox's own
// in-experience menu, and the top-bar button that opens it.

#ifndef VOX_MENU_UI_H_
#define VOX_MENU_UI_H_

#include <vector>

#include "overlay.h"

namespace nac::ui {

// Forget the built font atlas (the renderer was recreated).
void ResetFonts();
// (Re)builds fonts for this pixel scale. True when the atlas changed and
// must be uploaded again.
bool PrepareFonts(float px_per_unit);
void FeedInput(const std::vector<overlay::Input>& input, float px_per_unit);
// Builds this frame's UI. False when nothing needs drawing.
bool Draw(float px_per_unit, bool srgb_target);

// --- The headset (VR) ------------------------------------------------------------
// One texture holds the menu panel (top) and the stats pill (under it), in
// logical units; the texture is these times its pixels per unit.
inline constexpr float kVrPanelW = 864, kVrPanelH = 736, kVrStatsH = 60;

// Which surface's fonts are current: 0 the desktop, 1 the headset. Each
// draws with its own ImGui context and font atlas.
void SelectSurface(int surface);
// Builds the headset texture's UI with the current (headset) context: the
// menu when `panel_open`, and the stats pill. *stats_w is the pill's width
// in pixels, 0 when none. False when nothing needs drawing.
bool DrawVr(float px_per_unit, bool srgb_target, bool panel_open, float* stats_w);
// The panel's Close button was pressed (since last asked).
bool TakeVrCloseRequest();
// Headset frames, for the FPS counter while in VR.
void CountVrFrame();

}  // namespace nac::ui

#endif  // VOX_MENU_UI_H_
