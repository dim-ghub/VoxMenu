#include "media.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "app.h"
#include "process.h"
#include "speech.h"

namespace nac::media {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;                 // what parec is asked for
constexpr size_t kMaxBuffered = kRate / 4;   // keep at most 250 ms: low latency
constexpr auto kListEvery = std::chrono::milliseconds(1500);

// One app stream being recorded.
struct Capture {
  int index = -1;
  pid_t pid = -1;
  int fd = -1;
  std::thread reader;
  std::atomic<bool> ended{false};
  std::mutex mutex;
  std::deque<float> samples;
  double position = 0;  // fractional read position, for resampling
};

std::mutex g_mutex;  // the lists below
std::vector<App> g_apps;
std::vector<std::shared_ptr<Capture>> g_captures;
std::string g_status;
std::once_flag g_started;
std::atomic<int64_t> g_list_wanted_ms{0};
std::atomic<bool> g_no_parec{false};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

void Read(std::shared_ptr<Capture> c) {
  float buf[1024];
  size_t carry = 0;  // bytes of a sample split across reads
  char* bytes = reinterpret_cast<char*>(buf);
  while (true) {
    const ssize_t n = read(c->fd, bytes + carry, sizeof(buf) - carry);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    const size_t total = carry + static_cast<size_t>(n);
    const size_t count = total / sizeof(float);
    {
      std::lock_guard<std::mutex> lock(c->mutex);
      c->samples.insert(c->samples.end(), buf, buf + count);
      if (c->samples.size() > kMaxBuffered) {
        c->samples.erase(c->samples.begin(), c->samples.end() - kMaxBuffered);
        c->position = 0;
      }
    }
    carry = total % sizeof(float);
    std::memmove(bytes, bytes + count * sizeof(float), carry);
  }
  c->ended = true;
}

std::shared_ptr<Capture> Start(int index) {
  auto c = std::make_shared<Capture>();
  c->index = index;
  c->pid = StartProgram({"parec", "--monitor-stream=" + std::to_string(index), "--format=float32le",
                         "--rate=" + std::to_string(kRate), "--channels=1", "--latency-msec=20", "--raw"},
                        &c->fd);
  if (c->pid < 0) return nullptr;
  c->reader = std::thread(Read, c);
  return c;
}

void Stop(const std::shared_ptr<Capture>& c) {
  kill(c->pid, SIGTERM);
  if (c->reader.joinable()) c->reader.join();
  close(c->fd);
  while (waitpid(c->pid, nullptr, 0) < 0 && errno == EINTR) {
  }
}

bool IsNightcap(const Stream& s) {
  return s.pid == getpid() || s.binary == "mocktail" || s.binary == "nightcap";
}

void Worker() {
  Clock::time_point listed{};
  std::vector<Stream> streams;
  while (true) {
    const Config cfg = GetConfig();
    // Only while Nightcap reads the mic (you're unmuted in voice chat).
    const bool active = cfg.media_on && !cfg.media_source.empty() && MicInUse();
    const bool want_list = NowMs() - g_list_wanted_ms.load() < 3000;
    if ((active || want_list) && Clock::now() - listed >= kListEvery) {
      listed = Clock::now();
      std::string text;
      if (RunProgram({"pactl", "list", "sink-inputs"}, &text) == 0) {
        streams = ParseSinkInputs(text);
        streams.erase(std::remove_if(streams.begin(), streams.end(), IsNightcap), streams.end());
      } else {
        streams.clear();
      }
      std::vector<App> apps;
      for (const Stream& s : streams) {
        const auto same = [&](const App& a) { return a.name == s.app; };
        if (std::find_if(apps.begin(), apps.end(), same) == apps.end()) apps.push_back({s.app, s.title});
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      g_apps = apps;
    }

    // Record exactly the streams of the chosen app (or of every app).
    std::vector<int> wanted;
    if (active) {
      for (const Stream& s : streams) {
        if (cfg.media_source == kAllApps || s.app == cfg.media_source) wanted.push_back(s.index);
      }
    }
    std::vector<std::shared_ptr<Capture>> current;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      current = g_captures;
    }
    std::vector<std::shared_ptr<Capture>> keep, stop;
    for (const auto& c : current) {
      const bool still = !c->ended && std::find(wanted.begin(), wanted.end(), c->index) != wanted.end();
      (still ? keep : stop).push_back(c);
    }
    for (const int index : wanted) {
      const auto has = [&](const std::shared_ptr<Capture>& c) { return c->index == index; };
      if (std::find_if(keep.begin(), keep.end(), has) != keep.end() || g_no_parec) continue;
      if (auto c = Start(index)) {
        keep.push_back(c);
      } else {
        g_no_parec = true;
        Log("parec not found; install pulseaudio-utils (or libpulse) to share app audio");
      }
    }

    std::string status;
    if (cfg.media_on) {
      const std::string who = cfg.media_source == kAllApps ? "other apps" : cfg.media_source;
      if (g_no_parec) status = "Needs parec (pulseaudio-utils) to record an app";
      else if (cfg.media_source.empty()) status = "Pick an app to share";
      else if (!MicInUse()) status = "Join voice chat and unmute to share";
      else if (keep.empty()) status = (cfg.media_source == kAllApps ? "No other app" : who) + " isn't playing anything";
      else status = "Sharing " + who + " in voice chat";
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_captures = keep;
      g_status = status;
    }
    for (const auto& c : stop) Stop(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(active ? 300 : 500));
  }
}

void EnsureWorker() {
  std::call_once(g_started, [] { std::thread(Worker).detach(); });
}

}  // namespace

