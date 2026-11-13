#include "piano.h"

#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "app.h"
#include "automation.h"
#include "inject.h"
#include "midi_file.h"
#include "midi_input.h"

namespace nac::piano {
namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

// --- Songs ----------------------------------------------------------------------

struct Library {
  bool scanned = false;
  std::string folder;
  std::vector<std::string> names, paths;
};
std::mutex g_lib_mutex;
Library g_lib;

std::string Home() {
  const char* home = std::getenv("HOME");
  return home ? home : "";
}

// An XDG user folder ("MUSIC", "DOWNLOAD") from user-dirs.dirs, as
// xdg-user-dir reports it.
std::string UserDir(const std::string& name, const char* fallback) {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::ifstream in(((xdg && *xdg) ? std::string(xdg) : Home() + "/.config") + "/user-dirs.dirs");
  const std::string key = "XDG_" + name + "_DIR=";
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) != 0) continue;
    std::string v = line.substr(key.size());
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (v.rfind("$HOME", 0) == 0) v = Home() + v.substr(5);
    if (!v.empty()) return v;
  }
  return Home() + "/" + fallback;
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

void ScanLocked() {
  g_lib.folder = SongsFolder();
  g_lib.names.clear();
  g_lib.paths.clear();
  std::vector<std::pair<std::string, std::string>> found;  // name, path
  std::error_code ec;
  for (fs::recursive_directory_iterator it(g_lib.folder, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end && found.size() < 5000; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = Lower(it->path().extension().string());
    if (ext != ".mid" && ext != ".midi") continue;
    found.emplace_back(it->path().stem().string(), it->path().string());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
    const std::string la = Lower(a.first), lb = Lower(b.first);
    return la != lb ? la < lb : a.second < b.second;
  });
  for (auto& [name, path] : found) {
    g_lib.names.push_back(std::move(name));
    g_lib.paths.push_back(std::move(path));
  }
  g_lib.scanned = true;
}

void EnsureScanned() {
  std::lock_guard<std::mutex> lock(g_lib_mutex);
  if (!g_lib.scanned || g_lib.folder != SongsFolder()) ScanLocked();
}

// --- Keys -----------------------------------------------------------------------

SDL_Scancode CharKey(char c) {
  if (c == '0') return SDL_SCANCODE_0;
  if (c >= '1' && c <= '9') return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (c - '1'));
  if (c >= 'a' && c <= 'z') return static_cast<SDL_Scancode>(SDL_SCANCODE_A + (c - 'a'));
  return SDL_SCANCODE_UNKNOWN;
}

bool IsBlack(int note) {
  const int p = note % 12;
  return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
}

// Lowest and highest notes the layout plays.
int Lowest(bool all88) { return all88 ? 21 : 36; }
int Highest(bool all88) { return all88 ? 108 : 96; }

// What the main thread has pressed. Main thread only.
struct Keys {
  std::array<int, SDL_SCANCODE_COUNT> owner;  // the note holding each key down, or -1
  std::array<int, 128> sounding;              // the note each MIDI note pressed, or -1
  bool space = false;                         // sustain
  int velocity_key = -1;                      // the last Alt+key loudness sent
  std::array<int, 128> connect;               // MIDI Connect: the note sent for each, or -1
  bool connect_pedal = false;
  Keys() {
    owner.fill(-1);
    sounding.fill(-1);
    connect.fill(-1);
  }
};
Keys g_keys;

void Tap(SDL_Scancode key, SDL_Keymod mod = SDL_KMOD_NONE) {
  inject::Key(key, true, mod);
  inject::Key(key, false, mod);
}

// The note a MIDI note plays once transposed (and folded into range), or -1.
int Target(int note, const Config& cfg) {
  int n = note + static_cast<int>(std::lround(cfg.piano_transpose));
  const int lo = Lowest(cfg.piano_88), hi = Highest(cfg.piano_88);
  if (cfg.piano_fold) {
    while (n < lo) n += 12;
    while (n > hi) n -= 12;
  }
  return n >= lo && n <= hi ? n : -1;
}

// Virtual Piano's loudness keys: Alt plus one of 32 keys, quietest first.
void SetLoudness(int velocity) {
  static const char kKeys[] = "1234567890qwertyuiopasdfghjklzxc";
  const int index = std::clamp((velocity + 3) / 4 - 1, 0, 31);
  if (index == g_keys.velocity_key) return;
  g_keys.velocity_key = index;
  inject::Key(SDL_SCANCODE_LALT, true, SDL_KMOD_LALT);
  Tap(CharKey(kKeys[index]), SDL_KMOD_LALT);
  inject::Key(SDL_SCANCODE_LALT, false);
}

