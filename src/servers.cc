#include "servers.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#include "app.h"
#include "gamestate.h"
#include "nightcap.h"
#include "process.h"

namespace nac::servers {
namespace {

// Hopping skips servers joined this recently, like Cocktail does.
constexpr int kRecentSeconds = 600;

std::mutex g_mutex;
std::string g_status;
std::string g_clipboard;
bool g_has_clipboard = false;
std::atomic<bool> g_busy{false};

void SetStatus(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_status = text;
}

// HTTPS needs a TLS stack this library doesn't carry: borrow curl's, or
// Python's.
bool Fetch(const std::string& url, std::string* body) {
  if (RunProgram({"curl", "-fsSL", "--max-time", "12", "-A", "vox-menu", url}, body) == 0 && !body->empty()) return true;
  body->clear();
  return RunProgram({"python3", "-I", "-c",
              "import sys,urllib.request\n"
              "r=urllib.request.Request(sys.argv[1],headers={'User-Agent':'vox-menu'})\n"
              "sys.stdout.write(urllib.request.urlopen(r,timeout=12).read().decode())",
              url},
             body) == 0 &&
         !body->empty();
}

// Joins a server: the running Nightcap leaves the current game and joins
// it. Straight through Nightcap's launch broker; with an older Nightcap,
// by starting Nightcap again with the link, which hands it to this (single)
// instance the same way.
bool Join(const std::string& place, const std::string& job) {
  std::string error;
  if (nightcap::QueueJoin(place, job, &error)) {
    Log("asked Nightcap to join place %s", place.c_str());
    return true;
  }
  Log("launch broker: %s; handing the link over instead", error.c_str());
  const std::string uri = JoinUri(place, job);
  char exe[4096];
  const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return false;
  exe[n] = 0;
  Log("joining %s", uri.c_str());
  return RunProgram({exe, uri}, nullptr) != -1;
}

long Number(const std::string& json, size_t from, size_t to, const char* key, bool* found) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = json.find(needle, from);
  *found = at != std::string::npos && at < to;
  return *found ? std::strtol(json.c_str() + at + needle.size(), nullptr, 10) : 0;
}

}  // namespace

std::string JoinUri(const std::string& place, const std::string& job) {
  // A fresh attempt ID, so a rejoin isn't taken as the launch already active.
  return "roblox://experiences/start?placeId=" + place + "&gameInstanceId=" + job +
         "&joinAttemptId=" + nightcap::NewJoinAttemptId();
}

std::vector<Listed> ParseServerList(const std::string& json) {
  std::vector<Listed> out;
  const std::string mark = "\"id\":\"";
  size_t at = json.find(mark);
  while (at != std::string::npos) {
    const size_t start = at + mark.size();
    const size_t end = json.find('"', start);
    if (end == std::string::npos) break;
    const size_t next = json.find(mark, end);
    const size_t limit = next == std::string::npos ? json.size() : next;
    Listed s;
    s.id = json.substr(start, end - start);
    bool found_players, found_max, found_ping;
    s.playing = static_cast<int>(Number(json, end, limit, "playing", &found_players));
    s.max_players = static_cast<int>(Number(json, end, limit, "maxPlayers", &found_max));
    const long ping = Number(json, end, limit, "ping", &found_ping);
    s.ping = found_ping ? static_cast<double>(ping) : -1;
    if (found_max) out.push_back(s);
    at = next;
  }
  return out;
}

std::string PickServer(const std::vector<Listed>& servers, const std::string& current,
                       const std::vector<std::string>& recent, const std::string& mode, unsigned seed) {
  auto pick_from = [&](bool skip_recent) {
    std::vector<const Listed*> open;
    for (const Listed& s : servers) {
      if (s.id == current || s.playing >= s.max_players) continue;
      if (skip_recent && std::find(recent.begin(), recent.end(), s.id) != recent.end()) continue;
      open.push_back(&s);
    }
    if (open.empty()) return std::string();
    std::mt19937 rng(seed);
    std::shuffle(open.begin(), open.end(), rng);  // ties are broken at random
    if (mode == "smallest") {
      return (*std::min_element(open.begin(), open.end(),
                                [](const Listed* a, const Listed* b) { return a->playing < b->playing; }))
          ->id;
    }
    if (mode == "ping") {
      auto ping = [](const Listed* s) { return s->ping >= 0 ? s->ping : 1e9; };
      return (*std::min_element(open.begin(), open.end(),
                                [&](const Listed* a, const Listed* b) { return ping(a) < ping(b); }))
          ->id;
    }
    return open.front()->id;
  };
  const std::string fresh = pick_from(true);
  return fresh.empty() ? pick_from(false) : fresh;
}

void Rejoin() {
  const ServerInfo last = LastServer();
  if (last.job.empty() || last.place.empty()) {
    SetStatus("No server to rejoin yet");
    return;
  }
  if (g_busy.exchange(true)) return;
  SetStatus("Rejoining...");
  std::thread([last] {
    SetStatus(Join(last.place, last.job) ? "Rejoining the last server" : "Could not start the rejoin");
    g_busy = false;
  }).detach();
}

void Hop(const std::string& mode) {
  const ServerInfo last = LastServer();
  if (last.place.empty()) {
    SetStatus("Join a game first");
    return;
  }
  if (g_busy.exchange(true)) return;
  SetStatus("Looking for another server...");
  std::thread([last, mode] {
    std::string body;
    if (!Fetch("https://games.roblox.com/v1/games/" + last.place + "/servers/Public?sortOrder=Asc&limit=100",
               &body)) {
      SetStatus("Could not get the server list (is curl installed?)");
    } else {
      const std::vector<Listed> list = ParseServerList(body);
      const std::string id = PickServer(list, last.job, RecentServers(kRecentSeconds), mode,
                                        static_cast<unsigned>(std::random_device{}()));
      if (id.empty()) {
        SetStatus("No other server has room right now");
      } else {
        const auto it = std::find_if(list.begin(), list.end(), [&](const Listed& s) { return s.id == id; });
        char text[128];
        std::snprintf(text, sizeof(text), "Hopping to a server with %d/%d players", it->playing, it->max_players);
        SetStatus(Join(last.place, id) ? text : "Could not start the hop");
      }
    }
    g_busy = false;
  }).detach();
}

void CopyInvite() {
  const ServerInfo last = LastServer();
  if (last.job.empty() || last.place.empty()) {
    SetStatus("Join a game first");
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_clipboard = "https://www.roblox.com/games/start?placeId=" + last.place + "&gameInstanceId=" + last.job;
  g_has_clipboard = true;
  g_status = "Copied a link to this server";
}

std::string StatusText() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_status;
}

bool Busy() { return g_busy.load(); }

bool TakeClipboard(std::string* text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_has_clipboard) return false;
  g_has_clipboard = false;
  *text = g_clipboard;
  return true;
}

}  // namespace nac::servers
