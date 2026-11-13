# Configuration Reference

Vox Menu stores user preferences in a standard JSON format under the user's XDG configuration directory:

```
$XDG_CONFIG_HOME/vox-menu/config.json
(typically ~/.config/vox-menu/config.json)
```

Settings can be changed dynamically using the in-game overlay menu (press **F8** or click the top-bar button) or edited by modifying the JSON file directly.

---

## Configuration Schema

Below are the text-to-speech settings with their default values; the HUD, game and automation
settings follow in the tables further down. Settings missing from the file keep their defaults.

```json
{
  "enabled": true,
  "intercept_chat": false,
  "output": "vc",
  "engine": "piper",

  "piper_model": "",
  "piper_dir": "",
  "piper_speed": 1.0,

  "espeak_voice": "en-us",
  "espeak_speed": 175.0,
  "espeak_pitch": 50.0,

  "style": "classic",
  "voice": "normal",
  "pitch": 0.0,
  "speed": 1.0,
  "sample_pack": "",

  "vc_volume": 0.9,
  "local_volume": 0.7,
  "mix_real_mic": true,
  "skip_commands": true,
  "menu_button": "left"
}
```

---

## Field Details

### General & Routing

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | boolean | `true` | Master switch for Text-to-Speech synthesis. |
| `intercept_chat` | boolean | `false` | When true, chat messages are spoken via TTS and intercepted so they are not sent to Roblox text chat. |
| `output` | string | `"vc"` | Routing destination for speech audio. Options: `"vc"` (Voice Chat only), `"both"` (Voice Chat and local speakers), `"local"` (Speakers only). |
| `engine` | string | `"piper"` | Active speech engine: `"piper"` (Piper Neural), `"espeak"` (eSpeak-NG), or `"animalese"` (Animal Crossing). |
| `skip_commands` | boolean | `true` | If true, messages starting with `/` (e.g. `/e dance`, `/w player`) are ignored and not spoken aloud. |
| `menu_button` | string | `"left"` | Placement of the in-game menu button: `"left"` (top-left near Roblox icon), `"right"` (top-right), or `"off"` (hidden, menu accessible via F8 only). |

---

### Piper Settings (`engine == "piper"`)

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `piper_model` | string | `""` | Absolute path to the selected `.onnx` voice model file. If empty, the first discovered model is used. |
| `piper_dir` | string | `""` | Custom directory path to scan for additional `.onnx` models. |
| `piper_speed` | float | `1.0` | Speed multiplier for speech pace ($0.5$ to $2.0$). |

---

### eSpeak-NG Settings (`engine == "espeak"`)

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `espeak_voice` | string | `"en-us"` | Voice / accent code passed to eSpeak (e.g., `"en-us"`, `"en-gb"`, `"es"`, `"fr"`, `"ja"`). |
| `espeak_speed` | float | `175.0` | Speaking speed in Words Per Minute (WPM) ($80$ to $350$). |
| `espeak_pitch` | float | `50.0` | Pitch contour level ($0$ to $99$). |

---

### Animal Crossing Settings (`engine == "animalese"`)

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `voice` | string | `"normal"` | Villager personality archetype or voice code: `"normal"` (`f1`), `"peppy"` (`f2`), `"sisterly"` (`f3`), `"snooty"` (`f4`), `"jock"` (`m1`), `"lazy"` (`m2`), `"smug"` (`m3`), or `"cranky"` (`m4`). |
| `animalese_lang` | string | `"english"` | Authentic sound bank language: `"english"` or `"korean"`. |
| `pitch` | float | `0.0` | Pitch offset in semitones ($-12$ to $+12$). |
| `speed` | float | `1.0` | Tempo multiplier on the villager pace ($0.5$ to $2.0$). |
| `sample_pack` | string | `""` | Optional path to custom `a.wav`...`z.wav` recordings or an `Animalese.js` sound sprite. |

---

