#include "automation.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

#include "app.h"
#include "inject.h"
#include "overlay.h"

namespace nac::automation {
namespace {

using Clock = std::chrono::steady_clock;

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
Clock::duration Ms(double ms) {
  return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

const char* ButtonName(int button) { return button == 1 ? "right" : button == 2 ? "middle" : "left"; }

int ButtonFromName(const std::string& name) {
  if (name == "right") return 1;
  if (name == "middle") return 2;
  return 0;
}

// --- Shared with the UI (g_mutex) ------------------------------------------------

std::mutex g_mutex;
Macro g_current;
int g_revision = 0;
Status g_status;

std::atomic<bool> g_toggle_clicker{false};
std::atomic<bool> g_toggle_recording{false};
std::atomic<bool> g_toggle_playback{false};
std::atomic<bool> g_stop_all{false};

// --- Main thread only ------------------------------------------------------------

std::mt19937 g_rng{std::random_device{}()};

double Random(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(g_rng); }

// Injected events waiting for their time.
struct Pending {
  Clock::time_point at;
  enum Kind { kMove, kDown, kUp, kKeyDown, kKeyUp, kText } kind;
  int code = 0;
  float x = 0, y = 0;
  std::string text;
};
std::vector<Pending> g_pending;

// What we are holding down, so stopping never leaves a key or button stuck.
std::set<int> g_held_keys;
std::set<int> g_held_buttons;

void Queue(Pending p) { g_pending.push_back(std::move(p)); }

void Fire(const Pending& p) {
  switch (p.kind) {
    case Pending::kMove: inject::MouseMove(p.x, p.y); break;
    case Pending::kDown:
      inject::MouseButton(p.code, true, p.x, p.y);
      g_held_buttons.insert(p.code);
      break;
    case Pending::kUp:
      inject::MouseButton(p.code, false, p.x, p.y);
      g_held_buttons.erase(p.code);
      break;
    case Pending::kKeyDown:
      inject::Key(static_cast<SDL_Scancode>(p.code), true);
      g_held_keys.insert(p.code);
      break;
    case Pending::kKeyUp:
      inject::Key(static_cast<SDL_Scancode>(p.code), false);
      g_held_keys.erase(p.code);
      break;
    case Pending::kText: inject::Text(p.text); break;
  }
}

void FlushDue(Clock::time_point now) {
  // Stable order: events due at the same time keep the order they were queued.
  std::stable_sort(g_pending.begin(), g_pending.end(),
                   [](const Pending& a, const Pending& b) { return a.at < b.at; });
  size_t n = 0;
  while (n < g_pending.size() && g_pending[n].at <= now) Fire(g_pending[n++]);
  g_pending.erase(g_pending.begin(), g_pending.begin() + n);
}

// Stopping: releases and cursor moves already queued still happen (now),
// presses that haven't started yet don't.
void ReleaseAll() {
  std::stable_sort(g_pending.begin(), g_pending.end(),
                   [](const Pending& a, const Pending& b) { return a.at < b.at; });
  for (const Pending& p : g_pending) {
    if (p.kind == Pending::kUp || p.kind == Pending::kKeyUp || p.kind == Pending::kMove) Fire(p);
  }
  g_pending.clear();
  const overlay::Metrics m = overlay::GetMetrics();
  for (int b : std::set<int>(g_held_buttons)) inject::MouseButton(b, false, m.mouse_x, m.mouse_y);
  for (int k : std::set<int>(g_held_keys)) inject::Key(static_cast<SDL_Scancode>(k), false);
  g_held_buttons.clear();
  g_held_keys.clear();
}

// --- Autoclicker -----------------------------------------------------------------

struct Clicker {
  bool on = false;
  Clock::time_point started;  // when clicking begins (after the countdown)
  Clock::time_point next;
  int clicks = 0;
  size_t point = 0;
  bool holding = false;
} g_clicker;

// Where the next click lands, in window coordinates.
void ClickTarget(const Config& cfg, float* x, float* y) {
  const overlay::Metrics m = overlay::GetMetrics();
  *x = m.mouse_x;
  *y = m.mouse_y;
  if (cfg.click_target == "points") {
    const auto points = ParsePoints(cfg.click_points);
    if (!points.empty()) {
      const auto& p = points[g_clicker.point++ % points.size()];
      *x = p.first * m.window_w;
      *y = p.second * m.window_h;
    }
  }
  if (cfg.click_spread > 0) {
    const double a = Random(0, 6.283185307179586), r = cfg.click_spread * std::sqrt(Random(0, 1));
    *x += static_cast<float>(std::cos(a) * r);
    *y += static_cast<float>(std::sin(a) * r);
  }
}

// One press (and release) of the click button at its target.
void QueueClick(const Config& cfg, Clock::time_point at, double hold_ms, bool release) {
  float x, y;
  ClickTarget(cfg, &x, &y);
  const int button = ButtonFromName(cfg.click_button);
  const bool fixed = cfg.click_target == "points";
  const overlay::Metrics m = overlay::GetMetrics();
  if (fixed && cfg.click_move_cursor) {
    inject::Warp(x, y);
  } else if (fixed) {
    Queue({at, Pending::kMove, 0, x, y});
  }
  Queue({at, Pending::kDown, button, x, y});
  if (!release) return;
  const Clock::time_point up = at + Ms(hold_ms);
  Queue({up, Pending::kUp, button, x, y});
  // A virtual click puts the game's idea of the cursor back where it was.
  if (fixed && !cfg.click_move_cursor) Queue({up, Pending::kMove, 0, m.mouse_x, m.mouse_y});
}

void StartClicker(const Config& cfg) {
  g_clicker = Clicker{};
  g_clicker.on = true;
  g_clicker.started = Clock::now() + Ms(cfg.click_delay * 1000);
  g_clicker.next = g_clicker.started;
  Log("autoclicker on: %s %s, %.1f cps", cfg.click_type.c_str(), cfg.click_button.c_str(), cfg.click_cps);
}

void StopClicker() {
  if (!g_clicker.on) return;
  g_clicker.on = false;
  ReleaseAll();
  Log("autoclicker off after %d clicks", g_clicker.clicks);
}

void TickClicker(const Config& cfg, Clock::time_point now) {
  if (!g_clicker.on || now < g_clicker.started) return;
  const double elapsed = Seconds(now - g_clicker.started);
  if ((cfg.click_duration > 0 && elapsed >= cfg.click_duration) ||
      (cfg.click_limit > 0 && g_clicker.clicks >= cfg.click_limit)) {
    StopClicker();
    return;
  }
  if (cfg.pause_in_menu && overlay::MenuOpen()) {
    g_clicker.next = now;
    if (g_clicker.holding) {
      ReleaseAll();
      g_clicker.holding = false;
    }
    return;
  }
  if (cfg.click_type == "hold") {
    if (!g_clicker.holding) {
      QueueClick(cfg, now, 0, false);
      g_clicker.holding = true;
      ++g_clicker.clicks;
    }
    return;
  }
  const double interval = 1000.0 / std::clamp(cfg.click_cps, 0.1, 200.0);
  // A press never outlasts the gap to the next one.
  const double hold = std::clamp(cfg.click_hold_ms, 0.0, interval * 0.6);
  for (int burst = 0; g_clicker.next <= now && burst < 50; ++burst) {
    QueueClick(cfg, g_clicker.next, hold, true);
    if (cfg.click_type == "double") QueueClick(cfg, g_clicker.next + Ms(std::max(hold * 2, 45.0)), hold, true);
    ++g_clicker.clicks;
    const double jitter = interval * std::clamp(cfg.click_jitter, 0.0, 90.0) / 100.0;
    g_clicker.next += Ms(interval + Random(-jitter, jitter));
    if (cfg.click_limit > 0 && g_clicker.clicks >= cfg.click_limit) break;
  }
  // Fell far behind (a long frame): don't catch up with a flood of clicks.
  if (now - g_clicker.next > std::chrono::milliseconds(250)) g_clicker.next = now;
}

// --- Recording -------------------------------------------------------------------

struct Recorder {
  bool on = false;
  Macro macro;
  Clock::time_point last;
  Clock::time_point last_move;
} g_rec;

void RecordStep(Step step) {
  const Clock::time_point now = Clock::now();
  const int ms = static_cast<int>(std::lround(Seconds(now - g_rec.last) * 1000));
  g_rec.last = now;
  if (!g_rec.macro.steps.empty() && ms > 0) {
    Step& prev = g_rec.macro.steps.back();
    if (prev.kind == Step::kWait) {
      prev.ms += ms;
    } else {
      Step wait;
      wait.ms = ms;
      g_rec.macro.steps.push_back(wait);
    }
  }
  g_rec.macro.steps.push_back(std::move(step));
}

void StartRecording() {
  g_rec = Recorder{};
  g_rec.on = true;
  g_rec.last = Clock::now();
  Log("recording a macro");
}

void StopRecording() {
  if (!g_rec.on) return;
  g_rec.on = false;
  // Hotkey presses and releases around the recording are not part of it.
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current = g_rec.macro;
  ++g_revision;
  Log("recorded a macro of %zu steps", g_rec.macro.steps.size());
}

// --- Playback --------------------------------------------------------------------

struct Player {
  bool on = false;
  Macro macro;
  size_t index = 0;
  int loop = 0;
  Clock::time_point next;
} g_play;

void StartPlayback() {
  g_play = Player{};
  g_play.macro = Current();
  if (g_play.macro.steps.empty()) return;
  g_play.on = true;
  g_play.next = Clock::now();
}

void StopPlayback() {
  if (!g_play.on) return;
  g_play.on = false;
  ReleaseAll();
}

void TickPlayback(const Config& cfg, Clock::time_point now) {
  if (!g_play.on) return;
  if (cfg.pause_in_menu && overlay::MenuOpen()) {
    g_play.next = std::max(g_play.next, now);
    return;
  }
  const double speed = std::clamp(cfg.macro_speed, 0.1, 10.0);
  const overlay::Metrics m = overlay::GetMetrics();
  auto pos = [&](const Step& s, float* x, float* y) {
    *x = s.x >= 0 ? s.x * m.window_w : m.mouse_x;
    *y = s.y >= 0 ? s.y * m.window_h : m.mouse_y;
  };
  for (int guard = 0; g_play.on && g_play.next <= now && guard < 500; ++guard) {
    if (g_play.index >= g_play.macro.steps.size()) {
      ++g_play.loop;
      if (cfg.macro_repeat >= 1 && g_play.loop >= cfg.macro_repeat) {
        g_play.on = false;
        break;
      }
      g_play.index = 0;
      // A macro of no waits at all still yields between loops.
      g_play.next = std::max(g_play.next, now) + std::chrono::milliseconds(1);
      if (g_play.next > now) break;
    }
    const Step& s = g_play.macro.steps[g_play.index++];
    const Clock::time_point at = g_play.next;
    float x, y;
    pos(s, &x, &y);
    switch (s.kind) {
      case Step::kWait: g_play.next += Ms(s.ms / speed); break;
      case Step::kKeyDown: Queue({at, Pending::kKeyDown, s.code}); break;
      case Step::kKeyUp: Queue({at, Pending::kKeyUp, s.code}); break;
      case Step::kKeyTap:
        Queue({at, Pending::kKeyDown, s.code});
        Queue({at + Ms(30 / speed), Pending::kKeyUp, s.code});
        break;
      case Step::kMove: Queue({at, Pending::kMove, 0, x, y}); break;
      case Step::kMouseDown:
        if (s.x >= 0) Queue({at, Pending::kMove, 0, x, y});
        Queue({at, Pending::kDown, s.code, x, y});
        break;
      case Step::kMouseUp:
        if (s.x >= 0) Queue({at, Pending::kMove, 0, x, y});
        Queue({at, Pending::kUp, s.code, x, y});
        break;
      case Step::kClick:
        if (s.x >= 0) Queue({at, Pending::kMove, 0, x, y});
        Queue({at, Pending::kDown, s.code, x, y});
        Queue({at + Ms(30 / speed), Pending::kUp, s.code, x, y});
        break;
      case Step::kText: Queue({at, Pending::kText, 0, 0, 0, s.text}); break;
    }
  }
}

// --- Files -----------------------------------------------------------------------

std::string MacroDir() {
  const std::string config = Config::Path();
  return config.substr(0, config.rfind('/')) + "/macros";
}

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return "";
  return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

}  // namespace

// --- Text format ---------------------------------------------------------------------

std::string DescribeStep(const Step& s) {
  char buf[160];
  const char* key = SDL_GetScancodeName(static_cast<SDL_Scancode>(s.code));
  auto at = [&]() -> std::string {
    if (s.x < 0) return "";
    char p[48];
    std::snprintf(p, sizeof(p), " %.4f %.4f", s.x, s.y);
    return p;
  };
  switch (s.kind) {
    case Step::kKeyDown: std::snprintf(buf, sizeof(buf), "key_down %s", key); break;
    case Step::kKeyUp: std::snprintf(buf, sizeof(buf), "key_up %s", key); break;
    case Step::kKeyTap: std::snprintf(buf, sizeof(buf), "key %s", key); break;
    case Step::kMouseDown: std::snprintf(buf, sizeof(buf), "mouse_down %s%s", ButtonName(s.code), at().c_str()); break;
    case Step::kMouseUp: std::snprintf(buf, sizeof(buf), "mouse_up %s%s", ButtonName(s.code), at().c_str()); break;
    case Step::kClick: std::snprintf(buf, sizeof(buf), "click %s%s", ButtonName(s.code), at().c_str()); break;
    case Step::kMove: std::snprintf(buf, sizeof(buf), "move%s", at().c_str()); break;
    case Step::kWait: std::snprintf(buf, sizeof(buf), "wait %d", s.ms); break;
    case Step::kText: return "text " + s.text;
  }
  return buf;
}

std::string FormatMacro(const Macro& macro) {
  std::string out = "# Vox Menu macro\n";
  for (const Step& s : macro.steps) out += DescribeStep(s) + "\n";
  return out;
}

Macro ParseMacro(const std::string& text) {
  Macro macro;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find(' ');
    const std::string verb = line.substr(0, space);
    const std::string rest = space == std::string::npos ? "" : Trim(line.substr(space + 1));
    Step s;
    auto key = [&](Step::Kind kind) {
      s.kind = kind;
      s.code = SDL_GetScancodeFromName(rest.c_str());
      return s.code != SDL_SCANCODE_UNKNOWN;
    };
    auto mouse = [&](Step::Kind kind) {
      s.kind = kind;
      std::istringstream in(rest);
      std::string button;
      in >> button;
      s.code = ButtonFromName(button);
      float x, y;
      if (in >> x >> y) {
        s.x = std::clamp(x, 0.0f, 1.0f);
        s.y = std::clamp(y, 0.0f, 1.0f);
      }
      return true;
    };
    bool ok = false;
    if (verb == "key_down") ok = key(Step::kKeyDown);
    else if (verb == "key_up") ok = key(Step::kKeyUp);
    else if (verb == "key") ok = key(Step::kKeyTap);
    else if (verb == "mouse_down") ok = mouse(Step::kMouseDown);
    else if (verb == "mouse_up") ok = mouse(Step::kMouseUp);
    else if (verb == "click") ok = mouse(Step::kClick);
    if (verb == "move") {
      // "move x y" has no button word.
      s.kind = Step::kMove;
      std::istringstream in(rest);
      float x = 0, y = 0;
      ok = static_cast<bool>(in >> x >> y);
      s.x = std::clamp(x, 0.0f, 1.0f);
      s.y = std::clamp(y, 0.0f, 1.0f);
    } else if (verb == "wait") {
      s.kind = Step::kWait;
      s.ms = std::max(0, std::atoi(rest.c_str()));
      ok = true;
    } else if (verb == "text") {
      s.kind = Step::kText;
      s.text = rest;
      ok = !rest.empty();
    }
    if (ok) macro.steps.push_back(s);
  }
  return macro;
}

std::string SafeName(const std::string& name) {
  std::string out;
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') out += c;
  }
  out = Trim(out);
  return out.substr(0, 48);
}

