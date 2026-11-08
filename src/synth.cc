#include "synth.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>

namespace nac {
namespace {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "../third_party/stb_vorbis.c"
#pragma GCC diagnostic pop

constexpr double kPi = 3.14159265358979323846;

// --------------------------------------------------------------------------
// Voices and styles

const std::vector<VoiceInfo> kVoices = {
    {"normal", "Female 1 (Normal / Sweet)", 430, 1.48, 15.0, 1.3, 0.06, 1.05},
    {"peppy", "Female 2 (Peppy)", 540, 1.58, 17.5, 2.1, 0.05, 0.95},
    {"sisterly", "Female 3 (Sisterly)", 300, 1.30, 14.5, 1.1, 0.10, 1.10},
    {"snooty", "Female 4 (Snooty)", 390, 1.40, 13.5, 0.9, 0.08, 1.10},
    {"jock", "Male 1 (Jock)", 235, 1.22, 16.5, 1.7, 0.06, 1.05},
    {"lazy", "Male 2 (Lazy)", 205, 1.18, 11.5, 0.8, 0.07, 1.20},
    {"smug", "Male 3 (Smug)", 215, 1.22, 13.5, 1.0, 0.06, 1.10},
    {"cranky", "Male 4 (Cranky)", 105, 1.08, 12.0, 0.7, 0.09, 1.30},
};

const std::vector<StyleInfo> kStyles = {
    {"classic", "Classic"},
};

// --------------------------------------------------------------------------
// Phones. Formants are adult reference values (Hz); the voice's
// formant_scale lifts them toward a small animal's vocal tract.

struct F3 {
  double f[3];
};

struct Vowel {
  F3 start;
  bool glides;
  F3 glide;
};

const std::map<std::string, Vowel> kVowels = {
    {"i", {{{280, 2250, 2900}}, false, {}}},
    {"I", {{{400, 1950, 2550}}, false, {}}},
    {"e", {{{450, 2050, 2650}}, true, {{330, 2200, 2800}}}},  // "ay"
    {"E", {{{550, 1800, 2500}}, false, {}}},                  // "eh"
    {"ae", {{{680, 1700, 2400}}, false, {}}},                 // "a" in cat
    {"a", {{{750, 1150, 2450}}, false, {}}},                  // "ah"
    {"A", {{{640, 1200, 2400}}, false, {}}},                  // "uh"
    {"o", {{{520, 900, 2400}}, true, {{420, 980, 2300}}}},    // "oh"
    {"u", {{{320, 900, 2250}}, false, {}}},
    {"U", {{{430, 1050, 2300}}, false, {}}},
    {"ai", {{{730, 1250, 2500}}, true, {{400, 1950, 2600}}}},
    {"au", {{{730, 1200, 2450}}, true, {{420, 950, 2300}}}},
    {"oi", {{{520, 900, 2400}}, true, {{400, 1950, 2600}}}},
    {"@", {{{520, 1450, 2500}}, false, {}}},  // schwa
};

enum class Kind { kStop, kFric, kAffric, kNasal, kApprox, kAspir };

struct Consonant {
  Kind kind;
  bool voiced;
  double band_lo, band_hi;  // noise band for bursts / frication
  F3 locus;
  double noise;  // noise level relative to the vowel
  double ms;     // nominal length at normal speed
};

const std::map<std::string, Consonant> kConsonants = {
    {"b", {Kind::kStop, true, 200, 1600, {{250, 900, 2300}}, 0.35, 35}},
    {"p", {Kind::kStop, false, 200, 1600, {{250, 900, 2300}}, 0.55, 35}},
    {"d", {Kind::kStop, true, 2500, 6500, {{250, 1700, 2700}}, 0.35, 35}},
    {"t", {Kind::kStop, false, 3000, 8000, {{250, 1700, 2700}}, 0.6, 35}},
    {"g", {Kind::kStop, true, 1300, 3200, {{250, 2100, 2400}}, 0.35, 35}},
    {"k", {Kind::kStop, false, 1300, 3600, {{250, 2100, 2400}}, 0.6, 35}},
    {"s", {Kind::kFric, false, 4500, 11000, {{300, 1700, 2700}}, 0.55, 45}},
    {"z", {Kind::kFric, true, 4500, 11000, {{300, 1700, 2700}}, 0.35, 40}},
    {"S", {Kind::kFric, false, 2000, 6500, {{300, 1900, 2500}}, 0.55, 45}},
    {"Z", {Kind::kFric, true, 2000, 6500, {{300, 1900, 2500}}, 0.35, 40}},
    {"f", {Kind::kFric, false, 1200, 9000, {{300, 1000, 2300}}, 0.3, 35}},
    {"v", {Kind::kFric, true, 1200, 9000, {{300, 1000, 2300}}, 0.2, 30}},
    {"T", {Kind::kFric, false, 1500, 9000, {{300, 1500, 2600}}, 0.22, 35}},
    {"D", {Kind::kFric, true, 1500, 9000, {{300, 1500, 2600}}, 0.15, 25}},
    {"h", {Kind::kAspir, false, 0, 0, {{300, 1500, 2500}}, 0.32, 30}},
    {"tS", {Kind::kAffric, false, 2000, 6500, {{300, 1900, 2500}}, 0.55, 45}},
    {"dZ", {Kind::kAffric, true, 2000, 6500, {{300, 1900, 2500}}, 0.4, 40}},
    {"m", {Kind::kNasal, true, 0, 0, {{250, 1000, 2200}}, 0.5, 35}},
    {"n", {Kind::kNasal, true, 0, 0, {{250, 1500, 2500}}, 0.5, 35}},
    {"N", {Kind::kNasal, true, 0, 0, {{250, 2000, 2500}}, 0.5, 35}},
    {"l", {Kind::kApprox, true, 0, 0, {{360, 1050, 2800}}, 0.5, 30}},
    {"r", {Kind::kApprox, true, 0, 0, {{420, 1250, 1650}}, 0.5, 30}},
    {"w", {Kind::kApprox, true, 0, 0, {{300, 650, 2200}}, 0.5, 30}},
    {"y", {Kind::kApprox, true, 0, 0, {{270, 2250, 3000}}, 0.5, 28}},
};

struct Syllable {
  std::vector<std::string> onset;
  std::string vowel;
  std::vector<std::string> coda;
};

// Classic Animalese plays each letter as its clipped letter name.
const std::map<char, Syllable> kLetters = {
    {'a', {{}, "e", {}}},       {'b', {{"b"}, "i", {}}},
    {'c', {{"s"}, "i", {}}},    {'d', {{"d"}, "i", {}}},
    {'e', {{}, "i", {}}},       {'f', {{}, "E", {"f"}}},
    {'g', {{"dZ"}, "i", {}}},   {'h', {{}, "e", {"tS"}}},
    {'i', {{}, "ai", {}}},      {'j', {{"dZ"}, "e", {}}},
    {'k', {{"k"}, "e", {}}},    {'l', {{}, "E", {"l"}}},
    {'m', {{}, "E", {"m"}}},    {'n', {{}, "E", {"n"}}},
    {'o', {{}, "o", {}}},       {'p', {{"p"}, "i", {}}},
    {'q', {{"k", "y"}, "u", {}}}, {'r', {{}, "a", {"r"}}},
    {'s', {{}, "E", {"s"}}},    {'t', {{"t"}, "i", {}}},
    {'u', {{"y"}, "u", {}}},    {'v', {{"v"}, "i", {}}},
    {'w', {{"d"}, "A", {}}},    {'x', {{}, "E", {"k", "s"}}},
    {'y', {{"w"}, "ai", {}}},   {'z', {{"z"}, "i", {}}},
};

// --------------------------------------------------------------------------
// Utterance plan

struct Unit {
  bool pause = false;
  Syllable syllable;
  double slots = 1.0;  // length in text-box character slots
  double pitch = 0.0;  // semitones relative to the voice
  double glide = 0.0;  // pitch bend across the blip, semitones
  double gain = 1.0;
  char letter = 'e';   // source letter, for sample packs
};

double PauseSlots(char c) {
  switch (c) {
    case ',': case ';': case ':': return 3.0;
    case '-': return 1.0;
    case '.': case '!': case '?': case '\n': return 5.0;
    default: return -1.0;
  }
}

struct Word {
  std::string text;
  bool shout = false;
  double pause = -1.0;  // >= 0 marks a comma-style pause instead of a word
};

struct Sentence {
  std::vector<Word> words;
  char end = 0;  // '.', '!', '?', or 0 when the message just stops
  double pause = 0.0;
};

std::vector<Sentence> Sentences(const std::string& text) {
  std::vector<Sentence> sentences;
  std::vector<Word> words;
  std::string word;
  auto flush_word = [&]() {
    if (word.empty()) return;
    bool upper = word.size() >= 2;
    for (char c : word) upper = upper && std::isupper(static_cast<unsigned char>(c));
    words.push_back({word, upper, -1.0});
    word.clear();
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (std::isalpha(static_cast<unsigned char>(c))) {
      word += c;
    } else if (c == ' ') {
      flush_word();
    } else if (PauseSlots(c) >= 0) {
      flush_word();
      std::string run(1, c);
      while (i + 1 < text.size() && PauseSlots(text[i + 1]) >= 0) {
        run += text[++i];
      }
      char end = 0;
      if (run.find('?') != std::string::npos) end = '?';
      else if (run.find('!') != std::string::npos) end = '!';
      else if (run.find_first_of(".\n") != std::string::npos) end = '.';
      double pause = 0.0;
      for (char r : run) pause = std::max(pause, PauseSlots(r));
      if (std::count(run.begin(), run.end(), '.') >= 3) pause = 9.0;  // "..."
      if (end) {
        sentences.push_back({words, end, pause});
        words.clear();
      } else if (!words.empty()) {
        words.push_back({"", false, pause});
      }
    }
  }
  flush_word();
  if (!words.empty()) sentences.push_back({words, 0, 0.0});
  return sentences;
}

void Prosody(const Sentence& sentence, std::vector<std::pair<Unit, int>>& items,
             const std::string& style, const VoiceInfo& voice, std::mt19937& rng) {
  std::vector<std::pair<Unit*, int>> voiced;
  for (auto& [unit, wi] : items) {
    if (!unit.pause) voiced.push_back({&unit, wi});
  }
  if (voiced.empty()) return;
  const size_t n = voiced.size();
  const int last_word = voiced.back().second;
  std::vector<Unit*> tail;
  for (auto& [u, wi] : voiced) {
    if (wi == last_word) tail.push_back(u);
  }
  std::uniform_real_distribution<double> jitter_dist(-voice.jitter, voice.jitter);
  std::uniform_real_distribution<double> glide_dist(-0.6, 0.3);
  for (size_t idx = 0; idx < n; ++idx) {
    Unit* u = voiced[idx].first;
    const int wi = voiced[idx].second;
    const double jitter = jitter_dist(rng);
    if (style == "nh") {
      // Gentle declination over the sentence, smaller random wobble.
      const double pos = static_cast<double>(idx) / std::max<size_t>(1, n - 1);
      u->pitch = 1.2 - 2.4 * pos + 0.6 * jitter;
      u->glide = glide_dist(rng);
    } else {
      u->pitch = jitter;
      u->glide = -0.5;  // clipped letter samples droop slightly
    }
    if (sentence.words[wi].shout) {
      u->pitch += 2.5;
      u->gain *= 1.3;
    }
    if (sentence.end == '!') {
      u->pitch += 1.8;
      u->gain *= 1.25;
    }
  }
  if (sentence.end == '?') {
    // Questions climb over the last word and bend up at the very end.
    for (size_t k = 0; k < tail.size(); ++k) {
      tail[k]->pitch += 1.5 + 3.5 * static_cast<double>(k + 1) / tail.size();
    }
    tail.back()->glide = style == "nh" ? 4.0 : 2.0;
    if (style == "nh") tail.back()->slots *= 1.35;
  } else if (style == "nh") {
    tail.back()->glide = sentence.end != '!' ? -2.5 : -1.0;
    tail.back()->slots *= 1.25;
  }
}

std::vector<Unit> Plan(const std::string& text, const std::string& style,
                       const VoiceInfo& voice, std::mt19937& rng) {
  std::vector<Unit> units;
  for (const Sentence& sentence : Sentences(Normalize(text))) {
    std::vector<std::pair<Unit, int>> items;  // unit, word index
    for (size_t wi = 0; wi < sentence.words.size(); ++wi) {
      const Word& word = sentence.words[wi];
      if (word.pause >= 0) {
        Unit pause;
        pause.pause = true;
        pause.slots = word.pause;
        items.push_back({pause, static_cast<int>(wi)});
        continue;
      }
      if (!items.empty() && !items.back().first.pause) {
        Unit gap;
        gap.pause = true;
        gap.slots = 0.8;
        items.push_back({gap, -1});
      }
      for (char ch : word.text) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        auto found = kLetters.find(lower);
        if (found == kLetters.end()) continue;
        Unit u;
        u.syllable = found->second;
        u.letter = lower;
        items.push_back({u, static_cast<int>(wi)});
      }
    }
    Prosody(sentence, items, style, voice, rng);
    for (auto& item : items) units.push_back(item.first);
    if (sentence.pause > 0) {
      Unit pause;
      pause.pause = true;
      pause.slots = sentence.pause;
      units.push_back(pause);
    }
  }
  while (!units.empty() && units.back().pause) units.pop_back();
  return units;
}

// --------------------------------------------------------------------------
// Spectral helpers

using Complex = std::complex<double>;

void Fft(std::vector<Complex>& a, bool inverse) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = 2 * kPi / static_cast<double>(len) * (inverse ? 1 : -1);
    const Complex wlen(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      Complex w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const Complex u = a[i + j];
        const Complex v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
  if (inverse) {
    for (Complex& x : a) x /= static_cast<double>(n);
  }
}

size_t NextPow2(size_t n) {
  size_t size = 1;
  while (size < n) size <<= 1;
  return size;
}

// Applies a real, zero-phase magnitude response to x (length kept).
void ShapeSpectrum(std::vector<double>& x, size_t size,
                   const std::function<double(double)>& shape) {
  std::vector<Complex> spectrum(size);
  for (size_t i = 0; i < x.size() && i < size; ++i) spectrum[i] = x[i];
  Fft(spectrum, false);
  for (size_t k = 0; k <= size / 2; ++k) {
    const double gain = shape(static_cast<double>(k) * kSampleRate / size);
    spectrum[k] *= gain;
    if (k != 0 && k != size / 2) spectrum[size - k] *= gain;
  }
  Fft(spectrum, true);
  for (size_t i = 0; i < x.size(); ++i) x[i] = spectrum[i].real();
}

double Resonance(double f, double fc, double bw) {
  const double b = bw / 2.0;
  const double num = fc * fc + b * b;
  const double den = std::sqrt(((f - fc) * (f - fc) + b * b) * ((f + fc) * (f + fc) + b * b));
  return num / den;
}

double FormantGain(double f, const double formants[3], double scale) {
  const double bw = std::sqrt(scale);
  return Resonance(f, formants[0], 80.0 * bw) * Resonance(f, formants[1], 110.0 * bw) *
         Resonance(f, formants[2], 170.0 * bw) *
         Resonance(f, 3500.0 * scale, 250.0 * bw) *
         Resonance(f, 4600.0 * scale, 320.0 * bw);
}

std::vector<double> ShapedNoise(size_t n, std::mt19937& rng,
                                const std::function<double(double)>& shape) {
  if (n == 0) return {};
  const size_t size = std::max<size_t>(64, NextPow2(n));
  std::normal_distribution<double> normal;
  std::vector<double> noise(size);
  for (double& v : noise) v = normal(rng);
  ShapeSpectrum(noise, size, shape);
  noise.resize(n);
  double sum = 0;
  for (double v : noise) sum += v * v;
  const double rms = std::sqrt(sum / n);
  if (rms > 0) {
    for (double& v : noise) v /= rms;
  }
  return noise;
}

std::function<double(double)> Band(double lo, double hi) {
  return [lo, hi](double f) {
    const double edge = 0.25;
    const double lo_ramp = std::clamp((f - lo * (1 - edge)) / (lo * edge * 2 + 1), 0.0, 1.0);
    const double hi_ramp = std::clamp((hi * (1 + edge) - f) / (hi * edge * 2 + 1), 0.0, 1.0);
    return lo_ramp * hi_ramp;
  };
}

// --------------------------------------------------------------------------
// One blip

struct Key {
  double time;
  F3 formants;
  double voice;
};

struct NoiseEvent {
  double start, length;
  bool band;  // band-limited burst/frication; otherwise formant-shaped
  const Consonant* consonant;
  F3 target;
};

// Lays out consonant/vowel timing. rate < 1 compresses consonants so short
// blips stay mostly vowel, which keeps Animalese clipped rather than slurred.
double BlipTracks(const Syllable& syl, double dur, double rate,
                  std::vector<Key>& keys, std::vector<NoiseEvent>& noise) {
  const Vowel& vowel = kVowels.at(syl.vowel);
  const F3 v_start = vowel.start;
  const F3 v_end = vowel.glides ? vowel.glide : vowel.start;
  F3 start_f = v_start;
  if (!syl.onset.empty() && kConsonants.at(syl.onset[0]).kind != Kind::kAspir) {
    start_f = kConsonants.at(syl.onset[0]).locus;
  }
  keys.push_back({0.0, start_f, 0.0});
  auto at = [&](double time, const F3& f, double voice) { keys.push_back({time, f, voice}); };
  double t = 0.0;
  for (const std::string& name : syl.onset) {
    const Consonant& c = kConsonants.at(name);
    const double length = c.ms / 1000.0 * rate;
    F3 mix;
    for (int i = 0; i < 3; ++i) mix.f[i] = 0.55 * c.locus.f[i] + 0.45 * v_start.f[i];
    switch (c.kind) {
      case Kind::kStop: {
        const double closure = 0.006 * rate;
        at(t + 0.002, c.locus, c.voiced ? 0.12 : 0.0);
        at(t + closure, c.locus, c.voiced ? 0.12 : 0.0);
        const double burst = 0.006 * rate;
        noise.push_back({t + closure, burst, true, &c, v_start});
        t += closure + burst;
        if (!c.voiced) {
          const double asp = 0.016 * rate;
          noise.push_back({t, asp, false, &c, mix});
          at(t, mix, 0.0);
          t += asp;
        }
        at(t + 0.004, mix, 1.0);
        t += 0.004;
        break;
      }
      case Kind::kFric:
      case Kind::kAffric: {
        if (c.kind == Kind::kAffric) {
          const double closure = 0.006 * rate;
          at(t + closure, c.locus, c.voiced ? 0.1 : 0.0);
          t += closure;
        }
        at(t + 0.003, c.locus, c.voiced ? 0.35 : 0.0);
        noise.push_back({t, length, true, &c, v_start});
        t += length;
        at(t, c.locus, c.voiced ? 0.35 : 0.0);
        at(t + 0.004, mix, 1.0);
        t += 0.004;
        break;
      }
      case Kind::kAspir:
        noise.push_back({t, length, false, &c, v_start});
        at(t + length, v_start, 0.0);
        t += length;
        at(t + 0.004, v_start, 1.0);
        t += 0.004;
        break;
      default: {  // nasal / approximant: voiced, formants glide into the vowel
        const double level = c.kind == Kind::kNasal ? 0.55 : 0.75;
        at(t + 0.004, c.locus, level);
        at(t + length, c.locus, level);
        t += length;
        break;
      }
    }
  }
  double coda_len = 0;
  for (const std::string& name : syl.coda) coda_len += kConsonants.at(name).ms;
  coda_len = std::min(coda_len / 1000.0 * rate * 0.6, 0.35 * dur);
  const double v_len = std::max(0.02, dur - t - coda_len);
  at(t + std::min(0.025, v_len * 0.4), v_start, 1.0);
  at(t + v_len, v_end, 1.0);
  t += v_len;
  for (const std::string& name : syl.coda) {
    const Consonant& c = kConsonants.at(name);
    const double length = coda_len / syl.coda.size();
    if (c.kind == Kind::kNasal || c.kind == Kind::kApprox) {
      at(t + length, c.locus, 0.6);
    } else {
      const bool aspir = c.kind == Kind::kAspir;
      at(t + 0.005, aspir ? v_end : c.locus, c.voiced ? 0.3 : 0.0);
      noise.push_back({t, length, !aspir, &c, v_end});
    }
    t += length;
  }
  at(t + 0.004, keys.back().formants, 0.0);
  return t + 0.004;
}

double Interp(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
  if (x <= xs.front()) return ys.front();
  if (x >= xs.back()) return ys.back();
  const size_t hi = std::upper_bound(xs.begin(), xs.end(), x) - xs.begin();
  const size_t lo = hi - 1;
  const double span = xs[hi] - xs[lo];
  if (span <= 0) return ys[hi];
  return ys[lo] + (ys[hi] - ys[lo]) * (x - xs[lo]) / span;
}

std::vector<double> RenderBlip(const Syllable& syl, double dur, double f0, double glide,
                               const VoiceInfo& voice, double gain, std::mt19937& rng,
                               double scale_mult) {
  const double rate = std::min(1.0, std::max(0.45, dur / 0.085));
  std::vector<Key> keys;
  std::vector<NoiseEvent> noise_events;
  const double length = BlipTracks(syl, dur, rate, keys, noise_events);
  const size_t n = static_cast<size_t>(length * kSampleRate);
  if (n == 0) return {};
  std::stable_sort(keys.begin(), keys.end(),
                   [](const Key& a, const Key& b) { return a.time < b.time; });
  const double scale = voice.formant_scale * scale_mult;
  std::vector<double> times, fs[3], amps;
  for (const Key& k : keys) {
    times.push_back(k.time);
    for (int i = 0; i < 3; ++i) fs[i].push_back(k.formants.f[i] * scale);
    amps.push_back(k.voice);
  }

  std::vector<double> form[3], amp(n), f0_t(n);
  for (int i = 0; i < 3; ++i) form[i].resize(n);
  std::uniform_real_distribution<double> vib_phase(0, 6.28);
  const double vib = vib_phase(rng);
  double f0_min = 1e9;
  for (size_t s = 0; s < n; ++s) {
    const double t = static_cast<double>(s) / kSampleRate;
    for (int i = 0; i < 3; ++i) form[i][s] = Interp(times, fs[i], t);
    amp[s] = Interp(times, amps, t);
    f0_t[s] = f0 * std::pow(2.0, glide * (t / std::max(length, 1e-6)) / 12.0) *
              (1.0 + 0.004 * std::sin(2 * kPi * 5.5 * t + vib));
    f0_min = std::min(f0_min, f0_t[s]);
  }

  // Voice: harmonic sum with a formant-shaped spectrum. The spectral
  // envelope moves slowly, so harmonic gains are refreshed every few samples.
  const int nh = static_cast<int>(std::min(80.0, 10500.0 / f0_min));
  std::vector<double> voiced(n), harm_gain(nh + 1);
  double phase = 0;
  constexpr size_t kEnvelopeStep = 8;
  for (size_t s = 0; s < n; ++s) {
    phase += 2 * kPi * f0_t[s] / kSampleRate;
    if (s % kEnvelopeStep == 0) {
      const double f[3] = {form[0][s], form[1][s], form[2][s]};
      for (int k = 1; k <= nh; ++k) {
        const double freq = k * f0_t[s];
        harm_gain[k] = freq < 0.45 * kSampleRate
                           ? FormantGain(freq, f, scale) / std::pow(k, voice.tilt)
                           : 0.0;
      }
    }
    double sum = 0;
    for (int k = 1; k <= nh; ++k) sum += harm_gain[k] * std::sin(k * phase);
    voiced[s] = sum;
  }
  double loud_sum = 0;
  size_t loud = 0;
  for (size_t s = 0; s < n; ++s) {
    if (amp[s] > 0.5) {
      loud_sum += voiced[s] * voiced[s];
      ++loud;
    }
  }
  double ref = loud ? std::sqrt(loud_sum / loud) : 1.0;
  if (ref == 0) ref = 1.0;
  std::vector<double> out(n);
  for (size_t s = 0; s < n; ++s) out[s] = voiced[s] / ref * amp[s] * 0.25;

  if (voice.breath > 0) {
    const double mid[3] = {form[0][n / 2], form[1][n / 2], form[2][n / 2]};
    const std::vector<double> breath = ShapedNoise(n, rng, [&](double f) {
      return f > 400 ? FormantGain(f, mid, scale) : 0.0;
    });
    for (size_t s = 0; s < n; ++s) out[s] += breath[s] * amp[s] * 0.25 * voice.breath;
  }

  for (const NoiseEvent& ev : noise_events) {
    const size_t i0 = static_cast<size_t>(ev.start * kSampleRate);
    if (i0 >= n) continue;
    size_t m = std::max<size_t>(1, static_cast<size_t>(ev.length * kSampleRate));
    m = std::min(m, n - i0);
    std::function<double(double)> shape;
    if (ev.band) {
      const double s = std::min(scale, 1.25);
      shape = Band(ev.consonant->band_lo * s,
                   std::min(ev.consonant->band_hi * s, 0.47 * kSampleRate));
    } else {
      const double tgt[3] = {ev.target.f[0] * scale, ev.target.f[1] * scale,
                             ev.target.f[2] * scale};
      shape = [tgt, scale](double f) { return f > 300 ? FormantGain(f, tgt, scale) : 0.0; };
    }
    const std::vector<double> seg = ShapedNoise(m, rng, shape);
    const size_t ramp = std::min(m / 2, static_cast<size_t>(0.003 * kSampleRate));
    for (size_t j = 0; j < m; ++j) {
      double env = 1.0;
      if (ramp > 0 && j < ramp) env = static_cast<double>(j) / (ramp - 1 ? ramp - 1 : 1);
      if (ramp > 0 && j >= m - ramp) {
        env = static_cast<double>(m - 1 - j) / (ramp - 1 ? ramp - 1 : 1);
      }
      out[i0 + j] += seg[j] * env * 0.25 * ev.consonant->noise;
    }
  }

  const size_t fade_in = std::min(n, static_cast<size_t>(0.003 * kSampleRate));
  const size_t fade_out = std::min(n, static_cast<size_t>(0.010 * kSampleRate));
  for (size_t j = 0; j < fade_in; ++j) out[j] *= static_cast<double>(j) / std::max<size_t>(1, fade_in - 1);
  for (size_t j = 0; j < fade_out; ++j) {
    const double x = 1.0 - static_cast<double>(j) / std::max<size_t>(1, fade_out - 1);
    out[n - fade_out + j] *= std::pow(x, 1.5);
  }
  for (double& v : out) v *= gain;
  return out;
}

// --------------------------------------------------------------------------
// WAV reading for sample packs

uint32_t ReadLe(const unsigned char* p, int bytes) {
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

bool ReadWav(const std::string& path, std::vector<double>* out, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
      std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
    *error = path + " is not a WAV file";
    return false;
  }
  int channels = 0, rate = 0, bits = 0, format = 0;
  const unsigned char* pcm = nullptr;
  size_t pcm_size = 0;
  for (size_t pos = 12; pos + 8 <= data.size();) {
    const size_t size = ReadLe(&data[pos + 4], 4);
    if (pos + 8 + size > data.size()) break;
    if (std::memcmp(&data[pos], "fmt ", 4) == 0 && size >= 16) {
      format = static_cast<int>(ReadLe(&data[pos + 8], 2));
      channels = static_cast<int>(ReadLe(&data[pos + 10], 2));
      rate = static_cast<int>(ReadLe(&data[pos + 12], 4));
      bits = static_cast<int>(ReadLe(&data[pos + 22], 2));
    } else if (std::memcmp(&data[pos], "data", 4) == 0) {
      pcm = &data[pos + 8];
      pcm_size = size;
    }
    pos += 8 + size + (size & 1);
  }
  if (pcm == nullptr || channels <= 0 || rate <= 0 ||
      (format != 1 && format != 0xFFFE) || bits % 8 != 0 || bits < 8 || bits > 32) {
    *error = path + ": unsupported WAV (needs integer PCM)";
    return false;
  }
  const int bytes = bits / 8;
  const size_t frames = pcm_size / (bytes * channels);
  std::vector<double> mono(frames);
  for (size_t f = 0; f < frames; ++f) {
    double sum = 0;
    for (int c = 0; c < channels; ++c) {
      const unsigned char* p = pcm + (f * channels + c) * bytes;
      double v;
      if (bytes == 1) {
        v = (static_cast<double>(p[0]) - 128) / 128.0;
      } else {
        const uint32_t raw = ReadLe(p, bytes);
        const int shift = 32 - bits;
        v = static_cast<double>(static_cast<int32_t>(raw << shift) >> shift) /
            static_cast<double>(1u << (bits - 1));
      }
      sum += v;
    }
    mono[f] = sum / channels;
  }
  if (rate != kSampleRate && frames > 1) {
    const size_t n = static_cast<size_t>(frames * static_cast<double>(kSampleRate) / rate);
    out->resize(n);
    for (size_t i = 0; i < n; ++i) {
      const double pos = i * static_cast<double>(rate) / kSampleRate;
      const size_t lo = std::min(static_cast<size_t>(pos), frames - 1);
      const size_t hi = std::min(lo + 1, frames - 1);
      (*out)[i] = mono[lo] + (mono[hi] - mono[lo]) * (pos - lo);
    }
  } else {
    *out = std::move(mono);
  }
  return true;
}

class AnimaleseBank {
 public:
  bool Load(const std::string& path) {
    samples_.clear();
    short* raw = nullptr;
    int channels = 0, sample_rate = 0;
    int num_samples = stb_vorbis_decode_filename(path.c_str(), &channels, &sample_rate, &raw);
    if (num_samples <= 0 || raw == nullptr) return false;
    samples_.resize(num_samples);
    for (int i = 0; i < num_samples; ++i) {
      if (channels == 2) {
        samples_[i] = static_cast<float>(((static_cast<double>(raw[i * 2]) +
                                           static_cast<double>(raw[i * 2 + 1])) *
                                          0.5) / 32768.0);
      } else {
        samples_[i] = static_cast<float>(static_cast<double>(raw[i]) / 32768.0);
      }
    }
    free(raw);
    return true;
  }

