// SDL3 wrappers. This library is LD_PRELOADed into Nightcap, so these
// definitions come before libSDL3's. Each one calls the real SDL function and
// adds a little on top:
//
//   * Text input: track the focused Roblox text box to know what you sent,
//     from the physical keyboard here and the VR keyboard (vr_keyboard.cc).
//   * Events: open/close the menu (F8 or the top-bar button) and, while it
//     is open, give mouse and keyboard to the menu instead of the game.
//     While it is closed, the floating automation window and click-point
//     markers still take the mouse where they are drawn.
//   * Automation: hotkeys, recording macros, and the clicks and keys the
//     autoclicker and macros inject (automation.cc).
//   * Mouse mode: while the menu is open, keep the cursor free and visible
//     even if Nightcap asks for first-person mouse lock; restore it after.
//   * Audio: remember which streams are microphones and mix speech into
//     what Nightcap reads from them.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>

#include "app.h"
#include "automation.h"
#include "chat.h"
#include "control.h"
#include "inject.h"
#include "nightcap.h"
#include "overlay.h"
#include "servers.h"
#include "speech.h"
#include "vr_keyboard.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace nac {
namespace {

// ---------------------------------------------------------------------------
// Chat

// Fed from the main thread and, for the VR keyboard, Nightcap's render thread.
std::mutex g_chat_mutex;
ChatTracker g_chat;
bool g_roblox_text_active = false;
SDL_WindowID g_window_id = 0;

// Speaks a message that was just sent. Returns true when Intercept Chat
// wants it kept out of the game chat.
bool SpeakSent(const std::string& message) {
  const Config cfg = GetConfig();
  if (!cfg.enabled) return false;
  if (cfg.skip_commands && !message.empty() && message[0] == '/') return false;
  Log("speaking a chat message (%zu bytes)", message.size());
  Say(message);
  return cfg.intercept_chat;
}

// How many Backspaces clear a box that held `chars` characters, in case
// Ctrl+A is not handled.
size_t ClearPresses(size_t chars) { return std::min<size_t>(chars + 2, 100); }

// Injected synthetic events (for intercepting chat while clearing the text box).
std::mutex g_injected_mutex;
std::deque<SDL_Event> g_injected_events;
bool g_swallow_return_up = false;

bool PopInjectedEvent(SDL_Event* event) {
  std::lock_guard<std::mutex> lock(g_injected_mutex);
  if (g_injected_events.empty()) return false;
  if (event != nullptr) {
    *event = g_injected_events.front();
  }
  g_injected_events.pop_front();
  return true;
}

void InjectKeyEvent(SDL_EventType type, SDL_Keycode key, SDL_Scancode scancode,
                    SDL_Keymod mod, SDL_WindowID win) {
  SDL_Event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.key.type = type;
  ev.key.timestamp = SDL_GetTicksNS();
  ev.key.windowID = win;
  ev.key.key = key;
  ev.key.scancode = scancode;
  ev.key.mod = mod;
  ev.key.down = (type == SDL_EVENT_KEY_DOWN);
  ev.key.repeat = false;
  g_injected_events.push_back(ev);
}

// The last real mouse device, for injected mouse events.
SDL_MouseID g_mouse_id = 0;
SDL_MouseButtonFlags g_injected_buttons = 0;

void InjectKeyPress(SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod, SDL_WindowID win) {
  InjectKeyEvent(SDL_EVENT_KEY_DOWN, key, scancode, mod, win);
  InjectKeyEvent(SDL_EVENT_KEY_UP, key, scancode, mod, win);
}

void QueueClearAndCloseChat(SDL_WindowID win, size_t char_count) {
  std::lock_guard<std::mutex> lock(g_injected_mutex);

  // 1. Select all text with Ctrl+A
  InjectKeyEvent(SDL_EVENT_KEY_DOWN, SDLK_LCTRL, SDL_SCANCODE_LCTRL, SDL_KMOD_LCTRL, win);
  InjectKeyEvent(SDL_EVENT_KEY_DOWN, SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL, win);
  InjectKeyEvent(SDL_EVENT_KEY_UP, SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL, win);
  InjectKeyEvent(SDL_EVENT_KEY_UP, SDLK_LCTRL, SDL_SCANCODE_LCTRL, 0, win);

  // 2. Delete selected text with Backspace
  InjectKeyPress(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 0, win);

  // 3. Extra backspaces to ensure all characters are erased even if Ctrl+A is unhandled
  const size_t extra = ClearPresses(char_count);
  for (size_t i = 0; i < extra; ++i) {
    InjectKeyPress(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, 0, win);
  }

  // 4. Close the chatbox by sending Escape
  InjectKeyPress(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, 0, win);
}

bool ObserveChat(SDL_Event& e) {
  std::unique_lock<std::mutex> lock(g_chat_mutex);
  if (!g_chat.active()) return false;
  if (e.type == SDL_EVENT_TEXT_INPUT && e.text.text != nullptr) {
    g_chat.Text(e.text.text);
    return false;
  }
  if (e.type != SDL_EVENT_KEY_DOWN && e.type != SDL_EVENT_KEY_UP) return false;

  if (e.type == SDL_EVENT_KEY_DOWN) {
    const bool ctrl = (e.key.mod & SDL_KMOD_CTRL) != 0;
    std::optional<std::string> sent;
    switch (e.key.key) {
      case SDLK_A:
        if (ctrl) g_chat.Key(ChatKey::kSelectAll, ctrl);
        break;
      case SDLK_RETURN:
      case SDLK_KP_ENTER: sent = g_chat.Key(ChatKey::kReturn, ctrl); break;
      case SDLK_ESCAPE: g_chat.Key(ChatKey::kEscape, ctrl); break;
      case SDLK_BACKSPACE: g_chat.Key(ChatKey::kBackspace, ctrl); break;
      case SDLK_DELETE: g_chat.Key(ChatKey::kDelete, ctrl); break;
      case SDLK_LEFT: g_chat.Key(ChatKey::kLeft, ctrl); break;
      case SDLK_RIGHT: g_chat.Key(ChatKey::kRight, ctrl); break;
      case SDLK_HOME: g_chat.Key(ChatKey::kHome, ctrl); break;
      case SDLK_END: g_chat.Key(ChatKey::kEnd, ctrl); break;
      default: break;
    }
    if (sent) {
      const size_t count = g_chat.last_length() > 0 ? g_chat.last_length() : sent->size();
      lock.unlock();
      if (SpeakSent(*sent)) {
        g_swallow_return_up = true;
        QueueClearAndCloseChat(e.key.windowID ? e.key.windowID : g_window_id, count);
        return true;  // Swallow Return key down
      }
    }
  } else if (e.type == SDL_EVENT_KEY_UP) {
    if (g_swallow_return_up && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER)) {
      g_swallow_return_up = false;
      return true;  // Swallow Return key up
    }
  }
  return false;
}

