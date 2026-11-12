// Stands in for Nightcap's VR path when testing the OpenXR layer: makes its
// Vulkan instance and device through the runtime (XR_KHR_vulkan_enable2),
// starts a session with the Vulkan binding, sets up a game action set (a
// trigger on the simple controller), and runs frames.
//
//   xr_driver FRAMES
//
// Prints "game trigger: <0|1>" for the first and last frame, so a test can
// see whether the layer held the game's input back.

#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(call)                                                       \
  do {                                                                    \
    XrResult r_ = (call);                                                 \
    if (XR_FAILED(r_)) {                                                  \
      std::fprintf(stderr, "xr_driver: %s failed: %d\n", #call, r_);      \
      return 1;                                                           \
    }                                                                     \
  } while (0)

template <typename T>
T Fn(XrInstance instance, const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  xrGetInstanceProcAddr(instance, name, &fn);
  return reinterpret_cast<T>(fn);
}

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 60;
  const char* extensions[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strcpy(ici.applicationInfo.applicationName, "xr_driver");
  ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = 1;
  ici.enabledExtensionNames = extensions;
  XrInstance instance;
  CHECK(xrCreateInstance(&ici, &instance));
  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId system;
  CHECK(xrGetSystem(instance, &sgi, &system));

  auto reqs = Fn<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR");
  XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  CHECK(reqs(instance, system, &req));

  // Vulkan through the runtime, the way Nightcap's backend makes it.
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vici.pApplicationInfo = &app;
  XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xvici.systemId = system;
  xvici.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xvici.vulkanCreateInfo = &vici;
  VkInstance vk_instance;
  VkResult vr;
  CHECK(Fn<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR")(instance, &xvici, &vk_instance, &vr));
  XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gdi.systemId = system;
  gdi.vulkanInstance = vk_instance;
  VkPhysicalDevice physical;
  CHECK(Fn<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR")(instance, &gdi, &physical));
  uint32_t nfam = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &nfam, nullptr);
  std::vector<VkQueueFamilyProperties> fams(nfam);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &nfam, fams.data());
  uint32_t family = 0;
  while (family < nfam && !(fams[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
  const float prio = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xdci.systemId = system;
  xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
  xdci.vulkanPhysicalDevice = physical;
  xdci.vulkanCreateInfo = &dci;
  VkDevice device;
  CHECK(Fn<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR")(instance, &xdci, &device, &vr));

  // The game's own input: a trigger on the simple controller.
  XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(asci.actionSetName, "game");
  std::strcpy(asci.localizedActionSetName, "Game");
  XrActionSet set;
  CHECK(xrCreateActionSet(instance, &asci, &set));
  XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
  std::strcpy(aci.actionName, "fire");
  std::strcpy(aci.localizedActionName, "Fire");
  aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
  XrAction fire;
  CHECK(xrCreateAction(set, &aci, &fire));
  XrPath profile, select;
  xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profile);
  xrStringToPath(instance, "/user/hand/right/input/select/click", &select);
  XrActionSuggestedBinding binding{fire, select};
  XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  sb.interactionProfile = profile;
  sb.countSuggestedBindings = 1;
  sb.suggestedBindings = &binding;
  CHECK(xrSuggestInteractionProfileBindings(instance, &sb));

  XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  gb.instance = vk_instance;
  gb.physicalDevice = physical;
  gb.device = device;
  gb.queueFamilyIndex = family;
  gb.queueIndex = 0;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &gb;
  sci.systemId = system;
  XrSession session;
  CHECK(xrCreateSession(instance, &sci, &session));
  XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets = &set;
  CHECK(xrAttachSessionActionSets(session, &attach));

  bool running = false;
  int done = 0;
  for (int spins = 0; done < frames && spins < frames * 50; ++spins) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
      if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
        if (sc->state == XR_SESSION_STATE_READY && !running) {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          CHECK(xrBeginSession(session, &bi));
          running = true;
        }
      }
      ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!running) continue;
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    CHECK(xrWaitFrame(session, &fwi, &fs));
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    CHECK(xrBeginFrame(session, &fbi));
    XrActiveActionSet active{set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(session, &sync);
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = fire;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(session, &get, &state)) && (done == 0 || done == frames - 1)) {
      std::printf("game trigger: %d\n", state.currentState ? 1 : 0);
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    CHECK(xrEndFrame(session, &fei));
    ++done;
  }
  std::printf("frames: %d\n", done);
  xrDestroySession(session);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(vk_instance, nullptr);
  xrDestroyInstance(instance);
  return done == frames ? 0 : 1;
}
