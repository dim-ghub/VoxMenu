// Settings for Vox Menu, stored in $XDG_CONFIG_HOME/vox-menu/config.json
// (migrated from the older vox-tts/ and nightcap-ac/ locations).

#ifndef VOX_MENU_CONFIG_H_
#define VOX_MENU_CONFIG_H_

#include <string>

namespace nac {

// One movable, resizable HUD overlay. x and y are where it sits across the
// room the screen leaves it: 0 against the left / top edge, 1 against the
// right / bottom, 0.5 centred. Widgets stay lined up with the edge they are
// near as their text changes width, and layouts survive resolution changes.
struct HudWidget {
  bool on = false;
  double x = 0.5, y = 0.5;
  double scale = 1.0;
  // Joined onto the right end of this overlay ("fps", "ping", "server"), in
  // one pill; x and y then don't apply. Set by dropping it there.
  std::string attach;
};

struct Config {
  bool enabled = true;
  bool intercept_chat = false;  // speak via TTS without sending text to game chat
  std::string output = "vc";  // "vc", "both" or "local"
  std::string engine = "espeak";  // "espeak", "piper" or "animalese"

  // Piper neural TTS settings
  std::string piper_model;  // path to .onnx model
  std::string piper_dir;    // custom search directory for models
  double piper_speed = 1.0;

  // eSpeak-NG settings
  std::string espeak_voice = "en-us";
  double espeak_speed = 175.0;  // words per minute (80..450)
  double espeak_pitch = 50.0;   // 0..99

  // Animal Crossing (Animalese) settings
  std::string voice = "normal";
  std::string style = "classic";
  std::string animalese_lang = "english";  // "english" or "korean"
  double pitch = 0.0;
  double speed = 1.0;
  std::string sample_pack;

  // Audio output & UI settings
  double vc_volume = 0.9;
  double local_volume = 0.7;
  bool mix_real_mic = true;   // keep your real mic in voice chat

  // Sharing an app's sound into voice chat (media.h)
  bool media_on = false;
  std::string media_source;   // application name, "*" for every other app
  double media_volume = 0.6;
  bool skip_commands = true;  // don't speak "/e dance", "/w name" ...
  std::string menu_button = "left";  // "left", "right" or "off"

  // HUD overlays
  // Under Roblox's top bar, 16 px in and 8 px apart (at 720p).
  HudWidget hud_fps{false, 0.0135, 0.1006, 1.0};
  HudWidget hud_ping{false, 0.0135, 0.1775, 1.0};
  HudWidget hud_server{false, 0.0135, 0.2544, 1.0};
  HudWidget hud_keys{false, 0.0141, 0.9705, 1.0};
  bool hud_frametime = false;    // FPS widget also shows ms per frame
  bool hud_server_ip = false;    // server widget also shows the address
  bool hud_keys_mouse = true;    // keystrokes: LMB / RMB row
  bool hud_keys_space = true;    // keystrokes: Space bar row
  bool hud_keys_cps = true;      // keystrokes: clicks per second
  std::string hud_keys_pressed = "white";  // pressed keys: "white" or "blue"
  bool hud_match_roblox = true;  // look like Roblox's top bar (and window like its menu)
  double hud_background = 0.55;  // widget background opacity, when not matching
  bool hud_snap = true;          // snap while dragging in the layout editor
  bool hud_joins_adopted = false;  // pills side by side from before joining existed were joined

  // Game: frame rate and servers
  double fps_cap = 0;             // frames per second; 0 = unlimited
  double fps_cap_background = 0;  // while Roblox isn't focused; 0 = same as fps_cap
  std::string hop_mode = "random";  // "random", "smallest", "ping"

  // Autoclicker
  std::string click_button = "left";   // "left", "right", "middle"
  std::string click_type = "single";   // "single", "double", "hold"
  std::string click_target = "mouse";  // "mouse" (follow) or "points"
  std::string click_points;            // "x,y;x,y" fractions of the window
  double click_cps = 10;               // clicks per second
  double click_jitter = 0;             // random timing, % of the interval
  double click_spread = 0;             // random offset around a point, px
  double click_hold_ms = 20;           // how long each press is held
  double click_duration = 0;           // seconds; 0 = until stopped
  double click_limit = 0;              // clicks; 0 = no limit
  double click_delay = 0;              // countdown before starting, s
  bool click_move_cursor = false;      // warp the real cursor to points

  // Macros
  std::string macro_name;          // the macro loaded in the editor
  double macro_repeat = 1;         // 0 = loop until stopped
  double macro_speed = 1.0;        // playback speed multiplier
  bool macro_record_moves = false;  // record mouse movement, not just clicks

  // Piano (MIDI player, piano.h)
  std::string piano_folder;           // songs; empty = the Music folder
  bool piano_88 = true;               // all 88 keys (Ctrl for the outer ones), not just 61
  bool piano_fold = false;            // shift notes past the keyboard by octaves to fit
  double piano_transpose = 0;         // semitones
  std::string piano_sustain = "off";  // "off", "pedal" (Space while held) or "inverted"
  bool piano_velocity = false;        // Alt+key to set the loudness of each note
  double piano_human = 0;             // random timing, up to this many ms; 0 = exact
  bool piano_skip_drums = true;       // drum tracks start muted
  bool piano_live = false;            // play a MIDI keyboard into the game
  std::string piano_live_mode = "keys";  // "keys" or "midiconnect" (numpad codes)
  std::string key_piano_play = "F1";
  std::string key_piano_back = "F2";     // 10 s back
  std::string key_piano_forward = "F3";  // 10 s forward