// MIDI Connect: games that read the keyboard's notes from the numpad take
// "*" and four digits per message (base 12, with - and + as 10 and 11).
void SendMidiConnect(int type, int value) {
  static const SDL_Scancode kDigits[12] = {
      SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
      SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
      SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_PLUS};
  type = std::clamp(type, 0, 143);
  value = std::clamp(value, 0, 143);
  Tap(SDL_SCANCODE_KP_MULTIPLY);
  Tap(kDigits[type / 12]);
  Tap(kDigits[type % 12]);
  Tap(kDigits[value / 12]);
  Tap(kDigits[value % 12]);
}

bool MidiConnect(const Config& cfg) { return cfg.piano_output == "midiconnect"; }

void Press(int note, int velocity, const Config& cfg) {
  if (note < 0 || note > 127) return;
  if (MidiConnect(cfg)) {
    // The game plays the note itself, over its whole range.
    const int n = note + static_cast<int>(std::lround(cfg.piano_transpose));
    if (n < 0 || n > 127) return;
    if (g_keys.connect[note] >= 0) SendMidiConnect(g_keys.connect[note], 0);
    SendMidiConnect(n, std::max(velocity, 1));
    g_keys.connect[note] = n;
    return;
  }
  const int n = Target(note, cfg);
  Stroke s;
  if (n < 0 || !NoteStroke(n, cfg.piano_88, &s)) return;
  if (cfg.piano_velocity && velocity > 0) SetLoudness(velocity);
  // Already down (the same note again, or its sharp): lift it first so the
  // game sees a new press.
  if (g_keys.owner[s.key] >= 0) inject::Key(s.key, false);
  const SDL_Keymod mod = (s.shift ? SDL_KMOD_LSHIFT : 0) | (s.ctrl ? SDL_KMOD_LCTRL : 0);
  if (s.ctrl) inject::Key(SDL_SCANCODE_LCTRL, true, SDL_KMOD_LCTRL);
  if (s.shift) inject::Key(SDL_SCANCODE_LSHIFT, true, mod);
  inject::Key(s.key, true, mod);
  if (s.shift) inject::Key(SDL_SCANCODE_LSHIFT, false, mod & ~SDL_KMOD_LSHIFT);
  if (s.ctrl) inject::Key(SDL_SCANCODE_LCTRL, false);
  g_keys.owner[s.key] = n;
  g_keys.sounding[note] = n;
}

void Release(int note, const Config& cfg) {
  if (note < 0 || note > 127) return;
  if (g_keys.connect[note] >= 0) {
    SendMidiConnect(g_keys.connect[note], 0);
    g_keys.connect[note] = -1;
  }
  const int n = g_keys.sounding[note];
  g_keys.sounding[note] = -1;
  Stroke s;
  if (n < 0 || !NoteStroke(n, cfg.piano_88, &s) || g_keys.owner[s.key] != n) return;
  inject::Key(s.key, false);
  g_keys.owner[s.key] = -1;
}

void Pedal(bool down, int value, const Config& cfg) {
  if (MidiConnect(cfg)) {
    // MIDI Connect's pedal is message 143 with the pedal's value.
    SendMidiConnect(143, value);
    g_keys.connect_pedal = down;
    return;
  }
  if (cfg.piano_sustain != "pedal" && cfg.piano_sustain != "inverted") return;
  const bool want = cfg.piano_sustain == "pedal" ? down : !down;
  if (want == g_keys.space) return;
  inject::Key(SDL_SCANCODE_SPACE, want);
  g_keys.space = want;
}

void ReleaseAll() {
  for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc) {
    if (g_keys.owner[sc] < 0) continue;
    inject::Key(static_cast<SDL_Scancode>(sc), false);
    g_keys.owner[sc] = -1;
  }
  g_keys.sounding.fill(-1);
  if (g_keys.space) inject::Key(SDL_SCANCODE_SPACE, false);
  g_keys.space = false;
  for (int& n : g_keys.connect) {
    if (n >= 0) SendMidiConnect(n, 0);
    n = -1;
  }
  if (g_keys.connect_pedal) SendMidiConnect(143, 0);
  g_keys.connect_pedal = false;
}