  bool empty() const { return samples_.empty(); }

  std::vector<double> BlipSlice(int slice_idx, double dur, double rate, double gain) const {
    if (samples_.empty()) return {};
    if (slice_idx < 0 || slice_idx > 38) slice_idx = 4;
    const size_t start = static_cast<size_t>(slice_idx * 9600);
    size_t slice_len = (slice_idx >= 36) ? 28800 : 9600;
    if (start >= samples_.size()) return {};
    if (start + slice_len > samples_.size()) slice_len = samples_.size() - start;

    const size_t n = static_cast<size_t>(dur * kSampleRate);
    std::vector<double> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const double pos = i * rate;
      if (pos >= static_cast<double>(slice_len - 1)) break;
      const size_t lo = static_cast<size_t>(pos);
      const double frac = pos - lo;
      const double s0 = samples_[start + lo];
      const double s1 = samples_[start + lo + 1];
      out.push_back(s0 * (1.0 - frac) + s1 * frac);
    }
    const size_t fade = std::min(out.size(), static_cast<size_t>(0.008 * kSampleRate));
    for (size_t j = 0; j < fade; ++j) {
      out[out.size() - fade + j] *= 1.0 - static_cast<double>(j) / std::max<size_t>(1, fade - 1);
    }
    for (double& v : out) v *= gain * 0.9;
    return out;
  }

