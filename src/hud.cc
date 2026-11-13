// The HUD overlays (FPS, ping, server location, keystrokes), the layout
// editor that moves and resizes them, and the floating automation window
// for the autoclicker and macros.
//
// Overlays are plain draw-list shapes in the background, under any menu.
// The automation window is a regular Dear ImGui window that stays on screen
// while you play; the SDL hooks give it the mouse where it is drawn.

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "app.h"
#include "automation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "netstats.h"
#include "ui_internal.h"

namespace nac::ui {
namespace {

// Roblox's top-bar look, measured from the client: 44 px circles and pills,
// 8 px apart, filled #121215, with white icons and bold text. #0199ED (its
// active gear) is the one accent, and its muted-mic red marks problems.
ImU32 Fill(float a = 1.0f) { return C(18, 18, 21, a); }
ImU32 ChipFill() { return C(42, 43, 48); }
ImU32 Accent(float a = 1.0f) { return C(1, 153, 237, a); }
ImU32 Alert() { return C(240, 96, 93); }
ImU32 Units() { return C(179, 180, 182); }
ImU32 Unlit() { return C(85, 87, 94); }

ImU32 Mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImLerp(x, y, std::clamp(t, 0.0f, 1.0f)));
}

// --------------------------------------------------------------------------
// Frame rate

using Clock = std::chrono::steady_clock;
Clock::time_point g_fps_since = Clock::now();
int g_fps_frames = 0;
float g_fps = 0, g_frame_ms = 0;

// Headset frames: while they come, they are the frame rate that matters.
Clock::time_point g_vr_since = Clock::now(), g_vr_last{};
int g_vr_frames = 0;
float g_vr_fps = 0, g_vr_frame_ms = 0;

bool InVr() { return Clock::now() - g_vr_last < std::chrono::seconds(1); }
float Fps() { return InVr() ? g_vr_fps : g_fps; }
float FrameMs() { return InVr() ? g_vr_frame_ms : g_frame_ms; }

// --------------------------------------------------------------------------
// Overlays

struct WidgetDef {
  const char* id;  // as HudWidget::attach names it
  const char* name;
  HudWidget Config::*field;
};
const WidgetDef kWidgets[] = {
    {"fps", "FPS", &Config::hud_fps}, {"ping", "Ping", &Config::hud_ping}, {"server", "Server", &Config::hud_server},
    {"keys", "Keystrokes", &Config::hud_keys}};
constexpr int kNumWidgets = 4;
enum { kFps, kPing, kServer, kKeys };

// Sizes at 1x, in logical units, all scaled by u (pixels per unit).
constexpr float kPill = 44, kGap = 8, kIcon = 22, kPadL = 14, kPadR = 18, kText = 16;

// Text drawn at any size from the large baked HUD font.
void HudText(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, const char* text) {
  dl->AddText(g_fonts.hud, size, ImVec2(std::round(pos.x), std::round(pos.y)), col, text);
}

ImVec2 HudMeasure(float size, const char* text) { return g_fonts.hud->CalcTextSizeA(size, FLT_MAX, 0, text); }

// Coloured runs of text on one line.
struct Run {
  std::string text;
  ImU32 col;
};

struct Content {
  enum Icon { kGauge, kBars, kPin } icon = kGauge;
  ImU32 icon_col = 0;
  int bars = 0;           // kBars: how many are lit
  std::vector<Run> line;
  std::string sub;        // optional smaller second line
  // Width the text always gets, so a pill doesn't change size (and joined
  // pills don't shift) as a number goes from 2 to 3 digits.
  std::string reserve;
};

Content TextContent(const Config& cfg, int which) {
  Content c;
  c.icon_col = White();
  char buf[64];
  if (which == kFps) {
    std::snprintf(buf, sizeof(buf), "%.0f", Fps());
    if (Fps() > 0 && Fps() < 30) c.icon_col = Alert();
    c.line.push_back({buf, White()});
    std::snprintf(buf, sizeof(buf), cfg.hud_frametime ? " FPS \xC2\xB7 %.1f ms" : " FPS", FrameMs());
    c.line.push_back({buf, Units()});
    c.reserve = std::string("000") + buf;
  } else if (which == kPing) {
    const net::Stats st = net::Get(true, cfg.hud_server.on);
    c.icon = Content::kBars;
    c.reserve = "000 ms";
    if (st.ping_ms >= 0) {
      c.bars = st.ping_ms < 60 ? 4 : st.ping_ms < 120 ? 3 : st.ping_ms < 200 ? 2 : 1;
      if (c.bars == 1) c.icon_col = Alert();
      std::snprintf(buf, sizeof(buf), "%d", st.ping_ms);
      c.line.push_back({buf, White()});
      c.line.push_back({" ms", Units()});
    } else {
      c.line.push_back({"--", Units()});
      c.line.push_back({" ms", Units()});
    }
  } else if (which == kServer) {
    const net::Stats st = net::Get(cfg.hud_ping.on, true);
    c.icon = Content::kPin;
    if (st.address.empty()) {
      c.icon_col = Units();
      c.line.push_back({"Not in a server", Units()});
    } else if (st.location.empty()) {
      c.icon_col = Units();
      c.line.push_back({"Locating...", Units()});
    } else {
      c.line.push_back({st.location, White()});
      if (cfg.hud_server_ip) c.sub = st.address;
    }
  }
  return c;
}

// The icons, stroked like Roblox's, in a kIcon box at `o` (top-left).
void DrawIcon(ImDrawList* dl, const Content& c, ImVec2 o, float u) {
  const float s = kIcon * u / 24;  // the icons are drawn on a 24-unit grid
  auto p = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
  const float stroke = 2 * s;
  switch (c.icon) {
    case Content::kGauge: {
      dl->PathArcTo(p(12, 16), 8 * s, IM_PI, 2 * IM_PI, 24);
      dl->PathStroke(c.icon_col, 0, stroke);
      // The needle leans back when the frame rate is low.
      const bool low = c.icon_col == Alert();
      dl->AddLine(p(12, 16), low ? p(8, 11) : p(16, 11), c.icon_col, stroke);
      dl->AddCircleFilled(p(12, 16), stroke * 0.8f, c.icon_col);
      break;
    }
    case Content::kBars: {
      const float xs[4] = {3, 8.5f, 14, 19.5f}, ys[4] = {15, 11, 7, 3};
      for (int i = 0; i < 4; ++i) {
        const ImU32 col = i < c.bars ? (c.bars == 1 ? Alert() : White()) : Unlit();
        dl->AddRectFilled(p(xs[i], ys[i]), p(xs[i] + 3.5f, 20), col, s);
      }
      break;
    }
    case Content::kPin: {
      dl->PathArcTo(p(12, 10), 6 * s, IM_PI * 0.78f, IM_PI * 2.22f, 24);
      dl->PathLineTo(p(12, 21));
      dl->PathStroke(c.icon_col, ImDrawFlags_Closed, stroke);
      dl->AddCircle(p(12, 10), 2.2f * s, c.icon_col, 16, stroke);
      break;
    }
  }
}

float TextWidth(const Content& c, float u) {
  float w = 0;
  const float size = (c.sub.empty() ? kText : 15) * u;
  for (const Run& r : c.line) w += HudMeasure(size, r.text.c_str()).x;
  if (!c.sub.empty()) w = std::max(w, HudMeasure(12 * u, c.sub.c_str()).x);
  if (!c.reserve.empty()) w = std::max(w, HudMeasure(size, c.reserve.c_str()).x);
  return w;
}

// Keystrokes: three 44 px keys across, Space and the mouse buttons below.
ImVec2 KeysSize(const Config& cfg) {
  float h = kPill * 2 + kGap;
  if (cfg.hud_keys_space) h += kGap + 32;
  if (cfg.hud_keys_mouse) h += kGap + kPill;
  return ImVec2(kPill * 3 + kGap * 2, h);
}

// Size of a widget in swapchain pixels, at unit scale u (pixels per logical).
ImVec2 WidgetSize(const Config& cfg, int which, float u) {
  if (which == kKeys) {
    const ImVec2 s = KeysSize(cfg);
    return ImVec2(std::round(s.x * u), std::round(s.y * u));
  }
  const float w = (kPadL + kIcon + 8 + kPadR) * u + TextWidth(TextContent(cfg, which), u);
  return ImVec2(std::round(w), std::round(kPill * u));
}

// Fades for the keystroke keys.
std::map<int, float> g_key_glow;

float Glow(int id, bool on) {
  float& g = g_key_glow[id];
  g += ((on ? 1.0f : 0.0f) - g) * (on ? 0.6f : 0.25f);
  return g;
}

// A key: dark at rest; pressed, white with a dark label (or blue with a
// white one).
void KeyCap(ImDrawList* dl, ImVec2 a, ImVec2 b, float u, const char* label, const char* sub, float glow,
            const Config& cfg) {
  const bool blue = cfg.hud_keys_pressed == "blue";
  const ImU32 rest = Fill(OverlayOpacity(cfg));
  dl->AddRectFilled(a, b, Mix(rest, blue ? Accent() : White(), glow), (b.y - a.y) / 2);
  const ImU32 ink = Mix(White(), blue ? White() : Fill(), glow);
  if (*label == 0) {  // the Space bar: a line across
    const float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
    dl->AddRectFilled(ImVec2(cx - 28 * u, cy - 1.5f * u), ImVec2(cx + 28 * u, cy + 1.5f * u), ink, u);
    return;
  }
  const float size = (sub ? 14 : kText) * u;
  const ImVec2 ts = HudMeasure(size, label);
  const float cy = (a.y + b.y) / 2 - (sub ? 6 * u : 0);
  HudText(dl, size, ImVec2((a.x + b.x - ts.x) / 2, cy - ts.y / 2), ink, label);
  if (sub) {
    const ImVec2 ss = HudMeasure(11 * u, sub);
    HudText(dl, 11 * u, ImVec2((a.x + b.x - ss.x) / 2, cy + 7 * u), Mix(Units(), blue ? White() : Unlit(), glow),
            sub);
  }
}

