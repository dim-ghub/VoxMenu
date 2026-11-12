// The OpenXR API layer (xr_layer.cc): Vox Menu's panel and stats in the
// headset.

#ifndef VOX_MENU_XR_LAYER_H_
#define VOX_MENU_XR_LAYER_H_

namespace nac::xr {

// Whether headset frames are being shown (one within the last second).
bool Active();

}  // namespace nac::xr

#endif  // VOX_MENU_XR_LAYER_H_