  std::vector<double> Blip(char c, double dur, double rate, double gain) const {
    int slice = 4;
    if (c >= 'a' && c <= 'z') slice = c - 'a';
    else if (c >= 'A' && c <= 'Z') slice = c - 'A';
    else if (c >= '1' && c <= '9') slice = 26 + (c - '1');
    else if (c == '0') slice = 35;
    else if (c == '!') slice = 37;
    else if (c == '?') slice = 38;
    return BlipSlice(slice, dur, rate, gain);
  }

 private:
  std::vector<float> samples_;
};

std::string VoiceToFilename(const std::string& voice) {
  if (voice == "peppy" || voice == "f2") return "f2.ogg";
  if (voice == "sisterly" || voice == "f3") return "f3.ogg";
  if (voice == "snooty" || voice == "f4") return "f4.ogg";
  if (voice == "jock" || voice == "m1") return "m1.ogg";
  if (voice == "lazy" || voice == "m2") return "m2.ogg";
  if (voice == "smug" || voice == "m3") return "m3.ogg";
  if (voice == "cranky" || voice == "m4") return "m4.ogg";
  return "f1.ogg";
}

bool BankFileExists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string ResolveAnimalesePath(const std::string& lang_in, const std::string& voice) {
  const std::string lang = (lang_in == "korean" || lang_in == "ko") ? "korean" : "english";
  const std::string file = VoiceToFilename(voice);
  const char* home_env = std::getenv("HOME");
  const std::string home = home_env ? home_env : "";
  const std::string candidates[] = {
      "assets/voices/animalese/" + lang + "/" + file,
      "../assets/voices/animalese/" + lang + "/" + file,
      "/usr/local/share/vox-menu/voices/animalese/" + lang + "/" + file,
      "/usr/share/vox-menu/voices/animalese/" + lang + "/" + file,
      home + "/.local/share/vox-menu/voices/animalese/" + lang + "/" + file,
      "/usr/local/share/vox-tts/voices/animalese/" + lang + "/" + file,
      "/usr/share/vox-tts/voices/animalese/" + lang + "/" + file,
      home + "/.local/share/vox-tts/voices/animalese/" + lang + "/" + file,
  };
  for (const auto& c : candidates) {
    if (BankFileExists(c)) return c;
  }
  return "";
}

