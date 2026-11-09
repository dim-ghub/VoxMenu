#include "overlay.h"

#include <chrono>
#include <deque>
#include <mutex>

#include "app.h"

namespace nac::overlay {
namespace {

std::mutex g_mutex;
std::vector<Input> g_input;
Metrics g_metrics;
std::atomic<bool> g_open{false};
std::atomic<bool> g_close_requested{false};
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_wants_text{false};
std::atomic<bool> g_capture_mouse{false};
std::atomic<bool> g_wants_escape{false};
std::atomic<bool> g_cursor_free{true};
std::atomic<bool> g_focused{true};
std::vector<Rect> g_hot;  // under g_mutex

std::atomic<bool> g_keys[SDL_SCANCODE_COUNT];
std::atomic<bool> g_buttons[3];
std::mutex g_click_mutex;
std::deque<std::chrono::steady_clock::time_point> g_clicks[3];

void DropOldClicks(std::deque<std::chrono::steady_clock::time_point>& q,
                   std::chrono::steady_clock::time_point now) {
  while (!q.empty() && now - q.front() > std::chrono::seconds(1)) q.pop_front();
}

}  // namespace

void PushInput(const Input& input) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_input.size() < 4096) g_input.push_back(input);
}

std::vector<Input> TakeInput() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<Input> out;
  out.swap(g_input);
  return out;
}

bool MenuOpen() { return g_open.load(); }
void RequestClose() { g_close_requested = true; }
bool TakeCloseRequest() { return g_close_requested.exchange(false); }
void RequestOpen() { g_open_requested = true; }
bool TakeOpenRequest() { return g_open_requested.exchange(false); }

void SetMenuOpenFlag(bool open) {
  g_open = open;
  if (!open) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_input.clear();
  }
}

void SetWantsText(bool wants) { g_wants_text = wants; }
bool WantsText() { return g_wants_text.load(); }

Metrics GetMetrics() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_metrics;
}

void SetWindowSize(int w, int h) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_metrics.window_w = w;
  g_metrics.window_h = h;
}

void SetMousePos(float x, float y) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_metrics.mouse_x = x;
  g_metrics.mouse_y = y;
}

void SetHotRects(std::vector<Rect> rects) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hot = std::move(rects);
}

bool InHotRect(float x, float y) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Rect& r : g_hot) {
    if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return true;
  }
  return false;
}

void SetCaptureMouse(bool capture) { g_capture_mouse = capture; }
bool CaptureMouse() { return g_capture_mouse.load(); }
void SetWantsEscape(bool wants) { g_wants_escape = wants; }
bool WantsEscape() { return g_wants_escape.load(); }
void SetWindowFocused(bool focused) { g_focused = focused; }
bool WindowFocused() { return g_focused.load(); }
void SetCursorFree(bool free) { g_cursor_free = free; }
bool CursorFree() { return g_cursor_free.load(); }

void NoteKey(SDL_Scancode scancode, bool down) {
  if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) g_keys[scancode] = down;
}

bool KeyHeld(SDL_Scancode scancode) {
  return scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT && g_keys[scancode].load();
}

void NoteButton(int button, bool down) {
  if (button < 0 || button > 2) return;
  if (down && !g_buttons[button]) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_click_mutex);
    g_clicks[button].push_back(now);
    DropOldClicks(g_clicks[button], now);
  }
  g_buttons[button] = down;
}

bool ButtonHeld(int button) { return button >= 0 && button <= 2 && g_buttons[button].load(); }

int ClicksPerSecond(int button) {
  if (button < 0 || button > 2) return 0;
  std::lock_guard<std::mutex> lock(g_click_mutex);
  DropOldClicks(g_clicks[button], std::chrono::steady_clock::now());
  return static_cast<int>(g_clicks[button].size());
}

bool ButtonRect(int window_w, float* x, float* y, float* size) {
  const std::string where = GetConfig().menu_button;
  if (where == "off") return false;
  // Roblox's top bar: a 44 px tall row 12 px from the top. The Roblox
  // button and the menu/chat/mic pill end around x = 208, so ours sits just
  // after them, or mirrored on the right.
  *size = 44;
  *y = 12;
  *x = where == "right" ? window_w - 16 - *size : 216;
  return true;
}

}  // namespace nac::overlay
