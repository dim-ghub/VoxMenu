// The MIDI piano player (ported from MIDI++): plays .mid files, or a MIDI
// keyboard, on Roblox's virtual pianos by pressing the keys they use
// ("1234567890qwerty...", Shift for black keys, Ctrl past 61 keys). Like
// the autoclicker, every key goes through Nightcap's own event queue
// (inject.h), so it only ever reaches the Roblox window.
//
// Commands come from the render thread (the menu), the control socket and
// hotkeys; the keys are pressed on the main thread in Tick().
//
// The control socket, ~/.vox-menu-piano.sock, takes one line per
// connection and answers with one line: PLAY_PAUSE, REWIND, SKIP, RESTART,
// SPEED_UP, SPEED_DOWN, STOP, GET_TIME ({"current","total","speed",
// "paused","song"}), LIST_SONGS (a JSON array of names) and
// LOAD_SONG <index into that list>.

#ifndef VOX_MENU_PIANO_H_
#define VOX_MENU_PIANO_H_

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace nac::piano {

// The songs folder: the config's, else $XDG_MUSIC_DIR (or ~/Music).
std::string SongsFolder();
// .mid / .midi files under it (sub-folders too), by name; rescanned on
// request.
std::vector<std::string> Songs();
void Rescan();

struct TrackStatus {
  std::string name;
  int notes = 0;
  bool drums = false;
  bool muted = false;
  bool solo = false;
};

struct Status {
  bool loaded = false;
  bool playing = false;
  std::string song;  // name, without the extension
  int index = -1;    // in Songs()
  double position = 0, length = 0;  // seconds
  double speed = 1.0;
  std::vector<TrackStatus> tracks;
  std::string error;  // the last load that failed
  // MIDI keyboard
  bool live = false;  // listening
  std::vector<std::string> devices;
  std::string live_error;
};
Status GetStatus();

// From any thread.
bool Load(int index);  // in Songs(); starts playing it
void TogglePlay();
void Stop();          // back to the start, paused
void SeekBy(double seconds);
void SeekTo(double seconds);
void Restart();
void SpeedUp();
void SpeedDown();
void SetSpeed(double speed);
void SetTrack(int track, bool muted, bool solo);

// The control socket for StreamController. Started once, with the menu.
void StartServer();
// One socket command ("GET_TIME", "LOAD_SONG 3"...) and its answer line.
std::string Command(const std::string& line);

// --- Main thread ----------------------------------------------------------------

// Presses and releases whatever is due.
void Tick();
// The piano's hotkeys (play / pause, back and forward 10 s): true when the
// event was one and must not reach the game.
bool HandleHotkey(const SDL_KeyboardEvent& key);
bool HandleHotkeyButton(int sdl_button, bool down);

// Which keys a note presses, for the 61-key layout or all 88 keys. False
// when the note has none.
struct Stroke {
  SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
  bool shift = false, ctrl = false;
};
bool NoteStroke(int note, bool all88, Stroke* stroke);

}  // namespace nac::piano

#endif  // VOX_MENU_PIANO_H_