void DrawKeys(ImDrawList* dl, const Config& cfg, ImVec2 o, float u) {
  const float k = kPill * u, g = kGap * u;
  auto cap = [&](float x, float y, const char* label, SDL_Scancode sc) {
    const ImVec2 a(o.x + x, o.y + y);
    KeyCap(dl, a, ImVec2(a.x + k, a.y + k), u, label, nullptr, Glow(sc, overlay::KeyHeld(sc)), cfg);
  };
  cap(k + g, 0, "W", SDL_SCANCODE_W);
  cap(0, k + g, "A", SDL_SCANCODE_A);
  cap(k + g, k + g, "S", SDL_SCANCODE_S);
  cap(2 * (k + g), k + g, "D", SDL_SCANCODE_D);
  float y = 2 * (k + g);
  const float full = 3 * k + 2 * g;
  if (cfg.hud_keys_space) {
    const ImVec2 a(o.x, o.y + y);
    KeyCap(dl, a, ImVec2(a.x + full, a.y + 32 * u), u, "", nullptr,
           Glow(SDL_SCANCODE_SPACE, overlay::KeyHeld(SDL_SCANCODE_SPACE)), cfg);
    y += 32 * u + g;
  }
  if (cfg.hud_keys_mouse) {
    const float w = (full - g) / 2;
    for (int b = 0; b < 2; ++b) {
      char sub[24] = "";
      if (cfg.hud_keys_cps) std::snprintf(sub, sizeof(sub), "%d CPS", overlay::ClicksPerSecond(b));
      const ImVec2 a(o.x + b * (w + g), o.y + y);
      KeyCap(dl, a, ImVec2(a.x + w, a.y + k), u, b == 0 ? "LMB" : "RMB", cfg.hud_keys_cps ? sub : nullptr,
             Glow(1000 + b, overlay::ButtonHeld(b)), cfg);
    }
  }
}

// A text overlay's icon and text; its pill is drawn by the caller, so that
// overlays side by side can share one.
void DrawWidgetContent(ImDrawList* dl, const Config& cfg, int which, ImVec2 a, float u) {
  const Content c = TextContent(cfg, which);
  const float h = kPill * u;
  DrawIcon(dl, c, ImVec2(a.x + kPadL * u, a.y + (h - kIcon * u) / 2), u);
  float x = a.x + (kPadL + kIcon + 8) * u;
  if (c.sub.empty()) {
    const float y = a.y + (h - kText * u) / 2 - u;
    for (const Run& r : c.line) {
      HudText(dl, kText * u, ImVec2(x, y), r.col, r.text.c_str());
      x += HudMeasure(kText * u, r.text.c_str()).x;
    }
    return;
  }
  const float top = a.y + (h - (15 + 13) * u) / 2 - u;
  for (const Run& r : c.line) {
    HudText(dl, 15 * u, ImVec2(x, top), r.col, r.text.c_str());
    x += HudMeasure(15 * u, r.text.c_str()).x;
  }
  HudText(dl, 12 * u, ImVec2(a.x + (kPadL + kIcon + 8) * u, top + 16 * u), Units(), c.sub.c_str());
}

// Where a widget sits on screen (top-left): x and y share out the room the
// screen leaves around it (see HudWidget).
ImVec2 WidgetPos(const HudWidget& w, ImVec2 size, ImVec2 screen) {
  const float room_x = std::max(0.0f, screen.x - size.x), room_y = std::max(0.0f, screen.y - size.y);
  return ImVec2(std::round(std::clamp(static_cast<float>(w.x), 0.0f, 1.0f) * room_x),
                std::round(std::clamp(static_cast<float>(w.y), 0.0f, 1.0f) * room_y));
}

// The inverse: the x / y that put a widget's top-left at `pos`.
void SetWidgetPos(HudWidget* w, ImVec2 pos, ImVec2 size, ImVec2 screen) {
  const float room_x = screen.x - size.x, room_y = screen.y - size.y;
  w->x = room_x > 0 ? std::clamp(pos.x / room_x, 0.0f, 1.0f) : 0.0;
  w->y = room_y > 0 ? std::clamp(pos.y / room_y, 0.0f, 1.0f) : 0.0;
}

float Unit(const HudWidget& w) { return g_px * static_cast<float>(std::clamp(w.scale, 0.4, 4.0)); }

int WidgetIndex(const std::string& id) {
  for (int i = 0; i < kNumWidgets; ++i) {
    if (id == kWidgets[i].id) return i;
  }
  return -1;
}

// The overlay `which` is joined onto, or -1: only a text overlay that is
// on, never itself or one further along its own chain.
int AttachedTo(const Config& cfg, int which) {
  int target = WidgetIndex((cfg.*kWidgets[which].field).attach);
  if (target < 0 || target == kKeys || which == kKeys || !(cfg.*kWidgets[target].field).on) return -1;
  for (int at = target, steps = 0; at >= 0; at = WidgetIndex((cfg.*kWidgets[at].field).attach)) {
    if (at == which || ++steps > kNumWidgets) return -1;  // a loop
  }
  return target;
}

// Every overlay that is on, placed. One joined onto another sits right
// after it, the standard gap apart, in the same pill (`group`, the index of
// the first overlay in the chain), like Roblox's menu / chat / mic group.
struct Placed {
  int which = -1;
  ImVec2 min, size;
  float u = 1;
  int group = -1;
  bool on = false;
};

std::vector<Placed> LayoutWidgets(const Config& cfg, ImVec2 screen) {
  std::vector<Placed> all(kNumWidgets);
  for (int i = 0; i < kNumWidgets; ++i) {
    const HudWidget& w = cfg.*kWidgets[i].field;
    all[i].which = i;
    all[i].on = w.on;
    all[i].u = Unit(w);
    all[i].size = WidgetSize(cfg, i, all[i].u);
  }
  // Chains are at most kNumWidgets long, so placing in that many passes
  // reaches the end of every one.
  std::vector<bool> placed(kNumWidgets, false);
  for (int pass = 0; pass < kNumWidgets; ++pass) {
    for (int i = 0; i < kNumWidgets; ++i) {
      if (placed[i] || !all[i].on) continue;
      const int t = AttachedTo(cfg, i);
      if (t < 0) {
        all[i].min = WidgetPos(cfg.*kWidgets[i].field, all[i].size, screen);
        all[i].group = i;
      } else if (placed[t]) {
        all[i].min = ImVec2(all[t].min.x + all[t].size.x + std::round(kGap * all[t].u), all[t].min.y);
        all[i].group = all[t].group;
      } else {
        continue;
      }
      placed[i] = true;
    }
  }
  std::vector<Placed> out;
  for (const Placed& p : all) {
    if (p.on && p.group >= 0) out.push_back(p);
  }
  return out;
}

// --------------------------------------------------------------------------
// Layout editor

struct Drag {
  int widget = -1;
  bool resize = false;
  ImVec2 grab;        // mouse offset from the top-left when moving
  ImVec2 start_min;   // top-left at the start of a resize
  ImVec2 start_size;
  double start_scale = 1;
  std::vector<float> guides_x, guides_y;
  // The pill it would join if dropped now (-1: none), and on which side.
  int join = -1;
  bool join_after = true;  // true: after `join`; false: `join` goes after it
} g_drag;

// Moves `pos` (a box edge) so one of the box's left / centre / right lines
// lands on a nearby guide line. Returns the guide used, or -1.
float SnapAxis(float* pos, float size, const std::vector<float>& lines, float threshold) {
  float best = threshold, guide = -1, result = *pos;
  for (const float anchor : {0.0f, size / 2, size}) {
    for (const float line : lines) {
      const float d = std::fabs(*pos + anchor - line);
      if (d < best) {
        best = d;
        result = line - anchor;
        guide = line;
      }
    }
  }
  *pos = result;
  return guide;
}

// --------------------------------------------------------------------------
// Pieces of the top-bar look, for overlays drawn straight to a draw list

// A key hint chip ("F6", "Esc"): returns its width.
float KeyChip(ImDrawList* dl, ImVec2 left_middle, const char* key, float h) {
  const float size = h * 0.5f;
  const ImVec2 ts = HudMeasure(size, key);
  const float w = std::max(h, ts.x + h * 0.55f);
  const ImVec2 a(left_middle.x, left_middle.y - h / 2);
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), ChipFill(), h / 2);
  HudText(dl, size, ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2), White(), key);
  return w;
}

