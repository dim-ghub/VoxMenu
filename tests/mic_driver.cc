// Stands in for Nightcap's microphone and chat box when testing speech
// mixing. Opens the default recording device the way Nightcap does (a stream
// with a callback that pulls fixed-size frames with SDL_GetAudioStreamData),
// records what it reads to a file, and at one second types a chat message
// into a focused text box and presses Enter.
//
//   mic_driver OUT.raw SECONDS MESSAGE [vr]
//
// With "vr" the message is typed on a stand-in for Nightcap's in-headset
// keyboard instead: its host callbacks live where Nightcap keeps them
// (mocktail::vr::g_host) and are called from another thread, never as SDL
// events.
//
// OUT.raw is mono signed 16-bit at 48 kHz, as Nightcap reads it.

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Same layout and names as Nightcap's src/vr/virtual_keyboard.cc.
namespace mocktail::vr {
struct VirtualKeyboardHost {
  void* context = nullptr;
  bool (*input_requested)(void* context) = nullptr;
  void (*commit_text)(void* context, const char* utf8) = nullptr;
  void (*press_key)(void* context, std::uint32_t scancode, bool ctrl) = nullptr;
};
namespace {
__attribute__((used)) std::mutex g_host_mutex;
__attribute__((used)) VirtualKeyboardHost g_host;
}  // namespace
}  // namespace mocktail::vr

namespace {

std::mutex g_mutex;
std::vector<int16_t> g_captured;

void SDLCALL OnAudio(void*, SDL_AudioStream* stream, int, int) {
  int16_t frame[480];
  while (SDL_GetAudioStreamAvailable(stream) >= static_cast<int>(sizeof(frame))) {
    const int got = SDL_GetAudioStreamData(stream, frame, sizeof(frame));
    if (got != sizeof(frame)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_captured.insert(g_captured.end(), frame, frame + 480);
  }
}

// Roblox closes its text box after Enter or Escape (main thread).
std::atomic<bool> g_vr_close{false};

void VrCommitText(void*, const char* utf8) { std::fprintf(stderr, "vr host got text: %s\n", utf8); }

void VrPressKey(void*, std::uint32_t scancode, bool ctrl) {
  std::fprintf(stderr, "vr host got key: %u%s\n", scancode, ctrl ? " ctrl" : "");
  if (scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_ESCAPE) g_vr_close = true;
}

// What Nightcap's VR render thread does with a frame's keyboard output.
void VrType(std::string message) {
  mocktail::vr::VirtualKeyboardHost host;
  {
    std::lock_guard<std::mutex> lock(mocktail::vr::g_host_mutex);
    host = mocktail::vr::g_host;
  }
  host.commit_text(host.context, message.c_str());
  host.press_key(host.context, SDL_SCANCODE_RETURN, false);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  const double seconds = std::atof(argv[2]);
  const bool vr = argc > 4 && std::strcmp(argv[4], "vr") == 0;
  if (vr) {
    std::lock_guard<std::mutex> lock(mocktail::vr::g_host_mutex);
    mocktail::vr::g_host.commit_text = VrCommitText;
    mocktail::vr::g_host.press_key = VrPressKey;
  }
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 2;
  }
  SDL_Window* window = SDL_CreateWindow("mic test", 320, 240, 0);
  SDL_AudioSpec spec{SDL_AUDIO_S16, 1, 48000};
  SDL_AudioStream* mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, OnAudio, nullptr);
  if (mic == nullptr) {
    std::fprintf(stderr, "open mic: %s\n", SDL_GetError());
    return 2;
  }
  SDL_ResumeAudioStreamDevice(mic);

  const Uint64 start = SDL_GetTicks();
  bool typed = false;
  while (SDL_GetTicks() - start < seconds * 1000) {
    if (!typed && SDL_GetTicks() - start > 1000) {
      typed = true;
      // Roblox focuses its chat bar, the text arrives, Enter sends it.
      SDL_PropertiesID props = SDL_CreateProperties();
      SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, SDL_TEXTINPUT_TYPE_TEXT);
      SDL_StartTextInputWithProperties(window, props);
      SDL_DestroyProperties(props);
      if (vr) {
        std::thread(VrType, std::string(argv[3])).join();
        continue;
      }
      SDL_Event e;
      SDL_zero(e);
      e.type = SDL_EVENT_TEXT_INPUT;
      e.text.windowID = SDL_GetWindowID(window);
      e.text.text = SDL_strdup(argv[3]);
      SDL_PushEvent(&e);
      SDL_zero(e);
      e.type = SDL_EVENT_KEY_DOWN;
      e.key.windowID = SDL_GetWindowID(window);
      e.key.key = SDLK_RETURN;
      e.key.down = true;
      SDL_PushEvent(&e);
    }
    if (g_vr_close.exchange(false)) SDL_StopTextInput(window);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_KEY_DOWN) {
        std::fprintf(stderr, "driver received key: %d\n", static_cast<int>(e.key.key));
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_ESCAPE) {
          SDL_StopTextInput(window);
        }
      }
    }
    SDL_Delay(5);
  }
  SDL_DestroyAudioStream(mic);
  std::lock_guard<std::mutex> lock(g_mutex);
  FILE* f = std::fopen(argv[1], "wb");
  std::fwrite(g_captured.data(), sizeof(int16_t), g_captured.size(), f);
  std::fclose(f);
  std::printf("captured %.2f s\n", g_captured.size() / 48000.0);
  SDL_Quit();
  return 0;
}
