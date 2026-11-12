#include "ui.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "app.h"
#include "automation.h"
#include "framecap.h"
#include "gamestate.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "media.h"
#include "roblox_settings.h"
#include "servers.h"
#include "icons.h"
#include "ui_internal.h"
#include "speech.h"
#include "synth.h"
#include "tts.h"

// Layout and colours follow Roblox's in-experience menu (Settings page),
// measured from screenshots: a black, slightly see-through panel; flat rows
// with a label column on the left and a control column from ~40% across;
// big thin chevrons for pickers; ten-block sliders with circled -/+; dark
// bottom buttons with key hints and a navy primary; a white rounded outline
// for the keyboard/gamepad selection.

namespace nac::ui {

// --------------------------------------------------------------------------
// Scale, colour, animation (shared with hud.cc through ui_internal.h)

float g_px = 1.0f;     // swapchain pixels per logical unit
bool g_srgb = false;   // target stores linear values
float g_alpha = 1.0f;  // fade for the open / page-switch animations

float S(float logical) { return std::round(logical * g_px); }

float Linear(int v) {
  const float x = v / 255.0f;
  if (!g_srgb) return x;
  return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

ImU32 C(int r, int g, int b, float a) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(Linear(r), Linear(g), Linear(b), a * g_alpha));
}

ImVec4 C4(int r, int g, int b, float a) {
  return ImVec4(Linear(r), Linear(g), Linear(b), a * g_alpha);
}

ImU32 White(float a) { return C(255, 255, 255, a); }
ImU32 Muted() { return C(178, 179, 181); }         // inactive tabs, subtitles
ImU32 Panel() { return C(0, 0, 0, MenuOpacity()); }  // see-through black
// Roblox scales its backgrounds' transparency by PreferredTransparency.
float MenuOpacity() { return 1.0f - 0.18f * RobloxPreferredTransparency(); }

float OverlayOpacity(const Config& cfg) {
  return cfg.hud_match_roblox ? 1.0f : static_cast<float>(cfg.hud_background);
}

float WindowOpacity(const Config& cfg) {
  return cfg.hud_match_roblox ? MenuOpacity() : static_cast<float>(cfg.hud_background);
}

ImU32 Highlight() { return C(35, 37, 39); }         // hovered / selected row
ImU32 BlockOn() { return C(217, 217, 217); }
ImU32 BlockOff() { return C(57, 59, 61); }
ImU32 ButtonBg() { return C(24, 26, 30, 0.92f); }
ImU32 ButtonHover() { return C(38, 40, 45, 0.95f); }
ImU32 Primary() { return C(20, 38, 102); }
ImU32 PrimaryHover() { return C(29, 52, 136); }
ImU32 Divider() { return C(255, 255, 255, 0.1f); }

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point g_start = Clock::now();
}  // namespace
float Now() { return std::chrono::duration<float>(Clock::now() - g_start).count(); }

// --------------------------------------------------------------------------
// Fonts

Fonts g_fonts;

namespace {

bool IsFile(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Builder Sans ships with Roblox itself: use the newest downloaded payload.
std::string FindRobloxFonts() {
  std::vector<std::string> roots;
  if (const char* data = std::getenv("XDG_DATA_HOME")) roots.push_back(std::string(data) + "/mocktail/payloads");
  if (const char* home = std::getenv("HOME")) {
    roots.push_back(std::string(home) + "/.local/share/mocktail/payloads");
    roots.push_back(std::string(home) + "/.var/app/io.github.CoderDayton.nightcap/data/mocktail/payloads");
  }
  std::string best;
  time_t best_time = 0;
  for (const std::string& root : roots) {
    DIR* dir = opendir(root.c_str());
    if (dir == nullptr) continue;
    while (dirent* entry = readdir(dir)) {
      if (entry->d_name[0] == '.') continue;
      const std::string fonts = root + "/" + entry->d_name + "/assets/content/fonts/";
      struct stat st {};
      if (stat((fonts + "BuilderSans-Medium.otf").c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
          st.st_mtime >= best_time) {
        best = fonts;
        best_time = st.st_mtime;
      }
    }
    closedir(dir);
  }
  return best;
}

ImFont* AddFont(const std::string& dir, const char* file, float size, const char* fallback) {
  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = true;
  std::string path = dir.empty() ? "" : dir + file;
  if (path.empty() || !IsFile(path)) path = fallback ? fallback : "";
  if (!path.empty() && IsFile(path)) {
    if (ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg)) return f;
  }
  cfg.SizePixels = size;
  return io.Fonts->AddFontDefault(&cfg);
}

}  // namespace

// --------------------------------------------------------------------------
// Drawing helpers

ImDrawList* g_dl = nullptr;

void Text(ImFont* font, ImVec2 pos, ImU32 col, const char* text) {
  g_dl->AddText(font, font->FontSize, ImVec2(std::round(pos.x), std::round(pos.y)), col, text);
}

ImVec2 Measure(ImFont* font, const char* text) {
  return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
}

void TextCentered(ImFont* font, ImVec2 center, ImU32 col, const char* text) {
  const ImVec2 size = Measure(font, text);
  Text(font, ImVec2(center.x - size.x / 2, center.y - size.y / 2), col, text);
}

// Outline icons, like Roblox's menu tab icons.
// Vox Menu's logo: a rounded V with a dot over it, on a 24-unit grid
// (assets/vox-menu.svg), `size` units across, centred on c.
void VoxMark(ImDrawList* dl, ImVec2 c, float size, ImU32 ink, ImU32 dot) {
  const float s = size / 24;
  auto p = [&](float x, float y) { return ImVec2(c.x + (x - 12) * s, c.y + (y - 12.35f) * s); };
  const float w = 3 * s;
  const ImVec2 v[3] = {p(5, 7.5f), p(12, 19.5f), p(19, 7.5f)};
  dl->AddPolyline(v, 3, ink, 0, w);
  for (const ImVec2& pt : v) dl->AddCircleFilled(pt, w / 2, ink, 16);  // round caps and joint
  dl->AddCircleFilled(p(12, 5.2f), 2.1f * s, dot, 20);
}

void BubbleIcon(ImDrawList* dl, ImVec2 c, float size, ImU32 col, float stroke, bool filled, float dots) {
  const float w = size * 0.92f, h = size * 0.68f;
  const ImVec2 a(c.x - w / 2, c.y - h / 2 - size * 0.08f), b(c.x + w / 2, c.y + h / 2 - size * 0.08f);
  const ImVec2 tail[3] = {ImVec2(a.x + w * 0.2f, b.y - stroke * 0.5f), ImVec2(a.x + w * 0.16f, b.y + size * 0.22f),
                          ImVec2(a.x + w * 0.44f, b.y - stroke * 0.5f)};
  if (filled) {
    dl->AddRectFilled(a, b, col, h * 0.45f);
    dl->AddTriangleFilled(tail[0], tail[1], tail[2], col);
  } else {
    dl->AddRect(a, b, col, h * 0.45f, 0, stroke);
    dl->AddPolyline(tail, 3, col, 0, stroke);
  }
  const float r = size * 0.065f;
  const ImU32 dot = filled ? C(0, 0, 0, 0.85f) : col;
  for (int i = 0; i < 3; ++i) {
    float lift = 0;
    if (dots >= 0) lift = std::max(0.0f, std::sin(dots * 8.0f - i * 0.9f)) * size * 0.08f;
    dl->AddCircleFilled(ImVec2(c.x + (i - 1) * size * 0.22f, (a.y + b.y) / 2 - lift), r, dot);
  }
}

void SpeakerIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const ImVec2 body[6] = {ImVec2(c.x - s * 0.42f, c.y - s * 0.14f), ImVec2(c.x - s * 0.22f, c.y - s * 0.14f),
                          ImVec2(c.x + s * 0.02f, c.y - s * 0.36f), ImVec2(c.x + s * 0.02f, c.y + s * 0.36f),
                          ImVec2(c.x - s * 0.22f, c.y + s * 0.14f), ImVec2(c.x - s * 0.42f, c.y + s * 0.14f)};
  g_dl->AddPolyline(body, 6, col, ImDrawFlags_Closed, stroke);
  for (int i = 0; i < 2; ++i) {
    g_dl->PathArcTo(ImVec2(c.x + s * 0.04f, c.y), s * (0.18f + 0.15f * i), -0.8f, 0.8f, 12);
    g_dl->PathStroke(col, 0, stroke);
  }
}

void GearIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const int teeth = 8;
  const float outer = s * 0.48f, inner = s * 0.37f;
  for (int i = 0; i < teeth * 4; ++i) {
    // Each tooth: up the flank, across the top, down the flank, along the root.
    const float a = (i / 4.0f) * 2 * 3.14159265f / teeth;
    const float r = (i % 4 == 1 || i % 4 == 2) ? outer : inner;
    const float spread = (i % 4 == 0 || i % 4 == 1) ? -0.17f : 0.17f;
    g_dl->PathLineTo(ImVec2(c.x + std::cos(a + spread) * r, c.y + std::sin(a + spread) * r));
  }
  g_dl->PathStroke(col, ImDrawFlags_Closed, stroke);
  g_dl->AddCircle(c, s * 0.14f, col, 20, stroke);
}

void GaugeIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const ImVec2 o(c.x, c.y + s * 0.16f);
  g_dl->PathArcTo(o, s * 0.46f, 3.14159265f, 2 * 3.14159265f, 24);
  g_dl->PathStroke(col, 0, stroke);
  g_dl->AddLine(ImVec2(o.x - s * 0.46f, o.y), ImVec2(o.x + s * 0.46f, o.y), col, stroke);
  g_dl->AddLine(o, ImVec2(o.x + s * 0.22f, o.y - s * 0.28f), col, stroke);
  g_dl->AddCircleFilled(o, stroke * 1.4f, col);
}

void ControllerIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const ImVec2 a(c.x - s * 0.48f, c.y - s * 0.26f), b(c.x + s * 0.48f, c.y + s * 0.3f);
  g_dl->AddRect(a, b, col, s * 0.26f, 0, stroke);
  g_dl->AddLine(ImVec2(c.x - s * 0.3f, c.y + s * 0.02f), ImVec2(c.x - s * 0.1f, c.y + s * 0.02f), col, stroke);
  g_dl->AddLine(ImVec2(c.x - s * 0.2f, c.y - s * 0.08f), ImVec2(c.x - s * 0.2f, c.y + s * 0.12f), col, stroke);
  g_dl->AddCircleFilled(ImVec2(c.x + s * 0.18f, c.y - s * 0.02f), stroke * 1.3f, col);
  g_dl->AddCircleFilled(ImVec2(c.x + s * 0.3f, c.y + s * 0.1f), stroke * 1.3f, col);
}

void CursorIcon(ImVec2 c, float s, ImU32 col, float stroke) {
  const ImVec2 o(c.x - s * 0.24f, c.y - s * 0.42f);
  const ImVec2 pts[7] = {o, ImVec2(o.x, o.y + s * 0.74f), ImVec2(o.x + s * 0.18f, o.y + s * 0.58f),
                         ImVec2(o.x + s * 0.32f, o.y + s * 0.86f), ImVec2(o.x + s * 0.44f, o.y + s * 0.8f),
                         ImVec2(o.x + s * 0.3f, o.y + s * 0.53f), ImVec2(o.x + s * 0.54f, o.y + s * 0.53f)};
  g_dl->AddPolyline(pts, 7, col, ImDrawFlags_Closed, stroke);
}

// Roblox's icon font (BuilderIcons), when the payload has it.
IconFont g_icons;
ImFont* g_icon_font = nullptr;

// Draws a Roblox icon centred on c; false when the icon isn't available.
bool Icon(ImDrawList* dl, const char* name, ImVec2 c, float size, ImU32 col) {
  if (g_icon_font == nullptr) return false;
  const std::string glyph = g_icons.Utf8(name);
  if (glyph.empty()) return false;
  const ImVec2 ts = g_icon_font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph.c_str());
  dl->AddText(g_icon_font, size, ImVec2(std::round(c.x - ts.x / 2), std::round(c.y - ts.y / 2)), col,
              glyph.c_str());
  return true;
}

void Chevron(ImVec2 c, float height, bool right, ImU32 col) {
  if (Icon(g_dl, right ? "chevron-large-right" : "chevron-large-left", c, height * 1.25f, col)) return;
  const float d = right ? 1.0f : -1.0f, w = height * 0.5f;
  const ImVec2 pts[3] = {ImVec2(c.x - d * w / 2, c.y - height / 2), ImVec2(c.x + d * w / 2, c.y),
                         ImVec2(c.x - d * w / 2, c.y + height / 2)};
  g_dl->AddPolyline(pts, 3, col, 0, S(2.6f));
}

void CircleSign(ImVec2 c, bool plus, ImU32 col) {
  if (Icon(g_dl, plus ? "circle-plus" : "circle-minus", c, S(34), col)) return;
  const float r = S(14);
  g_dl->AddCircle(c, r, col, 32, S(2));
  g_dl->AddLine(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x + r * 0.5f, c.y), col, S(2.2f));
  if (plus) g_dl->AddLine(ImVec2(c.x, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), col, S(2.2f));
}

void KeyHint(ImVec2 c, const char* key, ImU32 col) {
  const std::string glyph = std::string("square-text-") + (std::strcmp(key, "ESC") == 0 ? "esc" : key);
  if (Icon(g_dl, glyph.c_str(), c, S(32), col)) return;
  const ImVec2 text = Measure(g_fonts.hint, key);
  const float w = std::max(S(26), text.x + S(9)), h = S(26);
  g_dl->AddRect(ImVec2(c.x - w / 2, c.y - h / 2), ImVec2(c.x + w / 2, c.y + h / 2), col, S(3), 0, S(1.5f));
  TextCentered(g_fonts.hint, c, col, key);
}

// The white rounded outline Roblox draws around the selected element.
void SelectionOutline(ImVec2 min, ImVec2 max, float rounding) {
  const float g = S(3);
  g_dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g), White(), rounding + g, 0, S(2));
}

namespace {

// --------------------------------------------------------------------------
// Keyboard / gamepad-style selection: Tab or ↑/↓ move between items,
// ←/→ adjust, Enter or Space activates. Clicking hides the outline.

struct Nav {
  int count = 0;       // items registered this frame
  int last_count = 0;  // ... and last frame
  int focus = -1;
  bool shown = false;  // outline visible (keyboard in use)
  bool scroll = false; // bring the focused item into view
} g_nav;

bool g_editing = false;  // a text field has the keyboard
bool g_vr = false;      // drawing the headset panel, not the desktop menu
bool g_vr_close = false;  // the headset panel's Close was pressed

struct NavItem {
  bool focused = false;
  bool left = false, right = false, activate = false;
};

bool g_nav_off = false;  // drawing a page that is sliding away

NavItem RegisterNav(ImVec2 min, ImVec2 max) {
  NavItem item;
  if (g_nav_off) return item;
  const int id = g_nav.count++;
  item.focused = g_nav.shown && id == g_nav.focus;
  if (!item.focused) return item;
  if (g_nav.scroll) {
    ImGui::ScrollToRect(ImGui::GetCurrentWindow(), ImRect(min, max));
    g_nav.scroll = false;
  }
  if (!g_editing) {
    item.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    item.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    item.activate = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space);
  }
  return item;
}

void UpdateNav() {
  g_nav.last_count = g_nav.count;
  g_nav.count = 0;
  const int n = g_nav.last_count;
  if (n == 0) return;
  const bool back = ImGui::IsKeyPressed(ImGuiKey_Tab) && ImGui::GetIO().KeyShift;
  const bool next = ImGui::IsKeyPressed(ImGuiKey_Tab) && !ImGui::GetIO().KeyShift;
  const bool up = !g_editing && ImGui::IsKeyPressed(ImGuiKey_UpArrow);
  const bool down = !g_editing && ImGui::IsKeyPressed(ImGuiKey_DownArrow);
  if (next || down || back || up) {
    if (!g_nav.shown || g_nav.focus < 0) {
      g_nav.focus = (back || up) ? n - 1 : 0;
    } else {
      g_nav.focus = (g_nav.focus + ((back || up) ? n - 1 : 1)) % n;
    }
    g_nav.shown = true;
    g_nav.scroll = true;
    if (g_editing) ImGui::ClearActiveID();
  }
  if (ImGui::IsMouseClicked(0)) g_nav.shown = false;
}

// --------------------------------------------------------------------------
// Rows and controls

bool Hit(const char* id, ImVec2 min, ImVec2 max, bool* hovered = nullptr, bool* held = nullptr) {
  ImGui::SetCursorScreenPos(min);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)));
  if (hovered) *hovered = ImGui::IsItemHovered();
  if (held) *held = ImGui::IsItemActive();
  return clicked;
}

// A settings row: label (and optional subtitle) on the left, the control in
// a column on the right. Rows are flat; hovering or selecting highlights.
struct Row {
  ImVec2 min, max;
  float ctrl_l, ctrl_r;  // control column
  float mid;             // vertical centre of the control
  NavItem nav;
  int id = 0;            // unique per frame, for widget IDs
};

Row BeginRow(const char* title, const char* subtitle, float height = 0) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x - S(10);
  const float h = height > 0 ? S(height) : S(subtitle ? 70 : 50);
  Row row;
  row.min = pos;
  row.max = ImVec2(pos.x + w, pos.y + h);
  row.ctrl_l = pos.x + std::round(w * 0.4f);
  row.ctrl_r = row.max.x - S(14);
  row.mid = (row.min.y + row.max.y) / 2;
  row.id = g_nav.count;
  row.nav = RegisterNav(row.min, row.max);
  const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(row.min, row.max);
  if (hovered || row.nav.focused) g_dl->AddRectFilled(row.min, row.max, Highlight(), S(8));
  if (row.nav.focused) SelectionOutline(row.min, row.max, S(8));
  const float title_h = g_fonts.title->FontSize;
  if (subtitle) {
    const float sub_w = row.ctrl_l - row.min.x - S(24);
    const ImVec2 sub = g_fonts.small->CalcTextSizeA(g_fonts.small->FontSize, FLT_MAX, sub_w, subtitle);
    const float top = (row.min.y + row.max.y) / 2 - (title_h + S(2) + sub.y) / 2;
    Text(g_fonts.title, ImVec2(pos.x + S(10), top), White(), title);
    g_dl->AddText(g_fonts.small, g_fonts.small->FontSize, ImVec2(pos.x + S(10), top + title_h + S(2)), Muted(),
                  subtitle, nullptr, sub_w);
  } else {
    Text(g_fonts.title, ImVec2(pos.x + S(10), (row.min.y + row.max.y) / 2 - title_h / 2), White(), title);
  }
  return row;
}