// A pill button for the layout editor's toolbar (which has no ImGui window):
// a dark chip, or white with dark text for the main action.
bool PillButton(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, bool primary) {
  const ImVec2 m = ImGui::GetIO().MousePos;
  const bool hovered = m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
  const ImU32 bg = primary ? (hovered ? C(226, 227, 230) : White()) : (hovered ? C(58, 59, 65) : ChipFill());
  dl->AddRectFilled(a, b, bg, (b.y - a.y) / 2);
  const float size = S(14);
  const ImVec2 ts = HudMeasure(size, label);
  HudText(dl, size, ImVec2((a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2), primary ? Fill() : White(), label);
  return hovered && ImGui::IsMouseClicked(0);
}

// --------------------------------------------------------------------------
// Automation window

bool g_picking = false;
int g_pick_step = -1;  // -1: a click point; otherwise the macro step to place
int g_drag_point = -1;

automation::Macro g_macro;
int g_macro_rev = -1;
bool g_macro_loaded = false;
int g_selected = -1;
char g_name[64] = "";
char g_key_buf[48] = "";
char g_text_buf[256] = "";
int g_buf_for = -2;  // the step g_key_buf / g_text_buf belong to
int g_tool_tab = 0;  // 0 autoclicker, 1 macros
bool g_more = false;  // the clicker's "random timing, limits, delay" section

bool OverMenuOrWindow() {
  ImGuiWindow* hovered = GImGui->HoveredWindow;
  if (hovered != nullptr && std::strcmp(hovered->RootWindow->Name, "##vox-menu") != 0) return true;
  return OverMenu(ImGui::GetIO().MousePos);
}

// The window's own controls, drawn like Roblox's: pills and chips, white
// when selected, a blue switch when on.

void Label(const char* text) {
  ImGui::PushFont(g_fonts.ui_bold);
  ImGui::PushStyleColor(ImGuiCol_Text, C4(179, 180, 182));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void Hint(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, C4(179, 180, 182));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// A chip: white with dark text when selected, dark otherwise.
bool Chip(const char* label, bool selected, float width, float height = 0) {
  if (height <= 0) height = S(32);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  const ImU32 bg = selected ? White() : (hovered ? C(58, 59, 65) : ChipFill());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + width, a.y + height), bg, height / 2);
  const char* end = ImGui::FindRenderedTextEnd(label);
  const ImVec2 ts = g_fonts.ui_bold->CalcTextSizeA(g_fonts.ui_bold->FontSize, FLT_MAX, 0, label, end);
  dl->AddText(g_fonts.ui_bold, g_fonts.ui_bold->FontSize,
              ImVec2(std::round(a.x + (width - ts.x) / 2), std::round(a.y + (height - ts.y) / 2)),
              selected ? Fill() : White(), label, end);
  return clicked;
}

// One choice out of a few, as a row of chips.
bool Segmented(std::string* value, const std::vector<std::pair<const char*, const char*>>& options) {
  const float gap = S(6), avail = ImGui::GetContentRegionAvail().x;
  const float w = std::floor((avail - gap * (options.size() - 1)) / options.size());
  bool changed = false;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, ImGui::GetStyle().ItemSpacing.y));
  for (size_t i = 0; i < options.size(); ++i) {
    if (i > 0) ImGui::SameLine();
    if (Chip(options[i].second, *value == options[i].first, w) && *value != options[i].first) {
      *value = options[i].first;
      changed = true;
    }
  }
  ImGui::PopStyleVar();
  return changed;
}

// A label at the left of the row, the chips filling the rest.
bool LabeledSegmented(const char* label, std::string* value,
                      const std::vector<std::pair<const char*, const char*>>& options) {
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float label_w = S(88), h = S(32);
  ImGui::PushFont(g_fonts.ui_bold);
  const ImVec2 ts = ImGui::CalcTextSize(label);
  ImGui::GetWindowDrawList()->AddText(ImVec2(start.x, std::round(start.y + (h - ts.y) / 2)), C(179, 180, 182), label);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(start.x + label_w, start.y));
  ImGui::PushID(label);
  ImGui::BeginGroup();
  const bool changed = Segmented(value, options);
  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

// A label with a switch at the right end of the row.
bool Switch(const char* label, bool* value) {
  const float w = S(44), h = S(26);
  const float avail = ImGui::GetContentRegionAvail().x;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  ImGui::PushFont(g_fonts.ui_bold);
  const ImVec2 ts = ImGui::CalcTextSize(label);
  ImGui::GetWindowDrawList()->AddText(ImVec2(start.x, std::round(start.y + (h - ts.y) / 2)), White(), label);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(start);
  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("switch", ImVec2(avail, h));
  ImGui::PopID();
  if (clicked) *value = !*value;
  const ImVec2 a(start.x + avail - w, start.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), *value ? Accent() : C(58, 59, 65), h / 2);
  const float r = h / 2 - S(3);
  dl->AddCircleFilled(ImVec2(*value ? a.x + w - h / 2 : a.x + h / 2, a.y + h / 2), r, White(), 24);
  return clicked;
}

// A thin slider: the label and value above, a white bar and knob below.
bool ThinSlider(const char* label, double* value, double lo, double hi, const char* format, bool log = false) {
  const float avail = ImGui::GetContentRegionAvail().x;
  char text[64];
  std::snprintf(text, sizeof(text), format, *value);
  ImGui::PushFont(g_fonts.ui_bold);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 vs = ImGui::CalcTextSize(text);
  ImGui::GetWindowDrawList()->AddText(ImVec2(start.x, start.y), C(179, 180, 182), label);
  ImGui::GetWindowDrawList()->AddText(ImVec2(start.x + avail - vs.x, start.y), White(), text);
  ImGui::PopFont();
  const float row = g_fonts.ui_bold->FontSize + S(8), h = S(20);
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + row));
  ImGui::PushID(label);
  ImGui::InvisibleButton("track", ImVec2(avail, h));
  ImGui::PopID();
  const auto to_t = [&](double v) {
    return log ? std::log(std::max(v, lo) / lo) / std::log(hi / lo) : (v - lo) / (hi - lo);
  };
  bool changed = false;
  if (ImGui::IsItemActive()) {
    const double t = std::clamp((ImGui::GetIO().MousePos.x - start.x) / avail, 0.0f, 1.0f);
    const double v = log ? lo * std::pow(hi / lo, t) : lo + t * (hi - lo);
    changed = v != *value;
    *value = v;
  }
  const float t = static_cast<float>(std::clamp(to_t(*value), 0.0, 1.0));
  const float y = start.y + row + h / 2;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(start.x, y - S(2)), ImVec2(start.x + avail, y + S(2)), ChipFill(), S(2));
  dl->AddRectFilled(ImVec2(start.x, y - S(2)), ImVec2(start.x + avail * t, y + S(2)), White(), S(2));
  dl->AddCircleFilled(ImVec2(start.x + std::clamp(avail * t, h / 2, avail - h / 2), y), h / 2, White(), 24);
  return changed;
}

// The big action: white with dark text; while it runs, dark with a dot in
// `dot` (and the hotkey as a chip either way).
bool ActionPill(const char* label, bool running, ImU32 dot, const std::string& key, float width) {
  const float h = S(44);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool disabled = GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = running ? (hovered ? C(58, 59, 65) : ChipFill()) : (hovered ? C(226, 227, 230) : White());
  dl->AddRectFilled(a, ImVec2(a.x + width, a.y + h), disabled ? C(42, 43, 48, 0.6f) : bg, h / 2);
  const char* end = ImGui::FindRenderedTextEnd(label);
  const float size = S(16);
  const ImVec2 ts = HudMeasure(size, std::string(label, end).c_str());
  const bool has_key = !key.empty() && key != "off";
  const float key_w = has_key ? HudMeasure(S(12), key.c_str()).x + S(14) : 0;
  const float total = S(10) + S(8) + ts.x + (has_key ? S(8) + key_w : 0);
  float x = a.x + (width - total) / 2;
  const float cy = a.y + h / 2;
  const ImU32 ink = disabled ? C(140, 142, 146) : running ? White() : Fill();
  dl->AddCircleFilled(ImVec2(x + S(5), cy), S(5), dot, 16);
  x += S(18);
  HudText(dl, size, ImVec2(x, cy - ts.y / 2), ink, std::string(label, end).c_str());
  x += ts.x + S(8);
  if (has_key) {
    const ImVec2 ks = HudMeasure(S(12), key.c_str());
    dl->AddRectFilled(ImVec2(x, cy - S(11)), ImVec2(x + key_w, cy + S(11)), running ? C(58, 59, 65) : C(227, 228, 230),
                      S(11));
    HudText(dl, S(12), ImVec2(x + (key_w - ks.x) / 2, cy - ks.y / 2), running ? White() : Fill(), key.c_str());
  }
  return clicked && !disabled;
}

// A dark row that opens a section, with a chevron.
bool Expander(const char* label, bool* open) {
  const float avail = ImGui::GetContentRegionAvail().x, h = S(36);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(avail, h));
  if (clicked) *open = !*open;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + avail, a.y + h), ImGui::IsItemHovered() ? ChipFill() : C(31, 32, 37), h / 2);
  ImGui::PushFont(g_fonts.ui_bold);
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(a.x + S(14), std::round(a.y + (h - ts.y) / 2)), White(), label);
  ImGui::PopFont();
  const ImVec2 c(a.x + avail - S(18), a.y + h / 2);
  const float r = S(4);
  if (*open) {
    dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - r, c.y - r / 2), ImVec2(c.x, c.y + r / 2),
                                          ImVec2(c.x + r, c.y - r / 2)}.data(), 3, White(), 0, S(2));
  } else {
    dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - r / 2, c.y - r), ImVec2(c.x + r / 2, c.y),
                                          ImVec2(c.x - r / 2, c.y + r)}.data(), 3, White(), 0, S(2));
  }
  return clicked;
}

