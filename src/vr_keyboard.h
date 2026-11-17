// Wraps the VR keyboard's host callbacks so its typing reaches the chat
// tracker (it bypasses SDL events).

#ifndef VOX_MENU_VR_KEYBOARD_H_
#define VOX_MENU_VR_KEYBOARD_H_

#include <cstdint>

namespace nac {

struct VrKeyboardObserver {
  // Text the keyboard is about to commit (UTF-8).
  void (*text)(const char* utf8) = nullptr;
  // A key the keyboard is about to press (an SDL scancode, with Ctrl held
  // when `ctrl`). Returns true to keep it from Roblox.
  bool (*key)(std::uint32_t scancode, bool ctrl) = nullptr;
};

// Main thread, once per event pump. Cheap after the first call. The
// observer is called from Nightcap's VR render thread.
void HookVrKeyboard(const VrKeyboardObserver& observer);

// Presses a key through the in-headset keyboard's path, skipping the
// observer. Any thread. Returns false when there is no VR keyboard.
bool VrPressKey(std::uint32_t scancode, bool ctrl);

}  // namespace nac

#endif  // VOX_MENU_VR_KEYBOARD_H_
