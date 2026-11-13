// Standard MIDI files (.mid), read into a flat, time-sorted list of note
// and sustain-pedal events for the piano player (piano.h).

#ifndef VOX_MENU_MIDI_FILE_H_
#define VOX_MENU_MIDI_FILE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace nac::midi {

struct Event {
  int64_t ns = 0;     // from the start of the song
  int note = 0;       // 0..127, or kSustain
  bool down = false;  // note on / pedal pressed
  int velocity = 0;   // note on velocity, or the pedal's value
  int track = 0;
};
constexpr int kSustain = -1;

struct Track {
  std::string name;
  int notes = 0;
  bool drums = false;  // named like a drum kit, or played on channel 10
};

struct Song {
  std::vector<Event> events;  // sorted by time
  std::vector<Track> tracks;  // only tracks with notes in them, in file order
  int64_t length_ns = 0;
};

// False (with a reason) when the data isn't a MIDI file we can play.
bool Parse(const std::string& data, Song* song, std::string* error);
bool Load(const std::string& path, Song* song, std::string* error);

// "C4" for 60.
std::string NoteName(int note);

}  // namespace nac::midi

#endif  // VOX_MENU_MIDI_FILE_H_
