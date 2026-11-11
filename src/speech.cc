#include "speech.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "app.h"
#include "media.h"
#include "synth.h"
#include "tts.h"

namespace nac {
namespace {

using Clock = std::chrono::steady_clock;

// Rendered speech waiting to be read by the microphone, at kSampleRate.
struct MicQueue {
  std::mutex mutex;
  std::deque<float> samples;
  double position = 0.0;  // fractional read position for resampling
};

struct Engine {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::pair<std::string, bool>> queue;
  std::string last_message;
  std::thread worker;
  bool started = false;

  MicQueue mic;
  std::atomic<int64_t> last_mic_read_ms{-1000000};
  std::atomic<int64_t> local_until_ms{0};

  SDL_AudioStream* local = nullptr;  // our speaker stream, worker thread only
  SamplePack pack;
  std::string pack_path;
};

Engine& E() {
  static Engine* engine = new Engine();  // never destroyed: threads outlive exit
  return *engine;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

constexpr size_t kMaxQueuedMessages = 3;
constexpr size_t kMaxMicSeconds = 30;

SDL_AudioStream* OpenLocal() {
  Engine& e = E();
  if (e.local != nullptr) return e.local;
  if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0 && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    Log("speaker output unavailable: %s", SDL_GetError());
    return nullptr;
  }
  SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kSampleRate};
  // Our own wrapper only tracks microphones, but go straight to SDL anyway.
  auto open = Real<SDL_AudioStream* (*)(SDL_AudioDeviceID, const SDL_AudioSpec*,
                                        SDL_AudioStreamCallback, void*)>(
      "SDL_OpenAudioDeviceStream");
  e.local = open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (e.local == nullptr) {
    Log("speaker output unavailable: %s", SDL_GetError());
    return nullptr;
  }
  SDL_ResumeAudioStreamDevice(e.local);
  return e.local;
}

void Play(const std::string& text) {
  Engine& e = E();
  const Config cfg = GetConfig();
  const SamplePack* pack = nullptr;
  if (!cfg.sample_pack.empty() && cfg.style == "classic") {
    if (cfg.sample_pack != e.pack_path) {
      e.pack_path = cfg.sample_pack;
      std::string error;
      if (!e.pack.Load(cfg.sample_pack, &error)) Log("sample pack: %s", error.c_str());
    }
    if (!e.pack.empty()) pack = &e.pack;
  }
  const std::vector<float> audio = SynthesizeSpeech(text, cfg, pack);
  if (audio.empty()) return;
  const int64_t duration_ms = static_cast<int64_t>(audio.size()) * 1000 / kSampleRate;

  // Voice chat: only while Nightcap is actually reading the mic, otherwise
  // the speech would pile up and play late when you join.
  if (cfg.uses_vc() && MicInUse()) {
    std::lock_guard<std::mutex> lock(e.mic.mutex);
    if (e.mic.samples.size() < kMaxMicSeconds * kSampleRate) {
      e.mic.samples.insert(e.mic.samples.end(), audio.begin(), audio.end());
    }
  }
  if (cfg.uses_local()) {
    if (SDL_AudioStream* local = OpenLocal()) {
      SDL_SetAudioStreamGain(local, static_cast<float>(cfg.local_volume));
      SDL_PutAudioStreamData(local, audio.data(), static_cast<int>(audio.size() * sizeof(float)));
    }
  }
  const int64_t start = std::max(NowMs(), e.local_until_ms.load());
  e.local_until_ms = start + duration_ms;
}

void WorkerLoop() {
  Engine& e = E();
  while (true) {
    std::pair<std::string, bool> item;
    {
      std::unique_lock<std::mutex> lock(e.mutex);
      e.wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return !e.queue.empty(); });
      if (e.queue.empty()) {
        lock.unlock();
        SaveConfigIfDirty();
        continue;
      }
      item = e.queue.front();
      e.queue.pop_front();
      e.last_message = item.first;
    }
    if (!item.second && !GetConfig().enabled) continue;
    Play(item.first);
    // Messages play one after another, like text boxes.
    while (IsSpeaking()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void EnsureWorker() {
  Engine& e = E();
  std::lock_guard<std::mutex> lock(e.mutex);
  if (!e.started) {
    e.started = true;
    e.worker = std::thread(WorkerLoop);
    e.worker.detach();
  }
}

// Adds speech (when `speech_on`) and the shared app audio in `media` to the
// mic buffer; "Keep My Microphone On" off leaves only those.
template <typename T>
void MixSamples(T* out, int frames, int channels, Engine& e, const Config& cfg, bool speech_on,
                const float* media, double ratio, float (*to_float)(T), T (*from_float)(float)) {
  std::lock_guard<std::mutex> lock(e.mic.mutex);
  const float vc = static_cast<float>(cfg.vc_volume);
  const float media_gain = static_cast<float>(cfg.media_volume);
  const bool keep_mic = cfg.mix_real_mic;
  for (int f = 0; f < frames; ++f) {
    float speech = media != nullptr ? media[f] * media_gain : 0.0f;
    auto& q = e.mic.samples;
    if (speech_on && !q.empty()) {
      const size_t i = static_cast<size_t>(e.mic.position);
      const double frac = e.mic.position - i;
      const float a = i < q.size() ? q[i] : 0.0f;
      const float b = i + 1 < q.size() ? q[i + 1] : 0.0f;
      speech += static_cast<float>(a + (b - a) * frac) * vc;
      e.mic.position += ratio;
      const size_t consumed = static_cast<size_t>(e.mic.position);
      const size_t drop = std::min(consumed, q.size());
      q.erase(q.begin(), q.begin() + drop);
      e.mic.position -= drop;
      if (q.empty()) e.mic.position = 0.0;
    }
    for (int c = 0; c < channels; ++c) {
      T& sample = out[f * channels + c];
      const float mic = keep_mic ? to_float(sample) : 0.0f;
      sample = from_float(std::clamp(mic + speech, -1.0f, 1.0f));
    }
  }
}

}  // namespace

void Say(const std::string& text, bool force) {
  EnsureWorker();
  Engine& e = E();
  {
    std::lock_guard<std::mutex> lock(e.mutex);
    if (e.queue.size() >= kMaxQueuedMessages) {
      Log("speech queue full; dropping a message");
      return;
    }
    e.queue.emplace_back(text, force);
  }
  e.wake.notify_one();
}

void StopSpeaking() {
  Engine& e = E();
  {
    std::lock_guard<std::mutex> lock(e.mutex);
    e.queue.clear();
  }
  {
    std::lock_guard<std::mutex> lock(e.mic.mutex);
    e.mic.samples.clear();
    e.mic.position = 0;
  }
  if (e.local != nullptr) SDL_ClearAudioStream(e.local);
  e.local_until_ms = 0;
}

void MixIntoMic(void* data, int bytes, const SDL_AudioSpec& spec) {
  Engine& e = E();
  e.last_mic_read_ms = NowMs();
  EnsureWorker();
  const Config cfg = GetConfig();
  const bool speech_on = cfg.enabled && cfg.uses_vc();
  if ((!speech_on && !cfg.media_on) || data == nullptr || bytes <= 0 || spec.channels <= 0 || spec.freq <= 0) {
    return;
  }
  const double ratio = static_cast<double>(kSampleRate) / spec.freq;
  const int channels = spec.channels;
  const int bytes_per_sample = spec.format == SDL_AUDIO_S16 ? 2 : 4;
  // The shared app audio for this buffer, resampled to the mic's rate.
  static thread_local std::vector<float> media;
  const float* shared = nullptr;
  if (cfg.media_on) {
    media.assign(bytes / (bytes_per_sample * channels), 0.0f);
    media::MixInto(media.data(), static_cast<int>(media.size()), spec.freq);
    shared = media.data();
  }
  switch (spec.format) {
    case SDL_AUDIO_F32: {
      const int frames = bytes / (4 * channels);
      MixSamples<float>(static_cast<float*>(data), frames, channels, e, cfg, speech_on, shared, ratio,
                        [](float v) { return v; }, [](float v) { return v; });
      break;
    }
    case SDL_AUDIO_S16: {
      const int frames = bytes / (2 * channels);
      MixSamples<int16_t>(static_cast<int16_t*>(data), frames, channels, e, cfg, speech_on, shared, ratio,
                          [](int16_t v) { return v / 32768.0f; },
                          [](float v) { return static_cast<int16_t>(v * 32767.0f); });
      break;
    }
    case SDL_AUDIO_S32: {
      const int frames = bytes / (4 * channels);
      MixSamples<int32_t>(static_cast<int32_t*>(data), frames, channels, e, cfg, speech_on, shared, ratio,
                          [](int32_t v) { return v / 2147483648.0f; },
                          [](float v) { return static_cast<int32_t>(v * 2147483520.0f); });
      break;
    }
    default:
      break;  // formats Nightcap doesn't use for the mic
  }
}

bool IsSpeaking() {
  Engine& e = E();
  {
    std::lock_guard<std::mutex> lock(e.mic.mutex);
    if (!e.mic.samples.empty() && MicInUse()) return true;
  }
  return NowMs() < e.local_until_ms.load();
}

bool MicInUse() { return NowMs() - E().last_mic_read_ms.load() < 1000; }

std::string LastMessage() {
  Engine& e = E();
  std::lock_guard<std::mutex> lock(e.mutex);
  return e.last_message;
}

}  // namespace nac
