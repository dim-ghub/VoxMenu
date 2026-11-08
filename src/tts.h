// Local text-to-speech synthesis engine dispatcher for Vox Menu.
// Supports:
//   - Piper TTS: Fast local neural AI voices (ONNX)
//   - eSpeak-NG: Lightweight, classic multi-language TTS
//   - Animalese: Animal Crossing villager voices (Classic, NH, Bebebese)

#ifndef VOX_MENU_TTS_H_
#define VOX_MENU_TTS_H_

#include <string>
#include <vector>

#include "config.h"
#include "synth.h"

namespace nac {

struct PiperModelInfo {
  std::string name;      // Friendly display name (e.g. "en_US-lessac-medium")
  std::string path;      // Path to .onnx file
  std::string config;    // Path to .onnx.json if found
  std::string language;  // Language code, e.g. "en_US"
};

struct ESpeakVoiceInfo {
  std::string name;  // Friendly label, e.g. "English (America)"
  std::string code;  // Code passed to -v, e.g. "en-us"
};

// Returns all discovered .onnx models in standard folders and custom_dir.
std::vector<PiperModelInfo> DiscoverPiperModels(const std::string& custom_dir = "");

// Returns a curated list of popular eSpeak-NG voices.
const std::vector<ESpeakVoiceInfo>& GetESpeakVoices();

bool IsPiperInstalled();
bool IsESpeakInstalled();

// Synthesize text into mono float32 audio at 48000 Hz.
std::vector<float> SynthesizeSpeech(const std::string& text, const Config& cfg,
                                    const SamplePack* pack = nullptr);

// Individual backend synthesis functions
std::vector<float> SynthesizeESpeak(const std::string& text, const std::string& voice,
                                    double pitch, double speed_wpm);

std::vector<float> SynthesizePiper(const std::string& text, const std::string& model_path,
                                   double speed, const std::string& custom_dir = "");

}  // namespace nac

#endif  // VOX_MENU_TTS_H_
