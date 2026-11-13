// OpenXR API layer: Vox Menu in the headset.
//
// The same library is named in an OpenXR API layer manifest (the launcher
// writes it next to the Vulkan one), so the OpenXR loader inside Nightcap
// puts these functions between Nightcap and the runtime:
//
//   * xrCreateSession: notes the Vulkan device and queue (the game's own,
//     which the Vulkan layer already follows) and makes a swapchain for the
//     panel.
//   * Input: our own action set (aim pose, trigger, the hold-to-open click,
//     thumbstick for scrolling) rides along with Nightcap's, added to its
//     binding suggestions, attach and sync calls. Bindings are only added for
//     controller profiles listed here, and if the runtime refuses them the
//     call is repeated without, so Nightcap's input never breaks.
//   * xrEndFrame: renders the panel (xr_render.h) and adds it to the frame as
//     quad layers: the menu in front of you, the stats pill low in view.
//   * While the menu is open, the game's buttons and sticks read as released,
//     like the desktop menu keeps the mouse and keyboard.

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app.h"
#include "ui.h"
#include "xr_layer.h"
#include "xr_math.h"
#include "xr_render.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace nac::xr {
namespace {

using Clock = std::chrono::steady_clock;

// --- Next in the chain --------------------------------------------------------------

PFN_xrGetInstanceProcAddr g_next_gipa = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;

#define XR_NEXT(name) PFN_##name next_##name = nullptr;
#define XR_NEXT_FUNCTIONS(X)                                                                                      \
  X(xrDestroyInstance) X(xrCreateSession) X(xrDestroySession) X(xrSuggestInteractionProfileBindings)             \
  X(xrAttachSessionActionSets) X(xrSyncActions) X(xrGetActionStateBoolean) X(xrGetActionStateFloat)              \
  X(xrGetActionStateVector2f) X(xrEndFrame) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction)         \
  X(xrStringToPath) X(xrPathToString) X(xrCreateActionSpace) X(xrCreateReferenceSpace) X(xrLocateSpace)          \
  X(xrDestroySpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain)                    \
  X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)
XR_NEXT_FUNCTIONS(XR_NEXT)
#undef XR_NEXT

std::mutex g_mutex;

// --- Our actions (per instance) -------------------------------------------------------

struct Actions {
  XrActionSet set = XR_NULL_HANDLE;
  XrAction aim = XR_NULL_HANDLE;      // pose
  XrAction trigger = XR_NULL_HANDLE;  // float
  XrAction toggle = XR_NULL_HANDLE;   // bool: hold to open / close
  XrAction scroll = XR_NULL_HANDLE;   // float, thumbstick up / down
  XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
  bool attached = false;
} g_actions;

XrPath Path(const char* text) {
  XrPath path = XR_NULL_PATH;
  if (next_xrStringToPath) next_xrStringToPath(g_instance, text, &path);
  return path;
}

std::string PathText(XrPath path) {
  char buf[XR_MAX_PATH_LENGTH];
  uint32_t n = 0;
  if (next_xrPathToString == nullptr || XR_FAILED(next_xrPathToString(g_instance, path, sizeof(buf), &n, buf))) {
    return "";
  }
  return std::string(buf, n > 0 ? n - 1 : 0);
}

bool CreateActions() {
  XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(set.actionSetName, "vox_menu");
  std::strcpy(set.localizedActionSetName, "Vox Menu");
  set.priority = 0;
  if (XR_FAILED(next_xrCreateActionSet(g_instance, &set, &g_actions.set))) return false;
  g_actions.hands[0] = Path("/user/hand/left");
  g_actions.hands[1] = Path("/user/hand/right");
  auto make = [](const char* name, const char* label, XrActionType type, XrAction* out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(info.actionName, name);
    std::strcpy(info.localizedActionName, label);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = g_actions.hands;
    return XR_SUCCEEDED(next_xrCreateAction(g_actions.set, &info, out));
  };
  return make("aim", "Point at the menu", XR_ACTION_TYPE_POSE_INPUT, &g_actions.aim) &&
         make("press", "Press in the menu", XR_ACTION_TYPE_FLOAT_INPUT, &g_actions.trigger) &&
         make("toggle", "Open the menu (hold)", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_actions.toggle) &&
         make("scroll", "Scroll the menu", XR_ACTION_TYPE_FLOAT_INPUT, &g_actions.scroll);
}