### Audio & Mixing

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `vc_volume` | float | `0.9` | Volume gain applied when injecting audio into voice chat ($0.0$ to $1.5$). |
| `local_volume` | float | `0.7` | Volume gain applied to local speaker playback ($0.0$ to $1.5$). |
| `media_on` | boolean | `false` | Share an app's sound into voice chat. |
| `media_source` | string | `""` | The app to share, by its `application.name` (as `pactl list sink-inputs` shows it), or `"*"` for every app except Nightcap. |
| `media_volume` | float | `0.6` | Volume of the shared audio ($0.0$ to $1.5$). |
| `mix_real_mic` | boolean | `true` | If `true`, your physical microphone input is blended with the TTS speech. If `false`, physical microphone input is silenced while synthetic speech is playing. |

---

### Game: Frame Rate and Servers

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `fps_cap` | float | `0` | Frame-rate cap; `0` is unlimited. Any value up to `1000`; type it in the menu's Exact FPS Cap field. |
| `fps_cap_background` | float | `0` | Cap while the Roblox window isn't focused; `0` keeps `fps_cap`. |
| `hop_mode` | string | `"random"` | Which server a hop picks: `"random"`, `"smallest"` (fewest players) or `"ping"` (best reported ping). |

---

### HUD Overlays

Each overlay has five settings, `<name>_on`, `<name>_x`, `<name>_y`, `<name>_scale` and
`<name>_attach`, for
`hud_fps`, `hud_ping`, `hud_server` and `hud_keys`. `x` and `y` run from `0` (against the left /
top edge) to `1` (against the right / bottom edge), with `0.5` centred, so an overlay stays
lined up with the edge it is near as its text changes width. `scale` runs from `0.5` to `3`.
`attach` (`"fps"`, `"ping"` or `"server"`, or `""`) joins the overlay onto the right end of that
one, in one pill; its own `x` and `y` then don't apply.

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `hud_fps_on`, `hud_ping_on`, `hud_server_on`, `hud_keys_on` | boolean | `false` | Which overlays are shown. |
| `hud_frametime` | boolean | `false` | FPS overlay also shows milliseconds per frame. |
| `hud_server_ip` | boolean | `false` | Server overlay also shows the server's address. |
| `hud_keys_space` | boolean | `true` | Keystrokes overlay shows the Space bar. |
| `hud_keys_mouse` | boolean | `true` | Keystrokes overlay shows the mouse buttons. |
| `hud_keys_cps` | boolean | `true` | Clicks per second under the mouse buttons. |
| `hud_match_roblox` | boolean | `true` | Overlays are solid like Roblox's top-bar buttons, and the automation window follows Roblox's Background Transparency setting (`PreferredTransparency`): `0.82` opaque normally, solid at `0`. |
| `hud_keys_pressed` | string | `"white"` | How a held key looks in the keystrokes overlay: `"white"` (white with a dark letter) or `"blue"`. |
| `hud_background` | float | `0.55` | Overlay and window background opacity (`0` to `1`) when not matching Roblox. |
| `hud_snap` | boolean | `true` | Snap while dragging in the layout editor (and join pills dropped beside each other). |
| `hud_joins_adopted` | boolean | `false` | Set after the one-time step that joins pills an older version showed side by side. |

---

### Autoclicker

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `click_button` | string | `"left"` | `"left"`, `"right"` or `"middle"`. |
| `click_type` | string | `"single"` | `"single"`, `"double"`, or `"hold"` (press and keep it down). |
| `click_target` | string | `"mouse"` | `"mouse"` (wherever the cursor is) or `"points"` (the points below, in turn). |
| `click_points` | string | `""` | `"x,y;x,y"`, as fractions of the window. |
| `click_cps` | float | `10` | Clicks per second (`0.1` to `200`). |
| `click_hold_ms` | float | `20` | How long each press lasts; never more than 60% of the gap between clicks. |
| `click_jitter` | float | `0` | Random timing, in percent of the interval (`0` to `90`). |
| `click_spread` | float | `0` | Random offset around a point, in pixels. |
| `click_duration` | float | `0` | Stop after this many seconds; `0` never stops. |
| `click_limit` | float | `0` | Stop after this many clicks; `0` has no limit. |
| `click_delay` | float | `0` | Countdown in seconds before clicking starts. |
| `click_move_cursor` | boolean | `false` | Move the real cursor to each point; otherwise only the game sees the click there. |

