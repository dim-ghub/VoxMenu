// The autoclicker and macro engine, with input injection replaced by a log
// of what would have reached the game.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../src/app.h"
#include "../src/automation.h"
#include "../src/gamestate.h"
#include "../src/inject.h"
#include "../src/media.h"
#include "../src/roblox_settings.h"
#include "../src/netstats.h"
#include "../src/nightcap.h"
#include "../src/servers.h"
#include "../src/xr_math.h"
#include "../src/overlay.h"

namespace {

std::vector<std::string> g_events;

void Note(const char* fmt, double a = 0, double b = 0, double c = 0) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  g_events.push_back(buf);
}

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    for (const std::string& e : g_events) std::fprintf(stderr, "    %s\n", e.c_str());
    ++g_failures;
  }
}

int Count(const std::string& prefix) {
  int n = 0;
  for (const std::string& e : g_events) n += e.rfind(prefix, 0) == 0;
  return n;
}

// Ticks the engine like Nightcap's event loop does, for `ms`.
void Run(int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    nac::automation::Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }
  nac::automation::Tick();
}

SDL_Event KeyEvent(SDL_Scancode sc, bool down) {
  SDL_Event e{};
  e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  e.key.scancode = sc;
  e.key.down = down;
  return e;
}

}  // namespace

namespace nac::inject {
void MouseMove(float x, float y) { Note("move %.0f %.0f", x, y); }
void MouseButton(int button, bool down, float x, float y) {
  Note(down ? "down %.0f %.0f %.0f" : "up %.0f %.0f %.0f", button, x, y);
}
void Key(SDL_Scancode scancode, bool down) {
  g_events.push_back(std::string(down ? "key_down " : "key_up ") + SDL_GetScancodeName(scancode));
}
void Text(const std::string& utf8) { g_events.push_back("text " + utf8); }
void Warp(float x, float y) { Note("warp %.0f %.0f", x, y); }
}  // namespace nac::inject

// media.cc only asks whether voice chat is reading the mic.
namespace nac {
bool MicInUse() { return false; }
}  // namespace nac

