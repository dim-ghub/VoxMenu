// MIDI piano player (ported from MIDI++). Keys are injected on the main
// thread in Tick(). Control socket: ~/.vox-menu-piano.sock, MIDI++ protocol.

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
  std::string added;  // a song just moved in from Downloads
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

// The control socket for StreamController, and the Downloads watcher
// (new .mid files are moved into the songs folder). Started once.
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