std::mutex g_bank_mutex;
std::map<std::pair<std::string, std::string>, AnimaleseBank> g_bank_cache;

const AnimaleseBank* GetAnimaleseBank(const std::string& lang, const std::string& voice) {
  std::lock_guard<std::mutex> lock(g_bank_mutex);
  const std::string norm_lang = (lang == "korean" || lang == "ko") ? "korean" : "english";
  const std::string file = VoiceToFilename(voice);
  auto key = std::make_pair(norm_lang, file);
  auto found = g_bank_cache.find(key);
  if (found != g_bank_cache.end()) return &found->second;

  const std::string path = ResolveAnimalesePath(norm_lang, voice);
  if (path.empty()) return nullptr;

  AnimaleseBank bank;
  if (!bank.Load(path)) return nullptr;

  auto res = g_bank_cache.emplace(key, std::move(bank));
  return &res.first->second;
}

}  // namespace

const std::vector<VoiceInfo>& Voices() { return kVoices; }
const std::vector<StyleInfo>& Styles() { return kStyles; }

const VoiceInfo& FindVoice(const std::string& key) {
  if (key == "f1") return kVoices[0];
  if (key == "f2") return kVoices[1];
  if (key == "f3") return kVoices[2];
  if (key == "f4") return kVoices[3];
  if (key == "m1") return kVoices[4];
  if (key == "m2") return kVoices[5];
  if (key == "m3") return kVoices[6];
  if (key == "m4") return kVoices[7];
  for (const VoiceInfo& v : kVoices) {
    if (key == v.key) return v;
  }
  return kVoices.front();
}

