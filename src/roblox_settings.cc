#include "roblox_settings.h"

#include <glob.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace nac {
namespace {

std::mutex g_mutex;
std::string g_path;
timespec g_mtime{};
float g_value = 1.0f;
std::chrono::steady_clock::time_point g_checked{};

// The newest-numbered settings file in Nightcap's Roblox data.
std::string FindSettingsFile() {
  std::vector<std::string> roots;
  if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) roots.push_back(data);
  if (const char* home = std::getenv("HOME")) {
    roots.push_back(std::string(home) + "/.local/share");
    roots.push_back(std::string(home) + "/.var/app/io.github.CoderDayton.nightcap/data");
  }
  for (const std::string& root : roots) {
    glob_t g{};
    std::string best;
    if (glob((root + "/mocktail/android/data/files/appData/GlobalBasicSettings_*.xml").c_str(), 0, nullptr, &g) ==
        0) {
      for (size_t i = 0; i < g.gl_pathc; ++i) {
        const std::string path = g.gl_pathv[i];
        // GlobalBasicSettings_13 beats _9: compare the numbers.
        auto number = [](const std::string& p) { return std::atoi(p.c_str() + p.rfind('_') + 1); };
        if (best.empty() || number(path) > number(best)) best = path;
      }
    }
    globfree(&g);
    if (!best.empty()) return best;
  }
  return "";
}

}  // namespace

float ParsePreferredTransparency(const std::string& xml, float fallback) {
  const std::string key = "name=\"PreferredTransparency\">";
  const size_t at = xml.find(key);
  if (at == std::string::npos) return fallback;
  char* end = nullptr;
  const float value = std::strtof(xml.c_str() + at + key.size(), &end);
  return end == xml.c_str() + at + key.size() ? fallback : std::clamp(value, 0.0f, 1.0f);
}

float RobloxPreferredTransparency() {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_checked < std::chrono::seconds(1)) return g_value;
  g_checked = now;
  if (g_path.empty()) g_path = FindSettingsFile();
  struct stat st {};
  if (g_path.empty() || stat(g_path.c_str(), &st) != 0) {
    g_path.clear();
    return g_value;
  }
  if (st.st_mtim.tv_sec == g_mtime.tv_sec && st.st_mtim.tv_nsec == g_mtime.tv_nsec) return g_value;
  g_mtime = st.st_mtim;
  std::ifstream file(g_path);
  std::stringstream text;
  text << file.rdbuf();
  g_value = ParsePreferredTransparency(text.str(), g_value);
  return g_value;
}

}  // namespace nac