bool IsOurs(XrAction action) {
  return action != XR_NULL_HANDLE && (action == g_actions.aim || action == g_actions.trigger ||
                                      action == g_actions.toggle || action == g_actions.scroll);
}

// --- The session --------------------------------------------------------------------------

struct Session {
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
  XrSpace aim[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSwapchain swapchain = XR_NULL_HANDLE;
  xr_render::Target* target = nullptr;
  float px = 1.5f;           // texture pixels per logical unit
  int32_t width = 0, height = 0;

  bool open = false;          // the menu panel is showing
  XrPosef panel{};            // its centre, in LOCAL space
  Clock::time_point toggle_since{};
  bool toggle_held = false, toggle_fired = false;
  int hand = 1;               // the hand pointing (right first)
  bool was_down = false;
  int frames = 0;             // since the last stats-only render
  bool rendered = false;      // the swapchain holds an image
  int frame_no = 0;           // frames ended, for scripted clicks
  float stats_w = 0;          // the stats pill's width when last rendered, px
} g_session;

std::atomic<bool> g_block_game{false};
std::atomic<int64_t> g_last_frame_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

constexpr float kPanelWidthM = 0.8f;  // metres
constexpr float kStatsHeightM = 0.04f;

void DestroySessionState() {
  Session& s = g_session;
  xr_render::Destroy(s.target);
  s.target = nullptr;
  if (s.swapchain) next_xrDestroySwapchain(s.swapchain);
  for (XrSpace* space : {&s.local, &s.view, &s.aim[0], &s.aim[1]}) {
    if (*space) next_xrDestroySpace(*space);
    *space = XR_NULL_HANDLE;
  }
  s = Session{};
  g_block_game = false;
}

void SetUpSession(XrSession session, const XrGraphicsBindingVulkanKHR* vk) {
  Session& s = g_session;
  s.session = session;
  XrReferenceSpaceCreateInfo ref{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  ref.poseInReferenceSpace.orientation.w = 1;
  ref.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  next_xrCreateReferenceSpace(session, &ref, &s.local);
  ref.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  next_xrCreateReferenceSpace(session, &ref, &s.view);
  for (int h = 0; h < 2; ++h) {
    XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action = g_actions.aim;
    info.subactionPath = g_actions.hands[h];
    info.poseInActionSpace.orientation.w = 1;
    if (g_actions.aim) next_xrCreateActionSpace(session, &info, &s.aim[h]);
  }

  // The panel's texture: the menu, and the stats pill under it.
  uint32_t count = 0;
  next_xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  next_xrEnumerateSwapchainFormats(session, count, &count, formats.data());
  int64_t format = 0;
  for (const int64_t want : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_FORMAT_B8G8R8A8_UNORM}) {
    if (format == 0 && std::find(formats.begin(), formats.end(), want) != formats.end()) format = want;
  }
  if (format == 0) {
    Log("headset panel: the runtime offers no 8-bit colour swapchain format");
    return;
  }
  s.width = static_cast<int32_t>(std::lround(ui::kVrPanelW * s.px));
  s.height = static_cast<int32_t>(std::lround((ui::kVrPanelH + ui::kVrStatsH) * s.px));
  XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  sc.format = format;
  sc.sampleCount = 1;
  sc.width = s.width;
  sc.height = s.height;
  sc.faceCount = 1;
  sc.arraySize = 1;
  sc.mipCount = 1;
  if (XR_FAILED(next_xrCreateSwapchain(session, &sc, &s.swapchain))) {
    Log("headset panel: could not create its swapchain");
    return;
  }
  next_xrEnumerateSwapchainImages(s.swapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  next_xrEnumerateSwapchainImages(s.swapchain, count, &count,
                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  std::vector<VkImage> vk_images;
  for (const auto& img : images) vk_images.push_back(img.image);
  s.target = xr_render::Create(vk->device, vk->queueFamilyIndex, vk->queueIndex, static_cast<VkFormat>(format),
                               {static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height)}, vk_images);
}

// --- Poses ----------------------------------------------------------------------------------



bool Locate(XrSpace space, XrSpace base, XrTime time, XrPosef* pose) {
  if (!space || !base) return false;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(next_xrLocateSpace(space, base, time, &loc))) return false;
  const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if ((loc.locationFlags & need) != need) return false;
  *pose = loc.pose;
  return true;
}

}  // namespace

