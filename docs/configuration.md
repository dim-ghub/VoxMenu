# Configuration

Settings live in `~/.config/vox-menu/config.json` (`$XDG_CONFIG_HOME`). Everything is editable in
the menu; missing keys keep their defaults. Old `vox-tts/` and `nightcap-ac/` configs are migrated
on first launch.

## Speech

| Key | Default | Values |
| --- | --- | --- |
| `enabled` | `true` | Speak chat messages |
| `intercept_chat` | `false` | Speak instead of sending to chat |
| `skip_commands` | `true` | Don't speak messages starting with `/` |
| `engine` | `"espeak"` | `"espeak"`, `"piper"`, `"animalese"` |
| `output` | `"vc"` | `"vc"`, `"both"`, `"local"` |
| `vc_volume`, `local_volume` | `0.9`, `0.7` | 0–1.5 |
| `mix_real_mic` | `true` | Keep your mic on while speaking |
| `menu_button` | `"left"` | `"left"`, `"right"`, `"off"` |
| `piper_model` | `""` | `.onnx` path; empty uses the first found |
| `piper_dir` | `""` | Extra folder to search for models |
| `piper_speed` | `1.0` | 0.5–2 |
| `espeak_voice` | `"en-us"` | eSpeak voice code |
| `espeak_speed`, `espeak_pitch` | `175`, `50` | WPM; 0–99 |
| `voice` | `"normal"` | Animalese villager: `normal`, `peppy`, `sisterly`, `snooty`, `jock`, `lazy`, `smug`, `cranky` |
| `animalese_lang` | `"english"` | `"english"`, `"korean"` |
| `pitch`, `speed` | `0`, `1.0` | Semitones (±12); 0.5–2 |
| `sample_pack` | `""` | Folder of `a.wav`…`z.wav`, or an Animalese.js sprite WAV |

Piper models are found under `~/.local/share/piper-voices`, `/usr/share/piper-voices`,
`/usr/local/share/piper-voices`, `~/.local/share/vox-menu/voices`, `~/.config/piper/voices` and
`piper_dir`.

## App audio

| Key | Default | Values |
| --- | --- | --- |
| `media_on` | `false` | Share an app's sound |
| `media_source` | `""` | `application.name` from `pactl list sink-inputs`, or `"*"` for all |
| `media_volume` | `0.6` | 0–1.5 |

## Game

| Key | Default | Values |
| --- | --- | --- |
| `fps_cap` | `0` | 0 = unlimited, up to 1000 |
| `fps_cap_background` | `0` | Cap when unfocused; 0 = same as `fps_cap` |
| `hop_mode` | `"random"` | `"random"`, `"smallest"`, `"ping"` |

## HUD

Each of `hud_fps`, `hud_ping`, `hud_server`, `hud_keys` has `_on`, `_x`, `_y` (0–1 across the
free space, 0 = left/top), `_scale` (0.5–3) and `_attach` (`"fps"`, `"ping"`, `"server"` or `""`:
joined onto that overlay's pill).

| Key | Default | Values |
| --- | --- | --- |
| `hud_frametime` | `false` | Show ms per frame |
| `hud_server_ip` | `false` | Show the server address |
| `hud_keys_space`, `hud_keys_mouse`, `hud_keys_cps` | `true` | Keystrokes rows |
| `hud_keys_pressed` | `"white"` | `"white"`, `"blue"` |
| `hud_match_roblox` | `true` | Follow Roblox's look and transparency |
| `hud_background` | `0.55` | Opacity when not matching Roblox |
| `hud_snap` | `true` | Snap in the layout editor |

## Autoclicker

| Key | Default | Values |
| --- | --- | --- |
| `click_button` | `"left"` | `"left"`, `"right"`, `"middle"` |
| `click_type` | `"single"` | `"single"`, `"double"`, `"hold"` |
| `click_target` | `"mouse"` | `"mouse"`, `"points"` |
| `click_points` | `""` | `"x,y;x,y"`, window fractions |
| `click_cps` | `10` | 0.1–200 |
| `click_hold_ms` | `20` | Press length (max 60% of the interval) |
| `click_jitter` | `0` | % of the interval |
| `click_spread` | `0` | Pixels around a point |
| `click_duration`, `click_limit` | `0` | Seconds / clicks; 0 = none |
| `click_delay` | `0` | Seconds before starting |
| `click_move_cursor` | `false` | Warp the real cursor to points |

## Macros, window and hotkeys

| Key | Default | Values |
| --- | --- | --- |
| `macro_name` | `""` | Macro loaded at start |
| `macro_repeat` | `1` | 0 = loop |
| `macro_speed` | `1` | Playback speed |
| `macro_record_moves` | `false` | Record mouse movement |
| `tools_open` | `false` | Show the automation window |
| `tools_x`, `tools_y` | `0.706`, `0.094` | Window position, screen fractions |
| `pause_in_menu` | `true` | No automation while the menu is open |
| `key_clicker` | `"F6"` | SDL key name or `"off"` |
| `key_clicker_mode` | `"toggle"` | `"toggle"`, `"hold"` |
| `key_macro`, `key_record`, `key_tools` | `"F7"`, `"F9"`, `"F4"` | |

## Piano

| Key | Default | Values |
| --- | --- | --- |
| `piano_folder` | `""` | Songs folder; empty = `XDG_MUSIC_DIR` |
| `piano_watch_downloads` | `true` | Move downloaded `.mid` files into it |
| `piano_88` | `true` | All 88 keys (Ctrl for the outer ones) |
| `piano_fold` | `false` | Shift out-of-range notes by octaves |
| `piano_transpose` | `0` | Semitones |
| `piano_sustain` | `"off"` | `"off"`, `"pedal"`, `"inverted"` |
| `piano_velocity` | `false` | Alt + key loudness before notes |
| `piano_human` | `0` | Random timing, ms |
| `piano_skip_drums` | `true` | Mute drum tracks |
| `piano_live` | `false` | Play MIDI keyboards into the game |
| `piano_output` | `"keys"` | `"keys"`, `"midiconnect"` |
| `key_piano_play`, `key_piano_back`, `key_piano_forward` | `"F1"`, `"F2"`, `"F3"` | |

## VR

| Key | Default | Values |
| --- | --- | --- |
| `vr_stats` | `"view"` | `"view"`, `"off"` |
| `vr_toggle` | `"left_stick"` | `"left_stick"`, `"right_stick"` |

## Macro files

`~/.config/vox-menu/macros/<name>.macro`, one step per line, `#` for comments. Points are window
fractions; omit them to use the cursor.

```
key_down W
wait 500              # ms
key_up W
key E                 # press and release
click left 0.5 0.5    # also mouse_down / mouse_up
move 0.25 0.75
text gg
```