void EndRow(const Row& row) {
  ImGui::SetCursorScreenPos(ImVec2(row.min.x, row.max.y));
  ImGui::Dummy(ImVec2(row.max.x - row.min.x, S(2)));
}

void Header(const char* text, bool first = false) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x - S(10);
  float y = pos.y;
  if (!first) {
    y += S(12);
    g_dl->AddLine(ImVec2(pos.x + S(10), y), ImVec2(pos.x + w, y), Divider(), S(1));
    y += S(16);
  } else {
    y += S(6);
  }
  Text(g_fonts.header, ImVec2(pos.x + S(10), y), White(), text);
  ImGui::Dummy(ImVec2(w, y - pos.y + g_fonts.header->FontSize + S(10)));
}

// Picker values swipe when changed: the old text slides a little toward the
// side away from the arrow you pressed and fades, the new one slides in.
struct Swipe {
  float at = -10;
  float dir = 0;  // +1: pressed ›, -1: pressed ‹
  std::string old;
};
std::map<ImGuiID, Swipe> g_swipes;

void ValueText(ImGuiID id, const std::string& before, const std::string& now, float dir, ImVec2 center) {
  Swipe& sw = g_swipes[id];
  if (dir != 0 && before != now) {
    sw.at = Now();
    sw.dir = dir;
    sw.old = before;
  }
  const float t = std::clamp((Now() - sw.at) / 0.16f, 0.0f, 1.0f);
  if (t >= 1.0f) {
    TextCentered(g_fonts.value, center, White(), now.c_str());
    return;
  }
  const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
  const float dist = S(26);
  const float a = g_alpha;
  const float out = std::max(0.0f, 1.0f - e * 1.8f);  // the old value clears quickly
  g_alpha = a * out;
  TextCentered(g_fonts.value, ImVec2(center.x - sw.dir * dist * e, center.y), White(), sw.old.c_str());
  g_alpha = a * e;
  TextCentered(g_fonts.value, ImVec2(center.x + sw.dir * dist * (1.0f - e), center.y), White(), now.c_str());
  g_alpha = a;
}

void Switch(Row& row, bool* value) {
  // Roblox shows on/off settings as a picker: ‹  On  ›
  const bool before = *value;
  const float mid_x = (row.ctrl_l + row.ctrl_r) / 2;
  bool hovered;
  float dir = 0;
  ImGui::PushID(row.id);
  if (Hit("toggle", ImVec2(row.ctrl_l, row.min.y), ImVec2(row.ctrl_r, row.max.y), &hovered)) {
    dir = ImGui::GetIO().MousePos.x < mid_x ? -1.0f : 1.0f;
  }
  if (row.nav.left) dir = -1.0f;
  if (row.nav.right || row.nav.activate) dir = 1.0f;
  if (dir != 0) *value = !*value;
  const ImGuiID id = ImGui::GetID("value");
  ImGui::PopID();
  const float ch = S(28);
  Chevron(ImVec2(row.ctrl_l + S(16), row.mid), ch, false, White(hovered ? 1.0f : 0.92f));
  Chevron(ImVec2(row.ctrl_r - S(16), row.mid), ch, true, White(hovered ? 1.0f : 0.92f));
  ValueText(id, before ? "On" : "Off", *value ? "On" : "Off", dir, ImVec2(mid_x, row.mid));
}

void Picker(Row& row, const std::vector<std::string>& labels, int* index) {
  const int n = static_cast<int>(labels.size());
  const float zone = S(48);
  const int before = *index;
  bool hover_l, hover_r;
  float dir = 0;
  ImGui::PushID(row.id);
  if (Hit("prev", ImVec2(row.ctrl_l, row.min.y), ImVec2(row.ctrl_l + zone, row.max.y), &hover_l) || row.nav.left) {
    *index = (*index + n - 1) % n;
    dir = -1.0f;
  }
  if (Hit("next", ImVec2(row.ctrl_r - zone, row.min.y), ImVec2(row.ctrl_r, row.max.y), &hover_r) || row.nav.right ||
      row.nav.activate) {
    *index = (*index + 1) % n;
    dir = 1.0f;
  }
  const ImGuiID id = ImGui::GetID("value");
  ImGui::PopID();
  const float ch = S(28);
  Chevron(ImVec2(row.ctrl_l + S(16), row.mid), ch, false, White(hover_l ? 1.0f : 0.92f));
  Chevron(ImVec2(row.ctrl_r - S(16), row.mid), ch, true, White(hover_r ? 1.0f : 0.92f));
  ValueText(id, labels[before], labels[*index], dir, ImVec2((row.ctrl_l + row.ctrl_r) / 2, row.mid));
}

// Ten blocks between a circled − and +; the − hides at the minimum and the
// + at the maximum, with small labels for the two ends underneath.
void Slider(Row& row, double* value, double lo, double hi, double step, const char* low_label,
            const char* high_label) {
  const float bar_l = row.ctrl_l + S(52), bar_r = row.ctrl_r - S(53);
  const float block_h = S(24), gap = S(4);
  const float cy = row.mid - S(6);
  ImGui::PushID(row.id);
  bool hover_minus = false, hover_plus = false, held = false;
  const bool can_lower = *value > lo + step * 0.5, can_raise = *value < hi - step * 0.5;
  if (can_lower && Hit("minus", ImVec2(row.ctrl_l, cy - S(18)), ImVec2(row.ctrl_l + S(36), cy + S(18)), &hover_minus)) {
    *value -= step;
  }
  if (can_raise && Hit("plus", ImVec2(row.ctrl_r - S(36), cy - S(18)), ImVec2(row.ctrl_r, cy + S(18)), &hover_plus)) {
    *value += step;
  }
  Hit("bar", ImVec2(bar_l, cy - block_h / 2 - S(4)), ImVec2(bar_r, cy + block_h / 2 + S(4)), nullptr, &held);
  ImGui::PopID();
  if (held) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - bar_l) / (bar_r - bar_l), 0.0f, 1.0f);
    *value = lo + std::round(t * (hi - lo) / step) * step;
  }
  if (row.nav.left) *value -= step;
  if (row.nav.right) *value += step;
  *value = std::clamp(std::round(*value / step) * step, lo, hi);

  if (*value > lo + step * 0.5) CircleSign(ImVec2(row.ctrl_l + S(16), cy), false, White(hover_minus ? 1.0f : 0.85f));
  if (*value < hi - step * 0.5) CircleSign(ImVec2(row.ctrl_r - S(17), cy), true, White(hover_plus ? 1.0f : 0.85f));
  const int blocks = 10;
  const float bw = (bar_r - bar_l - gap * (blocks - 1)) / blocks;
  const float filled = static_cast<float>((*value - lo) / (hi - lo)) * blocks;
  // One bar split into square blocks; only its two ends are rounded.
  for (int i = 0; i < blocks; ++i) {
    const float x = bar_l + i * (bw + gap);
    const ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft
                                : i == blocks - 1 ? ImDrawFlags_RoundCornersRight
                                                  : ImDrawFlags_RoundCornersNone;
    g_dl->AddRectFilled(ImVec2(x, cy - block_h / 2), ImVec2(x + bw, cy + block_h / 2),
                        filled >= i + 0.5f ? BlockOn() : BlockOff(), S(7), corners);
  }
  const float label_y = cy + block_h / 2 + S(7);
  Text(g_fonts.tiny, ImVec2(bar_l - S(2), label_y), Muted(), low_label);
  const ImVec2 hs = Measure(g_fonts.tiny, high_label);
  Text(g_fonts.tiny, ImVec2(bar_r - hs.x + S(2), label_y), Muted(), high_label);
}

// A dark rounded box like Roblox's dropdowns, holding a text field.
bool TextField(Row& row, const char* id, char* buf, size_t size, const char* placeholder, bool* done) {
  const float h = S(42);
  const ImVec2 min(row.ctrl_l, row.mid - h / 2);
  const float width = row.ctrl_r - row.ctrl_l;
  if (g_vr) {
    // No keyboard in the headset: show the value, edit it on the desktop.
    if (done) *done = false;
    g_dl->AddRectFilled(min, ImVec2(min.x + width, min.y + h), C(35, 37, 39), S(8));
    Text(g_fonts.value, ImVec2(min.x + S(15), row.mid - g_fonts.value->FontSize / 2), White(buf[0] ? 0.8f : 0.45f),
         buf[0] ? buf : placeholder);
    return false;
  }
  if (row.nav.activate) ImGui::SetKeyboardFocusHere();
  ImGui::PushFont(g_fonts.value);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(15), (h - g_fonts.value->FontSize) / 2));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, C4(35, 37, 39));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, C4(42, 44, 47));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, C4(42, 44, 47));
  ImGui::PushStyleColor(ImGuiCol_Text, C4(255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImGui::ColorConvertU32ToFloat4(C(51, 95, 255, 0.5f)));
  ImGui::SetCursorScreenPos(min);
  ImGui::SetNextItemWidth(width);
  const bool enter = ImGui::InputText(id, buf, size, ImGuiInputTextFlags_EnterReturnsTrue);
  if (done) *done = enter || ImGui::IsItemDeactivatedAfterEdit();
  if (ImGui::IsItemActive()) g_editing = true;
  if (buf[0] == 0 && !ImGui::IsItemActive()) {
    Text(g_fonts.value, ImVec2(min.x + S(15), row.mid - g_fonts.value->FontSize / 2), White(0.45f), placeholder);
  }
  ImGui::PopStyleColor(5);
  ImGui::PopStyleVar(4);
  ImGui::PopFont();
  return enter;
}