bool SamplePack::Load(const std::string& path, std::string* error) {
  letters_.clear();
  std::string dir = path;
  if (!dir.empty() && dir[0] == '~') {
    const char* home = std::getenv("HOME");
    dir = std::string(home ? home : "") + dir.substr(1);
  }
  std::ifstream probe(dir + "/a.wav");
  if (probe.good()) {
    for (char c = 'a'; c <= 'z'; ++c) {
      std::vector<double> samples;
      std::string ignored;
      if (ReadWav(dir + "/" + c + ".wav", &samples, &ignored)) letters_[c] = samples;
    }
  } else {
    std::vector<double> all;
    if (!ReadWav(dir, &all, error)) return false;
    const size_t size = all.size() / 26;
    for (int i = 0; i < 26 && size > 0; ++i) {
      letters_['a' + i] = std::vector<double>(all.begin() + i * size, all.begin() + (i + 1) * size);
    }
  }
  if (letters_.empty()) {
    *error = "no letter samples found in " + path;
    return false;
  }
  double peak = 0;
  for (const auto& [letter, s] : letters_) {
    for (double v : s) peak = std::max(peak, std::fabs(v));
  }
  if (peak > 0) {
    for (auto& [letter, s] : letters_) {
      for (double& v : s) v /= peak;
    }
  }
  return true;
}