  // Automation window and hotkeys
  // VR (headset) panel and stats
  std::string vr_stats = "view";        // "view": FPS / ping / server low in view; "off"
  std::string vr_toggle = "left_stick";  // hold to open the panel: "left_stick" or "right_stick"

  bool tools_open = false;
  double tools_x = 0.706, tools_y = 0.094;  // window's top-left: top right, under the top bar
  bool pause_in_menu = true;              // no clicks while this menu is open
  std::string key_clicker = "F6";
  std::string key_clicker_mode = "toggle";  // "toggle" or "hold" (clicks while held)
  std::string key_macro = "F7";
  std::string key_record = "F9";

  bool uses_vc() const { return output == "vc" || output == "both"; }
  bool uses_local() const { return output == "local" || output == "both"; }

  // Calls v(name, field) for every stored setting.
  template <typename V>
  void Visit(V&& v);

  std::string ToJson() const;
  static std::string Path();
  static Config Load();
  bool Save() const;
};

template <typename V>
void Config::Visit(V&& v) {
  v("enabled", enabled);
  v("intercept_chat", intercept_chat);
  v("output", output);
  v("engine", engine);
  v("piper_model", piper_model);
  v("piper_dir", piper_dir);
  v("piper_speed", piper_speed);
  v("espeak_voice", espeak_voice);
  v("espeak_speed", espeak_speed);
  v("espeak_pitch", espeak_pitch);
  v("voice", voice);
  v("style", style);
  v("animalese_lang", animalese_lang);
  v("pitch", pitch);
  v("speed", speed);
  v("sample_pack", sample_pack);
  v("vc_volume", vc_volume);
  v("local_volume", local_volume);
  v("mix_real_mic", mix_real_mic);
  v("media_on", media_on);
  v("media_source", media_source);
  v("media_volume", media_volume);
  v("skip_commands", skip_commands);
  v("menu_button", menu_button);

  const std::pair<const char*, HudWidget*> widgets[] = {
      {"hud_fps", &hud_fps}, {"hud_ping", &hud_ping}, {"hud_server", &hud_server}, {"hud_keys", &hud_keys}};
  for (const auto& [name, w] : widgets) {
    const std::string n = name;
    v((n + "_on").c_str(), w->on);
    v((n + "_x").c_str(), w->x);
    v((n + "_y").c_str(), w->y);
    v((n + "_scale").c_str(), w->scale);
    v((n + "_attach").c_str(), w->attach);
  }
  v("hud_frametime", hud_frametime);
  v("hud_server_ip", hud_server_ip);
  v("hud_keys_mouse", hud_keys_mouse);
  v("hud_keys_space", hud_keys_space);
  v("hud_keys_cps", hud_keys_cps);
  v("hud_keys_pressed", hud_keys_pressed);
  v("hud_match_roblox", hud_match_roblox);
  v("hud_background", hud_background);
  v("hud_snap", hud_snap);
  v("hud_joins_adopted", hud_joins_adopted);

  v("fps_cap", fps_cap);
  v("fps_cap_background", fps_cap_background);
  v("hop_mode", hop_mode);

  v("click_button", click_button);
  v("click_type", click_type);
  v("click_target", click_target);
  v("click_points", click_points);
  v("click_cps", click_cps);
  v("click_jitter", click_jitter);
  v("click_spread", click_spread);
  v("click_hold_ms", click_hold_ms);
  v("click_duration", click_duration);
  v("click_limit", click_limit);
  v("click_delay", click_delay);
  v("click_move_cursor", click_move_cursor);

  v("macro_name", macro_name);
  v("macro_repeat", macro_repeat);
  v("macro_speed", macro_speed);
  v("macro_record_moves", macro_record_moves);

  v("piano_folder", piano_folder);
  v("piano_88", piano_88);
  v("piano_fold", piano_fold);
  v("piano_transpose", piano_transpose);
  v("piano_sustain", piano_sustain);
  v("piano_velocity", piano_velocity);
  v("piano_human", piano_human);
  v("piano_skip_drums", piano_skip_drums);
  v("piano_live", piano_live);
  v("piano_live_mode", piano_live_mode);
  v("key_piano_play", key_piano_play);
  v("key_piano_back", key_piano_back);
  v("key_piano_forward", key_piano_forward);

  v("vr_stats", vr_stats);
  v("vr_toggle", vr_toggle);
  v("tools_open", tools_open);
  v("tools_x", tools_x);
  v("tools_y", tools_y);
  v("pause_in_menu", pause_in_menu);
  v("key_clicker", key_clicker);
  v("key_clicker_mode", key_clicker_mode);
  v("key_macro", key_macro);
  v("key_record", key_record);
}

inline bool operator==(const Config& a, const Config& b) { return a.ToJson() == b.ToJson(); }
inline bool operator!=(const Config& a, const Config& b) { return !(a == b); }

}  // namespace nac

#endif  // VOX_MENU_CONFIG_H_
