// Frame-rate cap applied at present (from Cocktail's fpslimit). Off in VR.

#ifndef VOX_MENU_FRAMECAP_H_
#define VOX_MENU_FRAMECAP_H_

namespace nac::framecap {

// Waits out the rest of this frame's time slot. Call just before presenting.
void Pace();
// The cap in force right now, 0 when unlimited.
double CurrentCap();

}  // namespace nac::framecap

#endif  // VOX_MENU_FRAMECAP_H_