// The VR keyboard sends SDL scancodes straight to Roblox (render thread).
void OnVrText(const char* utf8) {
  std::lock_guard<std::mutex> lock(g_chat_mutex);
  if (g_chat.active()) g_chat.Text(utf8);
}

bool OnVrKey(std::uint32_t scancode, bool ctrl) {
  std::optional<std::string> sent;
  size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    if (!g_chat.active()) return false;
    switch (scancode) {
      case SDL_SCANCODE_RETURN:
      case SDL_SCANCODE_KP_ENTER: sent = g_chat.Key(ChatKey::kReturn, ctrl); break;
      case SDL_SCANCODE_ESCAPE: g_chat.Key(ChatKey::kEscape, ctrl); break;
      case SDL_SCANCODE_BACKSPACE: g_chat.Key(ChatKey::kBackspace, ctrl); break;
      case SDL_SCANCODE_DELETE: g_chat.Key(ChatKey::kDelete, ctrl); break;
      case SDL_SCANCODE_LEFT: g_chat.Key(ChatKey::kLeft, ctrl); break;
      case SDL_SCANCODE_RIGHT: g_chat.Key(ChatKey::kRight, ctrl); break;
      case SDL_SCANCODE_HOME: g_chat.Key(ChatKey::kHome, ctrl); break;
      case SDL_SCANCODE_END: g_chat.Key(ChatKey::kEnd, ctrl); break;
      case SDL_SCANCODE_A:
        if (ctrl) g_chat.Key(ChatKey::kSelectAll, ctrl);
        break;
      default: break;
    }
    if (!sent) return false;
    count = g_chat.last_length() > 0 ? g_chat.last_length() : sent->size();
  }
  if (!SpeakSent(*sent)) return false;
  // Intercept Chat: Enter never reaches Roblox; clear and close the box
  // through the VR keyboard's own path instead.
  VrPressKey(SDL_SCANCODE_A, true);
  for (size_t i = 0; i <= ClearPresses(count); ++i) VrPressKey(SDL_SCANCODE_BACKSPACE, false);
  VrPressKey(SDL_SCANCODE_ESCAPE, false);
  return true;
}