// --- Hooks ------------------------------------------------------------------------------------

namespace {

XRAPI_ATTR XrResult XRAPI_CALL SuggestBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* s) {
  std::vector<XrActionSuggestedBinding> merged(s->suggestedBindings, s->suggestedBindings + s->countSuggestedBindings);
  const std::string profile = PathText(s->interactionProfile);
  if (g_actions.set) {
    const std::pair<const char*, XrAction> ours[] = {{"aim", g_actions.aim},
                                                     {"trigger", g_actions.trigger},
                                                     {"toggle", g_actions.toggle},
                                                     {"scroll", g_actions.scroll}};
    for (const char* hand : {"left", "right"}) {
      for (const auto& [what, action] : ours) {
        for (const std::string& path : ExtraBindings(profile, hand, what)) merged.push_back({action, Path(path.c_str())});
      }
    }
  }
  if (merged.size() == s->countSuggestedBindings) return next_xrSuggestInteractionProfileBindings(instance, s);
  XrInteractionProfileSuggestedBinding copy = *s;
  copy.countSuggestedBindings = static_cast<uint32_t>(merged.size());
  copy.suggestedBindings = merged.data();
  const XrResult r = next_xrSuggestInteractionProfileBindings(instance, &copy);
  if (XR_SUCCEEDED(r)) return r;
  Log("headset: the runtime refused Vox Menu's bindings for %s", profile.c_str());
  return next_xrSuggestInteractionProfileBindings(instance, s);
}

XRAPI_ATTR XrResult XRAPI_CALL AttachActionSets(XrSession session, const XrSessionActionSetsAttachInfo* info) {
  if (g_actions.set) {
    std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
    sets.push_back(g_actions.set);
    XrSessionActionSetsAttachInfo copy = *info;
    copy.countActionSets = static_cast<uint32_t>(sets.size());
    copy.actionSets = sets.data();
    if (XR_SUCCEEDED(next_xrAttachSessionActionSets(session, &copy))) {
      g_actions.attached = true;
      Log("headset: controller input for Vox Menu ready");
      return XR_SUCCESS;
    }
  }
  return next_xrAttachSessionActionSets(session, info);
}

