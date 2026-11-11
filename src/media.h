// Sharing another app's sound (music, a video) into voice chat, without
// virtual audio cables: each of the app's playback streams is recorded
// directly with `parec --monitor-stream` (PulseAudio, or PipeWire's
// PulseAudio layer), which links to that one stream and creates no devices.
// The audio is mixed into what Nightcap reads from the microphone, like the
// TTS voice. Nightcap's own streams are never captured, so voice chat can't
// echo back into itself.

#ifndef VOX_MENU_MEDIA_H_
#define VOX_MENU_MEDIA_H_

#include <string>
#include <vector>

namespace nac::media {

// "All other apps" as a source.
inline constexpr const char* kAllApps = "*";

struct App {
  std::string name;   // application.name, the key the config stores
  std::string title;  // what it's playing (media.name), when known
};

// Apps with sound streams right now, Nightcap excluded. Refreshed in the
// background while it keeps being asked for (call it while showing a list).
std::vector<App> Apps();

// Adds the shared audio, as mono samples at `rate`, to out[0..frames).
// Called from the microphone path; capturing runs only while it is.
void MixInto(float* out, int frames, int rate);

// What sharing is doing, for the menu ("" when it is off).
std::string Status();

// Exposed for tests: `pactl list sink-inputs` output, picked apart.
struct Stream {
  int index = -1;
  std::string app, title, binary;
  long pid = 0;
};
std::vector<Stream> ParseSinkInputs(const std::string& text);

}  // namespace nac::media

#endif  // VOX_MENU_MEDIA_H_