// ---------------------------------------------------------------------------
// Menu and cursor

// What Nightcap last asked for while the menu held the cursor.
bool g_want_relative = false;
bool g_want_cursor_visible = true;
bool g_swallow_button_up = false;
bool g_menu_started_text = false;

SDL_Window* Window() { return g_window_id ? SDL_GetWindowFromID(g_window_id) : nullptr; }

void SetMenuOpen(bool open) {
  if (open == overlay::MenuOpen()) return;
  SDL_Window* window = Window();
  if (open) {
    g_want_relative = window && SDL_GetWindowRelativeMouseMode(window);
    g_want_cursor_visible = SDL_CursorVisible();
    overlay::SetMenuOpenFlag(true);
    if (window) Real<bool (*)(SDL_Window*, bool)>("SDL_SetWindowRelativeMouseMode")(window, false);
    Real<bool (*)()>("SDL_ShowCursor")();
  } else {
    overlay::SetMenuOpenFlag(false);
    if (window) {
      Real<bool (*)(SDL_Window*, bool)>("SDL_SetWindowRelativeMouseMode")(window, g_want_relative);
    }
    if (!g_want_cursor_visible) Real<bool (*)()>("SDL_HideCursor")();
    if (g_menu_started_text && !g_roblox_text_active && window) {
      Real<bool (*)(SDL_Window*)>("SDL_StopTextInput")(window);
    }
    g_menu_started_text = false;
    overlay::SetWantsText(false);
  }
}

// Once per event pump: apply requests coming from the menu.
void MainThreadTick() {
  StartControl();
  nightcap::Attach();
  VrKeyboardObserver vr;
  vr.text = OnVrText;
  vr.key = OnVrKey;
  HookVrKeyboard(vr);
  if (overlay::TakeCloseRequest()) SetMenuOpen(false);
  if (overlay::TakeOpenRequest() && Window() != nullptr) {
    SetMenuOpen(true);
    SDL_RaiseWindow(Window());
  }
  SDL_Window* window = Window();
  overlay::SetCursorFree(overlay::MenuOpen() || window == nullptr ||
                         !Real<bool (*)(SDL_Window*)>("SDL_GetWindowRelativeMouseMode")(window));
  automation::Tick();
  std::string clip;
  if (servers::TakeClipboard(&clip)) SDL_SetClipboardText(clip.c_str());
  if (window == nullptr) return;
  // The menu, or a field in the floating window, wants typed text.
  const bool wants = overlay::WantsText();
  if (wants && !g_menu_started_text && !g_roblox_text_active) {
    g_menu_started_text = Real<bool (*)(SDL_Window*)>("SDL_StartTextInput")(window);
  } else if (!wants && g_menu_started_text) {
    if (!g_roblox_text_active) Real<bool (*)(SDL_Window*)>("SDL_StopTextInput")(window);
    g_menu_started_text = false;
  }
}

bool InButton(float x, float y) {
  const overlay::Metrics m = overlay::GetMetrics();
  float bx, by, size;
  if (!overlay::ButtonRect(m.window_w, &bx, &by, &size)) return false;
  return x >= bx && x < bx + size && y >= by && y < by + size;
}

void TrackWindow(SDL_WindowID id) {
  if (id == 0) return;
  g_window_id = id;
  if (SDL_Window* w = SDL_GetWindowFromID(id)) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    overlay::SetWindowSize(ww, wh);
  }
}

// Mouse buttons pressed on a floating window; their release (and the drag
// in between) belongs to it too.
unsigned g_floating_buttons = 0;

