// Stands in for Nightcap when testing the overlay: an SDL3 window presenting
// Vulkan frames (cleared to a game-ish green with a sky band), with input
// arriving through SDL_PollEvent like Nightcap's own loop.
//
//   vk_driver FRAMES [script]
//
// The optional script injects input at given frames, one action per line:
//   <frame> key <SDL keycode>          press and release a key
//   <frame> click <x> <y>              left click at window coordinates
//   <frame> move <x> <y>               move the mouse there
//   <frame> press <x> <y>              left button down there (for drags)
//   <frame> release <x> <y>            left button up there
//   <frame> text <utf8>                text input event

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

#define CHECK(x)                                                          \
  do {                                                                    \
    VkResult r_ = (x);                                                    \
    if (r_ != VK_SUCCESS) {                                               \
      std::fprintf(stderr, "%s failed: %d\n", #x, static_cast<int>(r_)); \
      std::exit(2);                                                       \
    }                                                                     \
  } while (0)

struct Action {
  int frame;
  std::string kind, arg;
  float x = 0, y = 0;
};

std::vector<Action> LoadScript(const char* path) {
  std::vector<Action> actions;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    Action a;
    if (!(ss >> a.frame >> a.kind)) continue;
    if (a.kind == "click" || a.kind == "move" || a.kind == "press" || a.kind == "release") ss >> a.x >> a.y;
    std::getline(ss >> std::ws, a.arg);
    actions.push_back(a);
  }
  return actions;
}

void Inject(SDL_Window* window, const Action& a) {
  SDL_Event e;
  SDL_zero(e);
  const SDL_WindowID id = SDL_GetWindowID(window);
  if (a.kind == "key") {
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = id;
    e.key.key = static_cast<SDL_Keycode>(std::strtoul(a.arg.c_str(), nullptr, 0));
    e.key.down = true;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    SDL_PushEvent(&e);
  } else if (a.kind == "click") {
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.x = a.x;
    e.motion.y = a.y;
    SDL_PushEvent(&e);
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = id;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.x = a.x;
    e.button.y = a.y;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
    SDL_PushEvent(&e);
  } else if (a.kind == "press" || a.kind == "release") {
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.x = a.x;
    e.motion.y = a.y;
    SDL_PushEvent(&e);
    SDL_zero(e);
    e.type = a.kind == "press" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = id;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = a.kind == "press";
    e.button.x = a.x;
    e.button.y = a.y;
    SDL_PushEvent(&e);
  } else if (a.kind == "move") {
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.x = a.x;
    e.motion.y = a.y;
    SDL_PushEvent(&e);
  } else if (a.kind == "text") {
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = id;
    e.text.text = SDL_strdup(a.arg.c_str());
    SDL_PushEvent(&e);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 60;
  const std::vector<Action> script = argc > 2 ? LoadScript(argv[2]) : std::vector<Action>{};
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  SDL_Window* window = SDL_CreateWindow("nightcap-ac overlay test", 1280, 720, SDL_WINDOW_VULKAN);
  if (window == nullptr) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 2;
  }
  Uint32 ext_count = 0;
  const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = ext_count;
  ici.ppEnabledExtensionNames = exts;
  VkInstance instance;
  CHECK(vkCreateInstance(&ici, nullptr, &instance));
  VkSurfaceKHR surface;
  if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
    std::fprintf(stderr, "surface: %s\n", SDL_GetError());
    return 2;
  }
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  std::vector<VkPhysicalDevice> gpus(count);
  vkEnumeratePhysicalDevices(instance, &count, gpus.data());
  VkPhysicalDevice gpu = gpus.at(0);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
  uint32_t family = 0;
  for (uint32_t i = 0; i < count; ++i) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
      family = i;
      break;
    }
  }
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_exts;
  VkDevice device;
  CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);

  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &count, formats.data());
  VkSurfaceFormatKHR format = formats[0];
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) format = f;
  }
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) extent = {1280, 720};
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface;
  sci.minImageCount = std::max(caps.minImageCount, 2u);
  sci.imageFormat = format.format;
  sci.imageColorSpace = format.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sci.clipped = VK_TRUE;
  VkSwapchainKHR swapchain;
  CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
  vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
  std::vector<VkImage> images(count);
  vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = family;
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore acquired, rendered;
  vkCreateSemaphore(device, &semi, nullptr, &acquired);
  vkCreateSemaphore(device, &semi, nullptr, &rendered);
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  vkCreateFence(device, &fci, nullptr, &fence);

  for (int frame = 0; frame < frames; ++frame) {
    for (const Action& a : script) {
      if (a.frame == frame) Inject(window, a);
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_KEY_DOWN) std::printf("game got key %u\n", static_cast<unsigned>(e.key.key));
      if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) std::printf("game got click\n");
    }
    uint32_t index;
    CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    // Grass, with a sky band on top, so the overlay's translucency shows.
    VkClearColorValue grass = {{0.13f, 0.42f, 0.13f, 1.0f}};
    VkClearColorValue sky = {{0.45f, 0.62f, 0.85f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grass, 1, &range);
    (void)sky;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &rendered;
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &index;
    CHECK(vkQueuePresentKHR(queue, &present));
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);
    SDL_Delay(16);  // about 60 fps, so animations play out like in-game
  }
  vkDeviceWaitIdle(device);
  vkDestroySwapchainKHR(device, swapchain, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
  SDL_DestroyWindow(window);
  SDL_Quit();
  std::printf("done %d frames\n", frames);
  return 0;
}