std::vector<double> SamplePack::Blip(char letter, double dur, double rate, double gain) const {
  auto found = letters_.find(letter);
  if (found == letters_.end()) found = letters_.find('e');
  if (found == letters_.end()) found = letters_.begin();
  const std::vector<double>& sample = found->second;
  const size_t n = static_cast<size_t>(dur * kSampleRate);
  std::vector<double> out;
  for (size_t i = 0; i < n; ++i) {
    const double pos = i * rate;
    if (pos >= static_cast<double>(sample.size()) - 1) break;
    const size_t lo = static_cast<size_t>(pos);
    out.push_back(sample[lo] + (sample[lo + 1] - sample[lo]) * (pos - lo));
  }
  const size_t fade = std::min(out.size(), static_cast<size_t>(0.008 * kSampleRate));
  for (size_t j = 0; j < fade; ++j) {
    out[out.size() - fade + j] *= 1.0 - static_cast<double>(j) / std::max<size_t>(1, fade - 1);
  }
  for (double& v : out) v *= gain * 0.8;
  return out;
}

// --------------------------------------------------------------------------
// Text normalization

namespace {

const char* kOnes[] = {"zero", "one", "two", "three", "four", "five", "six",
                       "seven", "eight", "nine", "ten", "eleven", "twelve",
                       "thirteen", "fourteen", "fifteen", "sixteen", "seventeen",
                       "eighteen", "nineteen"};
const char* kTens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty",
                       "seventy", "eighty", "ninety"};

