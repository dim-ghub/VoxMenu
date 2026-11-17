// Shares an app's sound into voice chat by recording its streams with
// `parec --monitor-stream` and mixing them into the mic.

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