std::vector<std::string> ListMacros() {
  std::vector<std::string> names;
  DIR* dir = opendir(MacroDir().c_str());
  if (dir == nullptr) return names;
  while (dirent* e = readdir(dir)) {
    const std::string file = e->d_name;
    const std::string ext = ".macro";
    if (file.size() > ext.size() && file.compare(file.size() - ext.size(), ext.size(), ext) == 0) {
      names.push_back(file.substr(0, file.size() - ext.size()));
    }
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

bool LoadMacro(const std::string& name, Macro* macro) {
  const std::string safe = SafeName(name);
  if (safe.empty()) return false;
  std::ifstream file(MacroDir() + "/" + safe + ".macro");
  if (!file) return false;
  std::stringstream ss;
  ss << file.rdbuf();
  *macro = ParseMacro(ss.str());
  return true;
}

bool SaveMacro(const std::string& name, const Macro& macro) {
  const std::string safe = SafeName(name);
  if (safe.empty()) return false;
  const std::string dir = MacroDir();
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/" + safe + ".macro";
  {
    std::ofstream out(path + ".tmp");
    if (!out) return false;
    out << FormatMacro(macro);
  }
  return std::rename((path + ".tmp").c_str(), path.c_str()) == 0;
}

bool DeleteMacro(const std::string& name) {
  const std::string safe = SafeName(name);
  return !safe.empty() && unlink((MacroDir() + "/" + safe + ".macro").c_str()) == 0;
}

Macro Current() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_current;
}

void SetCurrent(const Macro& macro) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_current = macro;
}