std::vector<Stream> ParseSinkInputs(const std::string& text) {
  std::vector<Stream> out;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("Sink Input #", 0) == 0) {
      out.emplace_back();
      out.back().index = std::atoi(line.c_str() + 12);
      continue;
    }
    if (out.empty()) continue;
    const size_t eq = line.find(" = ");
    if (eq == std::string::npos) continue;
    const size_t start = line.find_first_not_of(" \t");
    const std::string key = line.substr(start, eq - start);
    const std::string value = Unquote(line.substr(eq + 3));
    Stream& s = out.back();
    if (key == "application.name") s.app = value;
    else if (key == "media.name") s.title = value;
    else if (key == "application.process.binary") s.binary = value;
    else if (key == "application.process.id") s.pid = std::atol(value.c_str());
  }
  for (Stream& s : out) {
    if (s.app.empty()) s.app = !s.binary.empty() ? s.binary : "Stream " + std::to_string(s.index);
  }
  return out;
}

std::vector<App> Apps() {
  EnsureWorker();
  g_list_wanted_ms = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_apps;
}

std::string Status() {
  EnsureWorker();
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status;
}

void MixInto(float* out, int frames, int rate) {
  EnsureWorker();
  std::vector<std::shared_ptr<Capture>> captures;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    captures = g_captures;
  }
  const double ratio = static_cast<double>(kRate) / rate;
  for (const auto& c : captures) {
    std::lock_guard<std::mutex> lock(c->mutex);
    auto& q = c->samples;
    for (int f = 0; f < frames && !q.empty(); ++f) {
      const size_t i = static_cast<size_t>(c->position);
      const double frac = c->position - i;
      const float a = i < q.size() ? q[i] : 0.0f;
      const float b = i + 1 < q.size() ? q[i + 1] : a;
      out[f] += static_cast<float>(a + (b - a) * frac);
      c->position += ratio;
      const size_t drop = std::min(static_cast<size_t>(c->position), q.size());
      q.erase(q.begin(), q.begin() + drop);
      c->position -= drop;
    }
    if (q.empty()) c->position = 0;
  }
}

}  // namespace nac::media
