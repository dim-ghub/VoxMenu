#include "app.h"

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <mutex>

namespace nac {
namespace {

std::mutex g_mutex;
Config* g_config = nullptr;
bool g_dirty = false;
std::chrono::steady_clock::time_point g_changed;

void SaveAtExit() {
  Config copy;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_dirty || g_config == nullptr) return;
    g_dirty = false;
    copy = *g_config;
  }
  copy.Save();
}

Config& Locked() {
  if (g_config == nullptr) {
    g_config = new Config(Config::Load());
    // Changes made just before Nightcap quits still count.
    std::atexit(SaveAtExit);
  }
  return *g_config;
}

}  // namespace

Config GetConfig() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Locked();
}

void SetConfig(const Config& config) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Locked() = config;
  g_dirty = true;
  g_changed = std::chrono::steady_clock::now();
}

void SaveConfigIfDirty() {
  Config copy;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Wait for a pause so dragging a slider doesn't write on every step.
    if (!g_dirty || std::chrono::steady_clock::now() - g_changed < std::chrono::milliseconds(500)) {
      return;
    }
    g_dirty = false;
    copy = Locked();
  }
  if (!copy.Save()) Log("could not save %s", Config::Path().c_str());
}

const char* Env(const char* name) {
  for (const char* prefix : {"VOX_MENU_", "VOX_TTS_", "NIGHTCAP_AC_"}) {
    if (const char* value = std::getenv((std::string(prefix) + name).c_str())) return value;
  }
  return nullptr;
}

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  std::fprintf(stderr, "  [vox-menu] %s\n", buf);
}

}  // namespace nac