// --- Player ---------------------------------------------------------------------

struct Player {
  std::shared_ptr<const midi::Song> song;
  std::string name;
  int index = -1;
  std::vector<midi::Event> schedule;  // the song's events, with human timing
  double schedule_human = -1;         // what the schedule was made with
  size_t next = 0;                    // in the schedule
  bool playing = false;
  int64_t base = 0;  // position when last started, paused or moved (ns)
  Clock::time_point resumed;
  double speed = 1.0;
  std::vector<char> muted, solo;
  std::string error;
  bool release = false;  // the main thread lifts every key
};
std::mutex g_mutex;
Player g_p;

int64_t PositionLocked(Clock::time_point now) {
  if (!g_p.playing) return g_p.base;
  return g_p.base +
         static_cast<int64_t>(std::chrono::duration<double, std::nano>(now - g_p.resumed).count() * g_p.speed);
}

size_t FindLocked(int64_t ns) {
  return std::lower_bound(g_p.schedule.begin(), g_p.schedule.end(), ns,
                          [](const midi::Event& e, int64_t t) { return e.ns < t; }) -
         g_p.schedule.begin();
}

// Human mode: notes land early or late by up to `ms`, and are let go a
// little late, like hands rather than a sequencer.
void ScheduleLocked(double ms, Clock::time_point now) {
  g_p.schedule = g_p.song->events;
  g_p.schedule_human = ms;
  if (ms > 0) {
    std::mt19937 rng(std::random_device{}());
    const int64_t j = static_cast<int64_t>(ms * 1e6);
    std::uniform_int_distribution<int64_t> press(-j, j / 2), release(0, j);
    for (midi::Event& e : g_p.schedule) {
      if (e.note == midi::kSustain) continue;
      e.ns = std::max<int64_t>(0, e.ns + (e.down ? press(rng) : release(rng)));
    }
    std::stable_sort(g_p.schedule.begin(), g_p.schedule.end(),
                     [](const midi::Event& a, const midi::Event& b) { return a.ns < b.ns; });
  }
  g_p.next = FindLocked(PositionLocked(now));
}

void SeekLocked(int64_t ns, Clock::time_point now) {
  if (!g_p.song) return;
  g_p.base = std::clamp<int64_t>(ns, 0, g_p.song->length_ns);
  g_p.resumed = now;
  g_p.next = FindLocked(g_p.base);
  g_p.release = true;
}

bool TrackOn(int track) {
  if (track < 0 || track >= static_cast<int>(g_p.muted.size())) return true;
  const bool any_solo = std::any_of(g_p.solo.begin(), g_p.solo.end(), [](char s) { return s; });
  return any_solo ? g_p.solo[track] : !g_p.muted[track];
}

void Play(const midi::Event& e, const Config& cfg) {
  if (e.note == midi::kSustain) Pedal(e.down, e.velocity, cfg);
  else if (e.down) Press(e.note, e.velocity, cfg);
  else Release(e.note, cfg);
}

// --- Control socket ---------------------------------------------------------------

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

std::string SocketPath() { return Home() + "/.vox-menu-piano.sock"; }

void Serve() {
  const std::string path = SocketPath();
  sockaddr_un addr{};
  if (Home().empty() || path.size() >= sizeof(addr.sun_path)) return;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  // The newest game takes the socket over from any older one.
  unlink(path.c_str());
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 8) != 0) {
    Log("vox-menu: can't listen on %s: %s", path.c_str(), std::strerror(errno));
    close(fd);
    return;
  }
  chmod(path.c_str(), 0600);
  static ino_t inode = 0;
  struct stat st;
  if (stat(path.c_str(), &st) == 0) inode = st.st_ino;
  std::atexit([] {
    // Only ours: a newer game may have taken the path.
    struct stat now;
    if (stat(SocketPath().c_str(), &now) == 0 && now.st_ino == inode) unlink(SocketPath().c_str());
  });
  while (true) {
    const int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) continue;
    timeval tv{1, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string line;
    char buf[256];
    while (line.find('\n') == std::string::npos && line.size() < 1024) {
      const ssize_t n = recv(client, buf, sizeof(buf), 0);
      if (n <= 0) break;
      line.append(buf, n);
    }
    const std::string reply = Command(line.substr(0, line.find('\n'))) + "\n";
    send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
    close(client);
  }
}