void StatusDot(Row& row, bool on) {
  g_dl->AddCircleFilled(ImVec2(row.ctrl_r - S(8), row.mid), S(6), on ? C(57, 194, 113) : C(110, 112, 118));
}

// A dark button filling the control column, like the bottom bar's.
bool ActionButton(Row& row, const char* label, bool primary = false) {
  const float h = S(40);
  const ImVec2 a(row.ctrl_l, row.mid - h / 2), b(row.ctrl_r, row.mid + h / 2);
  bool hovered = false;
  ImGui::PushID(row.id);
  const bool clicked = Hit("action", a, b, &hovered) || row.nav.activate;
  ImGui::PopID();
  g_dl->AddRectFilled(a, b, primary ? (hovered ? PrimaryHover() : Primary()) : (hovered ? ButtonHover() : C(38, 40, 45)),
                      S(8));
  TextCentered(g_fonts.button, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), White(), label);
  return clicked;
}

// --------------------------------------------------------------------------
// Pages

int g_tab = 0;
char g_test[256] = "Hey there! How's it going?";
char g_pack[512] = "";
bool g_pack_loaded = false;
char g_piper_model[512] = "";
bool g_piper_model_loaded = false;
char g_piper_dir[512] = "";
bool g_piper_dir_loaded = false;

template <typename T, typename F>
std::vector<std::string> Labels(const std::vector<T>& items, F label) {
  std::vector<std::string> out;
  for (const T& item : items) out.push_back(label(item));
  return out;
}

int IndexOf(const std::vector<std::string>& keys, const std::string& key) {
  const auto it = std::find(keys.begin(), keys.end(), key);
  return it == keys.end() ? 0 : static_cast<int>(it - keys.begin());
}

void TTSPage(Config& cfg) {
  Header("Text-to-Speech", true);
  Row row = BeginRow("Speak My Chat Messages", "Read chat aloud in Roblox voice chat and/or speakers");
  Switch(row, &cfg.enabled);
  EndRow(row);

  const std::vector<std::string> engines = {"piper", "espeak", "animalese"};
  int engine_idx = IndexOf(engines, cfg.engine);
  row = BeginRow("Speech Engine", "Select your local TTS backend");
  Picker(row, {"Piper (Neural)", "eSpeak-NG", "Animal Crossing"}, &engine_idx);
  cfg.engine = engines[engine_idx];
  EndRow(row);

  if (cfg.engine == "piper") {
    Header("Piper Neural Voice Settings");
    auto piper_models = DiscoverPiperModels(cfg.piper_dir);
    if (!piper_models.empty()) {
      std::vector<std::string> model_labels;
      std::vector<std::string> model_paths;
      for (const auto& m : piper_models) {
        model_labels.push_back(m.name);
        model_paths.push_back(m.path);
      }
      int m_idx = IndexOf(model_paths, cfg.piper_model);
      row = BeginRow("Model Voice", "Locally installed Piper voice model");
      Picker(row, model_labels, &m_idx);
      cfg.piper_model = model_paths[m_idx];
      EndRow(row);
    } else {
      row = BeginRow("Piper Models", "No models found in ~/.local/share/piper-voices");
      EndRow(row);
    }

    if (!g_piper_model_loaded) {
      std::snprintf(g_piper_model, sizeof(g_piper_model), "%s", cfg.piper_model.c_str());
      g_piper_model_loaded = true;
    }
    row = BeginRow("Custom Model File", "Direct path to an .onnx model file");
    bool done = false;
    TextField(row, "##piper_model_input", g_piper_model, sizeof(g_piper_model), "Auto-detect or custom path", &done);
    if (done) cfg.piper_model = g_piper_model;
    EndRow(row);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%.0f%% of normal pace", cfg.piper_speed * 100);
    row = BeginRow("Speed", sub);
    Slider(row, &cfg.piper_speed, 0.5, 2.0, 0.05, "Slower", "Faster");
    EndRow(row);

    const bool piper_ok = IsPiperInstalled();
    row = BeginRow("Piper Binary", piper_ok ? "Found in PATH" : "Not found; install piper-tts or place in ~/.local/bin");
    StatusDot(row, piper_ok);
    EndRow(row);
  } else if (cfg.engine == "espeak") {
    Header("eSpeak-NG Settings");
    const auto& espeak_voices = GetESpeakVoices();
    std::vector<std::string> voice_labels;
    std::vector<std::string> voice_codes;
    for (const auto& v : espeak_voices) {
      voice_labels.push_back(v.name);
      voice_codes.push_back(v.code);
    }
    int v_idx = IndexOf(voice_codes, cfg.espeak_voice);
    row = BeginRow("Voice / Accent", "Language and accent style");
    Picker(row, voice_labels, &v_idx);
    cfg.espeak_voice = voice_codes[v_idx];
    EndRow(row);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%.0f words per minute", cfg.espeak_speed);
    row = BeginRow("Speed", sub);
    Slider(row, &cfg.espeak_speed, 80, 350, 5, "Slower", "Faster");
    EndRow(row);

    std::snprintf(sub, sizeof(sub), "Pitch: %.0f", cfg.espeak_pitch);
    row = BeginRow("Pitch", sub);
    Slider(row, &cfg.espeak_pitch, 0, 99, 1, "Lower", "Higher");
    EndRow(row);
  } else if (cfg.engine == "animalese") {
    Header("Animal Crossing Settings");

    const auto voice_keys = Labels(Voices(), [](const VoiceInfo& v) { return std::string(v.key); });
    int voice = IndexOf(voice_keys, cfg.voice);
    row = BeginRow("Personality", "Each villager type has its own pitch and pace");
    Picker(row, Labels(Voices(), [](const VoiceInfo& v) { return std::string(v.label); }), &voice);
    cfg.voice = voice_keys[voice];
    EndRow(row);

    const std::vector<std::string> lang_keys = {"english", "korean"};
    int lang_idx = IndexOf(lang_keys, cfg.animalese_lang);
    row = BeginRow("Language", "Authentic English or Korean villager sound bank");
    Picker(row, {"English", "Korean"}, &lang_idx);
    cfg.animalese_lang = lang_keys[lang_idx];
    EndRow(row);

    char sub[64];
    std::snprintf(sub, sizeof(sub), "%+.0f semitones", cfg.pitch);
    row = BeginRow("Pitch", sub);
    Slider(row, &cfg.pitch, -12, 12, 1, "Lower", "Higher");
    EndRow(row);

    std::snprintf(sub, sizeof(sub), "%.0f%% of the villager's pace", cfg.speed * 100);
    row = BeginRow("Speed", sub);
    Slider(row, &cfg.speed, 0.5, 2.0, 0.05, "Slower", "Faster");
    EndRow(row);

    if (!g_pack_loaded) {
      std::snprintf(g_pack, sizeof(g_pack), "%s", cfg.sample_pack.c_str());
      g_pack_loaded = true;
    }
    row = BeginRow("Letter Samples", "a.wav to z.wav for the exact game sound (Classic only)");
    bool done = false;
    TextField(row, "##pack", g_pack, sizeof(g_pack), "Built-in voice", &done);
    if (done) cfg.sample_pack = g_pack;
    EndRow(row);
  }

  Header("Try It");
  row = BeginRow("Test Message", g_vr ? "Use Test Voice below to hear it" : "Press Enter to hear the voice");
  if (TextField(row, "##test_tts", g_test, sizeof(g_test), "Type something", nullptr) && g_test[0]) Say(g_test, true);
  EndRow(row);
}