// An outlined pill, for less important actions ("+ Add point").
bool GhostPill(const char* label, float width, float height = 0) {
  if (height <= 0) height = S(32);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (ImGui::IsItemHovered()) dl->AddRectFilled(a, ImVec2(a.x + width, a.y + height), C(31, 32, 37), height / 2);
  dl->AddRect(a, ImVec2(a.x + width, a.y + height), C(58, 59, 65), height / 2, 0, S(1.5f));
  const char* end = ImGui::FindRenderedTextEnd(label);
  const ImVec2 ts = g_fonts.ui_bold->CalcTextSizeA(g_fonts.ui_bold->FontSize, FLT_MAX, 0, label, end);
  dl->AddText(g_fonts.ui_bold, g_fonts.ui_bold->FontSize,
              ImVec2(std::round(a.x + (width - ts.x) / 2), std::round(a.y + (height - ts.y) / 2)), White(), label, end);
  return clicked;
}

void ClickerTab(Config& cfg) {
  const automation::Status st = automation::GetStatus();
  const float avail = ImGui::GetContentRegionAvail().x;
  if (ActionPill(st.clicking ? "Stop clicking" : "Start clicking", st.clicking, st.clicking ? Accent() : Fill(),
                 cfg.key_clicker, avail)) {
    automation::ToggleClicker();
  }
  char note[64];
  if (st.clicking && st.countdown > 0) {
    std::snprintf(note, sizeof(note), "Starting in %.1f s", st.countdown);
  } else if (st.clicking) {
    std::snprintf(note, sizeof(note), "%d clicks \xC2\xB7 %.1f s", st.clicks, st.elapsed);
  } else if (cfg.key_clicker_mode == "hold" && cfg.key_clicker != "off") {
    std::snprintf(note, sizeof(note), "Hold %s to click",
                  cfg.key_clicker == "Mouse4" ? "Mouse Back" : cfg.key_clicker == "Mouse5" ? "Mouse Forward"
                                                                                           : cfg.key_clicker.c_str());
  } else {
    std::snprintf(note, sizeof(note), "Clicks go only to the Roblox window");
  }
  const float nw = ImGui::CalcTextSize(note).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - nw) / 2);
  ImGui::PushStyleColor(ImGuiCol_Text, C4(179, 180, 182));
  ImGui::TextUnformatted(note);
  ImGui::PopStyleColor();

  LabeledSegmented("Hotkey", &cfg.key_clicker_mode, {{"toggle", "Press to toggle"}, {"hold", "Hold to click"}});
  LabeledSegmented("Button", &cfg.click_button, {{"left", "Left"}, {"right", "Right"}, {"middle", "Middle"}});
  LabeledSegmented("Click", &cfg.click_type, {{"single", "Single"}, {"double", "Double"}, {"hold", "Hold"}});
  if (cfg.click_type != "hold") ThinSlider("Speed", &cfg.click_cps, 1, 100, "%.1f clicks/s", true);

  Label("Where to click");
  Segmented(&cfg.click_target, {{"mouse", "Follow mouse"}, {"points", "Fixed points"}});
  if (cfg.click_target == "points") {
    auto points = automation::ParsePoints(cfg.click_points);
    if (points.empty()) Hint("No points yet. Add one, then click where it should go.");
    const overlay::Metrics m = overlay::GetMetrics();
    int remove = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < points.size(); ++i) {
      const float h = S(32);
      const ImVec2 a = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(a, ImVec2(a.x + avail, a.y + h), C(31, 32, 37), h / 2);
      char n[24], where[48];
      std::snprintf(n, sizeof(n), "%zu", i + 1);
      std::snprintf(where, sizeof(where), "%.0f, %.0f", points[i].first * m.window_w, points[i].second * m.window_h);
      const ImVec2 c(a.x + S(21), a.y + h / 2);
      dl->AddCircle(c, S(10), White(), 20, S(2));
      const ImVec2 ns = g_fonts.ui_bold->CalcTextSizeA(S(12), FLT_MAX, 0, n);
      dl->AddText(g_fonts.ui_bold, S(12), ImVec2(std::round(c.x - ns.x / 2), std::round(c.y - ns.y / 2)), White(), n);
      dl->AddText(ImVec2(a.x + S(40), std::round(a.y + (h - ImGui::GetFontSize()) / 2)), White(), where);
      ImGui::SetCursorScreenPos(ImVec2(a.x + avail - S(30), a.y + S(3)));
      ImGui::PushID(static_cast<int>(i));
      if (Chip("\xC3\x97", false, S(26), S(26))) remove = static_cast<int>(i);
      ImGui::PopID();
      ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
      ImGui::Dummy(ImVec2(avail, 0));
    }
    if (remove >= 0) {
      points.erase(points.begin() + remove);
      cfg.click_points = automation::FormatPoints(points);
    }
    const float half = (avail - S(6)) / 2;
    if (GhostPill(g_picking && g_pick_step < 0 ? "Click the screen..." : "+ Add point", half)) {
      g_picking = !g_picking || g_pick_step >= 0;
      g_pick_step = -1;
    }
    ImGui::SameLine(0, S(6));
    if (GhostPill("Clear", half) && !points.empty()) cfg.click_points.clear();
    Switch("Move my real cursor", &cfg.click_move_cursor);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the game gets the click there, your cursor stays put.");
  }

  Expander("Press length, randomness, limits", &g_more);
  if (g_more) {
    if (cfg.click_type != "hold") ThinSlider("Press length", &cfg.click_hold_ms, 0, 200, "%.0f ms");
    ThinSlider("Timing jitter", &cfg.click_jitter, 0, 50, "%.0f%%");
    ThinSlider("Position spread", &cfg.click_spread, 0, 40, "%.0f px");
    ThinSlider("Stop after", &cfg.click_duration, 0, 600, cfg.click_duration >= 1 ? "%.0f s" : "never");
    if (cfg.click_duration < 1) cfg.click_duration = 0;
    ThinSlider("Click limit", &cfg.click_limit, 0, 10000, cfg.click_limit >= 1 ? "%.0f clicks" : "none");
    if (cfg.click_limit < 1) cfg.click_limit = 0;
    ThinSlider("Start delay", &cfg.click_delay, 0, 10, "%.1f s");
  }
}

void SyncStepBuffers(const automation::Step& s, int index) {
  if (g_buf_for == index) return;
  g_buf_for = index;
  std::snprintf(g_key_buf, sizeof(g_key_buf), "%s", SDL_GetScancodeName(static_cast<SDL_Scancode>(s.code)));
  std::snprintf(g_text_buf, sizeof(g_text_buf), "%s", s.text.c_str());
}

// Edits the selected step; true when it changed.
bool StepEditor(automation::Step& s, int index) {
  using automation::Step;
  SyncStepBuffers(s, index);
  bool changed = false;
  static const char* kKinds[] = {"Key down", "Key up", "Key press", "Mouse down", "Mouse up", "Click", "Move mouse",
                                 "Wait", "Type text"};
  int kind = s.kind;
  if (ImGui::Combo("Action", &kind, kKinds, IM_ARRAYSIZE(kKinds))) {
    s.kind = static_cast<Step::Kind>(kind);
    if (s.kind == Step::kWait && s.ms <= 0) s.ms = 100;
    changed = true;
  }
  switch (s.kind) {
    case Step::kKeyDown:
    case Step::kKeyUp:
    case Step::kKeyTap: {
      if (ImGui::InputText("Key", g_key_buf, sizeof(g_key_buf))) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(g_key_buf);
        if (sc != SDL_SCANCODE_UNKNOWN) {
          s.code = sc;
          changed = true;
        }
      }
      if (s.code == SDL_SCANCODE_UNKNOWN || SDL_GetScancodeFromName(g_key_buf) == SDL_SCANCODE_UNKNOWN) {
        Hint("Key names: W, Space, Left Shift, Return, 1, F5 ...");
      }
      break;
    }
    case Step::kMouseDown:
    case Step::kMouseUp:
    case Step::kClick:
    case Step::kMove: {
      if (s.kind != Step::kMove) {
        std::string button = s.code == 1 ? "right" : s.code == 2 ? "middle" : "left";
        if (Segmented(&button, {{"left", "Left"}, {"right", "Right"}, {"middle", "Middle"}})) {
          s.code = button == "right" ? 1 : button == "middle" ? 2 : 0;
          changed = true;
        }
        bool at_cursor = s.x < 0;
        if (Switch("At the cursor", &at_cursor)) {
          s.x = s.y = at_cursor ? -1.0f : 0.5f;
          changed = true;
        }
      }
      if (s.x >= 0) {
        const overlay::Metrics m = overlay::GetMetrics();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("At %.0f, %.0f", s.x * m.window_w, s.y * m.window_h);
        ImGui::SameLine();
        if (GhostPill(g_picking && g_pick_step == index ? "Click the screen..." : "Pick", S(150))) {
          g_picking = !(g_picking && g_pick_step == index);
          g_pick_step = index;
        }
      }
      break;
    }
    case Step::kWait: {
      int ms = s.ms;
      if (ImGui::InputInt("Milliseconds", &ms, 10, 100)) {
        s.ms = std::clamp(ms, 0, 600000);
        changed = true;
      }
      break;
    }
    case Step::kText:
      if (ImGui::InputText("Text", g_text_buf, sizeof(g_text_buf))) {
        s.text = g_text_buf;
        changed = true;
      }
      Hint("Typed into whatever has focus in Roblox (open the chat first).");
      break;
  }
  return changed;
}

