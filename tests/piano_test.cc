// The MIDI piano player: file parsing, the key layout, and playback with
// input injection replaced by a log of what would have reached the game.

#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/app.h"
#include "../src/inject.h"
#include "../src/midi_file.h"
#include "../src/piano.h"

namespace {

std::vector<std::string> g_events;
int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    for (const std::string& e : g_events) std::fprintf(stderr, "    %s\n", e.c_str());
    ++g_failures;
  }
}

int Count(const std::string& event) {
  int n = 0;
  for (const std::string& e : g_events) n += e == event;
  return n;
}

int IndexOf(const std::string& event) {
  for (size_t i = 0; i < g_events.size(); ++i) {
    if (g_events[i] == event) return static_cast<int>(i);
  }
  return -1;
}

void Run(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    nac::piano::Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  nac::piano::Tick();
}

// A tiny standard MIDI file builder.
struct Track {
  std::string bytes;
  void Delta(uint32_t ticks) {
    unsigned char buf[4];
    int n = 0;
    buf[n++] = ticks & 0x7F;
    while (ticks >>= 7) buf[n++] = 0x80 | (ticks & 0x7F);
    while (n) bytes += static_cast<char>(buf[--n]);
  }
  void Raw(std::initializer_list<int> b) {
    for (int c : b) bytes += static_cast<char>(c);
  }
  void Name(const std::string& name) {
    Delta(0);
    Raw({0xFF, 0x03, static_cast<int>(name.size())});
    bytes += name;
  }
};

std::string Be(uint32_t v, int n) {
  std::string s;
  for (int i = n - 1; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
  return s;
}

std::string File(const std::vector<Track>& tracks, int division) {
  std::string out = "MThd" + Be(6, 4) + Be(1, 2) + Be(tracks.size(), 2) + Be(division, 2);
  for (Track t : tracks) {
    t.Delta(0);
    t.Raw({0xFF, 0x2F, 0x00});
    out += "MTrk" + Be(t.bytes.size(), 4) + t.bytes;
  }
  return out;
}

// 100 ticks a beat at 0.1 s a beat: one tick is a millisecond.
std::string Song() {
  Track tempo;
  tempo.Delta(0);
  tempo.Raw({0xFF, 0x51, 0x03, 0x01, 0x86, 0xA0});  // 100000 us per beat
  Track piano;
  piano.Name("Grand Piano");
  piano.Delta(0);
  piano.Raw({0x90, 60, 100});  // C4 at 0
  piano.Delta(100);
  piano.Raw({60, 0});          // running status, velocity 0: off at 100 ms
  piano.Delta(0);
  piano.Raw({61, 90});         // C#4 at 100 ms
  piano.Delta(100);
  piano.Raw({0x80, 61, 0});    // off at 200 ms
  piano.Delta(0);
  piano.Raw({0x90, 21, 80});   // A0 at 200 ms: only with 88 keys
  piano.Delta(100);
  piano.Raw({0x80, 21, 0});
  piano.Delta(0);
  piano.Raw({0x80, 64, 0});    // a stray note off: ignored
  Track drums;
  drums.Name("Beat");
  drums.Delta(50);
  drums.Raw({0x99, 36, 100});  // channel 10
  drums.Delta(10);
  drums.Raw({0x89, 36, 0});
  return File({tempo, piano, drums}, 100);
}

}  // namespace

namespace nac::inject {
void MouseMove(float, float) {}
void MouseButton(int, bool, float, float) {}
void Key(SDL_Scancode scancode, bool down, SDL_Keymod mod) {
  std::string e = std::string(down ? "down " : "up ") + SDL_GetScancodeName(scancode);
  if (mod & SDL_KMOD_SHIFT) e += " +shift";
  if (mod & SDL_KMOD_CTRL) e += " +ctrl";
  if (mod & SDL_KMOD_ALT) e += " +alt";
  g_events.push_back(e);
}
void Text(const std::string&) {}
void Warp(float, float) {}
}  // namespace nac::inject

// media.cc only asks whether voice chat is reading the mic.
namespace nac {
bool MicInUse() { return false; }
}  // namespace nac

