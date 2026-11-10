// A frame-rate cap, applied where each frame is presented (ported from
// Cocktail's fpslimit layer). It reads the cap from the settings, so changes
// apply right away; a separate cap can apply while Roblox isn't focused.
// It is off while the game is shown in a VR headset.

#ifndef VOX_MENU_FRAMECAP_H_
#define VOX_MENU_FRAMECAP_H_

namespace nac::framecap {

// Waits out the rest of this frame's time slot. Call just before presenting.
void Pace();
// The cap in force right now, 0 when unlimited.
double CurrentCap();

}  // namespace nac::framecap

#endif  // VOX_MENU_FRAMECAP_H_