std::string NumberToWords(uint64_t n) {
  if (n < 20) return kOnes[n];
  if (n < 100) {
    std::string s = kTens[n / 10];
    if (n % 10) s += std::string(" ") + kOnes[n % 10];
    return s;
  }
  const std::pair<uint64_t, const char*> scales[] = {
      {1000000000, "billion"}, {1000000, "million"}, {1000, "thousand"}, {100, "hundred"}};
  for (const auto& [value, name] : scales) {
    if (n >= value) {
      std::string s = NumberToWords(n / value) + " " + name;
      if (n % value) s += " " + NumberToWords(n % value);
      return s;
    }
  }
  return "";
}

// Base letter for accented Latin letters (the NFKD + strip-combining step).
const char* Unaccent(uint32_t cp) {
  static const char* latin1 =  // U+00C0 .. U+00FF
      "AAAAAAACEEEEIIII"
      "DNOOOOO*OUUUUYTs"
      "aaaaaaaceeeeiiii"
      "dnooooo/ouuuuyty";
  if (cp >= 0xC0 && cp <= 0xFF) {
    static char out[2];
    out[0] = latin1[cp - 0xC0];
    out[1] = 0;
    if (out[0] == '*' || out[0] == '/') return "";
    if (cp == 0xC6) return "AE";
    if (cp == 0xE6) return "ae";
    if (cp == 0xDF) return "ss";
    return out;
  }
  if (cp >= 0x100 && cp <= 0x17F) {
    // Latin Extended-A pairs upper/lower forms of the base letters in order.
    static const char* ext = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    static char out[2];
    const size_t idx = cp - 0x100;
    out[0] = idx < std::strlen(ext) ? ext[idx] : 0;
    out[1] = 0;
    return out;
  }
  return nullptr;
}

std::vector<uint32_t> DecodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (len == 0 || i + len > s.size()) {
      ++i;
      continue;
    }
    uint32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
    for (int k = 1; k < len; ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
    out.push_back(cp);
    i += len;
  }
  return out;
}

}  // namespace