int main() {
  using namespace nac;
  char dir[] = "/tmp/vox-menu-automation-XXXXXX";
  setenv("XDG_CONFIG_HOME", mkdtemp(dir), 1);
  overlay::SetWindowSize(1000, 500);
  overlay::SetMousePos(300, 200);

  // --- Macro files ---
  const automation::Macro parsed = automation::ParseMacro(
      "# comment\nkey_down Left Shift\nwait 120\nclick right 0.25 0.5\nmove 0.1 0.9\ntext hello there\n"
      "key_up Left Shift\nbogus line\nkey NoSuchKey\n");
  Check(parsed.steps.size() == 6, "parses six good steps and skips the rest");
  Check(automation::ParseMacro(automation::FormatMacro(parsed)).steps.size() == 6, "format round-trips");
  Check(automation::FormatMacro(parsed).find("click right 0.2500 0.5000") != std::string::npos, "click with point");
  Check(automation::FormatMacro(parsed).find("text hello there") != std::string::npos, "text keeps spaces");
  Check(automation::SafeName("../../etc/passwd") == "etcpasswd", "macro names can't leave the folder");
  Check(automation::SaveMacro("farm loop", parsed), "saves");
  automation::Macro loaded;
  Check(automation::LoadMacro("farm loop", &loaded) && loaded.steps.size() == 6, "loads");
  Check(automation::ListMacros() == std::vector<std::string>{"farm loop"}, "lists");
  Check(automation::DeleteMacro("farm loop") && automation::ListMacros().empty(), "deletes");

  const auto points = automation::ParsePoints("0.5,0.25;oops;2,-1");
  Check(points.size() == 2 && points[1].first == 1.0f && points[1].second == 0.0f, "points parse and clamp");

  // --- Autoclicker following the mouse: ~20 clicks in a second at 20 cps ---
  Config cfg = GetConfig();
  cfg.click_cps = 20;
  cfg.click_hold_ms = 10;
  SetConfig(cfg);
  g_events.clear();
  automation::ToggleClicker();
  Run(1000);
  automation::ToggleClicker();
  Run(30);
  const int downs = Count("down 0 300 200");
  Check(downs >= 17 && downs <= 23, "about 20 left clicks at the cursor");
  Check(Count("down") == Count("up"), "every press is released");
  Check(Count("move") == 0, "following the mouse never moves it");
  Check(!automation::GetStatus().clicking, "stopped");

  // --- Fixed points, virtual cursor, a click limit ---
  cfg.click_target = "points";
  cfg.click_points = "0.5,0.5;0.1,0.1";
  cfg.click_limit = 4;
  cfg.click_button = "right";
  SetConfig(cfg);
  g_events.clear();
  automation::ToggleClicker();
  Run(400);
  Check(Count("down 1 500 250") == 2 && Count("down 1 100 50") == 2, "points are clicked in turn");
  Check(Count("move 500 250") == 2 && Count("move 300 200") == 4, "cursor jumps there and back");
  Check(Count("warp") == 0, "the real cursor stays put");
  Check(!automation::GetStatus().clicking, "stops at the click limit");

  // --- Hold mode releases on stop ---
  cfg.click_type = "hold";
  cfg.click_target = "mouse";
  cfg.click_limit = 0;
  SetConfig(cfg);
  g_events.clear();
  automation::ToggleClicker();
  Run(100);
  Check(Count("down 1") == 1 && Count("up") == 0, "holds the button down");
  automation::StopAll();
  Run(10);
  Check(Count("up 1") == 1, "stopping lets go of it");

  // --- Hotkeys ---
  SDL_KeyboardEvent f6{};
  f6.scancode = SDL_SCANCODE_F6;
  f6.down = true;
  Check(automation::HandleHotkey(f6), "F6 is the autoclicker hotkey");
  Run(10);
  Check(automation::GetStatus().clicking, "F6 started it");
  f6.down = false;
  Check(automation::HandleHotkey(f6), "its release is ours too");
  automation::StopAll();
  Run(10);

  // --- Hold-to-click hotkey ---
  cfg.key_clicker_mode = "hold";
  SetConfig(cfg);
  f6.down = true;
  Check(automation::HandleHotkey(f6), "F6 is still ours in hold mode");
  Run(10);
  Check(automation::GetStatus().clicking, "holding F6 clicks");
  f6.repeat = true;
  automation::HandleHotkey(f6);
  Run(10);
  Check(automation::GetStatus().clicking, "key repeat doesn't toggle it");
  f6.repeat = false;
  f6.down = false;
  automation::HandleHotkey(f6);
  Run(10);
  Check(!automation::GetStatus().clicking, "releasing F6 stops it");

  // A side mouse button as the hold key.
  cfg.key_clicker = "Mouse4";
  SetConfig(cfg);
  Check(automation::HandleHotkeyButton(SDL_BUTTON_X1, true), "Mouse Back is ours when bound");
  Run(10);
  Check(automation::GetStatus().clicking, "holding Mouse Back clicks");
  Check(automation::HandleHotkeyButton(SDL_BUTTON_X1, false), "its release too");
  Run(10);
  Check(!automation::GetStatus().clicking, "letting go stops it");
  Check(!automation::HandleHotkeyButton(SDL_BUTTON_X2, true), "Mouse Forward isn't bound: the game gets it");

  // Holding, then alt-tabbing away: the release never comes, so losing
  // focus stops it.
  automation::HandleHotkeyButton(SDL_BUTTON_X1, true);
  Run(10);
  overlay::SetWindowFocused(false);
  Run(10);
  Check(!automation::GetStatus().clicking, "a held clicker stops when Roblox loses focus");
  overlay::SetWindowFocused(true);
  automation::HandleHotkeyButton(SDL_BUTTON_X1, false);
  Run(10);
  cfg.key_clicker = "F6";
  cfg.key_clicker_mode = "toggle";
  SetConfig(cfg);

  SDL_KeyboardEvent w{};
  w.scancode = SDL_SCANCODE_W;
  w.down = true;
  Check(!automation::HandleHotkey(w), "W is the game's");
  automation::StopAll();
  Run(10);

  // --- Playback ---
  automation::SetCurrent(automation::ParseMacro("key W\nwait 60\nclick left 0.1 0.2\ntext hi\n"));
  g_events.clear();
  automation::TogglePlayback();
  Run(30);
  Check(Count("key_down W") == 1 && Count("down") == 0, "the wait holds back the click");
  Run(150);
  Check(Count("key_up W") == 1, "key tap releases");
  Check(Count("move 100 100") == 1 && Count("down 0 100 100") == 1 && Count("up 0 100 100") == 1, "clicks at its point");
  Check(Count("text hi") == 1, "types text");
  Check(!automation::GetStatus().playing, "plays once");

  cfg.macro_repeat = 0;  // forever
  SetConfig(cfg);
  automation::SetCurrent(automation::ParseMacro("key_down Space\nwait 20\n"));
  g_events.clear();
  automation::TogglePlayback();
  Run(100);
  Check(automation::GetStatus().playing && automation::GetStatus().loop >= 2, "loops until stopped");
  automation::TogglePlayback();
  Run(10);
  Check(Count("key_up Space") == 1, "stopping releases held keys");

  // --- Recording ---
  automation::ToggleRecording();
  Run(10);
  automation::Observe(KeyEvent(SDL_SCANCODE_E, true));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  automation::Observe(KeyEvent(SDL_SCANCODE_E, false));
  automation::ToggleRecording();
  Run(10);
  const automation::Macro rec = automation::Current();
  Check(rec.steps.size() == 3 && rec.steps[0].kind == automation::Step::kKeyDown &&
            rec.steps[1].kind == automation::Step::kWait && rec.steps[1].ms >= 70 &&
            rec.steps[2].kind == automation::Step::kKeyUp,
        "records key down, the wait, key up");

  // --- Network helpers ---
  Check(net::IsPublicAddress("128.116.1.2") && !net::IsPublicAddress("10.1.2.3") &&
            !net::IsPublicAddress("192.168.0.1") && !net::IsPublicAddress("127.0.0.1") &&
            !net::IsPublicAddress("not an ip"),
        "public addresses");
  Check(net::ParseLocation(R"({"status":"success","country":"United States","countryCode":"US",)"
                           R"("regionName":"Virginia","city":"Ashburn"})") == "Ashburn, Virginia, US",
        "location text");
  Check(net::ParseLocation(R"({"status":"success","countryCode":"SG","regionName":"Singapore","city":"Singapore"})") ==
            "Singapore, SG",
        "no repeated names");
  Check(net::ParseLocation(R"({"status":"fail"})").empty(), "failed lookups are empty");

  // --- Server tracking from Roblox's log lines ---
  ServerInfo join;
  Check(ParseJoinLine("2026-11-11T10:00:00Z,1.5,abc,6 [FLog::Output] ! Joining game "
                      "'f0adcb34-df75-4e59-9a5e-0008641fedea' place 18667984660 at 10.0.0.1",
                      &join) &&
            join.job == "f0adcb34-df75-4e59-9a5e-0008641fedea" && join.place == "18667984660" &&
            join.address == "10.0.0.1",
        "join lines give the server, place and address");
  SetDirectLogFeed(true);
  OnLogLine("[FLog::Output] ! Joining game 'aaaa-1' place 42 at 10.1.1.1");
  OnLogLine("[FLog::Network] UDMUX Address = 128.116.5.6, Port = 55123 | RCC Server Address = 10.1.1.1, Port = 1");
  in_addr ip{};
  inet_pton(AF_INET, "128.116.5.6", &ip);
  Check(InGame() && LastServer().job == "aaaa-1" && ServerAddress() == "128.116.5.6",
        "the UDMUX relay replaces the internal address");
  Check(GameEndpointKey() == EndpointKey(ip.s_addr, 55123), "the game's UDP endpoint");
  OnLogLine("[D/rbx.jni] onGameLeaveBegin() SessionReporterState_GameExitRequested");
  Check(!InGame() && LastServer().job == "aaaa-1", "leaving keeps the last server for a rejoin");
  Check(RecentServers(600) == std::vector<std::string>{"aaaa-1"}, "recently joined servers");

  // --- Ping from the game's own packets: ACKs ~40 ms after sends ---
  nightcap::AckPing ack;
  std::int64_t t = 5'000'000;
  for (int i = 0; i < 60; ++i, t += 16'000) {
    ack.OnSend(t);
    ack.OnReceive(t + 40'000, 54);
    ack.OnReceive(t + 41'000, 300);  // not an ACK: ignored
  }
  const int ms = ack.EstimateMs(t);
  Check(ms >= 38 && ms <= 43, "ACK timing finds the 40 ms round trip");
  ack.Reset();
  Check(ack.EstimateMs(t) == -1, "unsure without packets");

  // --- Server hopping ---
  const std::string list =
      R"({"data":[{"id":"cur","maxPlayers":10,"playing":3,"ping":50},)"
      R"({"id":"full","maxPlayers":10,"playing":10,"ping":20},)"
      R"({"id":"seen","maxPlayers":10,"playing":1,"ping":30},)"
      R"({"id":"big","maxPlayers":10,"playing":8,"ping":90,"playerTokens":["x"]},)"
      R"({"id":"small","maxPlayers":10,"playing":2,"ping":120}]})";
  const auto servers = servers::ParseServerList(list);
  Check(servers.size() == 5 && servers[3].playing == 8 && servers[3].ping == 90, "server list parses");
  Check(servers::PickServer(servers, "cur", {"seen"}, "smallest", 1) == "small", "fewest players, skipping recent");
  Check(servers::PickServer(servers, "cur", {"seen"}, "ping", 1) == "big", "best ping");
  for (unsigned seed = 0; seed < 20; ++seed) {
    const std::string id = servers::PickServer(servers, "cur", {"seen"}, "random", seed);
    Check(id == "big" || id == "small", "random never picks this, a full or a recent server");
  }
  Check(servers::PickServer(servers, "cur", {"seen", "big", "small"}, "smallest", 1) == "seen",
        "falls back to a recent server when nothing else has room");

  // --- Joining through Nightcap's launch broker (a stand-in owner) ---
  const std::string sock_path = std::string(dir) + "/mocktail.test.external-launch-v1.sock";
  const int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path.c_str());
  Check(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(listener, 4) == 0,
        "stand-in broker listens");
  std::string received;
  std::thread owner([&] {
    const int peer = accept(listener, nullptr, nullptr);
    char buf[2048];
    const ssize_t n = recv(peer, buf, sizeof(buf), 0);
    if (n > 0) received.assign(buf, n);
    const unsigned char ok[5] = {'M', 'T', 'L', '1', 0};
    send(peer, ok, sizeof(ok), 0);
    close(peer);
  });
  std::string error;
  const bool queued = nightcap::QueueJoin("42", "f0adcb34-df75", &error);
  owner.join();
  Check(queued, ("the broker accepts the join: " + error).c_str());
  const std::string expected_start = "MOCKTAIL-EXTERNAL-LAUNCH/1\n{\"placeId\":42,\"gameInstanceId\":\"f0adcb34-df75\",\"joinAttemptId\":\"";
  Check(received.rfind(expected_start, 0) == 0 && received.size() == expected_start.size() + 36 + 2 &&
            received.substr(received.size() - 2) == "\"}",
        "speaks MOCKTAIL-EXTERNAL-LAUNCH/1, with a join attempt ID");
  // Nightcap ignores a launch identical to the one that started the game:
  // rejoining the same server twice must not send the same request twice.
  const std::string first = nightcap::NewJoinAttemptId(), second = nightcap::NewJoinAttemptId();
  Check(first != second && first.size() == 36 && first[14] == '4' && std::string("89ab").find(first[19]) != std::string::npos,
        "each join gets a new UUID v4");
  Check(servers::JoinUri("42", "abc") != servers::JoinUri("42", "abc"), "rejoin links differ too");
  Check(!nightcap::QueueJoin("42", "bad\"id", &error), "refuses ids that would break the JSON");
  close(listener);

  // --- Shared app audio: which streams there are ---
  const auto streams = media::ParseSinkInputs(
      "Sink Input #4313\n\tDriver: PipeWire\n\tProperties:\n\t\tmedia.name = \"Lofi beats\"\n"
      "\t\tapplication.name = \"Zen\"\n\t\tapplication.process.id = \"12313\"\n"
      "\t\tapplication.process.binary = \"zen-bin\"\n"
      "Sink Input #77\n\tProperties:\n\t\tapplication.process.binary = \"mpv\"\n");
  Check(streams.size() == 2 && streams[0].index == 4313 && streams[0].app == "Zen" &&
            streams[0].title == "Lofi beats" && streams[0].pid == 12313 && streams[0].binary == "zen-bin",
        "sink inputs parse");
  Check(streams[1].index == 77 && streams[1].app == "mpv", "an unnamed stream goes by its program");

  // --- Roblox's Background Transparency ---
  Check(ParsePreferredTransparency("<Item><float name=\"PreferredTransparency\">0.25</float></Item>", 1) == 0.25f,
        "reads PreferredTransparency");
  Check(ParsePreferredTransparency("<float name=\"Other\">0</float>", 1) == 1.0f, "missing keeps the default");
  Check(ParsePreferredTransparency("<float name=\"PreferredTransparency\">7</float>", 1) == 1.0f, "clamped");

  // --- The headset panel's geometry ---
  {
    auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    XrPosef head{};
    head.orientation.w = 1;  // looking down -Z
    head.position = {0, 1.6f, 0};
    const XrPosef panel = xr::PanelInFront(head, 0.85f, 0.12f);
    Check(near(panel.position.x, 0) && near(panel.position.y, 1.48f) && near(panel.position.z, -0.85f),
          "opens in front of the head, a little low");
    XrPosef turned = head;  // looking down -X (turned 90 degrees left) and up a bit
    turned.orientation = {0, std::sin(0.785398f), 0, std::cos(0.785398f)};
    const XrPosef side = xr::PanelInFront(turned, 1.0f, 0.0f);
    Check(near(side.position.x, -1.0f) && near(side.position.z, 0), "follows where the head turns");
    const XrVector3f facing = xr::Rotate(side.orientation, {0, 0, 1});
    Check(near(facing.x, 1.0f) && near(facing.y, 0), "and faces the player");

    XrPosef aim{};
    aim.orientation.w = 1;
    aim.position = {0.2f, 1.48f + 0.1f, 0};  // pointing straight ahead from right of centre, a bit high
    float u = -1, v = -1;
    Check(xr::HitPanel(panel, 0.8f, 0.68f, aim, &u, &v) && near(u, 0.75f) && near(v, 0.5f - 0.1f / 0.68f),
          "the aim ray lands where it points");
    aim.position.x = 2;
    Check(!xr::HitPanel(panel, 0.8f, 0.68f, aim, &u, &v), "misses beside the panel");
    aim.position = {0, 1.48f, -2};  // behind it
    Check(!xr::HitPanel(panel, 0.8f, 0.68f, aim, &u, &v), "not from behind");

    Check(xr::ExtraBindings("/interaction_profiles/oculus/touch_controller", "left", "toggle") ==
              std::vector<std::string>{"/user/hand/left/input/thumbstick/click"},
          "Quest controllers: hold the thumbstick click");
    Check(xr::ExtraBindings("/interaction_profiles/htc/vive_controller", "right", "scroll") ==
              std::vector<std::string>{"/user/hand/right/input/trackpad/y"},
          "Vive wands scroll with the trackpad");
    Check(xr::ExtraBindings("/interaction_profiles/khr/simple_controller", "right", "scroll").empty() &&
              xr::ExtraBindings("/interaction_profiles/khr/simple_controller", "right", "trigger") ==
                  std::vector<std::string>{"/user/hand/right/input/select/click"},
          "the simple controller has no stick");
    Check(xr::ExtraBindings("/interaction_profiles/unknown/thing", "left", "aim").empty(),
          "nothing for unknown controllers");
  }

  if (g_failures == 0) std::printf("automation_test: all passed\n");
  std::fflush(stdout);
  _exit(g_failures == 0 ? 0 : 1);  // skip saving the config at exit
}
