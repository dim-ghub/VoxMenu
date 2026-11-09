// State shared between the SDL side (main thread: events, cursor) and the
// Vulkan layer (present thread: drawing the menu).

#ifndef VOX_MENU_OVERLAY_H_
#define VOX_MENU_OVERLAY_H_

#include <SDL3/SDL.h>

#include <atomic>
#include <string>
#include <vector>

namespace nac::overlay {

// Input captured while the menu is open, in window (logical) coordinates.
struct Input {
  // kBlur: the user clicked into the game; drop keyboard focus.
  enum Type { kMousePos, kMouseButton, kWheel, kKey, kText, kBlur } type;
  float x = 0, y = 0;      // kMousePos, kWheel (x/y deltas)
  int button = 0;          // kMouseButton: 0 left, 1 right, 2 middle
  bool down = false;       // kMouseButton, kKey
  SDL_Keycode key = 0;     // kKey
  SDL_Keymod mod = 0;      // kKey
  std::string text;        // kText
};

void PushInput(const Input& input);
std::vector<Input> TakeInput();

// Menu visibility. Only the main thread opens or closes the menu (it owns
// the cursor); the menu asks with RequestClose().
bool MenuOpen();
void RequestClose();
bool TakeCloseRequest();
// From the launcher: open the menu (and raise the window).
void RequestOpen();
bool TakeOpenRequest();
void SetMenuOpenFlag(bool open);

// The menu wants keyboard text (a text field is focused).
void SetWantsText(bool wants);
bool WantsText();

// Window size in logical units and the last mouse position, for mapping
// input onto the swapchain.
struct Metrics {
  int window_w = 0, window_h = 0;
  float mouse_x = -1, mouse_y = -1;
};
Metrics GetMetrics();
void SetWindowSize(int w, int h);
void SetMousePos(float x, float y);

// --- While the menu is closed -------------------------------------------------
// Floating overlay windows (the automation window, click-point markers) still
// take the mouse where they are drawn, as long as the cursor is free.

struct Rect {
  float x0, y0, x1, y1;  // logical units
};
// Set by the UI each frame: where the overlay wants the mouse.
void SetHotRects(std::vector<Rect> rects);
bool InHotRect(float x, float y);
// Every click goes to the overlay (picking a click point on the screen).
void SetCaptureMouse(bool capture);
bool CaptureMouse();
// The UI handles Escape itself (leaving the layout editor, cancelling a pick).
void SetWantsEscape(bool wants);
bool WantsEscape();
// Main thread: whether the Roblox window has keyboard focus.
void SetWindowFocused(bool focused);
bool WindowFocused();
// Main thread: whether the cursor is free (not first-person mouse lock).
void SetCursorFree(bool free);
bool CursorFree();

// --- What the player is pressing, for the keystrokes HUD -------------------

void NoteKey(SDL_Scancode scancode, bool down);
bool KeyHeld(SDL_Scancode scancode);
void NoteButton(int button, bool down);  // 0 left, 1 right, 2 middle
bool ButtonHeld(int button);
int ClicksPerSecond(int button);

// Where the top-bar menu button sits, in logical coordinates; false when the
// button is turned off.
bool ButtonRect(int window_w, float* x, float* y, float* size);

}  // namespace nac::overlay

#endif  // VOX_MENU_OVERLAY_H_