---

### VR

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `vr_stats` | string | `"view"` | In a headset, the FPS / ping / server overlays that are on show as one pill low in view (`"view"`), or not (`"off"`). |
| `vr_toggle` | string | `"left_stick"` | The thumbstick click to hold (half a second) to open or close the panel in VR: `"left_stick"` or `"right_stick"`. |

---

### Macros, Automation Window and Hotkeys

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `macro_name` | string | `""` | The saved macro loaded at start (`~/.config/vox-menu/macros/<name>.macro`). |
| `macro_repeat` | float | `1` | Times to play it; `0` loops until stopped. |
| `macro_speed` | float | `1` | Playback speed multiplier (`0.25` to `4` in the window, `0.1` to `10` by hand). |
| `macro_record_moves` | boolean | `false` | Record mouse movement too, not just clicks. |
| `tools_open` | boolean | `false` | Show the floating automation window. |
| `tools_x`, `tools_y` | float | `0.706`, `0.094` | Where the window's top-left corner is, as fractions of the screen. |
| `pause_in_menu` | boolean | `true` | No clicks or macro input while the menu is open. |
| `key_clicker` | string | `"F6"` | Autoclicker on/off. An SDL key name, or `"off"`. |
| `key_clicker_mode` | string | `"toggle"` | `"toggle"` (press to start, press again to stop) or `"hold"` (clicks only while the key is held). |
| `key_macro` | string | `"F7"` | Play / stop the macro. |
| `key_record` | string | `"F9"` | Start / stop recording. |
| `key_tools` | string | `"F4"` | Show / hide the automation window. |

### Piano

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `piano_folder` | string | `""` | Where the `.mid` files are (sub-folders included); empty is your Music folder (`XDG_MUSIC_DIR`). `~` is your home. |
| `piano_watch_downloads` | boolean | `true` | Move `.mid` / `.midi` files saved to the Downloads folder (`XDG_DOWNLOAD_DIR`) into the songs folder while the game runs. |
| `piano_88` | boolean | `true` | All 88 keys: notes below C2 and above C7 are Ctrl + key. Off: only the 61 keys from C2 to C7. |
| `piano_fold` | boolean | `false` | Move notes past the keyboard by octaves until they fit, instead of dropping them. |
| `piano_transpose` | float | `0` | Semitones up (or down, negative). |
| `piano_sustain` | string | `"off"` | `"pedal"` holds Space while the song's sustain pedal is down, `"inverted"` while it's up, `"off"` ignores it. |
| `piano_velocity` | boolean | `false` | Before notes, press Alt + one of 32 keys (`1`..`c`) for how loud they are. |
| `piano_human` | float | `0` | Random timing of up to this many milliseconds. |
| `piano_skip_drums` | boolean | `true` | Drum tracks (named like one, or on channel 10) start muted. |
| `piano_live` | boolean | `false` | Play MIDI keyboards into the game. |
| `piano_output` | string | `"keys"` | How notes from songs and the MIDI keyboard reach the game: `"keys"` (the piano layout) or `"midiconnect"` (`*` and four base-12 numpad digits per note and pedal message). |
| `key_piano_play` | string | `"F1"` | Play / pause the song. |
| `key_piano_back` | string | `"F2"` | 10 seconds back. |
| `key_piano_forward` | string | `"F3"` | 10 seconds forward. |

### Macro files

One step per line; `#` starts a comment. Points are fractions of the window; leave them out to
use wherever the cursor is.

```
key_down W            # hold a key (SDL key names: W, Space, Left Shift, Return, F5 ...)
wait 500              # milliseconds
key_up W
key E                 # press and release
click left 0.5 0.5    # mouse_down / mouse_up work the same way
move 0.25 0.75
text gg               # typed text, e.g. into the open chat box
```

---

## Automatic Migration

Settings from **Vox TTS** (`~/.config/vox-tts/config.json`) or **Nightcap AC**
(`~/.config/nightcap-ac/config.json`) are picked up and saved to `~/.config/vox-menu/config.json`
on the first launch.