// --- Downloads ------------------------------------------------------------------

std::mutex g_added_mutex;
std::string g_added;  // the last song moved in from Downloads
Clock::time_point g_added_at;

bool IsMidiName(const std::string& name) {
  const std::string ext = Lower(fs::path(name).extension().string());
  return ext == ".mid" || ext == ".midi";
}

// A finished MIDI file, not a browser's empty placeholder for one.
bool LooksLikeMidi(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  char head[1024];
  in.read(head, sizeof(head));
  return std::string(head, in.gcount()).find("MThd") != std::string::npos;
}

// Moves a downloaded song into the songs folder, under a free name.
void Import(const std::string& path) {
  if (!LooksLikeMidi(path)) return;
  const fs::path from(path);
  const fs::path dir(SongsFolder());
  std::error_code ec;
  if (fs::equivalent(from.parent_path(), dir, ec)) return;
  fs::create_directories(dir, ec);
  fs::path to = dir / from.filename();
  for (int n = 2; fs::exists(to, ec) && n < 1000; ++n) {
    to = dir / (from.stem().string() + " (" + std::to_string(n) + ")" + from.extension().string());
  }
  fs::rename(from, to, ec);
  if (ec) {  // another filesystem
    ec.clear();
    fs::copy_file(from, to, ec);
    if (!ec) fs::remove(from, ec);
  }
  if (ec) {
    Log("vox-menu: can't move %s to %s: %s", path.c_str(), to.c_str(), ec.message().c_str());
    return;
  }
  Log("vox-menu: moved %s into the songs folder", from.filename().c_str());
  Rescan();
  std::lock_guard<std::mutex> lock(g_added_mutex);
  g_added = to.stem().string();
  g_added_at = Clock::now();
}

// Watches the Downloads folder while the setting is on: a .mid file that
// finishes downloading (written and closed, or renamed from the browser's
// partial file) is moved into the songs folder.
void WatchDownloads() {
  const int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (fd < 0) return;
  int wd = -1;
  std::string watched;
  alignas(inotify_event) char buf[16384];
  while (true) {
    const Config cfg = GetConfig();
    const std::string want = cfg.piano_watch_downloads ? UserDir("DOWNLOAD", "Downloads") : "";
    if (want != watched || (wd < 0 && !want.empty())) {
      if (wd >= 0) inotify_rm_watch(fd, wd);
      wd = want.empty() ? -1 : inotify_add_watch(fd, want.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
      watched = want;
    }
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 1000) <= 0) continue;
    const ssize_t n = read(fd, buf, sizeof(buf));
    for (ssize_t i = 0; i < n;) {
      const auto* e = reinterpret_cast<const inotify_event*>(buf + i);
      i += sizeof(inotify_event) + e->len;
      if (e->wd != wd || e->len == 0 || (e->mask & IN_ISDIR)) continue;
      if (IsMidiName(e->name)) Import(watched + "/" + e->name);
    }
  }
}

std::once_flag g_server;
Clock::time_point g_live_tried;

}  // namespace

std::string SongsFolder() {
  const Config cfg = GetConfig();
  if (cfg.piano_folder.empty()) return UserDir("MUSIC", "Music");
  if (cfg.piano_folder[0] == '~') return Home() + cfg.piano_folder.substr(1);
  return cfg.piano_folder;
}

std::vector<std::string> Songs() {
  EnsureScanned();
  std::lock_guard<std::mutex> lock(g_lib_mutex);
  return g_lib.names;
}

void Rescan() {
  std::lock_guard<std::mutex> lock(g_lib_mutex);
  ScanLocked();
}

bool NoteStroke(int note, bool all88, Stroke* stroke) {
  static const char kWhite[] = "1234567890qwertyuiopasdfghjklzxcvbnm";  // C2 .. C7
  static const char kLow[] = "1234567890qwert";                         // A0 .. B1, with Ctrl
  static const char kHigh[] = "yuiopasdfghj";                           // C#7 .. C8, with Ctrl
  *stroke = Stroke{};
  if (note >= 36 && note <= 96) {
    // Black keys are Shift and the white key below.
    const int white = IsBlack(note) ? note - 1 : note;
    int index = 0;
    for (int n = 36; n < white; ++n) index += !IsBlack(n);
    stroke->key = CharKey(kWhite[index]);
    stroke->shift = IsBlack(note);
    return true;
  }
  if (!all88) return false;
  if (note >= 21 && note < 36) stroke->key = CharKey(kLow[note - 21]);
  else if (note > 96 && note <= 108) stroke->key = CharKey(kHigh[note - 97]);
  else return false;
  stroke->ctrl = true;
  return true;
}

