// MIDI keyboard input through the ALSA sequencer (libasound loaded lazily).

#ifndef VOX_MENU_MIDI_INPUT_H_
#define VOX_MENU_MIDI_INPUT_H_

#include <string>
#include <vector>

namespace nac::midi_input {

struct Message {
  int note = 0;  // 0..127, or midi::kSustain for the pedal
  bool down = false;
  int velocity = 0;  // or the pedal's value
};

// Start and stop listening (any thread).
void Start();
void Stop();
bool Running();
// The keyboards heard from, by name.
std::vector<std::string> Devices();
// Why it isn't running, when it should be.
std::string Error();

// The next message played, oldest first.
bool Poll(Message* message);

}  // namespace nac::midi_input

#endif  // VOX_MENU_MIDI_INPUT_H_