void OutputPage(Config& cfg) {
  Header("Where the Voice Plays", true);
  const std::vector<std::string> keys = {"vc", "both", "local"};
  int output = IndexOf(keys, cfg.output);
  Row row = BeginRow("Play the Voice In", "Choose who hears it");
  Picker(row, {"Voice Chat", "Voice Chat and Me", "Only Me"}, &output);
  cfg.output = keys[output];
  EndRow(row);

  char sub[48];
  std::snprintf(sub, sizeof(sub), "%.0f%%", cfg.vc_volume * 100);
  row = BeginRow("Voice Chat Volume", sub);
  Slider(row, &cfg.vc_volume, 0, 1.5, 0.05, "Quiet", "Loud");
  EndRow(row);

  std::snprintf(sub, sizeof(sub), "%.0f%%", cfg.local_volume * 100);
  row = BeginRow("My Speakers Volume", sub);
  Slider(row, &cfg.local_volume, 0, 1.5, 0.05, "Quiet", "Loud");
  EndRow(row);

  row = BeginRow("Keep My Microphone On", "Off: voice chat only hears the TTS voice and shared app audio");
  Switch(row, &cfg.mix_real_mic);
  EndRow(row);

  Header("Share App Audio");
  row = BeginRow("Share an App's Sound", "Play music or a video into voice chat, straight from the app");
  Switch(row, &cfg.media_on);
  EndRow(row);
  // The apps playing sound now; the chosen one stays listed when it stops.
  std::vector<std::string> apps, app_labels;
  if (cfg.media_source.empty()) {
    apps.push_back("");
    app_labels.push_back("Choose an App");
  }
  for (const media::App& a : media::Apps()) {
    apps.push_back(a.name);
    app_labels.push_back(a.name);
  }
  if (!cfg.media_source.empty() && cfg.media_source != media::kAllApps &&
      std::find(apps.begin(), apps.end(), cfg.media_source) == apps.end()) {
    apps.push_back(cfg.media_source);
    app_labels.push_back(cfg.media_source + " (silent)");
  }
  apps.push_back(media::kAllApps);
  app_labels.push_back("All Other Apps");
  int source = IndexOf(apps, cfg.media_source);
  row = BeginRow("App", "Roblox itself is never shared, so voice chat can't echo");
  Picker(row, app_labels, &source);
  cfg.media_source = apps[source];
  EndRow(row);

  std::snprintf(sub, sizeof(sub), "%.0f%%", cfg.media_volume * 100);
  row = BeginRow("Shared Audio Volume", sub);
  Slider(row, &cfg.media_volume, 0, 1.5, 0.05, "Quiet", "Loud");
  EndRow(row);
  if (cfg.media_on) {
    const std::string status = media::Status();
    row = BeginRow("Sharing", status.empty() ? "Starting..." : status.c_str());
    StatusDot(row, status.rfind("Sharing", 0) == 0);
    EndRow(row);
  }

  Header("Status");
  const bool mic = MicInUse();
  row = BeginRow("Voice Chat", mic ? "Your mic is open, so speech goes straight into voice chat"
                                   : "Not in voice chat right now. Join a voice server and unmute");
  StatusDot(row, mic);
  EndRow(row);
}

bool g_layout = false;  // the layout editor replaces the panel

// A picker over numbers; a value not in the list (set by hand) joins it.
void NumberPicker(Row& row, double* value, std::vector<double> options, const char* zero_label, const char* unit) {
  if (std::find(options.begin(), options.end(), *value) == options.end()) {
    options.push_back(*value);
    std::sort(options.begin(), options.end());
  }
  std::vector<std::string> labels;
  int idx = 0;
  for (size_t i = 0; i < options.size(); ++i) {
    char label[48];
    if (options[i] == 0) std::snprintf(label, sizeof(label), "%s", zero_label);
    else std::snprintf(label, sizeof(label), "%g %s", options[i], unit);
    labels.push_back(label);
    if (options[i] == *value) idx = static_cast<int>(i);
  }
  Picker(row, labels, &idx);
  *value = options[idx];
}

char g_fps_cap_text[32] = "";
bool g_fps_cap_editing = false;

void GamePage(Config& cfg) {
  Header("This Server", true);
  const ServerInfo last = LastServer();
  std::string where = "Not in a game yet";
  if (!last.job.empty()) {
    where = std::string(InGame() ? "Place " : "Left place ") + last.place + ", server " + last.job.substr(0, 8);
  }
  Row row = BeginRow("Rejoin", ("Join the same server again, even after a disconnect. " + where).c_str());
  if (ActionButton(row, "Rejoin", true)) servers::Rejoin();
  EndRow(row);

  row = BeginRow("Server Hop", "Another server of this game, skipping ones you joined in the last 10 minutes");
  if (ActionButton(row, servers::Busy() ? "Working..." : "Hop") && !servers::Busy()) servers::Hop(cfg.hop_mode);
  EndRow(row);

  const std::vector<std::string> modes = {"random", "smallest", "ping"};
  int mode = IndexOf(modes, cfg.hop_mode);
  row = BeginRow("Hop To", "Which server a hop picks");
  Picker(row, {"Any Server", "Fewest Players", "Best Ping"}, &mode);
  cfg.hop_mode = modes[mode];
  EndRow(row);

  row = BeginRow("Invite Link", "Copy a link friends can open to join this server");
  if (ActionButton(row, "Copy Link")) servers::CopyInvite();
  EndRow(row);

  const std::string status = servers::StatusText();
  if (!status.empty()) {
    row = BeginRow("Status", status.c_str());
    StatusDot(row, servers::Busy());
    EndRow(row);
  }

  Header("Frame Rate");
  if (g_vr) {
    row = BeginRow("FPS Cap", "Off while you play in VR, so it never holds the headset back");
    EndRow(row);
    return;
  }
  row = BeginRow("FPS Cap", "Limit how fast Roblox draws; changes apply instantly");
  NumberPicker(row, &cfg.fps_cap, {0, 30, 45, 60, 72, 75, 90, 100, 120, 144, 165, 180, 200, 240, 360}, "Unlimited",
               "FPS");
  EndRow(row);

  // Any number at all, typed.
  if (!g_fps_cap_editing) std::snprintf(g_fps_cap_text, sizeof(g_fps_cap_text), "%g", cfg.fps_cap);
  row = BeginRow("Exact FPS Cap", "Type any number and press Enter; 0 is unlimited");
  bool done = false;
  TextField(row, "##fps_cap", g_fps_cap_text, sizeof(g_fps_cap_text), "Unlimited", &done);
  g_fps_cap_editing = ImGui::IsItemActive();
  if (done) {
    char* end = nullptr;
    const double typed = std::strtod(g_fps_cap_text, &end);
    if (end != g_fps_cap_text && typed >= 0) cfg.fps_cap = std::min(typed, 1000.0);
  }
  EndRow(row);

  row = BeginRow("When Roblox Isn't Focused", "A lower cap while you're in another window saves power");
  NumberPicker(row, &cfg.fps_cap_background, {0, 5, 10, 15, 20, 30, 60}, "No Change", "FPS");
  EndRow(row);
}

void HudPage(Config& cfg) {
  Header("Overlays", true);
  Row row = BeginRow("FPS Counter", "Frames per second the game is drawing");
  Switch(row, &cfg.hud_fps.on);
  EndRow(row);
  if (cfg.hud_fps.on) {
    row = BeginRow("Show Frame Time", "Milliseconds per frame next to the FPS");
    Switch(row, &cfg.hud_frametime);
    EndRow(row);
  }

  row = BeginRow("Ping", "Round trip to the game server");
  Switch(row, &cfg.hud_ping.on);
  EndRow(row);

  row = BeginRow("Server Location", "Where the server is; its address is looked up at ip-api.com");
  Switch(row, &cfg.hud_server.on);
  EndRow(row);
  if (cfg.hud_server.on) {
    row = BeginRow("Show Server Address", "The server's IP under its location");
    Switch(row, &cfg.hud_server_ip);
    EndRow(row);
  }

  if (g_vr) {
    // Keystrokes and the layout editor are for the desktop screen.
    Header("In the Headset");
    const std::vector<std::string> where = {"view", "off"};
    int at = IndexOf(where, cfg.vr_stats);
    row = BeginRow("Show These Stats", "As one pill low in your view, while you play");
    Picker(row, {"In View", "Off"}, &at);
    cfg.vr_stats = where[at];
    EndRow(row);
    return;
  }

  row = BeginRow("Keystrokes", "W A S D lighting up as you press them");
  Switch(row, &cfg.hud_keys.on);
  EndRow(row);
  if (cfg.hud_keys.on) {
    row = BeginRow("Space Bar", nullptr);
    Switch(row, &cfg.hud_keys_space);
    EndRow(row);
    row = BeginRow("Mouse Buttons", nullptr);
    Switch(row, &cfg.hud_keys_mouse);
    EndRow(row);
    if (cfg.hud_keys_mouse) {
      row = BeginRow("Clicks per Second", "Under each mouse button");
      Switch(row, &cfg.hud_keys_cps);
      EndRow(row);
    }
    const std::vector<std::string> pressed = {"white", "blue"};
    int pressed_idx = IndexOf(pressed, cfg.hud_keys_pressed);
    row = BeginRow("Pressed Keys", "How a key looks while you hold it");
    Picker(row, {"White", "Blue"}, &pressed_idx);
    cfg.hud_keys_pressed = pressed[pressed_idx];
    EndRow(row);
  }

  Header("Layout");
  row = BeginRow("Move and Resize", "Drag overlays anywhere, with snapping to edges and each other");
  if (ActionButton(row, "Edit Layout", true)) {
    g_layout = true;
    g_nav.focus = -1;
  }
  EndRow(row);

  char sub[96];
  std::snprintf(sub, sizeof(sub), "Solid like the top bar; the window follows Roblox's Background Transparency");
  row = BeginRow("Match Roblox's Look", sub);
  Switch(row, &cfg.hud_match_roblox);
  EndRow(row);
  if (!cfg.hud_match_roblox) {
    std::snprintf(sub, sizeof(sub), "%.0f%%", cfg.hud_background * 100);
    row = BeginRow("Background", sub);
    Slider(row, &cfg.hud_background, 0, 1, 0.05, "Clear", "Solid");
    EndRow(row);
  }

  row = BeginRow("Snapping", "Line overlays up with the screen and each other while dragging");
  Switch(row, &cfg.hud_snap);
  EndRow(row);

  row = BeginRow("Reset Positions", "Put every overlay back in its default spot and size");
  if (ActionButton(row, "Reset")) ResetLayout(cfg);
  EndRow(row);
}

