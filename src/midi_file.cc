#include "midi_file.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace nac::midi {
namespace {

struct Raw {
  uint64_t tick;
  int order;  // file order, so events at one tick keep it
  int track;
  uint8_t status;
  uint8_t a, b;
  uint32_t tempo;  // set for tempo changes (status 0xFF)
};

struct Reader {
  const unsigned char* p;
  const unsigned char* end;
  bool ok = true;

  bool Has(size_t n) const { return ok && static_cast<size_t>(end - p) >= n; }
  uint8_t U8() {
    if (!Has(1)) {
      ok = false;
      return 0;
    }
    return *p++;
  }
  uint32_t Be(int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | U8();
    return v;
  }
  uint32_t Vlq() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t c = U8();
      v = (v << 7) | (c & 0x7F);
      if (!(c & 0x80)) return v;
    }
    ok = false;
    return v;
  }
  void Skip(size_t n) {
    if (!Has(n)) {
      ok = false;
      p = end;
      return;
    }
    p += n;
  }
};

bool LooksLikeDrums(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  for (const char* word : {"drum", "percussion", "ezdrummer", "bfd", "kit"}) {
    if (lower.find(word) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

std::string NoteName(int note) {
  static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (note < 0 || note > 127) return "?";
  return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

bool Parse(const std::string& data, Song* song, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  Reader r{reinterpret_cast<const unsigned char*>(data.data()),
           reinterpret_cast<const unsigned char*>(data.data()) + data.size()};
  // Some files carry a RIFF wrapper or junk before the header.
  const size_t header = data.find("MThd");
  if (header == std::string::npos) return fail("Not a MIDI file");
  r.Skip(header + 4);
  const uint32_t header_len = r.Be(4);
  if (!r.ok || header_len < 6) return fail("Not a MIDI file");
  r.Be(2);  // format: 0, 1 and 2 all read the same way here
  const int ntracks = static_cast<int>(r.Be(2));
  const uint16_t division = static_cast<uint16_t>(r.Be(2));
  r.Skip(header_len - 6);
  if (!r.ok || division == 0) return fail("Broken MIDI header");

  std::vector<Raw> raw;
  std::vector<std::string> names;
  std::vector<int> notes_on_ch10, notes_total;
  int order = 0;
  for (int t = 0; t < ntracks && r.Has(8); ++t) {
    const uint32_t id = r.Be(4);
    const uint32_t len = r.Be(4);
    if (id != 0x4D54726B) {  // "MTrk"; skip anything else
      r.Skip(len);
      --t;
      continue;
    }
    Reader tr{r.p, r.p + std::min<size_t>(len, r.end - r.p)};
    r.Skip(std::min<size_t>(len, r.end - r.p));
    names.emplace_back();
    notes_on_ch10.push_back(0);
    notes_total.push_back(0);
    uint64_t tick = 0;
    uint8_t running = 0;
    while (tr.Has(1)) {
      tick += tr.Vlq();
      uint8_t status = tr.U8();
      if (!tr.ok) break;
      if (status < 0x80) {
        if (running == 0) break;  // data with no status: give up on the track
        --tr.p;
        status = running;
      }
      if (status == 0xFF) {
        const uint8_t type = tr.U8();
        const uint32_t n = tr.Vlq();
        if (!tr.Has(n)) break;
        if (type == 0x2F) break;  // end of track
        if (type == 0x51 && n == 3) {
          raw.push_back({tick, order++, t, 0xFF, 0, 0,
                         static_cast<uint32_t>(tr.p[0] << 16 | tr.p[1] << 8 | tr.p[2])});
        } else if (type == 0x03 && names.back().empty()) {
          names.back().assign(reinterpret_cast<const char*>(tr.p), n);
        }
        tr.Skip(n);
        continue;
      }
      if (status == 0xF0 || status == 0xF7) {
        tr.Skip(tr.Vlq());
        continue;
      }
      if (status >= 0xF0) break;  // system messages don't belong in files
      running = status;
      const uint8_t kind = status & 0xF0;
      const uint8_t a = tr.U8();
      const uint8_t b = (kind == 0xC0 || kind == 0xD0) ? 0 : tr.U8();
      if (!tr.ok) break;
      if (kind == 0x90 || kind == 0x80 || (kind == 0xB0 && a == 64)) {
        raw.push_back({tick, order++, t, status, a, b, 0});
        if (kind == 0x90 && b > 0) {
          ++notes_total.back();
          if ((status & 0x0F) == 9) ++notes_on_ch10.back();
        }
      }
    }
  }
  if (names.empty()) return fail("No tracks in the file");
  std::stable_sort(raw.begin(), raw.end(), [](const Raw& x, const Raw& y) {
    return x.tick != y.tick ? x.tick < y.tick : x.order < y.order;
  });

  // Only tracks with notes are listed; the rest (tempo, lyrics) aren't.
  std::vector<int> index(names.size(), -1);
  song->tracks.clear();
  for (size_t t = 0; t < names.size(); ++t) {
    if (notes_total[t] == 0) continue;
    index[t] = static_cast<int>(song->tracks.size());
    Track track;
    track.name = names[t].empty() ? "Track " + std::to_string(song->tracks.size() + 1) : names[t];
    track.notes = notes_total[t];
    track.drums = LooksLikeDrums(names[t]) || notes_on_ch10[t] * 10 >= notes_total[t] * 8;
    song->tracks.push_back(track);
  }

  // Ticks to time: SMPTE divisions are fixed, otherwise the tempo map applies
  // (120 bpm until the first tempo change).
  const bool smpte = division & 0x8000;
  double ns_per_tick = 0;
  if (smpte) {
    const int fps = -static_cast<int8_t>(division >> 8);
    const int per_frame = division & 0xFF;
    if (fps <= 0 || per_frame == 0) return fail("Broken MIDI timing");
    ns_per_tick = 1e9 / (fps * per_frame);
  } else {
    ns_per_tick = 500000.0 * 1000.0 / division;
  }
  uint64_t last_tick = 0;
  double now = 0;

  song->events.clear();
  // Notes still sounding, per channel and key: a note off ends the latest.
  std::map<int, int> sounding;  // channel * 128 + note -> count
  for (const Raw& e : raw) {
    now += (e.tick - last_tick) * ns_per_tick;
    last_tick = e.tick;
    const int64_t ns = static_cast<int64_t>(now + 0.5);
    if (e.status == 0xFF) {
      if (!smpte && e.tempo > 0) ns_per_tick = e.tempo * 1000.0 / division;
      continue;
    }
    const uint8_t kind = e.status & 0xF0;
    const int key = (e.status & 0x0F) * 128 + e.a;
    const int track = index[e.track];
    if (kind == 0xB0) {
      // A pedal on a track of its own goes with the first track.
      song->events.push_back({ns, kSustain, e.b >= 64, e.b, std::max(track, 0)});
      continue;
    }
    if (track < 0) continue;
    if (kind == 0x90 && e.b > 0) {
      ++sounding[key];
      song->events.push_back({ns, e.a, true, e.b, track});
    } else {
      auto it = sounding.find(key);
      if (it == sounding.end() || it->second == 0) continue;
      --it->second;
      song->events.push_back({ns, e.a, false, 0, track});
    }
  }
  song->length_ns = static_cast<int64_t>(now + 0.5);
  if (song->tracks.empty()) return fail("No notes in the file");
  return true;
}

bool Load(const std::string& path, Song* song, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "Can't open the file";
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return Parse(buf.str(), song, error);
}

}  // namespace nac::midi