XRAPI_ATTR XrResult XRAPI_CALL SyncActions(XrSession session, const XrActionsSyncInfo* info) {
  if (!g_actions.attached) return next_xrSyncActions(session, info);
  std::vector<XrActiveActionSet> sets(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
  sets.push_back({g_actions.set, XR_NULL_PATH});
  XrActionsSyncInfo copy = *info;
  copy.countActiveActionSets = static_cast<uint32_t>(sets.size());
  copy.activeActionSets = sets.data();
  return next_xrSyncActions(session, &copy);
}

// While the menu is open the game reads its buttons and sticks as at rest.
XRAPI_ATTR XrResult XRAPI_CALL GetBoolean(XrSession session, const XrActionStateGetInfo* get,
                                          XrActionStateBoolean* state) {
  const XrResult r = next_xrGetActionStateBoolean(session, get, state);
  if (XR_SUCCEEDED(r) && g_block_game && !IsOurs(get->action)) {
    state->currentState = XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
  }
  return r;
}

XRAPI_ATTR XrResult XRAPI_CALL GetFloat(XrSession session, const XrActionStateGetInfo* get, XrActionStateFloat* state) {
  const XrResult r = next_xrGetActionStateFloat(session, get, state);
  if (XR_SUCCEEDED(r) && g_block_game && !IsOurs(get->action)) {
    state->currentState = 0;
    state->changedSinceLastSync = XR_FALSE;
  }
  return r;
}

XRAPI_ATTR XrResult XRAPI_CALL GetVector2(XrSession session, const XrActionStateGetInfo* get,
                                          XrActionStateVector2f* state) {
  const XrResult r = next_xrGetActionStateVector2f(session, get, state);
  if (XR_SUCCEEDED(r) && g_block_game && !IsOurs(get->action)) {
    state->currentState = {0, 0};
    state->changedSinceLastSync = XR_FALSE;
  }
  return r;
}

bool OurBool(XrAction action, int hand) {
  XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
  get.action = action;
  get.subactionPath = g_actions.hands[hand];
  XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
  return XR_SUCCEEDED(next_xrGetActionStateBoolean(g_session.session, &get, &st)) && st.isActive && st.currentState;
}

float OurFloat(XrAction action, int hand) {
  XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
  get.action = action;
  get.subactionPath = g_actions.hands[hand];
  XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
  return XR_SUCCEEDED(next_xrGetActionStateFloat(g_session.session, &get, &st)) && st.isActive ? st.currentState : 0;
}

// Input, the panel's texture, and our layers for this frame.
void Frame(const XrFrameEndInfo* info, std::vector<const XrCompositionLayerBaseHeader*>* layers,
           XrCompositionLayerQuad quads[2]) {
  Session& s = g_session;
  const Config cfg = GetConfig();
  const float panel_h_m = kPanelWidthM * ui::kVrPanelH / ui::kVrPanelW;

  // Hold the stick click (half a second) to open or close the panel.
  if (g_actions.attached) {
    const bool held = OurBool(g_actions.toggle, cfg.vr_toggle == "right_stick" ? 1 : 0);
    if (held && !s.toggle_held) s.toggle_since = Clock::now();
    if (!held) s.toggle_fired = false;
    s.toggle_held = held;
    if (held && !s.toggle_fired && Clock::now() - s.toggle_since >= std::chrono::milliseconds(500)) {
      s.toggle_fired = true;
      s.open = !s.open;
      XrPosef head;
      if (s.open && Locate(s.view, s.local, info->displayTime, &head)) s.panel = PanelInFront(head, 0.85f, 0.12f);
    }
  }
  if (Env("VR_OPEN") && !s.rendered) {  // debugging: start with it open
    s.open = true;
    XrPosef head{};
    head.orientation.w = 1;
    Locate(s.view, s.local, info->displayTime, &head);
    s.panel = PanelInFront(head, 0.85f, 0.12f);
  }
  if (ui::TakeVrCloseRequest()) s.open = false;
  g_block_game = s.open;

  // Pointing: the hand that last pulled its trigger, or whichever hits.
  xr_render::Pointer pointer;
  if (s.open && g_actions.attached) {
    for (int h = 0; h < 2; ++h) {
      if (OurFloat(g_actions.trigger, h) > 0.6f) s.hand = h;
    }
    for (int k = 0; k < 2 && !pointer.valid; ++k) {
      const int h = k == 0 ? s.hand : 1 - s.hand;
      XrPosef aim;
      float u, v;
      if (Locate(s.aim[h], s.local, info->displayTime, &aim) && HitPanel(s.panel, kPanelWidthM, panel_h_m, aim, &u, &v)) {
        pointer.valid = true;
        pointer.x = u * s.width;
        pointer.y = v * ui::kVrPanelH * s.px;
        pointer.down = OurFloat(g_actions.trigger, h) > 0.6f;
        const float stick = OurFloat(g_actions.scroll, h);
        if (std::fabs(stick) > 0.3f) pointer.wheel = stick * 0.25f;
      }
    }
  }

  // Debugging: VOX_MENU_VR_CLICKS="x,y,down,up;..." points at x, y (panel
  // pixels) from two frames before `down` to two after `up`, with the
  // trigger held from frame `down` until `up`.
  ++s.frame_no;
  if (const char* clicks = Env("VR_CLICKS")) {
    for (const char* at = clicks; at && *at; at = std::strchr(at, ';') ? std::strchr(at, ';') + 1 : nullptr) {
      float x, y;
      int down, up;
      if (std::sscanf(at, "%f,%f,%d,%d", &x, &y, &down, &up) == 4 && s.frame_no >= down - 2 && s.frame_no <= up + 2) {
        pointer = {true, x, y, s.frame_no >= down && s.frame_no < up, 0};
      }
    }
  }

  const bool stats_wanted = cfg.vr_stats != "off" && (cfg.hud_fps.on || cfg.hud_ping.on || cfg.hud_server.on);
  if (!s.open && !stats_wanted) return;
  // The menu redraws every frame; the stats alone ten times a second.
  const bool redraw = s.open || !s.rendered || ++s.frames >= 9;
  if (redraw && s.target) {
    s.frames = 0;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_SUCCEEDED(next_xrAcquireSwapchainImage(s.swapchain, &acquire, &index))) {
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait.timeout = 100'000'000;  // 100 ms
      if (XR_SUCCEEDED(next_xrWaitSwapchainImage(s.swapchain, &wait))) {
        xr_render::Render(s.target, index, s.open, pointer, &s.stats_w);
      }
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      next_xrReleaseSwapchainImage(s.swapchain, &release);
      s.rendered = true;
    }
  }
  if (!s.rendered) return;

  int n = 0;
  if (s.open) {
    XrCompositionLayerQuad& q = quads[n++];
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = s.swapchain;
    q.subImage.imageRect = {{0, 0}, {s.width, static_cast<int32_t>(std::lround(ui::kVrPanelH * s.px))}};
    q.pose = s.panel;
    q.size = {kPanelWidthM, panel_h_m};
  }
  if (stats_wanted && s.stats_w > 0) {
    // Low and to the left in view, like the desktop HUD under the top bar.
    const int32_t top = static_cast<int32_t>(std::lround(ui::kVrPanelH * s.px));
    const int32_t h = s.height - top, w = static_cast<int32_t>(std::ceil(s.stats_w));
    const float width_m = kStatsHeightM * w / h;
    XrCompositionLayerQuad& q = quads[n++];
    q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = s.view;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = s.swapchain;
    q.subImage.imageRect = {{0, top}, {w, h}};
    q.pose.orientation.w = 1;
    q.pose.position = {-0.2f + width_m / 2, -0.16f, -0.7f};
    q.size = {width_m, kStatsHeightM};
  }
  for (int i = 0; i < n; ++i) layers->push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[i]));
}