void MacrosTab(Config& cfg) {
  using automation::Step;
  if (!g_macro_loaded) {
    g_macro_loaded = true;
    std::snprintf(g_name, sizeof(g_name), "%s", cfg.macro_name.c_str());
    automation::Macro m;
    if (!cfg.macro_name.empty() && automation::LoadMacro(cfg.macro_name, &m)) automation::SetCurrent(m);
  }
  if (automation::Revision() != g_macro_rev) {
    g_macro_rev = automation::Revision();
    g_macro = automation::Current();
    g_selected = -1;
    g_buf_for = -2;
  }
  const automation::Status st = automation::GetStatus();
  const float avail = ImGui::GetContentRegionAvail().x, half = (avail - S(8)) / 2;
  if (ActionPill(st.recording ? "Stop" : "Record", st.recording, Alert(), cfg.key_record, half)) {
    automation::ToggleRecording();
  }
  ImGui::SameLine(0, S(8));
  ImGui::BeginDisabled(st.recording || (g_macro.steps.empty() && !st.playing));
  if (ActionPill(st.playing ? "Stop" : "Play", st.playing, st.playing ? Accent() : Fill(), cfg.key_macro, half)) {
    automation::TogglePlayback();
  }
  ImGui::EndDisabled();
  if (st.recording) {
    Hint(("Recording: " + std::to_string(st.recorded) + " steps. Play in the game, then stop.").c_str());
  } else if (st.playing) {
    char text[64];
    std::snprintf(text, sizeof(text), "Step %d of %d, loop %d", st.step, st.steps, st.loop + 1);
    Hint(text);
  }

  int repeat = static_cast<int>(cfg.macro_repeat);
  if (ImGui::InputInt("Repeat", &repeat)) cfg.macro_repeat = std::clamp(repeat, 0, 100000);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 loops until you stop it");
  ThinSlider("Speed", &cfg.macro_speed, 0.25, 4.0, "%.2fx", true);
  Switch("Record mouse movement", &cfg.macro_record_moves);

  char title[32];
  std::snprintf(title, sizeof(title), "Steps (%zu)", g_macro.steps.size());
  Label(title);
  bool changed = false;
  ImGui::BeginChild("steps", ImVec2(0, S(150)), ImGuiChildFlags_None);
  for (size_t i = 0; i < g_macro.steps.size(); ++i) {
    char row[160];
    std::snprintf(row, sizeof(row), "%3zu  %s", i + 1, automation::DescribeStep(g_macro.steps[i]).c_str());
    const bool now = st.playing && static_cast<int>(i) + 1 == st.step;
    if (now) ImGui::PushStyleColor(ImGuiCol_Text, C4(1, 153, 237));
    if (ImGui::Selectable(row, g_selected == static_cast<int>(i))) g_selected = static_cast<int>(i);
    if (now) ImGui::PopStyleColor();
  }
  if (g_macro.steps.empty()) Hint("Record one, or add steps below.");
  ImGui::EndChild();

  // Adding steps after the selected one.
  auto add = [&](Step s) {
    const int at = g_selected >= 0 ? g_selected + 1 : static_cast<int>(g_macro.steps.size());
    g_macro.steps.insert(g_macro.steps.begin() + at, s);
    g_selected = at;
    g_buf_for = -2;
    changed = true;
  };
  const float quarter = (avail - S(6) * 3) / 4;
  if (Chip("+ Key", false, quarter)) {
    Step s;
    s.kind = Step::kKeyTap;
    s.code = SDL_SCANCODE_E;
    add(s);
  }
  ImGui::SameLine(0, S(6));
  if (Chip("+ Click", false, quarter)) {
    Step s;
    s.kind = Step::kClick;
    add(s);
  }
  ImGui::SameLine(0, S(6));
  if (Chip("+ Wait", false, quarter)) {
    Step s;
    s.ms = 250;
    add(s);
  }
  ImGui::SameLine(0, S(6));
  if (Chip("+ Text", false, quarter)) {
    Step s;
    s.kind = Step::kText;
    s.text = "gg";
    add(s);
  }
  if (g_selected >= 0 && g_selected < static_cast<int>(g_macro.steps.size())) {
    const float third = (avail - S(6) * 2) / 3;
    if (Chip("Move up", false, third) && g_selected > 0) {
      std::swap(g_macro.steps[g_selected], g_macro.steps[g_selected - 1]);
      --g_selected;
      changed = true;
    }
    ImGui::SameLine(0, S(6));
    if (Chip("Move down", false, third) && g_selected + 1 < static_cast<int>(g_macro.steps.size())) {
      std::swap(g_macro.steps[g_selected], g_macro.steps[g_selected + 1]);
      ++g_selected;
      changed = true;
    }
    ImGui::SameLine(0, S(6));
    if (Chip("Delete", false, third)) {
      g_macro.steps.erase(g_macro.steps.begin() + g_selected);
      g_selected = std::min(g_selected, static_cast<int>(g_macro.steps.size()) - 1);
      g_buf_for = -2;
      changed = true;
    }
  }
  if (g_selected >= 0 && g_selected < static_cast<int>(g_macro.steps.size())) {
    changed |= StepEditor(g_macro.steps[g_selected], g_selected);
  }

  ImGui::SetNextItemWidth(avail - S(78));
  ImGui::InputTextWithHint("##name", "Macro name", g_name, sizeof(g_name));
  ImGui::SameLine(0, S(8));
  if (Chip("Save", true, S(70))) {
    const std::string name = automation::SafeName(g_name);
    if (!name.empty() && automation::SaveMacro(name, g_macro)) {
      cfg.macro_name = name;
      std::snprintf(g_name, sizeof(g_name), "%s", name.c_str());
    }
  }
  const std::vector<std::string> saved = automation::ListMacros();
  if (!saved.empty()) {
    ImGui::SetNextItemWidth(avail - S(78));
    if (ImGui::BeginCombo("##saved", cfg.macro_name.empty() ? "Load a saved macro" : cfg.macro_name.c_str())) {
      for (const std::string& name : saved) {
        if (ImGui::Selectable(name.c_str(), name == cfg.macro_name)) {
          automation::Macro m;
          if (automation::LoadMacro(name, &m)) {
            g_macro = m;
            g_selected = -1;
            g_buf_for = -2;
            changed = true;
            cfg.macro_name = name;
            std::snprintf(g_name, sizeof(g_name), "%s", name.c_str());
          }
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, S(8));
    if (Chip("Delete##file", false, S(70)) && !cfg.macro_name.empty()) {
      automation::DeleteMacro(cfg.macro_name);
      cfg.macro_name.clear();
    }
  }
  if (changed) automation::SetCurrent(g_macro);
}

// ImGui's own widgets in the window (fields, dropdowns, the step list) in
// the same look: pill fields on #1F2025, white text.
int PushToolStyle(const Config& cfg) {
  const float field_h = S(32);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(16));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, field_h / 2);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, field_h / 2);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(12));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(16));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), (field_h - g_fonts.ui->FontSize) / 2));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(S(8), S(6)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(6));
  ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
  const std::pair<ImGuiCol, ImVec4> colors[] = {
      {ImGuiCol_Text, C4(255, 255, 255)},
      {ImGuiCol_TextDisabled, C4(140, 142, 146)},
      {ImGuiCol_WindowBg, C4(18, 18, 21, WindowOpacity(cfg))},
      {ImGuiCol_ChildBg, C4(31, 32, 37)},
      {ImGuiCol_PopupBg, C4(31, 32, 37, 0.98f)},
      {ImGuiCol_Border, C4(255, 255, 255, 0.0f)},
      {ImGuiCol_FrameBg, C4(31, 32, 37)},
      {ImGuiCol_FrameBgHovered, C4(42, 43, 48)},
      {ImGuiCol_FrameBgActive, C4(42, 43, 48)},
      {ImGuiCol_Button, C4(42, 43, 48)},
      {ImGuiCol_ButtonHovered, C4(58, 59, 65)},
      {ImGuiCol_ButtonActive, C4(58, 59, 65)},
      {ImGuiCol_Header, C4(42, 43, 48)},
      {ImGuiCol_HeaderHovered, C4(42, 43, 48)},
      {ImGuiCol_HeaderActive, C4(58, 59, 65)},
      {ImGuiCol_CheckMark, C4(255, 255, 255)},
      {ImGuiCol_Separator, C4(255, 255, 255, 0.08f)},
      {ImGuiCol_ScrollbarBg, C4(0, 0, 0, 0)},
      {ImGuiCol_ScrollbarGrab, C4(85, 87, 94)},
      {ImGuiCol_ScrollbarGrabHovered, C4(120, 122, 128)},
      {ImGuiCol_ScrollbarGrabActive, C4(150, 152, 158)},
      {ImGuiCol_TextSelectedBg, C4(1, 153, 237, 0.45f)},
      {ImGuiCol_NavCursor, C4(255, 255, 255)},
  };
  for (const auto& [col, value] : colors) ImGui::PushStyleColor(col, value);
  return IM_ARRAYSIZE(colors);
}

void PopToolStyle(int colors) {
  ImGui::PopStyleColor(colors);
  ImGui::PopStyleVar(13);
}

}  // namespace

// --------------------------------------------------------------------------

void CountFrame() {
  ++g_fps_frames;
  const Clock::time_point now = Clock::now();
  const float elapsed = std::chrono::duration<float>(now - g_fps_since).count();
  if (elapsed >= 0.5f) {
    g_fps = g_fps_frames / elapsed;
    g_frame_ms = elapsed * 1000.0f / g_fps_frames;
    g_fps_frames = 0;
    g_fps_since = now;
  }
}

bool HudVisible(const Config& cfg) {
  const automation::Status st = automation::GetStatus();
  return cfg.hud_fps.on || cfg.hud_ping.on || cfg.hud_server.on || cfg.hud_keys.on || cfg.tools_open ||
         st.clicking || st.recording || st.playing || g_picking;
}