void AutomationPage(Config& cfg) {
  Header("Autoclicker and Macros", true);
  Row row = BeginRow("Automation Window", "A floating window that stays on screen while you play");
  Switch(row, &cfg.tools_open);
  EndRow(row);

  row = BeginRow("Pause While This Menu Is Open", "No clicks or macro keys reach the game behind the menu");
  Switch(row, &cfg.pause_in_menu);
  EndRow(row);

  Header("Hotkeys");
  const std::vector<std::string> keys = {"off", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F9", "F10",
                                          "F11", "F12", "Insert", "Home", "End", "PageUp", "PageDown", "Pause"};
  std::vector<std::string> labels = keys;
  labels[0] = "None";
  struct HotkeyRow { const char* title; const char* subtitle; std::string* value; } hotkeys[] = {
      {"Autoclicker On / Off", "Starts and stops clicking", &cfg.key_clicker},
      {"Play / Stop Macro", "Plays the macro loaded in the window", &cfg.key_macro},
      {"Record Macro", "Records your keys and clicks until pressed again", &cfg.key_record},
  };
  for (const HotkeyRow& h : hotkeys) {
    int idx = IndexOf(keys, *h.value);
    row = BeginRow(h.title, h.subtitle);
    Picker(row, labels, &idx);
    *h.value = keys[idx];
    EndRow(row);
  }

  Header("Status");
  const automation::Status st = automation::GetStatus();
  char sub[96];
  std::snprintf(sub, sizeof(sub), st.clicking ? "Running, %d clicks so far" : "Stopped", st.clicks);
  row = BeginRow("Autoclicker", sub);
  StatusDot(row, st.clicking);
  EndRow(row);
  std::snprintf(sub, sizeof(sub), "%s", st.recording ? "Recording" : st.playing ? "Playing" : "Idle");
  row = BeginRow("Macro", sub);
  StatusDot(row, st.recording || st.playing);
  EndRow(row);
}

void SettingsPage(Config& cfg) {
  Header("Chat", true);
  Row row = BeginRow("Intercept Chat", "Speak messages via TTS instead of sending them to Roblox chat");
  Switch(row, &cfg.intercept_chat);
  EndRow(row);

  row = BeginRow("Skip Chat Commands", "Don't speak messages that start with /, like /e dance");
  Switch(row, &cfg.skip_commands);
  EndRow(row);

  const std::vector<std::string> keys = {"left", "right", "off"};
  int where = IndexOf(keys, cfg.menu_button);
  row = BeginRow("Menu Button", "F8 always opens this menu");
  Picker(row, {"Top Left", "Top Right", "Hidden"}, &where);
  cfg.menu_button = keys[where];
  EndRow(row);

  const std::vector<std::string> sticks = {"left_stick", "right_stick"};
  int stick = IndexOf(sticks, cfg.vr_toggle);
  row = BeginRow("Open in VR", "Hold this thumbstick click for half a second; point and pull the trigger");
  Picker(row, {"Hold Left Stick", "Hold Right Stick"}, &stick);
  cfg.vr_toggle = sticks[stick];
  EndRow(row);

  Header("Piper Models Directory");
  if (!g_piper_dir_loaded) {
    std::snprintf(g_piper_dir, sizeof(g_piper_dir), "%s", cfg.piper_dir.c_str());
    g_piper_dir_loaded = true;
  }
  row = BeginRow("Custom Voices Folder", "Additional directory to scan for .onnx models");
  bool done = false;
  TextField(row, "##piper_dir", g_piper_dir, sizeof(g_piper_dir), "Default (~/.local/share/piper-voices)", &done);
  if (done) cfg.piper_dir = g_piper_dir;
  EndRow(row);

  Header("About");
  const std::string last = LastMessage();
  row = BeginRow("Vox Menu", last.empty() ? "Nothing spoken yet" : ("Last spoken: " + last).c_str());
  EndRow(row);
}


// --------------------------------------------------------------------------
// The menu
//
// Timings follow Roblox's menu, measured from a 60 fps recording: it opens
// by sliding up from below the screen (~0.18 s, decelerating) while the
// game dims; pages slide sideways (~0.12 s), the new one coming from the
// side of the tab you picked; closing drops the dim and the page at once
// and the empty panel falls off the bottom (~0.35 s, accelerating).

float g_opened_at = -10;
float g_closed_at = -10;
float g_switched_at = -10;
int g_prev_tab = 0;

constexpr float kOpenTime = 0.18f;
constexpr float kPageTime = 0.12f;
constexpr float kCloseTime = 0.35f;
constexpr float kDim = 0.45f;

float EaseOutQuart(float since, float seconds) {
  const float t = std::clamp((Now() - since) / seconds, 0.0f, 1.0f);
  const float u = 1.0f - t;
  return 1.0f - u * u * u * u;
}

bool Closing() { return Now() - g_closed_at < kCloseTime; }

void SwitchTab(int tab) {
  if (tab == g_tab) return;
  g_prev_tab = g_tab;
  g_tab = tab;
  g_switched_at = Now();
  g_nav.focus = -1;
}

void DrawPage(int tab, Config& cfg) {
  if (tab == 0) TTSPage(cfg);
  if (tab == 1) OutputPage(cfg);
  if (tab == 2) GamePage(cfg);
  if (tab == 3) HudPage(cfg);
  if (tab == 4) AutomationPage(cfg);
  if (tab == 5) SettingsPage(cfg);
  ImGui::Dummy(ImVec2(1, S(8)));
}

constexpr int kNumTabs = 6;

struct PanelRect {
  ImVec2 min, max;
};
PanelRect g_panel;