// With the menu closed: input for the floating windows. True when it must
// not reach the game.
bool FilterFloating(const SDL_Event& e, int button, bool cursor_free) {
  overlay::Input in;
  switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
      if (!cursor_free && g_floating_buttons == 0) return false;
      in.type = overlay::Input::kMousePos;
      in.x = e.motion.x;
      in.y = e.motion.y;
      overlay::PushInput(in);
      return g_floating_buttons != 0;  // dragging a window: keep the camera still
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      if (button < 0) return false;
      const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      const unsigned bit = 1u << button;
      const bool ours = down ? cursor_free && (overlay::CaptureMouse() || overlay::InHotRect(e.button.x, e.button.y))
                             : (g_floating_buttons & bit) != 0;
      if (!ours) {
        // Clicking into the game takes the keyboard back from a text field.
        if (down && overlay::WantsText()) {
          in.type = overlay::Input::kBlur;
          overlay::PushInput(in);
        }
        return false;
      }
      g_floating_buttons = down ? g_floating_buttons | bit : g_floating_buttons & ~bit;
      in.type = overlay::Input::kMouseButton;
      in.down = down;
      in.button = button;
      in.x = e.button.x;
      in.y = e.button.y;
      overlay::PushInput(in);
      return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      const overlay::Metrics m = overlay::GetMetrics();
      if (!cursor_free || !overlay::InHotRect(m.mouse_x, m.mouse_y)) return false;
      in.type = overlay::Input::kWheel;
      in.x = e.wheel.x;
      in.y = e.wheel.y;
      overlay::PushInput(in);
      return true;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      // Typing into a field of the floating window.
      if (!overlay::WantsText()) return false;
      if (e.key.key == SDLK_ESCAPE) {
        in.type = overlay::Input::kBlur;
      } else {
        in.type = overlay::Input::kKey;
        in.down = e.type == SDL_EVENT_KEY_DOWN;
        in.key = e.key.key;
        in.mod = e.key.mod;
      }
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_TEXT_INPUT:
      if (!overlay::WantsText() || e.text.text == nullptr) return false;
      in.type = overlay::Input::kText;
      in.text = e.text.text;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
      return overlay::WantsText();
    default:
      return false;
  }
}

// Returns true when the event belongs to us and must not reach Nightcap.
bool Filter(SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: TrackWindow(e.key.windowID); break;
    case SDL_EVENT_MOUSE_MOTION: TrackWindow(e.motion.windowID); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: TrackWindow(e.button.windowID); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      overlay::SetWindowFocused(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: TrackWindow(e.window.windowID); break;
    default: break;
  }

  const bool is_key = e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP;
  const bool is_button = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP;
  const int button = !is_button                             ? -1
                     : e.button.button == SDL_BUTTON_LEFT   ? 0
                     : e.button.button == SDL_BUTTON_RIGHT  ? 1
                     : e.button.button == SDL_BUTTON_MIDDLE ? 2
                                                            : -1;
  // What the player presses, for the keystrokes HUD.
  if (is_key) overlay::NoteKey(e.key.scancode, e.type == SDL_EVENT_KEY_DOWN);
  if (button >= 0) overlay::NoteButton(button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
  if (e.type == SDL_EVENT_MOUSE_MOTION) g_mouse_id = e.motion.which;
  if (is_button) g_mouse_id = e.button.which;

  const bool open = overlay::MenuOpen();
  if (is_key && e.key.key == SDLK_F8) {
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) SetMenuOpen(!open);
    return true;
  }
  // The layout editor or a click-point pick: Escape backs out of that.
  if (is_key && e.key.key == SDLK_ESCAPE && overlay::WantsEscape()) {
    overlay::Input in;
    in.type = overlay::Input::kKey;
    in.down = e.type == SDL_EVENT_KEY_DOWN;
    in.key = e.key.key;
    overlay::PushInput(in);
    return true;
  }
  if (is_key && !overlay::WantsText() && automation::HandleHotkey(e.key)) return true;
  if (!open) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) overlay::SetMousePos(e.motion.x, e.motion.y);
    // The top-bar button and floating windows work whenever the cursor is
    // free (not in first-person mouse lock).
    SDL_Window* window = Window();
    const bool cursor_free = window == nullptr || !SDL_GetWindowRelativeMouseMode(window);
    if (FilterFloating(e, button, cursor_free)) return true;
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT &&
        cursor_free && InButton(e.button.x, e.button.y)) {
      g_swallow_button_up = true;
      SetMenuOpen(true);
      return true;
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT &&
        g_swallow_button_up) {
      g_swallow_button_up = false;
      return true;
    }
    if (ObserveChat(e)) return true;
    automation::Observe(e);
    return false;
  }

  // Menu open: it gets the mouse and keyboard.
  overlay::Input in;
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
      if (e.key.key == SDLK_ESCAPE) {
        SetMenuOpen(false);
        return true;
      }
      [[fallthrough]];
    case SDL_EVENT_KEY_UP:
      in.type = overlay::Input::kKey;
      in.down = e.type == SDL_EVENT_KEY_DOWN;
      in.key = e.key.key;
      in.mod = e.key.mod;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_TEXT_INPUT:
      if (e.text.text != nullptr) {
        in.type = overlay::Input::kText;
        in.text = e.text.text;
        overlay::PushInput(in);
      }
      return true;
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
      return true;
    case SDL_EVENT_MOUSE_MOTION:
      overlay::SetMousePos(e.motion.x, e.motion.y);
      in.type = overlay::Input::kMousePos;
      in.x = e.motion.x;
      in.y = e.motion.y;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      in.type = overlay::Input::kMouseButton;
      in.down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      in.button = e.button.button == SDL_BUTTON_LEFT    ? 0
                  : e.button.button == SDL_BUTTON_RIGHT ? 1
                                                        : 2;
      in.x = e.button.x;
      in.y = e.button.y;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_MOUSE_WHEEL:
      in.type = overlay::Input::kWheel;
      in.x = e.wheel.x;
      in.y = e.wheel.y;
      overlay::PushInput(in);
      return true;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Microphone streams