void CountVrFrame() {
  ++g_vr_frames;
  const Clock::time_point now = Clock::now();
  g_vr_last = now;
  const float elapsed = std::chrono::duration<float>(now - g_vr_since).count();
  if (elapsed >= 0.5f) {
    g_vr_fps = g_vr_frames / elapsed;
    g_vr_frame_ms = elapsed * 1000.0f / g_vr_frames;
    g_vr_frames = 0;
    g_vr_since = now;
  }
}

float DrawStatsPill(ImDrawList* dl, const Config& cfg, ImVec2 origin, float u) {
  float x = origin.x;
  bool any = false;
  for (const int i : {kFps, kPing, kServer}) {
    if (!(cfg.*kWidgets[i].field).on) continue;
    if (any) x += std::round(kGap * u);
    any = true;
    x += WidgetSize(cfg, i, u).x;
  }
  if (!any) return 0;
  dl->AddRectFilled(origin, ImVec2(x, origin.y + kPill * u), Fill(), kPill * u / 2);
  x = origin.x;
  for (const int i : {kFps, kPing, kServer}) {
    if (!(cfg.*kWidgets[i].field).on) continue;
    DrawWidgetContent(dl, cfg, i, ImVec2(x, origin.y), u);
    x += WidgetSize(cfg, i, u).x + std::round(kGap * u);
  }
  return x - std::round(kGap * u) - origin.x;
}

void AdoptSideBySide(Config& cfg) {
  if (cfg.hud_joins_adopted || g_fonts.hud == nullptr) return;
  cfg.hud_joins_adopted = true;
  std::vector<Placed> row = LayoutWidgets(cfg, ImGui::GetIO().DisplaySize);
  row.erase(std::remove_if(row.begin(), row.end(), [](const Placed& p) { return p.which == kKeys; }), row.end());
  std::sort(row.begin(), row.end(), [](const Placed& a, const Placed& b) { return a.min.x < b.min.x; });
  for (size_t i = 0; i + 1 < row.size(); ++i) {
    for (size_t j = i + 1; j < row.size(); ++j) {
      const Placed &l = row[i], &r = row[j];
      HudWidget& right = cfg.*kWidgets[r.which].field;
      const float gap = r.min.x - (l.min.x + l.size.x);
      // Side by side then; FPS and ping have since grown to fit 3 digits,
      // so allow some overlap.
      if (std::fabs(l.min.y - r.min.y) > 1.5f || std::fabs(l.size.y - r.size.y) > 1.5f || gap < -S(20) ||
          gap > kGap * l.u + S(2) || !right.attach.empty()) {
        continue;
      }
      bool taken = false;  // something already joins onto `l`
      for (int k = 0; k < kNumWidgets; ++k) taken |= k != l.which && AttachedTo(cfg, k) == l.which;
      if (taken) continue;
      right.attach = kWidgets[l.which].id;
      if (AttachedTo(cfg, r.which) != l.which) right.attach.clear();  // would loop
      break;
    }
  }
}

void DrawHud(const Config& cfg) {
  if (g_fonts.hud == nullptr) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  // One pill per group, then what is in each.
  const std::vector<Placed> placed = LayoutWidgets(cfg, screen);
  std::map<int, std::pair<ImVec2, ImVec2>> pills;
  for (const Placed& p : placed) {
    if (p.which == kKeys) continue;
    const ImVec2 max(p.min.x + p.size.x, p.min.y + p.size.y);
    auto [it, fresh] = pills.try_emplace(p.group, p.min, max);
    if (!fresh) {
      it->second.first = ImMin(it->second.first, p.min);
      it->second.second = ImMax(it->second.second, max);
    }
  }
  for (const auto& [group, box] : pills) {
    dl->AddRectFilled(box.first, box.second, Fill(OverlayOpacity(cfg)), (box.second.y - box.first.y) / 2);
  }
  for (const Placed& p : placed) {
    if (p.which == kKeys) DrawKeys(dl, cfg, p.min, p.u);
    else DrawWidgetContent(dl, cfg, p.which, p.min, p.u);
  }
}

void ResetLayout(Config& cfg) {
  const Config defaults;
  for (const WidgetDef& def : kWidgets) {
    HudWidget& w = cfg.*def.field;
    const bool on = w.on;
    w = defaults.*def.field;
    w.on = on;
  }
}

