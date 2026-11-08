#include "tts.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#include "app.h"

namespace nac {
namespace {

std::string HomeDir() {
  const char* h = std::getenv("HOME");
  return h != nullptr ? h : "";
}

std::string DataHome() {
  const char* d = std::getenv("XDG_DATA_HOME");
  if (d != nullptr && *d != '\0') return d;
  std::string home = HomeDir();
  return home.empty() ? "" : home + "/.local/share";
}

bool FileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsExecutable(const std::string& path) {
  return access(path.c_str(), X_OK) == 0;
}

bool CheckFileInPath(const char* filename) {
  const char* path_env = std::getenv("PATH");
  if (!path_env) return false;
  std::stringstream ss(path_env);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (dir.empty()) continue;
    std::string full = dir + "/" + filename;
    if (access(full.c_str(), X_OK) == 0) return true;
  }
  return false;
}

std::string ResolvePiperBin() {
  const std::string home = HomeDir();
  const std::string candidates[] = {
    home + "/.local/bin/piper-tts",
    home + "/.local/bin/piper",
    "/usr/local/bin/piper-tts",
    "/usr/local/bin/piper",
    "/usr/bin/piper-tts",
    "/usr/bin/piper",
  };
  for (const auto& c : candidates) {
    if (IsExecutable(c)) return c;
  }
  if (CheckFileInPath("piper-tts")) return "piper-tts";
  if (CheckFileInPath("piper")) return "piper";
  return "";
}

std::string ResolveESpeakBin() {
  if (IsExecutable("/usr/bin/espeak-ng")) return "/usr/bin/espeak-ng";
  if (IsExecutable("/usr/local/bin/espeak-ng")) return "/usr/local/bin/espeak-ng";
  if (IsExecutable("/usr/bin/espeak")) return "/usr/bin/espeak";
  if (CheckFileInPath("espeak-ng")) return "espeak-ng";
  if (CheckFileInPath("espeak")) return "espeak";
  return "";
}

bool EndsWith(const std::string& str, const std::string& suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string BaseNameNoExt(const std::string& path) {
  size_t slash = path.rfind('/');
  std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (EndsWith(file, ".onnx")) {
    file = file.substr(0, file.size() - 5);
  }
  return file;
}

void ScanModelsRecursive(const std::string& dir, int depth, std::vector<PiperModelInfo>* out) {
  if (depth <= 0) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;

  while (dirent* entry = readdir(d)) {
    if (entry->d_name[0] == '.') continue;
    std::string full_path = dir + "/" + entry->d_name;
    struct stat st {};
    if (stat(full_path.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      ScanModelsRecursive(full_path, depth - 1, out);
    } else if (S_ISREG(st.st_mode) && EndsWith(entry->d_name, ".onnx")) {
      PiperModelInfo info;
      info.path = full_path;
      info.name = BaseNameNoExt(full_path);
      std::string json_path = full_path + ".json";
      if (FileExists(json_path)) {
        info.config = json_path;
      }
      out->push_back(std::move(info));
    }
  }
  closedir(d);
}

std::vector<float> ConvertWavToFloat48k(void* wav_data, size_t wav_size) {
  if (!wav_data || wav_size == 0) return {};
  SDL_IOStream* io = SDL_IOFromMem(wav_data, wav_size);
  if (!io) return {};

  SDL_AudioSpec src_spec;
  Uint8* src_buf = nullptr;
  Uint32 src_len = 0;
  std::vector<float> result;

  if (SDL_LoadWAV_IO(io, true, &src_spec, &src_buf, &src_len)) {
    SDL_AudioSpec dst_spec{SDL_AUDIO_F32, 1, 48000};
    Uint8* dst_buf = nullptr;
    int dst_len = 0;
    if (SDL_ConvertAudioSamples(&src_spec, src_buf, src_len, &dst_spec, &dst_buf, &dst_len)) {
      const size_t count = dst_len / sizeof(float);
      const float* fbuf = reinterpret_cast<const float*>(dst_buf);
      result.assign(fbuf, fbuf + count);
      SDL_free(dst_buf);
    } else {
      Log("SDL_ConvertAudioSamples failed: %s", SDL_GetError());
    }
    SDL_free(src_buf);
  } else {
    Log("SDL_LoadWAV_IO failed: %s", SDL_GetError());
  }
  return result;
}

std::vector<float> RunTTSProcess(const std::vector<std::string>& arg_strings,
                                 const std::string& stdin_text) {
  std::vector<const char*> args;
  args.reserve(arg_strings.size() + 1);
  for (const auto& a : arg_strings) args.push_back(a.c_str());
  args.push_back(nullptr);

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_Process* proc = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);

  if (!proc) {
    Log("failed to launch %s: %s", args[0], SDL_GetError());
    return {};
  }

  SDL_IOStream* in = SDL_GetProcessInput(proc);
  if (in) {
    if (!stdin_text.empty()) {
      SDL_WriteIO(in, stdin_text.data(), stdin_text.size());
    }
    SDL_CloseIO(in);
  }

  size_t data_size = 0;
  int exitcode = 0;
  void* data = SDL_ReadProcess(proc, &data_size, &exitcode);
  SDL_DestroyProcess(proc);

  if (!data || data_size == 0 || exitcode != 0) {
    if (data) SDL_free(data);
    Log("%s process exited with code %d (output %zu bytes)", args[0], exitcode, data_size);
    return {};
  }

  std::vector<float> samples = ConvertWavToFloat48k(data, data_size);
  SDL_free(data);
  return samples;
}

}  // namespace

std::vector<PiperModelInfo> DiscoverPiperModels(const std::string& custom_dir) {
  static std::mutex mutex;
  static std::string last_dir;
  static std::vector<PiperModelInfo> cached_models;
  static uint64_t last_scan_ms = 0;

  const uint64_t now = SDL_GetTicks();
  std::lock_guard<std::mutex> lock(mutex);
  if (!cached_models.empty() && custom_dir == last_dir && (now - last_scan_ms < 5000)) {
    return cached_models;
  }
  last_dir = custom_dir;
  last_scan_ms = now;

  std::vector<PiperModelInfo> models;
  std::vector<std::string> search_roots;

  if (!custom_dir.empty()) search_roots.push_back(custom_dir);

  const std::string data_home = DataHome();
  if (!data_home.empty()) {
    search_roots.push_back(data_home + "/piper-voices");
    search_roots.push_back(data_home + "/vox-menu/voices");
    search_roots.push_back(data_home + "/vox-tts/voices");
    search_roots.push_back(data_home + "/nightcap-ac/voices");
  }

  search_roots.push_back("/usr/share/piper-voices");
  search_roots.push_back("/usr/local/share/piper-voices");

  const std::string home = HomeDir();
  if (!home.empty()) {
    search_roots.push_back(home + "/.config/piper/voices");
  }

  for (const auto& root : search_roots) {
    struct stat st {};
    if (stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      ScanModelsRecursive(root, 8, &models);
    }
  }

  // Deduplicate by path
  std::vector<PiperModelInfo> unique;
  for (auto& m : models) {
    if (std::none_of(unique.begin(), unique.end(),
                     [&](const PiperModelInfo& u) { return u.path == m.path; })) {
      unique.push_back(std::move(m));
    }
  }
  std::sort(unique.begin(), unique.end(),
            [](const PiperModelInfo& a, const PiperModelInfo& b) { return a.name < b.name; });
  cached_models = std::move(unique);
  return cached_models;
}

const std::vector<ESpeakVoiceInfo>& GetESpeakVoices() {
  static const std::vector<ESpeakVoiceInfo> kVoices = {
      {"English (America)", "en-us"},
      {"English (Great Britain)", "en-gb"},
      {"English (Scotland)", "en-gb-scotland"},
      {"English (New York)", "en-us-nyc"},
      {"Spanish (Spain)", "es"},
      {"Spanish (Latin America)", "es-la"},
      {"French", "fr"},
      {"German", "de"},
      {"Italian", "it"},
      {"Portuguese (Brazil)", "pt-br"},
      {"Japanese", "ja"},
      {"Russian", "ru"},
      {"Chinese (Mandarin)", "zh"},
  };
  return kVoices;
}

bool IsPiperInstalled() {
  static bool checked = false;
  static bool installed = false;
  static uint64_t last_check = 0;
  const uint64_t now = SDL_GetTicks();
  if (!checked || now - last_check > 5000) {
    checked = true;
    last_check = now;
    installed = !ResolvePiperBin().empty();
  }
  return installed;
}

bool IsESpeakInstalled() {
  static bool checked = false;
  static bool installed = false;
  static uint64_t last_check = 0;
  const uint64_t now = SDL_GetTicks();
  if (!checked || now - last_check > 5000) {
    checked = true;
    last_check = now;
    installed = !ResolveESpeakBin().empty();
  }
  return installed;
}

std::vector<float> SynthesizeESpeak(const std::string& text, const std::string& voice,
                                    double pitch, double speed_wpm) {
  if (text.empty()) return {};

  const std::string bin = ResolveESpeakBin();
  if (bin.empty()) return {};

  char pitch_buf[32];
  std::snprintf(pitch_buf, sizeof(pitch_buf), "-p%d",
                std::clamp(static_cast<int>(pitch), 0, 99));

  char speed_buf[32];
  std::snprintf(speed_buf, sizeof(speed_buf), "-s%d",
                std::clamp(static_cast<int>(speed_wpm), 80, 450));

  std::string voice_arg = "-v" + (voice.empty() ? std::string("en-us") : voice);

  std::vector<std::string> args = {bin, "--stdin", "--stdout", voice_arg, pitch_buf, speed_buf};
  return RunTTSProcess(args, text);
}

std::vector<float> SynthesizePiper(const std::string& text, const std::string& model_path,
                                   double speed, const std::string& custom_dir) {
  if (text.empty()) return {};

  std::string model = model_path;
  if (model.empty()) {
    auto discovered = DiscoverPiperModels(custom_dir);
    if (!discovered.empty()) model = discovered[0].path;
  }
  if (model.empty() || !FileExists(model)) {
    Log("piper: model not found (%s)", model.c_str());
    return {};
  }

  const std::string bin = ResolvePiperBin();
  if (bin.empty()) {
    Log("piper: binary not found");
    return {};
  }

  char temp_path[] = "/tmp/vox_piper_XXXXXX.wav";
  int fd = mkstemps(temp_path, 4);
  if (fd < 0) {
    Log("piper: failed to create temporary file");
    return {};
  }
  close(fd);

  char scale_buf[32];
  const double length_scale = speed > 0.05 ? 1.0 / speed : 1.0;
  std::snprintf(scale_buf, sizeof(scale_buf), "%.2f", length_scale);

  std::vector<std::string> arg_strings = {
      bin,
      "-m", model,
      "-f", temp_path,
      "--length-scale", scale_buf
  };
  std::string config_path = model + ".json";
  if (FileExists(config_path)) {
    arg_strings.push_back("-c");
    arg_strings.push_back(config_path);
  }

  std::vector<const char*> args;
  args.reserve(arg_strings.size() + 1);
  for (const auto& a : arg_strings) args.push_back(a.c_str());
  args.push_back(nullptr);

  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
  SDL_Process* proc = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);

