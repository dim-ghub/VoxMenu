// The autoclicker and macros: recorded or hand-written sequences of key
// presses, clicks, mouse moves, waits and typed text, played back into the
// game. Everything is injected through Nightcap's own event queue
// (inject.h), so it only ever reaches the Roblox window.
//
// The UI asks for things from the render thread; the work happens on the
// main thread in Tick(), once per event poll.

#ifndef VOX_MENU_AUTOMATION_H_
#define VOX_MENU_AUTOMATION_H_

#include <SDL3/SDL.h>

#include <string>
#include <utility>
#include <vector>

namespace nac::automation {

struct Step {
  enum Kind { kKeyDown, kKeyUp, kKeyTap, kMouseDown, kMouseUp, kClick, kMove, kWait, kText };
  Kind kind = kWait;
  int code = 0;          // scancode, or mouse button (0 left, 1 right, 2 middle)
  float x = -1, y = -1;  // fractions of the window; below 0: wherever the cursor is
  int ms = 0;            // kWait
  std::string text;      // kText
};

struct Macro {
  std::vector<Step> steps;
};

// Plain-text macro files, one step per line ("key_down W", "wait 120",
// "click left 0.5 0.5", "text gg", ...).
std::string FormatMacro(const Macro& macro);
Macro ParseMacro(const std::string& text);
std::string DescribeStep(const Step& step);

// Saved macros live in $XDG_CONFIG_HOME/vox-menu/macros/<name>.macro.
std::string SafeName(const std::string& name);
std::vector<std::string> ListMacros();
bool LoadMacro(const std::string& name, Macro* macro);
bool SaveMacro(const std::string& name, const Macro& macro);
bool DeleteMacro(const std::string& name);

// The macro in the editor, which playback uses and recording replaces.
// The revision changes whenever recording replaces it.
Macro Current();
void SetCurrent(const Macro& macro);
int Revision();

// Click points stored in the config as "x,y;x,y".
std::vector<std::pair<float, float>> ParsePoints(const std::string& text);
std::string FormatPoints(const std::vector<std::pair<float, float>>& points);

struct Status {
  bool clicking = false;
  float countdown = 0;  // seconds before clicking starts
  int clicks = 0;
  float elapsed = 0;    // seconds clicking
  bool recording = false;
  int recorded = 0;     // steps so far
  bool playing = false;
  int step = 0, steps = 0;
  int loop = 0;
};
Status GetStatus();

// From any thread; carried out on the main thread.
void ToggleClicker();
// Hold-to-click: starts on true, stops on false.
void HoldClicker(bool down);
void ToggleRecording();
void TogglePlayback();
void StopAll();
// The window hotkey: the menu's draw applies it to the config it holds,
// so the toggle can't be lost to a frame that saves its own copy.
void ToggleWindow();
bool TakeWindowToggle();

// --- Main thread ----------------------------------------------------------------

// Injects whatever is due.
void Tick();
// One of the automation hotkeys: true when it must not reach the game.
bool HandleHotkey(const SDL_KeyboardEvent& key);
// The same for the mouse's side buttons (SDL_BUTTON_X1 / X2), which can be
// hotkeys too ("Mouse4", "Mouse5").
bool HandleHotkeyButton(int sdl_button, bool down);
// Whether a key event is the hotkey named in the config ("F6", "off").
bool KeyMatches(const std::string& name, const SDL_KeyboardEvent& key);
// "Mouse4" / "Mouse5" for the side buttons, else nullptr.
const char* SideButtonName(int sdl_button);
// Player input that reaches the game, for recording.
void Observe(const SDL_Event& event);

}  // namespace nac::automation

#endif  // VOX_MENU_AUTOMATION_H_
