// Animalese speech synthesis.
//
// Authentic Animal Crossing voices built from authentic character voice sprite banks
// (Female 1–4, Male 1–4) for English and Korean, playing in step with the text box,
// one blip per character, with subtle pitch jitter and question inflections.

#ifndef VOX_MENU_SYNTH_H_
#define VOX_MENU_SYNTH_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nac {

constexpr int kSampleRate = 48000;

struct VoiceInfo {
  const char* key;
  const char* label;
  double f0;             // base pitch in Hz
  double formant_scale;  // >1 shifts the vocal tract up (the "chipmunk" part)
  double cps;            // characters per second of the text box
  double jitter;         // random pitch spread per blip, semitones
  double breath;         // aspiration noise mixed into vowels, 0..1
  double tilt;           // source spectral slope; higher is darker
};

struct StyleInfo {
  const char* key;
  const char* label;
};

const std::vector<VoiceInfo>& Voices();
const std::vector<StyleInfo>& Styles();
const VoiceInfo& FindVoice(const std::string& key);

struct VoiceSettings {
  std::string voice = "normal";
  std::string style = "classic";
  std::string lang = "english";
  double pitch = 0.0;  // semitones on top of the voice
  double speed = 1.0;  // multiplier on the text-box speed
};

// Your own per-letter recordings, played back Animalese style: a folder with
// a.wav ... z.wav, or one WAV holding the 26 letters back to back in equal
// slices (the Animalese.js sound-sprite layout).
class SamplePack {
 public:
  bool Load(const std::string& path, std::string* error);
  bool empty() const { return letters_.empty(); }
  std::vector<double> Blip(char letter, double dur, double rate,
                           double gain) const;

 private:
  std::map<char, std::vector<double>> letters_;
};

// Text reduced to ASCII letters, spaces and pause punctuation, with numbers
// and symbols spelled out.
std::string Normalize(const std::string& utf8);

// Renders chat text to mono float audio at kSampleRate. Classic style uses
// the sample pack when one is given.
std::vector<float> Synthesize(const std::string& utf8,
                              const VoiceSettings& settings, uint32_t seed,
                              const SamplePack* pack = nullptr);

}  // namespace nac

#endif  // VOX_MENU_SYNTH_H_