Status GetStatus() {
  Status st;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    st.loaded = g_p.song != nullptr;
    st.playing = g_p.playing;
    st.song = g_p.name;
    st.index = g_p.index;
    st.speed = g_p.speed;
    st.error = g_p.error;
    if (g_p.song) {
      st.position = std::min(PositionLocked(Clock::now()), g_p.song->length_ns) / 1e9;
      st.length = g_p.song->length_ns / 1e9;
      for (size_t i = 0; i < g_p.song->tracks.size(); ++i) {
        const midi::Track& t = g_p.song->tracks[i];
        st.tracks.push_back({t.name, t.notes, t.drums, g_p.muted[i] != 0, g_p.solo[i] != 0});
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_added_mutex);
    if (!g_added.empty() && Clock::now() - g_added_at < std::chrono::seconds(8)) st.added = g_added;
  }
  st.live = midi_input::Running();
  st.devices = midi_input::Devices();
  st.live_error = midi_input::Error();
  return st;
}

bool Load(int index) {
  std::string name, path;
  {
    EnsureScanned();
    std::lock_guard<std::mutex> lock(g_lib_mutex);
    if (index < 0 || index >= static_cast<int>(g_lib.paths.size())) return false;
    name = g_lib.names[index];
    path = g_lib.paths[index];
  }
  auto song = std::make_shared<midi::Song>();
  std::string error;
  const bool ok = midi::Load(path, song.get(), &error);
  const Config cfg = GetConfig();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!ok) {
    g_p.error = name + ": " + error;
    Log("vox-menu: can't play %s: %s", path.c_str(), error.c_str());
    return false;
  }
  const Clock::time_point now = Clock::now();
  g_p.song = song;
  g_p.name = name;
  g_p.index = index;
  g_p.error.clear();
  g_p.muted.assign(song->tracks.size(), 0);
  g_p.solo.assign(song->tracks.size(), 0);
  for (size_t i = 0; i < song->tracks.size(); ++i) g_p.muted[i] = cfg.piano_skip_drums && song->tracks[i].drums;
  g_p.playing = false;
  g_p.base = 0;
  ScheduleLocked(cfg.piano_human, now);
  SeekLocked(0, now);
  g_p.playing = true;
  return true;
}

void TogglePlay() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_p.song) return;
  const Clock::time_point now = Clock::now();
  if (g_p.playing) {
    g_p.base = PositionLocked(now);
    g_p.playing = false;
    g_p.release = true;
    return;
  }
  // Played to the end: from the top again.
  if (g_p.base >= g_p.song->length_ns) SeekLocked(0, now);
  g_p.resumed = now;
  g_p.playing = true;
}

void Stop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_p.playing = false;
  SeekLocked(0, Clock::now());
  g_p.release = true;
}

void SeekBy(double seconds) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Clock::time_point now = Clock::now();
  SeekLocked(PositionLocked(now) + static_cast<int64_t>(seconds * 1e9), now);
}

void SeekTo(double seconds) {
  std::lock_guard<std::mutex> lock(g_mutex);
  SeekLocked(static_cast<int64_t>(seconds * 1e9), Clock::now());
}

void Restart() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_p.song) return;
  SeekLocked(0, Clock::now());
  g_p.playing = true;
}

void SetSpeed(double speed) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Clock::time_point now = Clock::now();
  g_p.base = PositionLocked(now);
  g_p.resumed = now;
  speed = std::clamp(speed, 0.25, 2.0);
  g_p.speed = std::fabs(speed - 1.0) < 0.05 ? 1.0 : speed;
}

void SpeedUp() { SetSpeed(GetStatus().speed * 1.1); }
void SpeedDown() { SetSpeed(GetStatus().speed / 1.1); }

void SetTrack(int track, bool muted, bool solo) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (track < 0 || track >= static_cast<int>(g_p.muted.size())) return;
  g_p.muted[track] = muted;
  g_p.solo[track] = solo;
  g_p.release = true;  // nothing left hanging from a track turned off
}

void StartServer() {
  std::call_once(g_server, [] {
    std::thread(Serve).detach();
    std::thread(WatchDownloads).detach();
  });
}