bool LayoutEditor(Config& cfg) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = io.DisplaySize;
  const ImVec2 mouse = io.MousePos;
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), screen, C(0, 0, 0, 0.3f));

  // Boxes of the overlays that are on, where they are drawn.
  struct Box {
    int i;
    ImVec2 min, size;
    float u;
  };
  std::vector<Box> boxes;
  for (const Placed& p : LayoutWidgets(cfg, screen)) boxes.push_back({p.which, p.min, p.size, p.u});
  // Whether `i` is joined (directly or down a chain) onto `to`, so moves with it.
  auto follows = [&](int i, int to) {
    for (int at = AttachedTo(cfg, i), steps = 0; at >= 0 && steps <= kNumWidgets; at = AttachedTo(cfg, at), ++steps) {
      if (at == to) return true;
    }
    return false;
  };

  // The toolbar: a pill in the top-bar row.
  const char* hint = boxes.empty() ? "Turn on an overlay in the HUD tab first"
                                   : "Drag to move \xC2\xB7 drag a corner or scroll to resize \xC2\xB7 Shift: no snapping";
  const float h = S(44), bh = S(32);
  const ImVec2 title_size = HudMeasure(S(16), "Layout"), hint_size = HudMeasure(S(14), hint);
  const float snap_w = S(84), reset_w = HudMeasure(S(14), "Reset").x + S(28), done_w = HudMeasure(S(14), "Done").x + S(32);
  const float tb_w = S(18) + title_size.x + S(8) + hint_size.x + S(14) + snap_w + S(8) + reset_w + S(8) + done_w + S(6);
  const ImVec2 t0(std::round((screen.x - tb_w) / 2), S(12)), t1(t0.x + tb_w, t0.y + h);
  dl->AddRectFilled(t0, t1, Fill(), h / 2);
  const bool over_toolbar = mouse.x >= t0.x && mouse.x < t1.x && mouse.y >= t0.y && mouse.y < t1.y;
  float x = t0.x + S(18);
  HudText(dl, S(16), ImVec2(x, t0.y + (h - title_size.y) / 2), White(), "Layout");
  x += title_size.x + S(8);
  HudText(dl, S(14), ImVec2(x, t0.y + (h - hint_size.y) / 2), Units(), hint);
  x += hint_size.x + S(14);
  const float by = t0.y + (h - bh) / 2;
  bool stay = true;
  // Snap: a chip with a small switch in it.
  {
    const ImVec2 a(x, by), b(x + snap_w, by + bh);
    if (PillButton(dl, a, b, "", false)) cfg.hud_snap = !cfg.hud_snap;
    const ImVec2 ts = HudMeasure(S(14), "Snap");
    HudText(dl, S(14), ImVec2(a.x + S(12), a.y + (bh - ts.y) / 2), White(), "Snap");
    const ImVec2 s0(b.x - S(12) - S(30), a.y + (bh - S(18)) / 2);
    dl->AddRectFilled(s0, ImVec2(s0.x + S(30), s0.y + S(18)), cfg.hud_snap ? Accent() : C(85, 87, 94), S(9));
    dl->AddCircleFilled(ImVec2(cfg.hud_snap ? s0.x + S(21) : s0.x + S(9), s0.y + S(9)), S(7), White(), 16);
    x = b.x + S(8);
  }
  if (PillButton(dl, ImVec2(x, by), ImVec2(x + reset_w, by + bh), "Reset", false)) ResetLayout(cfg);
  x += reset_w + S(8);
  if (PillButton(dl, ImVec2(x, by), ImVec2(x + done_w, by + bh), "Done", true)) stay = false;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) stay = false;

  // Picking up a box: the topmost one under the mouse.
  const float handle = S(14);
  int hovered = -1;
  bool on_handle = false;
  for (int b = static_cast<int>(boxes.size()) - 1; b >= 0 && !over_toolbar; --b) {
    const Box& box = boxes[b];
    const ImVec2 max(box.min.x + box.size.x, box.min.y + box.size.y);
    const bool in_handle = mouse.x >= max.x - handle && mouse.x < max.x + handle / 2 && mouse.y >= max.y - handle &&
                           mouse.y < max.y + handle / 2;
    if (in_handle || (mouse.x >= box.min.x && mouse.x < max.x && mouse.y >= box.min.y && mouse.y < max.y)) {
      hovered = b;
      on_handle = in_handle;
      break;
    }
  }
  if (ImGui::IsMouseClicked(0) && hovered >= 0) {
    const Box& box = boxes[hovered];
    g_drag = Drag{};
    g_drag.widget = box.i;
    g_drag.resize = on_handle;
    g_drag.grab = ImVec2(mouse.x - box.min.x, mouse.y - box.min.y);
    g_drag.start_min = box.min;
    g_drag.start_size = box.size;
    g_drag.start_scale = (cfg.*kWidgets[box.i].field).scale;
    // Picking up a joined overlay takes it out of its pill, where it is;
    // anything joined after it comes along.
    HudWidget& w = cfg.*kWidgets[box.i].field;
    if (!w.attach.empty()) {
      w.attach.clear();
      SetWidgetPos(&w, box.min, box.size, screen);
    }
  }
  if (ImGui::IsMouseClicked(1) && hovered >= 0) (cfg.*kWidgets[boxes[hovered].i].field).on = false;
  if (hovered >= 0 && io.MouseWheel != 0 && g_drag.widget < 0) {
    HudWidget& w = cfg.*kWidgets[boxes[hovered].i].field;
    w.scale = std::clamp(std::round((w.scale + io.MouseWheel * 0.1) * 20) / 20, 0.5, 3.0);
  }
  // Dropped where it would join a pill (shown while dragging), it joins.
  const auto has_follower = [&](int i) {
    for (int j = 0; j < kNumWidgets; ++j) {
      if (j != i && AttachedTo(cfg, j) == i) return true;
    }
    return false;
  };
  if (ImGui::IsMouseReleased(0) && g_drag.widget >= 0 && g_drag.join >= 0) {
    if (g_drag.join_after) (cfg.*kWidgets[g_drag.widget].field).attach = kWidgets[g_drag.join].id;
    else (cfg.*kWidgets[g_drag.join].field).attach = kWidgets[g_drag.widget].id;
  }
  if (!ImGui::IsMouseDown(0)) g_drag.widget = -1;

  if (g_drag.widget >= 0) {
    HudWidget& w = cfg.*kWidgets[g_drag.widget].field;
    g_drag.guides_x.clear();
    g_drag.guides_y.clear();
    const bool snap = cfg.hud_snap && !io.KeyShift;
    if (g_drag.resize) {
      // Corner drags scale the widget about its top-left.
      const float sx = (mouse.x - g_drag.start_min.x) / std::max(1.0f, g_drag.start_size.x);
      const float sy = (mouse.y - g_drag.start_min.y) / std::max(1.0f, g_drag.start_size.y);
      double scale = g_drag.start_scale * std::max(sx, sy);
      scale = std::clamp(std::round(scale * 20) / 20, 0.5, 3.0);
      w.scale = scale;
      SetWidgetPos(&w, g_drag.start_min, WidgetSize(cfg, g_drag.widget, Unit(w)), screen);
    } else {
      const ImVec2 size = WidgetSize(cfg, g_drag.widget, Unit(w));
      float px = mouse.x - g_drag.grab.x, py = mouse.y - g_drag.grab.y;
      // Near the gap beside another text pill, it locks in there and joins
      // that pill when dropped. Generous, so a nearby guide never wins.
      g_drag.join = -1;
      const int d = g_drag.widget;
      const float reach = S(16), dgap = std::round(kGap * Unit(w));
      for (const Box& o : boxes) {
        if (!snap || d == kKeys || o.i == d || o.i == kKeys || follows(o.i, d)) continue;
        if (std::fabs(o.size.y - size.y) > 1.5f || std::fabs(py - o.min.y) > reach) continue;
        const float after = o.min.x + o.size.x + std::round(kGap * o.u);
        const float before = o.min.x - dgap - size.x;
        if (std::fabs(px - after) <= reach && !has_follower(o.i)) {
          g_drag.join = o.i;
          g_drag.join_after = true;
          px = after;
        } else if (std::fabs(px - before) <= reach && (cfg.*kWidgets[o.i].field).attach.empty() && !has_follower(d)) {
          g_drag.join = o.i;
          g_drag.join_after = false;
          px = before;
        } else {
          continue;
        }
        py = o.min.y;
        // Preview: the gap between them filled in, as one pill.
        const float g0 = g_drag.join_after ? o.min.x + o.size.x : px + size.x;
        const float g1 = g_drag.join_after ? px : o.min.x;
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(g0 - size.y / 2, py), ImVec2(g1 + size.y / 2, py + size.y),
                                                      Fill(OverlayOpacity(cfg)));
        break;
      }
      if (snap && g_drag.join < 0) {
        // Screen edges (Roblox's 16 / 12 px margins), the centre, the row
        // under the top bar, and the other overlays (and the 8 px gap
        // beside them, where pills join up).
        std::vector<float> xs = {S(16), screen.x / 2, screen.x - S(16)};
        std::vector<float> ys = {S(12), S(68), screen.y / 2, screen.y - S(16)};
        for (const Box& box : boxes) {
          if (box.i == g_drag.widget || follows(box.i, g_drag.widget)) continue;
          xs.insert(xs.end(), {box.min.x, box.min.x + box.size.x / 2, box.min.x + box.size.x});
          ys.insert(ys.end(), {box.min.y, box.min.y + box.size.y / 2, box.min.y + box.size.y});
          // The gap beside it, where dropping joins the two into one pill.
          const float gap = std::round(kGap * box.u);
          xs.insert(xs.end(), {box.min.x - gap, box.min.x + box.size.x + gap});
          ys.insert(ys.end(), {box.min.y - gap, box.min.y + box.size.y + gap});
        }
        const float gx = SnapAxis(&px, size.x, xs, S(9));
        const float gy = SnapAxis(&py, size.y, ys, S(9));
        if (gx >= 0) g_drag.guides_x.push_back(gx);
        if (gy >= 0) g_drag.guides_y.push_back(gy);
      }
      SetWidgetPos(&w, ImVec2(px, py), size, screen);
    }
  }
  // Guides pass behind the overlays (drawn after this, over the dimming).
  ImDrawList* under = ImGui::GetBackgroundDrawList();
  for (const float gx : g_drag.guides_x) {
    under->AddRectFilled(ImVec2(gx - S(1), 0), ImVec2(gx + S(1), screen.y), Accent());
  }
  for (const float gy : g_drag.guides_y) {
    under->AddRectFilled(ImVec2(0, gy - S(1)), ImVec2(screen.x, gy + S(1)), Accent());
  }

  // Outlines, names and resize handles over the (already drawn) overlays.
  const std::vector<Placed> drawn = LayoutWidgets(cfg, screen);  // after this frame's changes
  for (const Placed& p : drawn) {
    const int i = p.which;
    const int b = static_cast<int>(&p - drawn.data());
    const HudWidget& w = cfg.*kWidgets[i].field;
    const float u = p.u;
    const ImVec2 size = p.size;
    const ImVec2 a = p.min, z(a.x + size.x, a.y + size.y);
    const bool active = g_drag.widget == i || (g_drag.widget >= 0 && g_drag.join == i) ||
                        (g_drag.widget < 0 && hovered == b);
    const float round = i == kKeys ? kPill * u / 2 : size.y / 2;
    const float g = i == kKeys ? S(4) : S(1);
    dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(z.x + g, z.y + g), active ? Accent() : White(0.5f), round + g, 0,
                S(2));
    char label[48];
    std::snprintf(label, sizeof(label), "%s \xC2\xB7 %.0f%%", kWidgets[i].name, w.scale * 100);
    const float lh = active ? S(22) : S(20);
    const ImVec2 ls = HudMeasure(S(12), label);
    const float ly = a.y - S(6) - lh < S(60) ? z.y + S(6) : a.y - S(6) - lh;
    dl->AddRectFilled(ImVec2(a.x, ly), ImVec2(a.x + ls.x + S(16), ly + lh), active ? Accent() : Fill(0.85f), lh / 2);
    HudText(dl, S(12), ImVec2(a.x + S(8), ly + (lh - ls.y) / 2), White(), label);
    dl->AddCircleFilled(ImVec2(z.x, z.y), S(7), White(), 20);
    dl->AddCircle(ImVec2(z.x, z.y), S(7), Accent(), 20, S(2));
  }
  if (g_drag.widget >= 0 || hovered >= 0) {
    ImGui::SetMouseCursor(g_drag.resize || (g_drag.widget < 0 && on_handle) ? ImGuiMouseCursor_ResizeNWSE
                                                                            : ImGuiMouseCursor_ResizeAll);
  }
  if (!stay) g_drag = Drag{};
  return stay;
}

bool Picking() { return g_picking; }
void CancelPick() { g_picking = false; }

// A click point: a crosshair through a numbered dark circle; blue while
// it is held or hovered.
void Marker(ImDrawList* dl, ImVec2 c, const char* number, bool active) {
  const ImU32 ink = active ? Accent() : White();
  dl->AddRectFilled(ImVec2(c.x - S(1), c.y - S(20)), ImVec2(c.x + S(1), c.y + S(20)), ink);
  dl->AddRectFilled(ImVec2(c.x - S(20), c.y - S(1)), ImVec2(c.x + S(20), c.y + S(1)), ink);
  dl->AddCircleFilled(c, S(14), Fill(), 32);
  dl->AddCircle(c, S(14), ink, 32, S(2));
  const ImVec2 ts = HudMeasure(S(14), number);
  HudText(dl, S(14), ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), White(), number);
}

