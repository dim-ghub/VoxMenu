// The headset panel's geometry and controller bindings: plain functions,
// kept apart from the layer so tests can use them on their own.

#ifndef VOX_MENU_XR_MATH_H_
#define VOX_MENU_XR_MATH_H_

#include <openxr/openxr.h>

#include <string>
#include <vector>

namespace nac::xr {

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v);
XrQuaternionf Conjugate(const XrQuaternionf& q);

// Where the panel opens: `distance` in front of the head, level, facing it,
// `drop` metres below eye height.
XrPosef PanelInFront(const XrPosef& head, float distance, float drop);
// Where a controller's aim ray meets a width x height quad (facing +Z), as
// u, v from its top-left (0..1). False when it misses.
bool HitPanel(const XrPosef& panel, float width, float height, const XrPosef& aim, float* u, float* v);
// The binding paths Vox Menu adds for one of its actions ("aim", "trigger",
// "toggle", "scroll") on one hand ("left", "right") of a controller profile;
// empty for profiles it doesn't know.
std::vector<std::string> ExtraBindings(const std::string& profile, const char* hand, const char* what);

}  // namespace nac::xr

#endif  // VOX_MENU_XR_MATH_H_