std::string Command(const std::string& raw) {
  std::string line = raw;
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
  if (line == "PLAY_PAUSE") {
    TogglePlay();
  } else if (line == "REWIND") {
    SeekBy(-10);
  } else if (line == "SKIP") {
    SeekBy(10);
  } else if (line == "RESTART") {
    Restart();
  } else if (line == "STOP") {
    Stop();
  } else if (line == "SPEED_UP") {
    SpeedUp();
  } else if (line == "SPEED_DOWN") {
    SpeedDown();
  } else if (line == "RESCAN") {
    Rescan();
  } else if (line == "GET_TIME") {
    const Status st = GetStatus();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "{\"current\":%.3f,\"total\":%.3f,\"speed\":%.3f,\"paused\":%s,\"song\":",
                  st.position, st.length, st.speed, st.playing ? "false" : "true");
    return buf + Json(st.song) + "}";
  } else if (line == "LIST_SONGS") {
    std::string out = "[";
    for (const std::string& name : Songs()) out += (out.size() > 1 ? "," : "") + Json(name);
    return out + "]";
  } else if (line.rfind("LOAD_SONG ", 0) == 0) {
    char* end = nullptr;
    const long index = std::strtol(line.c_str() + 10, &end, 10);
    return end != line.c_str() + 10 && Load(static_cast<int>(index)) ? "OK" : "ERR";
  } else {
    return "ERR_UNKNOWN";
  }
  return "OK";
}

bool HandleHotkey(const SDL_KeyboardEvent& key) {
  const Config cfg = GetConfig();
  void (*action)() = nullptr;
  if (automation::KeyMatches(cfg.key_piano_play, key)) action = TogglePlay;
  else if (automation::KeyMatches(cfg.key_piano_back, key)) action = [] { SeekBy(-10); };
  else if (automation::KeyMatches(cfg.key_piano_forward, key)) action = [] { SeekBy(10); };
  if (action == nullptr) return false;
  if (key.down && !key.repeat) action();
  return true;
}

bool HandleHotkeyButton(int sdl_button, bool down) {
  const char* name = automation::SideButtonName(sdl_button);
  if (name == nullptr) return false;
  const Config cfg = GetConfig();
  void (*action)() = nullptr;
  if (cfg.key_piano_play == name) action = TogglePlay;
  else if (cfg.key_piano_back == name) action = [] { SeekBy(-10); };
  else if (cfg.key_piano_forward == name) action = [] { SeekBy(10); };
  if (action == nullptr) return false;
  if (down) action();
  return true;
}

void Tick() {
  const Config cfg = GetConfig();
  const Clock::time_point now = Clock::now();

  // The MIDI keyboard: (re)started every few seconds while it should run.
  if (!cfg.piano_live) {
    midi_input::Stop();
  } else if (!midi_input::Running() && now - g_live_tried > std::chrono::seconds(3)) {
    g_live_tried = now;
    midi_input::Start();
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_p.release) {
    g_p.release = false;
    ReleaseAll();
  }
  // Switching between keys and MIDI Connect lets go of what the other held.
  static std::string output;
  if (cfg.piano_output != output) {
    output = cfg.piano_output;
    ReleaseAll();
  }
  midi_input::Message m;
  while (midi_input::Poll(&m)) Play({0, m.note, m.down, m.velocity, -1}, cfg);
  if (!g_p.song) return;
  if (cfg.piano_human != g_p.schedule_human) ScheduleLocked(cfg.piano_human, now);
  if (!g_p.playing) return;

  // Everything due, releases before presses so a repeated note sounds again.
  const int64_t pos = PositionLocked(now);
  const size_t from = g_p.next;
  while (g_p.next < g_p.schedule.size() && g_p.schedule[g_p.next].ns <= pos) ++g_p.next;
  for (size_t i = from; i < g_p.next; ++i) {
    const midi::Event& e = g_p.schedule[i];
    if (!e.down && TrackOn(e.track)) Play(e, cfg);
  }
  for (size_t i = from; i < g_p.next; ++i) {
    const midi::Event& e = g_p.schedule[i];
    if (e.down && TrackOn(e.track)) Play(e, cfg);
  }
  if (g_p.next >= g_p.schedule.size() && pos >= g_p.song->length_ns) {
    g_p.playing = false;
    g_p.base = g_p.song->length_ns;
    ReleaseAll();
  }
}

}  // namespace nac::piano