int Revision() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_revision;
}

std::vector<std::pair<float, float>> ParsePoints(const std::string& text) {
  std::vector<std::pair<float, float>> points;
  std::istringstream in(text);
  std::string item;
  while (std::getline(in, item, ';')) {
    float x, y;
    if (std::sscanf(item.c_str(), "%f,%f", &x, &y) == 2) {
      points.emplace_back(std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f));
    }
  }
  return points;
}

std::string FormatPoints(const std::vector<std::pair<float, float>>& points) {
  std::string out;
  char buf[48];
  for (const auto& [x, y] : points) {
    std::snprintf(buf, sizeof(buf), "%s%.4f,%.4f", out.empty() ? "" : ";", x, y);
    out += buf;
  }
  return out;
}

Status GetStatus() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status;
}

void ToggleClicker() { g_toggle_clicker = true; }
void ToggleRecording() { g_toggle_recording = true; }
void TogglePlayback() { g_toggle_playback = true; }
void StopAll() { g_stop_all = true; }

void Tick() {
  const Config cfg = GetConfig();
  if (g_stop_all.exchange(false)) {
    StopClicker();
    StopPlayback();
    StopRecording();
  }
  if (g_toggle_clicker.exchange(false)) {
    if (g_clicker.on) StopClicker();
    else StartClicker(cfg);
  }
  if (g_toggle_recording.exchange(false)) {
    if (g_rec.on) {
      StopRecording();
    } else {
      StopPlayback();
      StartRecording();
    }
  }
  if (g_toggle_playback.exchange(false)) {
    if (g_play.on) {
      StopPlayback();
    } else if (!g_rec.on) {
      StartPlayback();
    }
  }
  const Clock::time_point now = Clock::now();
  TickClicker(cfg, now);
  TickPlayback(cfg, now);
  FlushDue(now);

  Status st;
  st.clicking = g_clicker.on;
  st.countdown = g_clicker.on ? static_cast<float>(std::max(0.0, Seconds(g_clicker.started - now))) : 0;
  st.clicks = g_clicker.clicks;
  st.elapsed = g_clicker.on ? static_cast<float>(std::max(0.0, Seconds(now - g_clicker.started))) : 0;
  st.recording = g_rec.on;
  st.recorded = static_cast<int>(g_rec.macro.steps.size());
  st.playing = g_play.on;
  st.step = static_cast<int>(g_play.index);
  st.steps = static_cast<int>(g_play.macro.steps.size());
  st.loop = g_play.loop;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_status = st;
}