XRAPI_ATTR XrResult XRAPI_CALL EndFrame(XrSession session, const XrFrameEndInfo* info) {
  ui::CountVrFrame();
  g_last_frame_ms = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (session != g_session.session || g_session.swapchain == XR_NULL_HANDLE) return next_xrEndFrame(session, info);
  std::vector<const XrCompositionLayerBaseHeader*> layers(info->layers, info->layers + info->layerCount);
  XrCompositionLayerQuad quads[2];
  Frame(info, &layers, quads);
  if (layers.size() == info->layerCount) return next_xrEndFrame(session, info);
  XrFrameEndInfo copy = *info;
  copy.layerCount = static_cast<uint32_t>(layers.size());
  copy.layers = layers.data();
  return next_xrEndFrame(session, &copy);
}

XRAPI_ATTR XrResult XRAPI_CALL CreateSession(XrInstance instance, const XrSessionCreateInfo* info, XrSession* out) {
  const XrResult r = next_xrCreateSession(instance, info, out);
  if (XR_FAILED(r)) return r;
  const XrGraphicsBindingVulkanKHR* vk = nullptr;
  for (auto* next = static_cast<const XrBaseInStructure*>(info->next); next; next = next->next) {
    if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) vk = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_session.session != XR_NULL_HANDLE) return r;  // one at a time
  if (vk == nullptr) {
    Log("headset: Vox Menu needs Nightcap's Vulkan graphics (direct-vulkan); no panel in VR");
    return r;
  }
  SetUpSession(*out, vk);
  return r;
}

