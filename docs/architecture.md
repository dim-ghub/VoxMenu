# Vox Menu Architecture

Vox Menu is an in-game menu for Roblox on Linux through [Nightcap](https://github.com/CoderDayton/nightcap): text-to-speech into voice chat, HUD overlays, an autoclicker and macros, and rejoin / server hop / FPS cap controls.

Unlike typical voice-chat soundboards or TTS tools, Vox Menu **does not require**:
- Virtual audio cables (PulseAudio null sinks, PipeWire virtual nodes).
- Virtual microphone loops.
- Separate Electron or Python companion applications running in the background.

Instead, the entire system is implemented as a single, lightweight shared library (`libvox_menu.so`) that operates simultaneously across two complementary subsystem layers:

1. **SDL3 Dynamic Interception (`LD_PRELOAD`)**: Hooks audio capture and user input to extract chat and inject audio.
2. **Implicit Vulkan Overlay Layer**: Intercepts Vulkan swapchain presentation to draw an in-game settings menu.

```
                  Nightcap (Unmodified Binary)
 ┌─────────────────────────────────────────────────────────────┐
 │ libSDL3.so ◄─── libvox_menu.so (via LD_PRELOAD)              │
 │                   · Intercepts text input events            │
 │                   · Rebuilds chat messages via ChatTracker  │
 │                   · Mixes TTS audio into mic stream in-line │
 │                                                             │
 │ Vulkan API ◄─── libvox_menu.so (via Vulkan Layer Manifest)   │
 │                   · Intercepts vkQueuePresentKHR            │
 │                   · Renders Dear ImGui menu into swapchain  │
 │                   · Roblox Builder Sans font & BuilderIcons │
 └─────────────────────────────────────────────────────────────┘
```

Both halves reside within the exact same loaded shared library, sharing process memory, state structures, and thread synchronizations.

---

## 1. SDL3 Interception (`src/sdl_hooks.cc`)

Nightcap interacts with the host windowing and audio subsystem through libSDL3. By using `LD_PRELOAD`, `libvox_menu.so` wraps key SDL3 symbols before forwarding them to the real SDL runtime (`dlsym(RTLD_NEXT)`):

### A. Chat Message Tracking (`src/chat.cc`, `src/chat.h`)
Roblox chat text boxes open on keystroke (`/`) and commit on Enter.
- `ObserveChat()` intercepts `SDL_EVENT_TEXT_INPUT` and `SDL_EVENT_KEY_DOWN`.
- `ChatTracker` mirrors the active Roblox text input field:
  - Tracks character insertions, deletions, cursor navigation, home/end, and word jumps.
  - On `Return` / `Enter`, the completed message buffer is emitted.
  - If the message begins with `/` (and `skip_commands` is enabled in config), the message is ignored by TTS and passed through to Roblox (e.g. `/e dance`, `/w user`).
  - Otherwise, the message is dispatched to the speech synthesis worker thread via `nac::Say()`.
  - **Chat Interception (`intercept_chat`)**: When enabled, the `SDLK_RETURN` key event is dynamically transformed into `SDLK_ESCAPE` before reaching Roblox. This causes Roblox to close the chat box and discard the draft without sending it into the game chat, while Vox Menu speaks the message through TTS in voice chat.

#### VR keyboard (`src/vr_keyboard.cc`)
In VR, Nightcap's in-headset keyboard types into Roblox through Nightcap's own input queue (`QueueVirtualTextInput` / `QueueVirtualKeyPress`), so its keys never pass through the SDL event functions above. Roblox still focuses and unfocuses text boxes through `SDL_StartTextInput*` / `SDL_StopTextInput`, so the tracker still knows when a box is open.
- On the main thread, Vox Menu finds the keyboard's host callbacks (`mocktail::vr::g_host` and its mutex) through the static symbol table of `/proc/self/exe`, then swaps its own wrappers in for `commit_text` and `press_key`.
- The wrappers feed the same `ChatTracker`, which a mutex now guards because they run on Nightcap's VR render thread, and then forward to Nightcap.
- With Intercept Chat on, Enter is kept from Roblox. The box is cleared and closed with Ctrl+A, Backspace and Escape, sent through the keyboard's own path.
- If Nightcap's binary is stripped, the symbols aren't found. The session log then says so, and VR chat isn't spoken.

### B. Microphone Audio Injection (`src/speech.cc`)
- Nightcap opens the microphone capture stream via `SDL_OpenAudioDeviceStream()`.
- Whenever Nightcap reads recorded audio samples from the microphone, `MixIntoMic()` is called on the buffer before it is passed to Roblox.
- If TTS audio is queued:
  - Synthetic samples are resampled to match the device stream rate.
  - Volume scaling is applied (`vc_volume`).
  - If `mix_real_mic` is enabled, the synthetic speech is mixed on top of the real hardware microphone samples; if disabled, real microphone audio is muted while speech plays.

### C. Sharing an App's Sound (`src/media.cc`)
- While sharing is on and Nightcap reads the mic, a background thread lists the playback streams every 1.5 s with `pactl list sink-inputs`, leaving out Nightcap's own (its pid, or a `mocktail`/`nightcap` binary).
- Each stream of the chosen app is recorded with `parec --monitor-stream=<index>` as 48 kHz mono float. That links to the one stream through PulseAudio (or PipeWire's PulseAudio layer) without creating sinks, sources or loopbacks. Streams that end are dropped and new ones picked up on the next listing.
- Each recording keeps at most 250 ms buffered (older audio is dropped, keeping latency low). `MixIntoMic()` resamples them to the mic's rate, sums them and adds them at `media_volume`, together with any TTS speech.

### D. Speaker Playback
- If the user selects output to speakers (`local` or `both`), a dedicated playback stream is created via `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)`.
- Playback volume is controlled independently via `local_volume`.

### E. Input Forwarding & Cursor Unlocking
- While the in-game menu is open:
  - Keystrokes, mouse clicks, and scroll events are consumed by the overlay and blocked from reaching Roblox.
  - When Roblox requests first-person mouse lock (`SDL_SetWindowRelativeMouseMode(true)`), Vox Menu temporarily disables relative mouse mode and shows the system cursor so the user can interact with the menu.
  - Upon closing the menu, previous mouse lock and cursor states are restored seamlessly.

---

## 2. Vulkan Overlay Subsystem (`src/layer.cc`, `src/overlay.cc`, `src/ui.cc`)

The in-game menu is rendered natively within the game's Vulkan rendering pipeline using an implicit Vulkan layer.

### A. Vulkan Layer Negotiation
- The Vulkan loader detects `vox_menu_layer.json`, which the launcher writes next to the library it loads.
- `vkNegotiateLoaderLayerInterfaceVersion()` negotiates layer hooks with Vulkan loader v2 interface.
- Hooks are established for:
  - `CreateInstance` / `DestroyInstance`
  - `CreateDevice` / `DestroyDevice`
  - `CreateSwapchainKHR` / `DestroySwapchainKHR`
  - `QueuePresentKHR`

### B. Rendering Synchronization
- On each frame, inside `QueuePresentKHR`:
  - A secondary command buffer is acquired for the swapchain image.
  - Dear ImGui generates draw commands for the Roblox-styled overlay menu.
  - Synchronization semaphores ensure rendering occurs strictly after the game finishes drawing its frame, but before the image is presented to the display server.

### C. Performance Optimization & UI Responsiveness
To avoid frame drops or input stutter:
- Model directory discovery and executable availability checks use in-process tokenized PATH checks (`access(..., X_OK)`) with a 5-second TTL cache (`src/tts.cc`).
- No shell processes or subshells are spawned on the UI rendering thread.
- Render loop operates at the game's full native refresh rate (e.g., 60–144+ FPS), unless the FPS cap is on.

### D. HUD Overlays and the Layout Editor (`src/hud.cc`)
- The FPS, ping, server and keystroke overlays are draw-list shapes in ImGui's background list, under any menu. FPS is counted from `QueuePresentKHR` calls; the keystroke state comes from every key and button event the SDL hooks see.
- The overlays copy Roblox's top bar, measured from the client: 44 px circles and pills, 8 px apart, filled `#121215`, white icons (drawn, on a 24-unit grid) and bold text, `#0199ED` for active states and the muted-mic red for problems. While dragging, an overlay within 16 px of the gap beside another locks into it, with the joined pill previewed; dropped there, it is saved as joined to it (`<name>_attach`) and from then on is laid out after it, sharing one pill like Roblox's menu / chat / mic group, so the two stay joined however their widths change; dragging it out detaches it. FPS and ping always keep room for three digits, so their pills don't change size as the numbers do.
- The overlays are solid like the top-bar buttons. The automation window and the menu panel are as opaque as Roblox's menu: `1 - 0.18 × PreferredTransparency`, read from Roblox's `GlobalBasicSettings_*.xml` whenever the file changes (`src/roblox_settings.cc`).
- Positions are stored as fractions of the room the screen leaves around each overlay (`0` against the left/top edge, `1` against the right/bottom), so overlays stay aligned with their edge as their text changes width.
- The layout editor hides the panel and hit-tests the overlays itself: dragging snaps any of an overlay's left / centre / right (top / middle / bottom) lines to the screen margins, its centre lines, and the other overlays' lines (and an 8-pixel gap beside them), drawing the guide that matched.

### E. Floating Windows While the Menu Is Closed
- The automation window is a regular ImGui window. Each frame the UI publishes the screen rectangles of its windows, popups and click-point markers (`overlay::SetHotRects`).
- With the menu closed and the cursor free, the SDL hooks route a mouse press inside one of those rectangles (and everything until its release) to the overlay, and every other press to the game. Keys go to the overlay only while one of its text fields has focus; clicking into the game drops that focus.
- While picking a click point, every press goes to the overlay and Escape cancels.

### F. FPS Cap (`src/framecap.cc`)
- Ported from Cocktail's fpslimit layer: before each present, the present thread sleeps in slices of at most 50 ms (re-reading the cap in between, so a change applies mid-wait) and busy-waits the last 0.5 ms. Running slower than the cap never builds up debt.
- A second cap applies while the window has lost focus (`SDL_EVENT_WINDOW_FOCUS_LOST`).

---

## 3. IPC & Control Socket (`src/control.cc`)

Vox Menu runs a UNIX domain datagram socket at:
`$XDG_RUNTIME_DIR/vox-menu/control-<pid>.sock`.

When a user executes `vox-menu` from a terminal, application launcher, or `roblox://` link while Nightcap is already active:
- The launcher finds the running game's PID from Mocktail/Nightcap instance locks.
- It transmits an `open` datagram to the socket.
- The game immediately brings up the overlay menu and raises the window.

---

## 4. Automation (`src/automation.cc`, `src/inject.h`)

- The autoclicker and macro player run on Nightcap's main thread, ticked from every event poll. Due events are queued with their times and injected into the same synthetic-event queue Intercept Chat uses (`PopInjectedEvent` in the SDL hooks), so Nightcap's own event loop delivers them to Roblox like real input, only ever to its window.
- A virtual click at a fixed point is a motion event to the point, the press and release, and a motion event back to where the real cursor is. With "Move Real Cursor" the real cursor is warped there instead.
- Several clicks per frame are sent in one burst at high speeds; after a long stall the schedule restarts rather than catching up.
- Recording observes the key, text, button (and optionally motion) events that reach the game, with the time between them as `wait` steps. Hotkey presses and clicks on the automation window never reach the game, so they are never recorded.
- Stopping fires any queued releases and moves, drops queued presses, and releases every key and button still held.

### The piano (`src/piano.cc`, `src/midi_file.cc`, `src/midi_input.cc`)

- A ported MIDI++: `midi_file.cc` reads a standard MIDI file (format 0/1, tempo map, SMPTE timing) into one time-sorted list of note and sustain events, pairing note-offs with the latest note-on of that key and channel. Tracks named like drum kits, or played mostly on channel 10, are flagged.
- Playback is ticked from the same main-thread event polls as the autoclicker, against a monotonic clock scaled by the speed, so notes land in the first poll at or after their time. Human timing jitters a copy of the schedule. Releases in a tick go before presses, so a repeated note is a fresh press.
- Notes become Virtual Piano keys: the 36 white keys C2–C7 are `1234567890qwertyuiopasdfghjklzxcvbnm`, a black key is Shift + the white key below, and with 88 keys A0–B1 and C#7–C8 are Ctrl + key. Each key is injected with `inject::Key(scancode, down, mod)`: a Shift or Ctrl down event, the key with that modifier in its `mod` field (Nightcap derives the key from the scancode and modifiers), then the modifier's release. A key already held by another note (C4 and C#4 share `t`) is released first.
- MIDI keyboards come through the ALSA sequencer, with `libasound` loaded at run time: a "Vox Menu" client subscribes to every readable port (rescanned every two seconds) and its thread queues notes for the next tick.
- MIDI Connect output (songs and keyboard alike) replaces the key layout: each note on / off is `*` and four base-12 numpad digits (note, then velocity, with `-` and `+` as 10 and 11), and the pedal is message 143 with its value, as MIDI++ sent them. Notes are only transposed, never folded or dropped: the game maps them itself.
- A thread watches the Downloads folder with inotify (`IN_CLOSE_WRITE`, and `IN_MOVED_TO` for browsers that rename a `.part` file when done). A `.mid` file with a MIDI header is moved into the songs folder (copied across filesystems, numbered if the name is taken) and the list rescanned; empty placeholders browsers create first are left alone.
- The control socket (`~/.vox-menu-piano.sock`, stream, one line in and one out per connection) speaks MIDI++'s protocol so its StreamController plugin works unchanged apart from the path.

---

## 5. Nightcap Host Integration (`src/nightcap.cc`, `src/gamestate.cc`, `src/servers.cc`)

Vox Menu runs inside Nightcap, so it uses Nightcap's own host interfaces rather than files or extra processes. It stays at Nightcap's layer: it never reads or changes Roblox's engine or game state. Each piece is optional, with a fallback for other Nightcap builds.

| What | Through | Fallback |
| --- | --- | --- |
| Server joins, the UDMUX relay, leaves | `mocktail_android_log_add_observer` (Nightcap's liblog), each Roblox log line as it is logged | Polling Nightcap's session log (`latest.log`) |
| Ping | Nightcap's datagram observer slot (`g_datagram_observer`, found by name in the static symbol table), chained in front of Nightcap's own server-info overlay. ACKs (54-byte datagrams) from the UDMUX endpoint are matched against recent sends; the most common delay is the round trip (adapted from Nightcap's `AckPingEstimator`). | ICMP echo, then TCP handshake timing |
| Rejoin, server hop | Nightcap's external-launch broker socket, found among this process's own sockets: one `SOCK_SEQPACKET` packet `MOCKTAIL-EXTERNAL-LAUNCH/1\n{"placeId":…,"gameInstanceId":"…"}`, answered `MTL1` + a status byte. The game loop drains it and switches games in place. | Starting Nightcap with the `roblox://` link, which forwards it to the running instance |
| Server address | The UDMUX line | The game's connected UDP socket in `/proc/self/net/udp` |

- Server hop reads `games.roblox.com/v1/games/<place>/servers/Public` with `curl` (or Python) in the background, skips the current server, full ones and those joined in the last 10 minutes, and picks at random, by fewest players, or by best reported ping.
- The server's location is looked up once per address at `ip-api.com`, only while the Server Location overlay is on.

---

## 6. In a VR Headset (`src/xr_layer.cc`, `src/xr_render.h`, `src/xr_math.cc`)

Nightcap shows the game in a headset through OpenXR, with the game's own Vulkan device. Vox Menu is also an **OpenXR API layer**: the launcher writes a manifest for the same library (`vox_menu_xr_layer.json`, in its own folder) and enables it with `XR_API_LAYER_PATH` and `XR_ENABLE_API_LAYERS`, so the OpenXR loader built into Nightcap puts it between Nightcap and the runtime.

- **Drawing:** at `xrCreateSession` the layer notes the Vulkan binding (device, queue family and index) and makes one swapchain: the menu panel (864 × 736 logical) above a strip for the stats pill, at 1.5 pixels per unit. The device is the game's, which the Vulkan layer already follows, so `xr_render` draws with a second ImGui context (its own size, input and font atlas) into the swapchain images, through the same function table. Both ImGui contexts take the Vulkan layer's lock, so the desktop overlay and the headset panel never draw at once. The menu runs in a headset mode: it fills the texture, hides the Automation tab and desktop-only rows, and shows text fields read-only.
- **Showing:** at `xrEndFrame` the layer renders (every frame while the menu is open, ten times a second for stats alone, not at all when nothing is on) and appends quad layers to Nightcap's: the menu in LOCAL space where it was opened (0.85 m ahead, level, facing you), the stats pill in VIEW space low on the left.
- **Input:** the layer creates its own action set (aim pose, trigger, the hold-to-open click, thumbstick for scrolling). It adds bindings to Nightcap's suggestions only for controller profiles it knows (Quest/Touch, Index, WMR, Vive, Pico, Cosmos/Focus 3, the simple controller), and repeats the call without them if the runtime refuses, so Nightcap's own input can't break. It joins Nightcap's attach and every sync. The aim ray is intersected with the panel to give ImGui a mouse position; the trigger is the button. While the menu is open, `xrGetActionState*` for Nightcap's actions report rest, like the desktop menu keeps the mouse and keyboard.
- **Frame rate:** headset frames are counted for the FPS overlay, and the FPS cap is off while they come.
