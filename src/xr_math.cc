#include "xr_math.h"

#include <cmath>

namespace nac::xr {

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  // v + 2w(u×v) + 2u×(u×v), with u the vector part of q
  const XrVector3f u{q.x, q.y, q.z};
  const XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
          v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

XrQuaternionf Conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

XrPosef PanelInFront(const XrPosef& head, float distance, float drop) {
  // Facing the way the head faces, level (yaw only), a little low.
  const XrVector3f fwd = Rotate(head.orientation, {0, 0, -1});
  float fx = fwd.x, fz = fwd.z;
  const float len = std::sqrt(fx * fx + fz * fz);
  if (len < 1e-4f) {
    fx = 0;
    fz = -1;
  } else {
    fx /= len;
    fz /= len;
  }
  XrPosef pose{};
  pose.position = {head.position.x + fx * distance, head.position.y - drop, head.position.z + fz * distance};
  const float yaw = std::atan2(-fx, -fz);  // turn about +Y so -Z points along fwd
  pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
  return pose;
}

bool HitPanel(const XrPosef& panel, float width, float height, const XrPosef& aim, float* u, float* v) {
  // In the panel's frame the quad lies in z = 0, facing +Z.
  const XrQuaternionf inv = Conjugate(panel.orientation);
  const XrVector3f rel{aim.position.x - panel.position.x, aim.position.y - panel.position.y,
                       aim.position.z - panel.position.z};
  const XrVector3f o = Rotate(inv, rel);
  const XrVector3f d = Rotate(inv, Rotate(aim.orientation, {0, 0, -1}));
  if (d.z >= -1e-5f || o.z <= 0) return false;  // pointing away, or from behind
  const float t = -o.z / d.z;
  const float x = o.x + d.x * t, y = o.y + d.y * t;
  if (std::fabs(x) > width / 2 || std::fabs(y) > height / 2) return false;
  *u = x / width + 0.5f;
  *v = 0.5f - y / height;
  return true;
}

std::vector<std::string> ExtraBindings(const std::string& profile, const char* hand, const char* what) {
  // Per controller profile: the input that does each job, or nothing.
  struct Profile {
    const char* path;
    const char* trigger;
    const char* toggle;
    const char* scroll;
  };
  static const Profile kProfiles[] = {
      {"/interaction_profiles/oculus/touch_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/meta/touch_controller_plus", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/meta/touch_pro_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/facebook/touch_controller_pro", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/valve/index_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/microsoft/motion_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/hp/mixed_reality_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/samsung/odyssey_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/bytedance/pico4_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/bytedance/pico_neo3_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/htc/vive_cosmos_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/htc/vive_focus3_controller", "/input/trigger/value", "/input/thumbstick/click",
       "/input/thumbstick/y"},
      {"/interaction_profiles/htc/vive_controller", "/input/trigger/value", "/input/trackpad/click",
       "/input/trackpad/y"},
      {"/interaction_profiles/khr/simple_controller", "/input/select/click", "/input/menu/click", nullptr},
  };
  for (const Profile& p : kProfiles) {
    if (profile != p.path) continue;
    const std::string base = std::string("/user/hand/") + hand;
    const std::string w = what;
    const char* input = w == "aim" ? "/input/aim/pose" : w == "trigger" ? p.trigger : w == "toggle" ? p.toggle : p.scroll;
    if (input == nullptr) return {};
    return {base + input};
  }
  return {};
}

}  // namespace nac::xr
