// Nightcap's in-headset (VR) keyboard types straight into Roblox through
// Nightcap's own input queue, so its keys and text never pass through the
// SDL event functions this library wraps. This finds the keyboard's host
// callbacks in Nightcap (by symbol name, from /proc/self/exe) and puts ours
// in front of them, so VR typing reaches the chat tracker like a physical
// keyboard does.

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
