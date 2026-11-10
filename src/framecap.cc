#include "framecap.h"

#include <time.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "app.h"
#include "overlay.h"
#include "xr_layer.h"

namespace nac::framecap {
namespace {

constexpr uint64_t kCheckNs = 50'000'000;  // re-read the cap while waiting this often
constexpr uint64_t kSpinNs = 500'000;      // busy-wait the last 0.5 ms for precision

std::mutex g_mutex;
uint64_t g_last_frame_ns = 0;
uint64_t g_next_check_ns = 0;
std::atomic<double> g_cap{0};

uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + ts.tv_nsec;
}

void SleepUntil(uint64_t t) {
  const timespec ts{static_cast<time_t>(t / 1'000'000'000ull), static_cast<long>(t % 1'000'000'000ull)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

uint64_t Interval(uint64_t now) {
  if (now >= g_next_check_ns) {
    g_next_check_ns = now + kCheckNs;
    const Config cfg = GetConfig();
    double cap = cfg.fps_cap;
    // Never in VR: holding frames back here would stall the headset.
    if (xr::Active()) cap = 0;
    if (!overlay::WindowFocused() && cfg.fps_cap_background > 0) cap = cfg.fps_cap_background;
    g_cap = cap > 0 ? cap : 0;
  }
  const double cap = g_cap.load();
  return cap > 0 ? static_cast<uint64_t>(1e9 / cap) : 0;
}

}  // namespace

double CurrentCap() { return g_cap.load(); }

void Pace() {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint64_t now = NowNs();
  // Wait in slices, so a new cap (say from 5 back to 144) applies mid-wait
  // instead of after a long frame.
  while (true) {
    const uint64_t interval = Interval(now);
    if (interval == 0) break;
    const uint64_t target = g_last_frame_ns + interval;
    if (now >= target) break;
    if (target - now > kSpinNs) {
      uint64_t wake = target - kSpinNs;
      if (wake - now > kCheckNs) wake = now + kCheckNs;
      SleepUntil(wake);
    } else {
      while (NowNs() < target) {
      }
      g_last_frame_ns = target;
      return;
    }
    now = NowNs();
  }
  // Uncapped, or slower than the cap: don't build up debt.
  g_last_frame_ns = now;
}

}  // namespace nac::framecap