int main() {
  using namespace nac;
  char dir[] = "/tmp/vox-menu-piano-XXXXXX";
  const std::string root = mkdtemp(dir);
  setenv("XDG_CONFIG_HOME", root.c_str(), 1);
  setenv("HOME", root.c_str(), 1);

  // --- Parsing ---
  midi::Song song;
  std::string error;
  Check(midi::Parse(Song(), &song, &error), "parses");
  Check(song.tracks.size() == 2, "two tracks with notes (the tempo track isn't one)");
  Check(song.tracks.size() == 2 && song.tracks[0].name == "Grand Piano" && !song.tracks[0].drums, "piano track");
  Check(song.tracks.size() == 2 && song.tracks[1].drums, "a channel 10 track is drums");
  Check(song.events.size() == 8, "four notes on and off, the stray off dropped");
  bool sharp_at_100 = false;
  for (const midi::Event& e : song.events) sharp_at_100 |= e.note == 61 && e.down && e.ns == 100000000;
  Check(sharp_at_100, "C#4 starts at 100 ms");
  Check(song.length_ns == 300000000, "300 ms long");
  Check(!midi::Parse("not midi", &song, &error) && !error.empty(), "rejects other files");
  Check(midi::NoteName(60) == "C4" && midi::NoteName(61) == "C#4" && midi::NoteName(21) == "A0", "note names");

  // --- The key layout (MIDI++'s, Virtual Piano's) ---
  auto key = [](int note, bool all88) {
    piano::Stroke s;
    if (!piano::NoteStroke(note, all88, &s)) return std::string("none");
    std::string k = SDL_GetScancodeName(s.key);
    if (s.shift) k = "shift+" + k;
    if (s.ctrl) k = "ctrl+" + k;
    return k;
  };
  Check(key(36, false) == "1", "C2 is 1");
  Check(key(37, false) == "shift+1", "C#2 is !");
  Check(key(60, false) == "T", "C4 is t");
  Check(key(61, false) == "shift+T", "C#4 is T");
  Check(key(83, false) == "K", "B5 is k");
  Check(key(94, false) == "shift+B", "A#6 is B");
  Check(key(96, false) == "M", "C7 is m");
  Check(key(21, false) == "none" && key(97, false) == "none", "61 keys stop at C2 and C7");
  Check(key(21, true) == "ctrl+1", "A0 is ctrl+1");
  Check(key(35, true) == "ctrl+T", "B1 is ctrl+t");
  Check(key(97, true) == "ctrl+Y", "C#7 is ctrl+y");
  Check(key(108, true) == "ctrl+J", "C8 is ctrl+j");
  Check(key(109, true) == "none", "nothing past C8");

  // --- The songs folder ---
  const std::string songs = root + "/songs";
  std::system(("mkdir -p '" + songs + "/sub'").c_str());
  std::ofstream(songs + "/sub/Test Song.mid", std::ios::binary) << Song();
  std::ofstream(songs + "/broken.mid", std::ios::binary) << "nope";
  std::ofstream(songs + "/notes.txt") << "not a song";
  Config cfg = GetConfig();
  cfg.piano_folder = songs;
  cfg.piano_88 = false;
  SetConfig(cfg);
  Check(piano::Songs() == std::vector<std::string>({"broken", "Test Song"}), "lists .mid files, sub-folders too");
  Check(piano::Command("LIST_SONGS") == "[\"broken\",\"Test Song\"]", "LIST_SONGS");
  Check(piano::Command("LOAD_SONG 0") == "ERR" && !piano::GetStatus().error.empty(), "a broken file fails to load");
  Check(piano::Command("LOAD_SONG 9") == "ERR", "no such song");
  Check(piano::Command("DANCE") == "ERR_UNKNOWN", "unknown command");

  // --- Playback: 61 keys, drums muted ---
  g_events.clear();
  Check(piano::Command("LOAD_SONG 1") == "OK", "loads");
  piano::Status st = piano::GetStatus();
  Check(st.loaded && st.playing && st.song == "Test Song", "plays once loaded");
  Check(st.tracks.size() == 2 && !st.tracks[0].muted && st.tracks[1].muted, "drum track starts muted");
  Run(450);
  Check(g_events.size() > 0 && g_events[0] == "down T", "C4 pressed first, plain t");
  const int sharp = IndexOf("down T +shift");
  Check(sharp > 0 && g_events[sharp - 1] == "down Left Shift +shift" && g_events[sharp + 1] == "up Left Shift",
        "C#4 is Shift held around t");
  Check(IndexOf("up T") >= 0 && IndexOf("up T") < sharp, "C4 let go before its sharp (the same key)");
  Check(Count("down 1 +ctrl") == 0, "A0 skipped on 61 keys");
  Check(Count("down 1") == 0, "muted drums don't play");
  Check(!piano::GetStatus().playing, "stops at the end");
  std::string time = piano::Command("GET_TIME");
  Check(time.find("\"paused\":true") != std::string::npos && time.find("\"song\":\"Test Song\"") != std::string::npos &&
            time.find("\"total\":0.300") != std::string::npos,
        "GET_TIME");

  // --- 88 keys, drums on, played again from the top ---
  cfg = GetConfig();
  cfg.piano_88 = true;
  SetConfig(cfg);
  piano::SetTrack(1, false, false);
  g_events.clear();
  Check(piano::Command("PLAY_PAUSE") == "OK", "play again");
  Run(450);
  const int low = IndexOf("down 1 +ctrl");
  Check(low > 0 && g_events[low - 1] == "down Left Ctrl +ctrl" && g_events[low + 1] == "up Left Ctrl",
        "A0 is Ctrl held around 1");
  Check(Count("down 1") == 1, "the drum track plays once unmuted (C2)");
  Check(Count("up 1") == 2, "both let go");

  // --- Transpose, pausing ---
  cfg = GetConfig();
  cfg.piano_transpose = 12;
  SetConfig(cfg);
  piano::SetTrack(1, true, false);
  g_events.clear();
  piano::Restart();
  Run(50);
  Check(IndexOf("down S") == 0, "an octave up, C4 plays C5 (s)");
  piano::TogglePlay();
  Run(10);
  Check(Count("up S") == 1, "pausing lets go of held keys");
  Check(!piano::GetStatus().playing, "paused");
  const double at = piano::GetStatus().position;
  Run(100);
  Check(piano::GetStatus().position == at, "time stands still while paused");
  piano::SeekTo(0.25);
  Check(std::abs(piano::GetStatus().position - 0.25) < 0.001, "seeks");
  piano::SetSpeed(5);
  Check(piano::GetStatus().speed == 2.0, "speed tops out at 2x");
  piano::SetSpeed(1.02);
  Check(piano::GetStatus().speed == 1.0, "speed snaps to 1x");
  piano::Stop();
  Check(!piano::GetStatus().playing && piano::GetStatus().position == 0, "stop goes back to the start");

  // --- MIDI Connect: each note is * and four numpad digits (base 12) ---
  cfg = GetConfig();
  cfg.piano_transpose = 0;
  cfg.piano_output = "midiconnect";
  SetConfig(cfg);
  g_events.clear();
  piano::Restart();
  Run(450);
  std::vector<std::string> taps;
  for (const std::string& e : g_events) {
    if (e.rfind("down ", 0) == 0) taps.push_back(e.substr(5));
  }
  const std::vector<std::string> c4_on = {"Keypad *", "Keypad 5", "Keypad 0", "Keypad 8", "Keypad 4"};  // 60, 100
  const std::vector<std::string> c4_off = {"Keypad *", "Keypad 5", "Keypad 0", "Keypad 0", "Keypad 0"};  // 60, 0
  const std::vector<std::string> sharp_on = {"Keypad *", "Keypad 5", "Keypad 1", "Keypad 7", "Keypad 6"};  // 61, 90
  Check(taps.size() >= 15 && std::vector<std::string>(taps.begin(), taps.begin() + 5) == c4_on, "C4 on");
  Check(taps.size() >= 15 && std::vector<std::string>(taps.begin() + 5, taps.begin() + 10) == c4_off, "C4 off");
  Check(taps.size() >= 15 && std::vector<std::string>(taps.begin() + 10, taps.begin() + 15) == sharp_on,
        "C#4 on, with no Shift");
  Check(Count("down T") == 0 && Count("down Left Shift +shift") == 0, "no piano keys");
  Check(Count("down Keypad *") == 6, "A0 too: MIDI Connect isn't limited to 61 keys (3 notes, on and off)");
  cfg.piano_output = "keys";
  SetConfig(cfg);

  // --- Songs downloaded into Downloads move into the songs folder ---
  const std::string downloads = root + "/dl";
  std::system(("mkdir -p '" + downloads + "'").c_str());
  std::ofstream(root + "/user-dirs.dirs") << "XDG_DOWNLOAD_DIR=\"$HOME/dl\"\n";
  auto in_songs = [&](const std::string& name) {
    for (int i = 0; i < 300; ++i) {
      for (const std::string& s : piano::Songs()) {
        if (s == name) return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };
  piano::StartServer();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // Like Firefox: an empty placeholder, then the finished .part renamed over it.
  std::ofstream(downloads + "/New Song.mid").close();
  std::ofstream(downloads + "/New Song.mid.part", std::ios::binary) << Song();
  std::rename((downloads + "/New Song.mid.part").c_str(), (downloads + "/New Song.mid").c_str());
  Check(in_songs("New Song"), "a downloaded song shows up in the list");
  Check(access((songs + "/New Song.mid").c_str(), F_OK) == 0, "moved into the songs folder");
  Check(access((downloads + "/New Song.mid").c_str(), F_OK) != 0, "and out of Downloads");
  Check(piano::GetStatus().added == "New Song", "the window says so");
  std::ofstream(downloads + "/broken.mid", std::ios::binary) << Song();
  Check(in_songs("broken (2)"), "a name already taken gets a number");
  std::ofstream(downloads + "/readme.txt") << "not a song";
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  Check(access((downloads + "/readme.txt").c_str(), F_OK) == 0, "other downloads stay put");

  if (g_failures == 0) std::printf("piano_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
