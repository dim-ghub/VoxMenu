#include "vr_keyboard.h"

#include <pthread.h>

#include <atomic>
#include <cstring>
#include <string_view>

#include "app.h"
#include "exe_symbols.h"

namespace nac {
namespace {

// Mirrors mocktail::vr::VirtualKeyboardHost (include/mocktail/vr/
// virtual_keyboard.h in Nightcap). The symbol's size is checked against it.
struct Host {
  void* context;
  bool (*input_requested)(void* context);
  void (*commit_text)(void* context, const char* utf8);
  void (*press_key)(void* context, std::uint32_t scancode, bool ctrl);
};

// mocktail::vr::(anonymous namespace)::g_host and g_host_mutex. Matched by
// suffix so either compiler's mangling of the anonymous namespace works.
constexpr const char kPrefix[] = "_ZN8mocktail2vr";
constexpr const char kHostSuffix[] = "6g_hostE";
constexpr const char kMutexSuffix[] = "12g_host_mutexE";

enum class State { kUnresolved, kFound, kMissing };
State g_state = State::kUnresolved;  // main thread only
Host* g_host = nullptr;
pthread_mutex_t* g_host_mutex = nullptr;

VrKeyboardObserver g_observer;  // set before the wrappers are installed
std::atomic<void*> g_context{nullptr};
std::atomic<void (*)(void*, const char*)> g_real_commit{nullptr};
std::atomic<void (*)(void*, std::uint32_t, bool)> g_real_press{nullptr};

bool EndsWith(std::string_view s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool FindSymbols() {
  void* found[2];
  const bool ok = FindExeObjects(
      [](std::string_view name, size_t size) {
        if (name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return -1;
        if (EndsWith(name, kHostSuffix) && size == sizeof(Host)) return 0;
        if (EndsWith(name, kMutexSuffix) && size == sizeof(pthread_mutex_t)) return 1;
        return -1;
      },
      found, 2);
  if (!ok) return false;
  g_host = static_cast<Host*>(found[0]);
  g_host_mutex = static_cast<pthread_mutex_t*>(found[1]);
  return true;
}

void CommitText(void* context, const char* utf8) {
  if (utf8 != nullptr && g_observer.text != nullptr) g_observer.text(utf8);
  if (auto real = g_real_commit.load()) real(context, utf8);
}

void PressKey(void* context, std::uint32_t scancode, bool ctrl) {
  if (g_observer.key != nullptr && g_observer.key(scancode, ctrl)) return;
  if (auto real = g_real_press.load()) real(context, scancode, ctrl);
}

}  // namespace

void HookVrKeyboard(const VrKeyboardObserver& observer) {
  if (g_state == State::kMissing) return;
  if (g_state == State::kUnresolved) {
    if (!FindSymbols()) {
      g_state = State::kMissing;
      Log("VR keyboard not found in Nightcap (stripped or non-VR build); "
          "chat typed in a headset won't be spoken");
      return;
    }
    g_state = State::kFound;
    g_observer = observer;
  }
  // Nightcap installs its host once VR starts, and could replace it; wrap
  // whatever is there now. The render thread copies it under this mutex.
  pthread_mutex_lock(g_host_mutex);
  bool wrapped = false;
  if (g_host->commit_text != nullptr && g_host->commit_text != CommitText) {
    g_real_commit.store(g_host->commit_text);
    g_host->commit_text = CommitText;
    wrapped = true;
  }
  if (g_host->press_key != nullptr && g_host->press_key != PressKey) {
    g_real_press.store(g_host->press_key);
    g_host->press_key = PressKey;
    wrapped = true;
  }
  if (wrapped) g_context.store(g_host->context);
  pthread_mutex_unlock(g_host_mutex);
  if (wrapped) Log("following the VR keyboard for chat");
}

bool VrPressKey(std::uint32_t scancode, bool ctrl) {
  auto real = g_real_press.load();
  if (real == nullptr) return false;
  real(g_context.load(), scancode, ctrl);
  return true;
}

}  // namespace nac