XRAPI_ATTR XrResult XRAPI_CALL DestroySession(XrSession session) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (session == g_session.session) DestroySessionState();
  }
  return next_xrDestroySession(session);
}

XRAPI_ATTR XrResult XRAPI_CALL DestroyInstance(XrInstance instance) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session.session) DestroySessionState();
    g_actions = Actions{};  // destroyed with the instance
  }
  return next_xrDestroyInstance(instance);
}

XRAPI_ATTR XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
#define HOOK(xr_name, ours)                                     \
  if (std::strcmp(name, #xr_name) == 0) {                       \
    *fn = reinterpret_cast<PFN_xrVoidFunction>(&ours);          \
    return XR_SUCCESS;                                          \
  }
  HOOK(xrGetInstanceProcAddr, GetInstanceProcAddr)
  HOOK(xrDestroyInstance, DestroyInstance)
  HOOK(xrCreateSession, CreateSession)
  HOOK(xrDestroySession, DestroySession)
  HOOK(xrSuggestInteractionProfileBindings, SuggestBindings)
  HOOK(xrAttachSessionActionSets, AttachActionSets)
  HOOK(xrSyncActions, SyncActions)
  HOOK(xrGetActionStateBoolean, GetBoolean)
  HOOK(xrGetActionStateFloat, GetFloat)
  HOOK(xrGetActionStateVector2f, GetVector2)
  HOOK(xrEndFrame, EndFrame)
#undef HOOK
  return g_next_gipa(instance, name, fn);
}

XRAPI_ATTR XrResult XRAPI_CALL CreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                      const XrApiLayerCreateInfo* layer_info, XrInstance* instance) {
  if (layer_info == nullptr || layer_info->nextInfo == nullptr) return XR_ERROR_INITIALIZATION_FAILED;
  PFN_xrGetInstanceProcAddr next_gipa = layer_info->nextInfo->nextGetInstanceProcAddr;
  XrApiLayerCreateInfo next_info = *layer_info;
  next_info.nextInfo = layer_info->nextInfo->next;
  const XrResult r = layer_info->nextInfo->nextCreateApiLayerInstance(info, &next_info, instance);
  if (XR_FAILED(r)) return r;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_next_gipa = next_gipa;
  g_instance = *instance;
#define XR_RESOLVE(name) next_gipa(*instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&next_##name));
  XR_NEXT_FUNCTIONS(XR_RESOLVE)
#undef XR_RESOLVE
  if (!CreateActions()) {
    Log("headset: could not create Vox Menu's controller actions");
    g_actions = Actions{};
  }
  Log("headset layer loaded");
  return r;
}

}  // namespace

bool Active() { return NowMs() - g_last_frame_ms.load() < 1000; }

}  // namespace nac::xr

EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loader,
                                                                         const char* /*layer_name*/,
                                                                         XrNegotiateApiLayerRequest* request) {
  if (loader == nullptr || request == nullptr || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
      loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
      loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  request->layerApiVersion = XR_CURRENT_API_VERSION;
  request->getInstanceProcAddr = &nac::xr::GetInstanceProcAddr;
  request->createApiLayerInstance = &nac::xr::CreateApiLayerInstance;
  return XR_SUCCESS;
}