  if (!proc) {
    Log("failed to launch %s: %s", bin.c_str(), SDL_GetError());
    unlink(temp_path);
    return {};
  }

  SDL_IOStream* in = SDL_GetProcessInput(proc);
  if (in) {
    SDL_WriteIO(in, text.data(), text.size());
    SDL_CloseIO(in);
  }

  int exitcode = 0;
  SDL_WaitProcess(proc, true, &exitcode);
  SDL_DestroyProcess(proc);

  std::vector<float> samples;
  if (exitcode == 0 && FileExists(temp_path)) {
    SDL_AudioSpec src_spec;
    Uint8* src_buf = nullptr;
    Uint32 src_len = 0;
    if (SDL_LoadWAV(temp_path, &src_spec, &src_buf, &src_len)) {
      SDL_AudioSpec dst_spec{SDL_AUDIO_F32, 1, 48000};
      Uint8* dst_buf = nullptr;
      int dst_len = 0;
      if (SDL_ConvertAudioSamples(&src_spec, src_buf, src_len, &dst_spec, &dst_buf, &dst_len)) {
        const size_t count = dst_len / sizeof(float);
        const float* fbuf = reinterpret_cast<const float*>(dst_buf);
        samples.assign(fbuf, fbuf + count);
        SDL_free(dst_buf);
      } else {
        Log("SDL_ConvertAudioSamples failed on piper output: %s", SDL_GetError());
      }
      SDL_free(src_buf);
    } else {
      Log("SDL_LoadWAV failed on piper output: %s", SDL_GetError());
    }
  } else {
    Log("%s exited with code %d", bin.c_str(), exitcode);
  }
  unlink(temp_path);
  return samples;
}

std::vector<float> SynthesizeSpeech(const std::string& text, const Config& cfg,
                                    const SamplePack* pack) {
  if (text.empty()) return {};

  if (cfg.engine == "piper") {
    std::vector<float> audio =
        SynthesizePiper(text, cfg.piper_model, cfg.piper_speed, cfg.piper_dir);
    if (!audio.empty()) return audio;
    Log("piper synthesis returned no audio; falling back to eSpeak");
  }

  if (cfg.engine == "piper" || cfg.engine == "espeak") {
    std::vector<float> audio =
        SynthesizeESpeak(text, cfg.espeak_voice, cfg.espeak_pitch, cfg.espeak_speed);
    if (!audio.empty()) return audio;
    Log("eSpeak synthesis returned no audio; falling back to Animalese");
  }

  // Animalese fallback or primary
  VoiceSettings vs;
  vs.voice = cfg.voice;
  vs.style = cfg.style;
  vs.lang = cfg.animalese_lang;
  vs.pitch = cfg.pitch;
  vs.speed = cfg.speed;
  return Synthesize(text, vs, std::random_device{}(), pack);
}

}  // namespace nac
