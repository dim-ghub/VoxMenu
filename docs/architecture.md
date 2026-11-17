# Architecture

`libvox_menu.so` is loaded into Nightcap three ways at once, sharing one copy in memory:

* `LD_PRELOAD`: wraps SDL3 functions for input, chat and the microphone (`sdl_hooks.cc`).
* An implicit Vulkan layer: draws the menu and HUD before each present (`layer.cc`).
* An OpenXR API layer: draws the menu in a headset (`xr_layer.cc`).

The launcher (`scripts/vox-menu`) writes the layer manifests next to the library and enables them.

## SDL hooks

* **Chat** (`chat.cc`): `ChatTracker` rebuilds the focused text box from key and text events; the
  box's contents on Enter is the message. With Intercept Chat, Enter is replaced by Escape so
  Roblox discards the draft.
* **VR keyboard** (`vr_keyboard.cc`): Nightcap's headset keyboard bypasses SDL events, so its
  host callbacks (`mocktail::vr::g_host`, found via `/proc/self/exe` symbols) are wrapped to feed
  the same tracker.
* **Microphone** (`speech.cc`): `MixIntoMic()` mixes queued speech and shared app audio into
  what Nightcap reads from the capture stream.
* **App audio** (`media.cc`): each of the chosen app's streams is recorded with
  `parec --monitor-stream`, capped at 250 ms of buffer.
* **Input routing:** while the menu is open it takes the mouse and keyboard and keeps the cursor
  unlocked. While closed, presses inside the automation window or click-point markers
  (`overlay::SetHotRects`) go to the overlay; everything else goes to the game.
* **Injection** (`inject.h`): synthetic events are queued and returned from Nightcap's next event
  poll, so they only reach the Roblox window.

## Vulkan overlay

Before each `vkQueuePresentKHR`, Dear ImGui draws into the swapchain image, waiting on the game's
semaphores and signalling ours. The HUD (`hud.cc`) is drawn in the background list; the menu
(`ui.cc`) on top. Window opacity follows Roblox's `PreferredTransparency`
(`roblox_settings.cc`). The FPS cap (`framecap.cc`, from Cocktail) sleeps before present.

## Automation and piano

The autoclicker, macros (`automation.cc`) and piano (`piano.cc`) tick on Nightcap's main thread
at every event poll and inject through `inject.h`. A virtual click at a fixed point is
move → press → release → move back, unless "Move Real Cursor" is on.

Piano notes map to Virtual Piano keys (`1234567890qwerty...` for C2–C7, Shift for black keys,
Ctrl for the 88-key extremes) or to MIDI Connect codes (`*` plus four base-12 numpad digits).
MIDI files are parsed by `midi_file.cc`; keyboards come through the ALSA sequencer
(`midi_input.cc`, `libasound` loaded at run time).

## Nightcap integration

Each piece falls back when the Nightcap build lacks it.

| What | Through | Fallback |
| --- | --- | --- |
| Server joins / leaves | `mocktail_android_log_add_observer` | Polling `latest.log` |
| Ping | Nightcap's datagram observer; ACK timing as in its `AckPingEstimator` | ICMP, then TCP handshake |
| Rejoin / hop | The external-launch broker socket (`MOCKTAIL-EXTERNAL-LAUNCH/1`) | Starting Nightcap with the `roblox://` link |
| Server address | The UDMUX log line | `/proc/self/net/udp` |

Server hop reads `games.roblox.com/v1/games/<place>/servers/Public`. Location is looked up at
`ip-api.com`, only while that overlay is on.

## Control socket

`$XDG_RUNTIME_DIR/vox-menu/control-<pid>.sock` (datagram). Running `vox-menu` while Nightcap is
up sends `open`, which opens the menu.

## VR

At `xrCreateSession` the layer notes the game's Vulkan device and creates a swapchain for the
panel and stats pill, drawn by a second ImGui context (`xr_render.h`). At `xrEndFrame` it adds
them as quad layers. Its own action set is added to Nightcap's bindings for known controller
profiles (retried without them if the runtime refuses). While the panel is open, the game's
actions read as idle. The FPS cap is off in VR.