void DrawTools(Config& cfg, std::vector<ImVec4>* hot) {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 screen = io.DisplaySize;
  const ImVec2 mouse = io.MousePos;
  const bool show_points = cfg.click_target == "points" && (cfg.tools_open || overlay::MenuOpen() ||
                                                            automation::GetStatus().clicking);

  // Click-point markers: draggable, right-click removes.
  if (show_points) {
    auto points = automation::ParsePoints(cfg.click_points);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float r = S(16);
    int hovered = -1;
    for (int i = static_cast<int>(points.size()) - 1; i >= 0 && !g_picking; --i) {
      const ImVec2 c(points[i].first * screen.x, points[i].second * screen.y);
      if ((mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= r * r) {
        hovered = i;
        break;
      }
    }
    if (hovered >= 0 && OverMenuOrWindow()) hovered = -1;
    if (hovered >= 0 && ImGui::IsMouseClicked(0)) g_drag_point = hovered;
    if (hovered >= 0 && ImGui::IsMouseClicked(1)) {
      points.erase(points.begin() + hovered);
      cfg.click_points = automation::FormatPoints(points);
      hovered = -1;
    }
    if (!ImGui::IsMouseDown(0)) g_drag_point = -1;
    if (g_drag_point >= 0 && g_drag_point < static_cast<int>(points.size())) {
      points[g_drag_point] = {std::clamp(mouse.x / screen.x, 0.0f, 1.0f), std::clamp(mouse.y / screen.y, 0.0f, 1.0f)};
      cfg.click_points = automation::FormatPoints(points);
    }
    for (size_t i = 0; i < points.size(); ++i) {
      const ImVec2 c(std::round(points[i].first * screen.x), std::round(points[i].second * screen.y));
      char n[24];
      std::snprintf(n, sizeof(n), "%zu", i + 1);
      Marker(dl, c, n, static_cast<int>(i) == hovered || static_cast<int>(i) == g_drag_point);
      hot->push_back(ImVec4(c.x - S(20), c.y - S(20), c.x + S(20), c.y + S(20)));
    }
  }

  // Placing a point: a crosshair at the mouse and a tip beside it.
  if (g_picking) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(mouse.x - S(1), mouse.y - S(14)), ImVec2(mouse.x + S(1), mouse.y + S(14)), White());
    dl->AddRectFilled(ImVec2(mouse.x - S(14), mouse.y - S(1)), ImVec2(mouse.x + S(14), mouse.y + S(1)), White());
    const char* tip = "Click to place";
    const ImVec2 ts = HudMeasure(S(14), tip);
    const float h = S(32);
    const ImVec2 a(mouse.x + S(20), mouse.y + S(16));
    const float w = S(12) + ts.x + S(8) + HudMeasure(S(11), "Esc").x + S(14) + S(6);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Fill(), h / 2);
    HudText(dl, S(14), ImVec2(a.x + S(12), a.y + (h - ts.y) / 2), White(), tip);
    KeyChip(dl, ImVec2(a.x + S(12) + ts.x + S(8), a.y + h / 2), "Esc", S(22));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_picking = false;
    if (ImGui::IsMouseClicked(1)) g_picking = false;
    if (g_picking && ImGui::IsMouseClicked(0) && !OverMenuOrWindow()) {
      const float fx = std::clamp(mouse.x / screen.x, 0.0f, 1.0f), fy = std::clamp(mouse.y / screen.y, 0.0f, 1.0f);
      if (g_pick_step < 0) {
        auto points = automation::ParsePoints(cfg.click_points);
        points.emplace_back(fx, fy);
        cfg.click_points = automation::FormatPoints(points);
      } else if (g_pick_step < static_cast<int>(g_macro.steps.size())) {
        g_macro.steps[g_pick_step].x = fx;
        g_macro.steps[g_pick_step].y = fy;
        automation::SetCurrent(g_macro);
      }
      g_picking = false;
    }
  }

  if (!cfg.tools_open) {
    g_picking = false;
    return;
  }
  const int colors = PushToolStyle(cfg);
  ImGui::PushFont(g_fonts.ui);
  const float width = S(360);
  ImGui::SetNextWindowPos(ImVec2(std::round(static_cast<float>(cfg.tools_x) * screen.x),
                                 std::round(static_cast<float>(cfg.tools_y) * screen.y)),
                          ImGuiCond_Appearing);
  // Never taller than the screen below it; past that it scrolls.
  const float top = std::clamp(static_cast<float>(cfg.tools_y) * screen.y, 0.0f, screen.y - S(120));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, screen.y - top - S(16)));
  bool open = true;
  // No title bar: the window draws its own header, and drags from anywhere
  // that isn't a control.
  if (ImGui::Begin("Vox Automation", nullptr,
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                       ImGuiWindowFlags_NoCollapse)) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    // Snap to the screen edges while it is dragged.
    if (GImGui->MovingWindow != nullptr && GImGui->MovingWindow->RootWindow == window) {
      ImVec2 pos = window->Pos;
      const float m = S(16), t = S(14);
      if (std::fabs(pos.x - m) < t) pos.x = m;
      if (std::fabs(pos.y - S(68)) < t) pos.y = S(68);
      if (std::fabs(pos.x + window->Size.x - (screen.x - m)) < t) pos.x = screen.x - m - window->Size.x;
      if (std::fabs(pos.y + window->Size.y - (screen.y - m)) < t) pos.y = screen.y - m - window->Size.y;
      if (pos.x != window->Pos.x || pos.y != window->Pos.y) ImGui::SetWindowPos(pos);
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Header: cursor icon, title, close.
    const ImVec2 h0 = ImGui::GetCursorScreenPos();
    const float hh = S(32);
    {
      const float s = S(20) / 24;
      auto p = [&](float x, float y) { return ImVec2(h0.x + x * s, h0.y + (hh - S(20)) / 2 + y * s); };
      const ImVec2 arrow[7] = {p(6, 3), p(18, 12), p(12.5f, 13.2f), p(15, 20), p(12.4f, 21.1f), p(9.8f, 14.5f), p(6, 18)};
      dl->AddPolyline(arrow, 7, White(), ImDrawFlags_Closed, S(2));
      const ImVec2 ts = HudMeasure(S(18), "Automation");
      HudText(dl, S(18), ImVec2(h0.x + S(30), h0.y + (hh - ts.y) / 2), White(), "Automation");
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x + avail - hh, h0.y));
    if (Chip("##close", false, hh, hh)) open = false;
    {
      const ImVec2 c(h0.x + avail - hh / 2, h0.y + hh / 2);
      const float r = S(5);
      dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), White(), S(2.5f));
      dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), White(), S(2.5f));
    }
    ImGui::SetCursorScreenPos(ImVec2(h0.x, h0.y + hh + ImGui::GetStyle().ItemSpacing.y));

    // Tabs: chips in a dark track.
    {
      const ImVec2 a = ImGui::GetCursorScreenPos();
      const float th = S(44), pad = S(4), tw = (avail - pad * 2 - S(6)) / 2;
      dl->AddRectFilled(a, ImVec2(a.x + avail, a.y + th), C(31, 32, 37), th / 2);
      ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
      if (Chip("Autoclicker", g_tool_tab == 0, tw, th - pad * 2)) g_tool_tab = 0;
      ImGui::SameLine(0, S(6));
      if (Chip("Macros", g_tool_tab == 1, tw, th - pad * 2)) g_tool_tab = 1;
      ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + th + ImGui::GetStyle().ItemSpacing.y));
    }

    ImGui::PushItemWidth(-S(96));
    if (g_tool_tab == 0) ClickerTab(cfg);
    else MacrosTab(cfg);
    ImGui::PopItemWidth();
    if (GhostPill("Stop everything", avail, S(40))) automation::StopAll();
    // Remember where it was left.
    const ImVec2 pos = window->Pos;
    const double fx = pos.x / screen.x, fy = pos.y / screen.y;
    if (std::fabs(fx - cfg.tools_x) > 0.001 || std::fabs(fy - cfg.tools_y) > 0.001) {
      cfg.tools_x = fx;
      cfg.tools_y = fy;
    }
  }
  ImGui::End();
  ImGui::PopFont();
  PopToolStyle(colors);
  if (!open) {
    cfg.tools_open = false;
    g_picking = false;
  }
}

void DrawStatusBadge(const Config& cfg) {
  const automation::Status st = automation::GetStatus();
  if (!st.clicking && !st.recording && !st.playing) return;
  // A pill centred in the top-bar row: what runs, how far, and its hotkey.
  enum { kDot, kRing, kPlay } mark = kDot;
  ImU32 dot = Accent();
  std::string title, detail, key;
  char buf[64];
  if (st.recording) {
    dot = Alert();
    title = "Recording";
    std::snprintf(buf, sizeof(buf), " %d steps", st.recorded);
    detail = buf;
    key = cfg.key_record;
  } else if (st.playing) {
    mark = kPlay;
    title = "Macro";
    std::snprintf(buf, sizeof(buf), " step %d of %d", st.step, st.steps);
    detail = buf;
    key = cfg.key_macro;
  } else if (st.countdown > 0) {
    mark = kRing;
    title = "Autoclicker starts in";
    std::snprintf(buf, sizeof(buf), " %.1f s", st.countdown);
    detail = buf;
  } else {
    title = "Autoclicker";
    std::snprintf(buf, sizeof(buf), " %d clicks", st.clicks);
    detail = buf;
    key = cfg.key_clicker;
  }
  if (key == "off") key.clear();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const float h = S(44), size = S(16);
  const ImVec2 ts = HudMeasure(size, title.c_str()), ds = HudMeasure(size, detail.c_str());
  const float key_w = key.empty() ? 0 : std::max(S(26), HudMeasure(S(13), key.c_str()).x + S(14));
  const float w = S(16) + S(10) + S(10) + ts.x + ds.x + (key.empty() ? S(16) : S(10) + key_w + S(9));
  const ImVec2 a(std::round((screen.x - w) / 2), S(12));
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Fill(), h / 2);
  const ImVec2 mc(a.x + S(21), a.y + h / 2);
  if (mark == kDot) dl->AddCircleFilled(mc, S(5), dot, 16);
  if (mark == kRing) dl->AddCircle(mc, S(4), Accent(), 16, S(2));
  if (mark == kPlay) dl->AddTriangleFilled(ImVec2(mc.x - S(4), mc.y - S(6)), ImVec2(mc.x + S(6), mc.y),
                                           ImVec2(mc.x - S(4), mc.y + S(6)), White());
  float x = a.x + S(36);
  HudText(dl, size, ImVec2(x, a.y + (h - ts.y) / 2), White(), title.c_str());
  x += ts.x;
  HudText(dl, size, ImVec2(x, a.y + (h - ds.y) / 2), Units(), detail.c_str());
  x += ds.x + S(10);
  if (!key.empty()) KeyChip(dl, ImVec2(x, a.y + h / 2), key.c_str(), S(26));
}

}  // namespace nac::ui