std::mutex g_audio_mutex;
std::unordered_set<SDL_AudioStream*> g_mic_streams;

bool IsRecording(SDL_AudioDeviceID devid) {
  if (devid == SDL_AUDIO_DEVICE_DEFAULT_RECORDING) return true;
  if (devid == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK || devid == 0) return false;
  return !SDL_IsAudioDevicePlayback(devid);
}

void TrackMic(SDL_AudioStream* stream) {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  g_mic_streams.insert(stream);
}

bool IsMic(SDL_AudioStream* stream) {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  return g_mic_streams.count(stream) != 0;
}

template <typename Fn>
Fn Cached(Fn& slot, const char* name) {
  if (slot == nullptr) slot = Real<Fn>(name);
  return slot;
}

}  // namespace

// ---------------------------------------------------------------------------
// Injected input (automation)

namespace inject {

void MouseMove(float x, float y) {
  SDL_Event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.type = SDL_EVENT_MOUSE_MOTION;
  ev.motion.timestamp = SDL_GetTicksNS();
  ev.motion.windowID = g_window_id;
  ev.motion.which = g_mouse_id;
  ev.motion.state = g_injected_buttons;
  ev.motion.x = x;
  ev.motion.y = y;
  std::lock_guard<std::mutex> lock(g_injected_mutex);
  g_injected_events.push_back(ev);
}

void MouseButton(int button, bool down, float x, float y) {
  const Uint8 sdl_button = button == 1 ? SDL_BUTTON_RIGHT : button == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_LEFT;
  SDL_Event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  ev.button.timestamp = SDL_GetTicksNS();
  ev.button.windowID = g_window_id;
  ev.button.which = g_mouse_id;
  ev.button.button = sdl_button;
  ev.button.down = down;
  ev.button.clicks = 1;
  ev.button.x = x;
  ev.button.y = y;
  if (down) g_injected_buttons |= SDL_BUTTON_MASK(sdl_button);
  else g_injected_buttons &= ~SDL_BUTTON_MASK(sdl_button);
  std::lock_guard<std::mutex> lock(g_injected_mutex);
  g_injected_events.push_back(ev);
}

void Key(SDL_Scancode scancode, bool down) {
  std::lock_guard<std::mutex> lock(g_injected_mutex);
  InjectKeyEvent(down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false),
                 scancode, SDL_KMOD_NONE, g_window_id);
}