void DrawMenu(Config& cfg, float W, float H, bool closing) {
  // In the headset the panel is the whole texture: no dimming or sliding.
  const float pw = g_vr ? W : std::min(S(864), W - S(32));
  const float ph = g_vr ? H : std::min(S(736), H - S(110));
  const float rest_y = g_vr ? 0 : std::round(std::max(S(64), (H - ph) / 2 + S(20)));
  float y = rest_y, dim = 0;
  if (g_vr) {
  } else if (!closing) {
    const float e = EaseOutQuart(g_opened_at, kOpenTime);
    y = std::round(H + (rest_y - H) * e);  // top edge starts at the bottom of the screen
    dim = kDim * e;
  } else {
    const float t = std::clamp((Now() - g_closed_at) / kCloseTime, 0.0f, 1.0f);
    y = std::round(rest_y + (H - rest_y) * t * t);
  }
  if (dim > 0) ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), C(0, 0, 0, dim));
  const ImVec2 p0(std::round((W - pw) / 2), y);
  const ImVec2 p1(p0.x + pw, p0.y + ph);
  g_panel = {p0, p1};
  g_dl->AddRectFilled(p0, p1, Panel(), S(8));

  // Tabs: Roblox icon + label; the active one is white with an underline.
  struct Tab { const char* label; const char* icon; } tabs[] = {
      {"TTS", "frame-bubble-soundwave"}, {"Output", "speaker"}, {"Game", "controller"}, {"HUD", "gauge"},
      {"Automation", "cursor-arrow"}, {"Settings", "gear"}};
  // Automation (clicks and keys) has no place in VR.
  std::vector<int> shown;
  for (int i = 0; i < kNumTabs; ++i) {
    if (!(g_vr && i == 4)) shown.push_back(i);
  }
  if (std::find(shown.begin(), shown.end(), g_tab) == shown.end()) SwitchTab(0);
  const int shown_at = static_cast<int>(std::find(shown.begin(), shown.end(), g_tab) - shown.begin());
  const float inset = S(20), tab_h = S(60);
  const float tab_w = (pw - inset * 2) / shown.size();
  for (int k = 0; k < static_cast<int>(shown.size()); ++k) {
    const int i = shown[k];
    const ImVec2 a(p0.x + inset + k * tab_w, p0.y), b(a.x + tab_w, p0.y + tab_h);
    bool hovered = false;
    if (!closing) {
      ImGui::PushID(i);
      if (Hit("tab", a, b, &hovered)) SwitchTab(i);
      ImGui::PopID();
    }
    const bool active = g_tab == i;
    const ImU32 col = active ? White() : (hovered ? White(0.9f) : Muted());
    const ImVec2 ts = Measure(g_fonts.tab, tabs[i].label);
    const float icon = S(24), gap = S(10);
    const float x = (a.x + b.x) / 2 - (icon + gap + ts.x) / 2;
    const ImVec2 ic(x + icon / 2, (a.y + b.y) / 2);
    if (!Icon(g_dl, tabs[i].icon, ic, S(26), col)) {
      if (i == 0) BubbleIcon(g_dl, ic, icon, col, S(1.6f), false, -1);
      if (i == 1) SpeakerIcon(ic, icon, col, S(1.6f));
      if (i == 2) ControllerIcon(ic, icon, col, S(1.6f));
      if (i == 3) GaugeIcon(ic, icon, col, S(1.6f));
      if (i == 4) CursorIcon(ic, icon, col, S(1.6f));
      if (i == 5) GearIcon(ic, icon, col, S(1.6f));
    }
    Text(g_fonts.tab, ImVec2(x + icon + gap, ic.y - ts.y / 2), col, tabs[i].label);
  }
  // The thin line under the whole tab row; the active underline sits on it.
  g_dl->AddRectFilled(ImVec2(p0.x + inset, p0.y + tab_h - S(1)), ImVec2(p1.x - inset, p0.y + tab_h), C(51, 51, 51));
  if (!closing) {
    const ImVec2 a(p0.x + inset + shown_at * tab_w, p0.y + tab_h - S(2));
    g_dl->AddRectFilled(a, ImVec2(a.x + tab_w, p0.y + tab_h), White());
  }
  // The page and buttons vanish as soon as the menu closes.
  if (closing && Now() - g_closed_at > 0.1f) return;

  // Q / E switch pages too, like the gamepad bumpers.
  if (!closing && !g_editing && (ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_E))) {
    const int n = static_cast<int>(shown.size());
    SwitchTab(shown[(shown_at + (ImGui::IsKeyPressed(ImGuiKey_E) ? 1 : n - 1)) % n]);
  }

  // Page content in a clipped viewport; on a page switch the old page slides
  // out one way while the new one slides in from the other.
  const float bar_h = S(48), bar_y = p1.y - S(20) - bar_h;
  // The area starts a few pixels outside the rows so the selection outline,
  // drawn just outside a row, isn't clipped.
  const float pad = S(6);
  const ImVec2 c0(p0.x + S(32) - pad, p0.y + tab_h + S(10) - pad);
  const ImVec2 csize(pw - S(32) - S(17) + pad, bar_y - S(12) - c0.y);
  const float e = EaseOutQuart(g_switched_at, kPageTime);
  const float dir = g_tab > g_prev_tab ? 1.0f : -1.0f;
  const bool sliding = e < 1.0f && g_prev_tab != g_tab;

  ImGui::SetCursorScreenPos(c0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("viewport", csize, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(3));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, C4(128, 128, 128));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, C4(160, 160, 160));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, C4(190, 190, 190));
  ImDrawList* outer = g_dl;
  auto page = [&](int tab, float offset, bool live) {
    char id[16];
    std::snprintf(id, sizeof(id), "page%d", tab);
    ImGui::SetCursorScreenPos(ImVec2(c0.x + std::round(offset), c0.y));
    ImGui::BeginChild(id, csize, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    g_dl = ImGui::GetWindowDrawList();
    if (live && !closing) {
      DrawPage(tab, cfg);
    } else {
      // A page sliding away (or the closing frame): drawn, not interactive.
      Config scratch = cfg;
      g_nav_off = true;
      ImGui::BeginDisabled();
      DrawPage(tab, scratch);
      ImGui::EndDisabled();
      g_nav_off = false;
    }
    ImGui::EndChild();
  };
  if (sliding) page(g_prev_tab, -dir * e * csize.x, false);
  page(g_tab, sliding ? dir * (1.0f - e) * csize.x : 0.0f, true);
  g_dl = outer;
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(5);
  ImGui::EndChild();

  // Bottom buttons, like Leave / Respawn / Resume.
  const float gap = S(8);
  const float bw = (pw - inset * 2 - gap * 2) / 3;
  struct Button { const char* label; const char* icon; bool primary; } buttons[] = {
      {"Test Voice", "play-large", false}, {"Stop", "stop-large", false}, {"Close", nullptr, true}};
  for (int i = 0; i < 3; ++i) {
    const Button& bt = buttons[i];
    const ImVec2 a(p0.x + inset + i * (bw + gap), bar_y), b(a.x + bw, bar_y + bar_h);
    bool hovered = false, clicked = false;
    NavItem nav;
    if (!closing) {
      ImGui::PushID(100 + i);
      clicked = Hit("button", a, b, &hovered);
      ImGui::PopID();
      nav = RegisterNav(a, b);
      clicked = clicked || nav.activate;
      if (!g_editing && i < 2 && ImGui::IsKeyPressed(i == 0 ? ImGuiKey_T : ImGuiKey_S)) clicked = true;
    }
    g_dl->AddRectFilled(a, b, bt.primary ? (hovered ? PrimaryHover() : Primary()) : (hovered ? ButtonHover() : ButtonBg()),
                        S(8));
    if (nav.focused) SelectionOutline(a, b, S(8));
    const ImVec2 ts = Measure(g_fonts.button, bt.label);
    const float lead = S(28), lgap = S(12);
    const float x = (a.x + b.x) / 2 - (lead + lgap + ts.x) / 2;
    const ImVec2 lc(x + lead / 2, (a.y + b.y) / 2);
    if (bt.icon == nullptr && g_vr) {
      // No keyboard in the headset: no key hint.
    } else if (bt.icon == nullptr) {
      KeyHint(lc, "ESC", White());
    } else if (!Icon(g_dl, bt.icon, lc, S(24), White())) {
      KeyHint(lc, i == 0 ? "T" : "S", White());
    }
    Text(g_fonts.button, ImVec2(x + lead + lgap, (a.y + b.y) / 2 - ts.y / 2), White(), bt.label);
    if (clicked) {
      if (i == 0 && g_test[0]) Say(g_test, true);
      if (i == 1) StopSpeaking();
      if (i == 2 && g_vr) g_vr_close = true;
      if (i == 2 && !g_vr) overlay::RequestClose();
    }
  }
}

// The top-bar button: a solid circle like Roblox's own top-bar buttons.

void DrawTopBarButton(bool open, bool closing, float W) {
  const overlay::Metrics m = overlay::GetMetrics();
  float x, y, size;
  if (!overlay::ButtonRect(m.window_w > 0 ? m.window_w : static_cast<int>(W / g_px), &x, &y, &size)) return;
  const bool forced = Env("SHOW_BUTTON") != nullptr;
  if (!open && !closing && !forced && !InGame()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 c(S(x + size / 2), S(y + size / 2));
  const float r = S(size / 2);
  const ImVec2 mouse = open ? ImGui::GetIO().MousePos : ImVec2(m.mouse_x * g_px, m.mouse_y * g_px);
  const bool hovered = (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y) <= r * r;
  // Like Roblox's own top-bar buttons: #121215, lighter when hovered, and
  // the icon turns blue (like its active gear) while the menu is open.
  float scale = 1.0f;
  if (IsSpeaking()) scale *= 1.0f + 0.04f * std::sin(Now() * 12.0f);  // talking
  dl->AddCircleFilled(c, r, hovered ? C(42, 43, 48) : C(18, 18, 21), 48);
  const ImU32 ink = open ? C(1, 153, 237) : White();
  // The logo: white with a blue dot; blue with a white dot while open.
  VoxMark(dl, c, S(24) * scale, ink, open ? White() : C(1, 153, 237));
  if (open) {
    ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
    if (ImGui::InvisibleButton("topbar", ImVec2(r * 2, r * 2))) overlay::RequestClose();
  }
}

bool g_was_open = false;
bool g_blur = false;  // the player clicked into the game: drop keyboard focus

}  // namespace

bool OverMenu(ImVec2 p) {
  return overlay::MenuOpen() && !g_layout && p.x >= g_panel.min.x && p.x < g_panel.max.x && p.y >= g_panel.min.y &&
         p.y < g_panel.max.y;
}

void ResetFonts() { g_fonts.px = 0; }