std::string Normalize(const std::string& utf8) {
  std::string text = utf8;
  const std::pair<const char*, const char*> symbols[] = {
      {"<3", " heart "}, {"&", " and "}, {"@", " at "}, {"%", " percent "},
      {"+", " plus "},   {"=", " equals "}, {"#", " number "}, {"$", " dollars "}};
  for (const auto& [symbol, words] : symbols) {
    for (size_t pos = 0; (pos = text.find(symbol, pos)) != std::string::npos;) {
      text.replace(pos, std::strlen(symbol), words);
      pos += std::strlen(words);
    }
  }
  // Fold to ASCII.
  std::string ascii;
  for (uint32_t cp : DecodeUtf8(text)) {
    if (cp < 0x80) {
      ascii += static_cast<char>(cp);
    } else if (cp == 0x2026) {
      ascii += "...";
    } else if (cp == 0xA0 || cp == 0x2009 || cp == 0x202F || cp == 0x3000) {
      ascii += ' ';
    } else if (cp >= 0xAC00 && cp <= 0xD7A3) {
      // Korean Hangul syllables decomposed to standard 2-beolsik keyboard letter slices
      static const char* kChoKeys[] = {"r", "r", "s", "e", "e", "f", "a", "q", "q", "t",
                                        "t", "d", "w", "w", "c", "z", "x", "v", "g"};
      static const char* kJungKeys[] = {"k", "o", "i", "o", "j", "p", "u", "p", "h", "hk",
                                         "ho", "hl", "y", "n", "nj", "np", "nl", "b", "m", "ml", "l"};
      static const char* kJongKeys[] = {"", "r", "r", "rt", "s", "sw", "sg", "e", "f", "fr",
                                         "fa", "fq", "ft", "fx", "fv", "fg", "a", "q", "qt", "t",
                                         "t", "d", "w", "c", "z", "x", "v", "g"};
      uint32_t syl = cp - 0xAC00;
      uint32_t cho = syl / 588;
      uint32_t jung = (syl % 588) / 28;
      uint32_t jong = syl % 28;
      if (cho < 19) ascii += kChoKeys[cho];
      if (jung < 21) ascii += kJungKeys[jung];
      if (jong < 28) ascii += kJongKeys[jong];
    } else if (cp >= 0x3131 && cp <= 0x314E) {
      // Hangul compatibility jamo consonants
      static const char* kJamoConsonants[] = {
          "r", "r", "rt", "s", "sw", "sg", "e", "e", "f", "fr", "fa", "fq", "ft",
          "fx", "fv", "fg", "a", "q", "q", "qt", "t", "t", "d", "w", "w", "c", "z", "x", "v", "g"
      };
      size_t idx = cp - 0x3131;
      if (idx < sizeof(kJamoConsonants) / sizeof(kJamoConsonants[0])) ascii += kJamoConsonants[idx];
    } else if (cp >= 0x314F && cp <= 0x3163) {
      // Hangul compatibility jamo vowels
      static const char* kJamoVowels[] = {
          "k", "o", "i", "o", "j", "p", "u", "p", "h", "hk", "ho", "hl", "y", "n",
          "nj", "np", "nl", "b", "m", "ml", "l"
      };
      size_t idx = cp - 0x314F;
      if (idx < sizeof(kJamoVowels) / sizeof(kJamoVowels[0])) ascii += kJamoVowels[idx];
    } else if (const char* base = Unaccent(cp)) {
      ascii += base;
    }
    // Anything else (emoji, symbols) is silent, like in-game.
  }
  // Numbers to words; long digit runs (codes, ids) read digit by digit.
  std::string spelled;
  for (size_t i = 0; i < ascii.size();) {
    if (!std::isdigit(static_cast<unsigned char>(ascii[i]))) {
      spelled += ascii[i++];
      continue;
    }
    size_t j = i;
    while (j < ascii.size() && std::isdigit(static_cast<unsigned char>(ascii[j]))) ++j;
    const std::string digits = ascii.substr(i, j - i);
    spelled += ' ';
    if (digits.size() > 9 || (digits.size() > 1 && digits[0] == '0')) {
      for (size_t k = 0; k < digits.size(); ++k) {
        if (k) spelled += ' ';
        spelled += kOnes[digits[k] - '0'];
      }
    } else {
      spelled += NumberToWords(std::stoull(digits));
    }
    spelled += ' ';
    i = j;
  }
  std::string out;
  for (char c : spelled) {
    if (std::isalpha(static_cast<unsigned char>(c)) || PauseSlots(c) >= 0) {
      out += c;
    } else if (std::isspace(static_cast<unsigned char>(c)) || std::strchr("_/\\|", c)) {
      out += ' ';
    }
  }
  // Collapse runs of spaces and trim.
  std::string collapsed;
  for (char c : out) {
    if (c == ' ' && !collapsed.empty() && collapsed.back() == ' ') continue;
    collapsed += c;
  }
  const size_t first = collapsed.find_first_not_of(" \n");
  if (first == std::string::npos) return "";
  const size_t last = collapsed.find_last_not_of(" \n");
  return collapsed.substr(first, last - first + 1);
}

// --------------------------------------------------------------------------

std::vector<float> Synthesize(const std::string& utf8, const VoiceSettings& settings,
                              uint32_t seed, const SamplePack* pack) {
  const VoiceInfo& voice = FindVoice(settings.voice);
  std::mt19937 rng(seed);
  const std::vector<Unit> units = Plan(utf8, settings.style, voice, rng);
  if (units.empty()) return {};
  const double slot = 1.0 / (voice.cps * std::max(0.25, settings.speed));
  double total_slots = 0;
  for (const Unit& u : units) total_slots += u.slots;
  std::vector<double> out(static_cast<size_t>((total_slots * slot + 0.3) * kSampleRate) +
                          kSampleRate / 10);
  const bool use_custom_pack = pack != nullptr && !pack->empty();
  const AnimaleseBank* bank = (!use_custom_pack) ? GetAnimaleseBank(settings.lang, settings.voice) : nullptr;
  const bool use_bank = (bank != nullptr && !bank->empty());
  double cursor = 0;
  for (const Unit& u : units) {
    const double dur = u.slots * slot;
    if (!u.pause) {
      const double semis = settings.pitch + u.pitch;
      const double blip_dur = dur * 1.12;  // neighbouring blips overlap a little
      std::vector<double> blip;
      if (use_custom_pack) {
        const double rate = std::pow(2.0, semis / 12.0) * (voice.f0 / 300.0);
        blip = pack->Blip(u.letter, blip_dur, rate, u.gain);
      } else if (use_bank) {
        const double rate = std::pow(2.0, semis / 12.0);
        blip = bank->Blip(u.letter, blip_dur, rate, u.gain);
      } else {
        // Pitching a recording up also lifts its formants; follow the pitch
        // a little so high blips get brighter, like the games' samples.
        const double f0 = voice.f0 * std::pow(2.0, semis / 12.0);
        blip = RenderBlip(u.syllable, blip_dur, f0, u.glide, voice, u.gain, rng,
                          std::pow(2.0, u.pitch / 12.0 * 0.35));
      }
      const size_t i0 = static_cast<size_t>(cursor * kSampleRate);
      for (size_t j = 0; j < blip.size() && i0 + j < out.size(); ++j) out[i0 + j] += blip[j];
    }
    cursor += dur;
  }
  out.resize(std::min(out.size(), static_cast<size_t>((cursor + 0.08) * kSampleRate)));
  // Gentle low-pass: the games' 32 kHz samples have no air on top.
  if (!out.empty()) {
    const size_t size = NextPow2(out.size());
    std::vector<double> padded = out;
    ShapeSpectrum(padded, size, [](double f) {
      const double x = f / 9500.0;
      return 1.0 / std::sqrt(1.0 + x * x * x * x * x * x * x * x);
    });
    out.swap(padded);
  }
  double peak = 0;
  for (double v : out) peak = std::max(peak, std::fabs(v));
  if (peak == 0) peak = 1;
  std::vector<float> result(out.size());
  for (size_t i = 0; i < out.size(); ++i) result[i] = static_cast<float>(out[i] / peak * 0.85);
  return result;
}

}  // namespace nac