void Text(const std::string& utf8) {
  // SDL events point at their text; keep the last few alive until read.
  static std::deque<std::string> texts;
  std::lock_guard<std::mutex> lock(g_injected_mutex);
  texts.push_back(utf8);
  if (texts.size() > 256) texts.pop_front();
  SDL_Event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.type = SDL_EVENT_TEXT_INPUT;
  ev.text.timestamp = SDL_GetTicksNS();
  ev.text.windowID = g_window_id;
  ev.text.text = texts.back().c_str();
  g_injected_events.push_back(ev);
}

void Warp(float x, float y) {
  if (SDL_Window* window = Window()) Real<void (*)(SDL_Window*, float, float)>("SDL_WarpMouseInWindow")(window, x, y);
}

}  // namespace inject
}  // namespace nac

using namespace nac;

// --- text input -------------------------------------------------------------

EXPORT bool SDL_StartTextInputWithProperties(SDL_Window* window, SDL_PropertiesID props) {
  static bool (*fn)(SDL_Window*, SDL_PropertiesID);
  const bool ok = Cached(fn, "SDL_StartTextInputWithProperties")(window, props);
  if (ok) {
    const Sint64 type = SDL_GetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, 0);
    const bool multiline = SDL_GetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
    const bool secret = type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN ||
                        type == SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_VISIBLE ||
                        type == SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN ||
                        type == SDL_TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE;
    g_roblox_text_active = true;
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    g_chat.Open(secret, multiline);
  }
  return ok;
}

EXPORT bool SDL_StartTextInput(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  const bool ok = Cached(fn, "SDL_StartTextInput")(window);
  if (ok) {
    g_roblox_text_active = true;
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    g_chat.Open(false, false);
  }
  return ok;
}

EXPORT bool SDL_StopTextInput(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  g_roblox_text_active = false;
  {
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    g_chat.Close();
  }
  // Keep text flowing to the menu if one of its fields still has focus.
  if ((overlay::MenuOpen() || overlay::WantsText()) && g_menu_started_text) return true;
  return Cached(fn, "SDL_StopTextInput")(window);
}

EXPORT char* SDL_GetClipboardText(void) {
  static char* (*fn)(void);
  char* text = Cached(fn, "SDL_GetClipboardText")();
  // Roblox reads the clipboard when you paste into a focused box.
  if (text != nullptr && !overlay::MenuOpen()) {
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    if (g_chat.active()) g_chat.Paste(text);
  }
  return text;
}

// --- events -----------------------------------------------------------------

EXPORT bool SDL_PollEvent(SDL_Event* event) {
  static bool (*fn)(SDL_Event*);
  Cached(fn, "SDL_PollEvent");
  MainThreadTick();
  if (event == nullptr) return fn(nullptr);
  if (PopInjectedEvent(event)) return true;
  while (fn(event)) {
    if (!Filter(*event)) return true;
    if (PopInjectedEvent(event)) return true;
  }
  return false;
}

EXPORT bool SDL_WaitEvent(SDL_Event* event) {
  static bool (*fn)(SDL_Event*);
  Cached(fn, "SDL_WaitEvent");
  MainThreadTick();
  if (event == nullptr) return fn(nullptr);
  if (PopInjectedEvent(event)) return true;
  while (fn(event)) {
    if (!Filter(*event)) return true;
    if (PopInjectedEvent(event)) return true;
  }
  return false;
}

EXPORT bool SDL_WaitEventTimeout(SDL_Event* event, Sint32 timeout_ms) {
  static bool (*fn)(SDL_Event*, Sint32);
  Cached(fn, "SDL_WaitEventTimeout");
  MainThreadTick();
  if (event == nullptr) return fn(nullptr, timeout_ms);
  if (PopInjectedEvent(event)) return true;
  while (fn(event, timeout_ms)) {
    if (!Filter(*event)) return true;
    if (PopInjectedEvent(event)) return true;
    timeout_ms = 0;  // one consumed event already used up the wait
  }
  return false;
}