bool PrepareFonts(float px) {
  if (g_fonts.px > 0 && std::fabs(g_fonts.px - px) < 0.01f) return false;
  g_fonts.px = px;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  const std::string dir = FindRobloxFonts();
  if (g_fonts.family != dir) {
    Log(dir.empty() ? "overlay: Builder Sans not found, using a fallback font"
                    : "overlay: using Roblox's Builder Sans from %s", dir.c_str());
    g_fonts.family = dir;
  }
  const char* fallback = "/usr/share/fonts/noto/NotoSans-Medium.ttf";
  // Sizes match Roblox's menu text (its cap heights: tabs 11 px, rows and
  // buttons 10 px, headers 14 px, subtitles 7 px; Builder Sans capitals are
  // ~0.56 of the size ImGui is given).
  g_fonts.title = AddFont(dir, "BuilderSans-Medium.otf", std::round(18 * px), fallback);
  g_fonts.value = AddFont(dir, "BuilderSans-Regular.otf", std::round(18 * px), fallback);
  g_fonts.small = AddFont(dir, "BuilderSans-Regular.otf", std::round(13 * px), fallback);
  g_fonts.tiny = AddFont(dir, "BuilderSans-Regular.otf", std::round(12.5f * px), fallback);
  g_fonts.tab = AddFont(dir, "BuilderSans-Medium.otf", std::round(20 * px), fallback);
  g_fonts.header = AddFont(dir, "BuilderSans-Bold.otf", std::round(25 * px), fallback);
  g_fonts.button = AddFont(dir, "BuilderSans-Bold.otf", std::round(18 * px), fallback);
  g_fonts.hint = AddFont(dir, "BuilderSans-Bold.otf", std::round(10 * px), fallback);
  g_fonts.ui = AddFont(dir, "BuilderSans-Medium.otf", std::round(15 * px), fallback);
  g_fonts.ui_bold = AddFont(dir, "BuilderSans-Bold.otf", std::round(15 * px), fallback);
  // HUD overlays are drawn at any size from one large bake.
  g_fonts.hud = AddFont(dir, "BuilderSans-Bold.otf", std::round(32 * px), fallback);
  io.FontDefault = g_fonts.value;

  // Roblox's icons, baked large and drawn at the sizes each spot needs.
  g_icon_font = nullptr;
  const std::string icon_path = FindRobloxIconFont();
  if (!icon_path.empty() && (g_icons.path() == icon_path || g_icons.Load(icon_path))) {
    static ImVector<ImWchar> ranges;
    ranges.clear();
    for (const char* name : {"frame-bubble-soundwave", "speaker", "gear", "chevron-large-left",
                             "chevron-large-right", "circle-minus", "circle-plus", "square-text-esc",
                             "play-large", "stop-large", "gauge", "cursor-arrow", "controller"}) {
      if (const uint32_t cp = g_icons.Find(name); cp != 0 && cp <= 0xFFFF) {
        ranges.push_back(static_cast<ImWchar>(cp));
        ranges.push_back(static_cast<ImWchar>(cp));
      }
    }
    ranges.push_back(0);
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    g_icon_font = io.Fonts->AddFontFromFileTTF(icon_path.c_str(), std::round(40 * px), &cfg, ranges.Data);
  }
  if (g_icon_font == nullptr) Log("overlay: Roblox's icon font not found, drawing icons instead");
  return true;
}
void FeedInput(const std::vector<overlay::Input>& input, float px) {
  ImGuiIO& io = ImGui::GetIO();
  for (const overlay::Input& in : input) {
    switch (in.type) {
      case overlay::Input::kMousePos:
        io.AddMousePosEvent(in.x * px, in.y * px);
        break;
      case overlay::Input::kMouseButton:
        io.AddMousePosEvent(in.x * px, in.y * px);
        io.AddMouseButtonEvent(in.button, in.down);
        break;
      case overlay::Input::kWheel:
        io.AddMouseWheelEvent(in.x, in.y);
        break;
      case overlay::Input::kText:
        io.AddInputCharactersUTF8(in.text.c_str());
        break;
      case overlay::Input::kBlur:
        g_blur = true;
        break;
      case overlay::Input::kKey: {
        io.AddKeyEvent(ImGuiMod_Ctrl, (in.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (in.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (in.mod & SDL_KMOD_ALT) != 0);
        ImGuiKey key = ImGuiKey_None;
        switch (in.key) {
          case SDLK_TAB: key = ImGuiKey_Tab; break;
          case SDLK_LEFT: key = ImGuiKey_LeftArrow; break;
          case SDLK_RIGHT: key = ImGuiKey_RightArrow; break;
          case SDLK_UP: key = ImGuiKey_UpArrow; break;
          case SDLK_DOWN: key = ImGuiKey_DownArrow; break;
          case SDLK_HOME: key = ImGuiKey_Home; break;
          case SDLK_END: key = ImGuiKey_End; break;
          case SDLK_DELETE: key = ImGuiKey_Delete; break;
          case SDLK_BACKSPACE: key = ImGuiKey_Backspace; break;
          case SDLK_RETURN: key = ImGuiKey_Enter; break;
          case SDLK_SPACE: key = ImGuiKey_Space; break;
          case SDLK_Q: key = ImGuiKey_Q; break;
          case SDLK_E: key = ImGuiKey_E; break;
          case SDLK_S: key = ImGuiKey_S; break;
          case SDLK_T: key = ImGuiKey_T; break;
          case SDLK_KP_ENTER: key = ImGuiKey_KeypadEnter; break;
          case SDLK_ESCAPE: key = ImGuiKey_Escape; break;
          case SDLK_A: key = ImGuiKey_A; break;
          case SDLK_C: key = ImGuiKey_C; break;
          case SDLK_V: key = ImGuiKey_V; break;
          case SDLK_X: key = ImGuiKey_X; break;
          case SDLK_Z: key = ImGuiKey_Z; break;
          default: break;
        }
        if (key != ImGuiKey_None) io.AddKeyEvent(key, in.down);
        break;
      }
    }
  }
}

bool Draw(float px, bool srgb) {
  g_px = px;
  g_srgb = srgb;
  CountFrame();
  ImGuiIO& io = ImGui::GetIO();
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const bool open = overlay::MenuOpen();
  if (!open && !overlay::CursorFree()) {
    // First-person mouse lock: the mouse belongs to the game alone.
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
  if (open && !g_was_open) {
    g_opened_at = Now();
    g_closed_at = -10;
    g_prev_tab = g_tab;
    g_nav = Nav{};
  } else if (!open && g_was_open) {
    g_closed_at = Now();
    g_layout = false;
    // A press that was still held when the menu closed.
    for (int b = 0; b < 3; ++b) io.AddMouseButtonEvent(b, false);
  }
  g_was_open = open;
  const bool closing = !open && Closing();
  Config cfg = GetConfig();
  const Config before = cfg;
  if (g_blur) {
    g_blur = false;
    ImGui::ClearActiveID();
    ImGui::FocusWindow(nullptr);
  }

  AdoptSideBySide(cfg);
  if (open && g_layout) g_layout = LayoutEditor(cfg);
  DrawHud(cfg);

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(W, H));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
  if (!open) flags |= ImGuiWindowFlags_NoInputs;
  ImGui::Begin("##vox-menu", nullptr, flags);
  ImGuiWindow* main_window = ImGui::GetCurrentWindow();
  g_dl = ImGui::GetWindowDrawList();
  if (open && !g_layout) {
    UpdateNav();
    DrawMenu(cfg, W, H, false);
    g_editing = io.WantTextInput;
    // Clicking outside the panel (and off the floating windows) closes the
    // menu, like Roblox's.
    const ImVec2 mp = io.MousePos;
    const bool inside = mp.x >= g_panel.min.x && mp.x < g_panel.max.x && mp.y >= g_panel.min.y && mp.y < g_panel.max.y;
    ImGuiWindow* hovered = GImGui->HoveredWindow;
    const bool on_window = hovered != nullptr && hovered->RootWindow != main_window;
    if (ImGui::IsMouseClicked(0) && !inside && !on_window && !overlay::InHotRect(mp.x / px, mp.y / px) &&
        !Picking()) {
      overlay::RequestClose();
    }
  } else if (closing) {
    DrawMenu(cfg, W, H, true);
  }
  DrawTopBarButton(open, closing, W);
  ImGui::End();
  ImGui::PopStyleVar(2);

  // The floating automation window and click points, over everything.
  std::vector<ImVec4> hot;
  DrawTools(cfg, &hot);
  DrawStatusBadge(cfg);

  // Tell the SDL side where the overlay takes the mouse while the menu is
  // closed: the floating windows (and their popups) and the markers.
  std::vector<overlay::Rect> rects;
  for (ImGuiWindow* w : GImGui->Windows) {
    // Not ImGui's implicit "Debug" window (active until the frame ends) or
    // tooltips, which follow the mouse.
    if (!w->Active || w->Hidden || w->IsFallbackWindow || (w->Flags & ImGuiWindowFlags_Tooltip) ||
        w->RootWindow == main_window) {
      continue;
    }
    hot.push_back(ImVec4(w->Pos.x, w->Pos.y, w->Pos.x + w->Size.x, w->Pos.y + w->Size.y));
  }
  for (const ImVec4& r : hot) rects.push_back({r.x / px, r.y / px, r.z / px, r.w / px});
  overlay::SetHotRects(std::move(rects));
  overlay::SetCaptureMouse(Picking());
  overlay::SetWantsEscape((open && g_layout) || Picking());
  overlay::SetWantsText(io.WantTextInput);

  if (cfg != before) SetConfig(cfg);
  SaveConfigIfDirty();
  const bool button_on = cfg.menu_button != "off" &&
                         (InGame() || Env("SHOW_BUTTON") != nullptr);
  return open || closing || button_on || HudVisible(cfg);
}

}  // namespace nac::ui

namespace nac::ui {

// --------------------------------------------------------------------------
// The headset (VR) panel

namespace {
int g_surface = 0;
Fonts g_surface_fonts[2];
ImFont* g_surface_icons[2] = {nullptr, nullptr};
}  // namespace

void SelectSurface(int surface) {
  if (surface == g_surface) return;
  g_surface_fonts[g_surface] = g_fonts;
  g_surface_icons[g_surface] = g_icon_font;
  g_surface = surface;
  g_fonts = g_surface_fonts[surface];
  g_icon_font = g_surface_icons[surface];
}

bool TakeVrCloseRequest() {
  const bool close = g_vr_close;
  g_vr_close = false;
  return close;
}

bool DrawVr(float px, bool srgb, bool panel_open, float* stats_w) {
  g_px = px;
  g_srgb = srgb;
  g_alpha = 1.0f;
  g_vr = true;
  ImGuiIO& io = ImGui::GetIO();
  Config cfg = GetConfig();
  const Config before = cfg;

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##vox-vr", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollWithMouse);
  g_dl = ImGui::GetWindowDrawList();
  if (panel_open) {
    g_editing = false;
    DrawMenu(cfg, S(kVrPanelW), S(kVrPanelH), false);
    // Where the controller points: a blue dot with a white ring.
    const ImVec2 m = io.MousePos;
    if (m.x >= 0 && m.y >= 0 && m.x < S(kVrPanelW) && m.y < S(kVrPanelH)) {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      fg->AddCircleFilled(m, S(7), C(1, 153, 237), 24);
      fg->AddCircle(m, S(7), White(), 24, S(2));
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  *stats_w = 0;
  if (cfg.vr_stats != "off") {
    *stats_w = DrawStatsPill(ImGui::GetForegroundDrawList(), cfg,
                             ImVec2(0, S(kVrPanelH) + std::round((S(kVrStatsH) - S(44)) / 2)), g_px);
  }
  g_vr = false;
  if (cfg != before) SetConfig(cfg);
  return panel_open || *stats_w > 0;
}

}  // namespace nac::ui