bool HandleHotkey(const SDL_KeyboardEvent& key) {
  const Config cfg = GetConfig();
  auto is = [&](const std::string& name) {
    if (name == "off" || name.empty()) return false;
    // By scancode or key, so it works whatever the keyboard layout reports.
    const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    const SDL_Keycode code = SDL_GetKeyFromName(name.c_str());
    return (sc != SDL_SCANCODE_UNKNOWN && key.scancode == sc) || (code != SDLK_UNKNOWN && key.key == code);
  };
  void (*action)() = nullptr;
  if (is(cfg.key_clicker)) action = ToggleClicker;
  else if (is(cfg.key_macro)) action = TogglePlayback;
  else if (is(cfg.key_record)) action = ToggleRecording;
  if (action == nullptr) return false;
  if (key.down && !key.repeat) action();
  return true;
}

void Observe(const SDL_Event& e) {
  if (!g_rec.on) return;
  const Config cfg = GetConfig();
  const overlay::Metrics m = overlay::GetMetrics();
  auto frac = [&](float x, float y, Step* s) {
    if (m.window_w <= 0 || m.window_h <= 0) return;
    s->x = std::clamp(x / m.window_w, 0.0f, 1.0f);
    s->y = std::clamp(y / m.window_h, 0.0f, 1.0f);
  };
  Step s;
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      if (e.key.repeat) return;
      s.kind = e.type == SDL_EVENT_KEY_DOWN ? Step::kKeyDown : Step::kKeyUp;
      s.code = e.key.scancode;
      break;
    case SDL_EVENT_TEXT_INPUT:
      if (e.text.text == nullptr || *e.text.text == 0) return;
      s.kind = Step::kText;
      s.text = e.text.text;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      s.kind = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Step::kMouseDown : Step::kMouseUp;
      s.code = e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 0;
      frac(e.button.x, e.button.y, &s);
      break;
    case SDL_EVENT_MOUSE_MOTION: {
      if (!cfg.macro_record_moves) return;
      const Clock::time_point now = Clock::now();
      if (now - g_rec.last_move < std::chrono::milliseconds(16)) return;
      g_rec.last_move = now;
      s.kind = Step::kMove;
      frac(e.motion.x, e.motion.y, &s);
      if (s.x < 0) return;
      break;
    }
    default: return;
  }
  RecordStep(std::move(s));
}

}  // namespace nac::automation