EXPORT int SDL_PeepEvents(SDL_Event* events, int numevents, SDL_EventAction action,
                          Uint32 min_type, Uint32 max_type) {
  static int (*fn)(SDL_Event*, int, SDL_EventAction, Uint32, Uint32);
  if (action == SDL_GETEVENT && events != nullptr && numevents > 0) {
    int injected_count = 0;
    while (injected_count < numevents) {
      if (!PopInjectedEvent(&events[injected_count])) break;
      ++injected_count;
    }
    if (injected_count > 0) return injected_count;
  }
  const int count = Cached(fn, "SDL_PeepEvents")(events, numevents, action, min_type, max_type);
  // Only removed events count; a peek would see the same event again later.
  if (action != SDL_GETEVENT || events == nullptr || count <= 0) return count;
  MainThreadTick();
  int kept = 0;
  for (int i = 0; i < count; ++i) {
    if (!Filter(events[i])) {
      events[kept++] = events[i];
    } else {
      while (kept < numevents && PopInjectedEvent(&events[kept])) {
        ++kept;
      }
    }
  }
  return kept;
}

// --- mouse mode while the menu is open -----------------------------------------

EXPORT bool SDL_SetWindowRelativeMouseMode(SDL_Window* window, bool enabled) {
  static bool (*fn)(SDL_Window*, bool);
  if (overlay::MenuOpen()) {
    g_want_relative = enabled;
    return true;
  }
  return Cached(fn, "SDL_SetWindowRelativeMouseMode")(window, enabled);
}

EXPORT bool SDL_GetWindowRelativeMouseMode(SDL_Window* window) {
  static bool (*fn)(SDL_Window*);
  if (overlay::MenuOpen()) return g_want_relative;
  return Cached(fn, "SDL_GetWindowRelativeMouseMode")(window);
}

EXPORT bool SDL_HideCursor(void) {
  static bool (*fn)(void);
  if (overlay::MenuOpen()) {
    g_want_cursor_visible = false;
    return true;
  }
  return Cached(fn, "SDL_HideCursor")();
}

EXPORT bool SDL_ShowCursor(void) {
  static bool (*fn)(void);
  if (overlay::MenuOpen()) {
    g_want_cursor_visible = true;
    return true;
  }
  return Cached(fn, "SDL_ShowCursor")();
}

EXPORT void SDL_WarpMouseInWindow(SDL_Window* window, float x, float y) {
  static void (*fn)(SDL_Window*, float, float);
  if (overlay::MenuOpen()) return;  // first-person re-centering
  Cached(fn, "SDL_WarpMouseInWindow")(window, x, y);
}

// --- microphone ---------------------------------------------------------------

EXPORT SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                                  SDL_AudioStreamCallback callback, void* userdata) {
  static SDL_AudioStream* (*fn)(SDL_AudioDeviceID, const SDL_AudioSpec*, SDL_AudioStreamCallback, void*);
  SDL_AudioStream* stream = Cached(fn, "SDL_OpenAudioDeviceStream")(devid, spec, callback, userdata);
  if (stream != nullptr && IsRecording(devid)) TrackMic(stream);
  return stream;
}

EXPORT bool SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream* stream) {
  static bool (*fn)(SDL_AudioDeviceID, SDL_AudioStream*);
  const bool ok = Cached(fn, "SDL_BindAudioStream")(devid, stream);
  if (ok && stream != nullptr && IsRecording(devid)) TrackMic(stream);
  return ok;
}

EXPORT bool SDL_BindAudioStreams(SDL_AudioDeviceID devid, SDL_AudioStream* const* streams, int num) {
  static bool (*fn)(SDL_AudioDeviceID, SDL_AudioStream* const*, int);
  const bool ok = Cached(fn, "SDL_BindAudioStreams")(devid, streams, num);
  if (ok && streams != nullptr && IsRecording(devid)) {
    for (int i = 0; i < num; ++i) TrackMic(streams[i]);
  }
  return ok;
}

EXPORT void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
  static void (*fn)(SDL_AudioStream*);
  {
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_mic_streams.erase(stream);
  }
  Cached(fn, "SDL_DestroyAudioStream")(stream);
}

EXPORT int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* buf, int len) {
  static int (*fn)(SDL_AudioStream*, void*, int);
  const int got = Cached(fn, "SDL_GetAudioStreamData")(stream, buf, len);
  if (got > 0 && IsMic(stream)) {
    SDL_AudioSpec dst{};
    if (SDL_GetAudioStreamFormat(stream, nullptr, &dst)) MixIntoMic(buf, got, dst);
  }
  return got;
}
