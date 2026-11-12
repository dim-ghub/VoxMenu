// Drawing the headset panel (ui::DrawVr) into the images of an OpenXR
// swapchain, on the Vulkan device the OpenXR session uses. That is the
// game's own device, which the Vulkan layer (layer.cc) already follows, so
// the same ImGui renderer works there.

#ifndef VOX_MENU_XR_RENDER_H_
#define VOX_MENU_XR_RENDER_H_

#include <vulkan/vulkan.h>

#include <vector>

namespace nac::xr_render {

struct Target;

// nullptr when the Vulkan layer doesn't follow `device`.
Target* Create(VkDevice device, uint32_t queue_family, uint32_t queue_index, VkFormat format, VkExtent2D extent,
               const std::vector<VkImage>& images);
void Destroy(Target* target);

// Where a controller points on the texture, in pixels.
struct Pointer {
  bool valid = false;
  float x = 0, y = 0;
  bool down = false;   // trigger held
  float wheel = 0;     // thumbstick, as scroll steps this frame
};

// Draws this frame into images[index], leaving it in
// COLOR_ATTACHMENT_OPTIMAL, and waits for the GPU. *stats_w is the stats
// pill's width in pixels (0 when none). False when nothing is on it.
bool Render(Target* target, uint32_t index, bool panel_open, const Pointer& pointer, float* stats_w);

}  // namespace nac::xr_render

#endif  // VOX_MENU_XR_RENDER_H_
