# Vox TTS Architecture

Vox TTS is designed to provide fast, local text-to-speech synthesis directly into Roblox voice chat through [Nightcap](https://github.com/CoderDayton/nightcap) on Linux.

Unlike typical voice-chat soundboards or TTS tools, Vox TTS **does not require**:
- Virtual audio cables (PulseAudio null sinks, PipeWire virtual nodes).
- Virtual microphone loops.
- Separate Electron or Python companion applications running in the background.

Instead, the entire system is implemented as a single, lightweight shared library (`libvox_tts.so`) that operates simultaneously across two complementary subsystem layers:

1. **SDL3 Dynamic Interception (`LD_PRELOAD`)**: Hooks audio capture and user input to extract chat and inject audio.
2. **Implicit Vulkan Overlay Layer**: Intercepts Vulkan swapchain presentation to draw an in-game settings menu.

```
                  Nightcap (Unmodified Binary)
 ┌─────────────────────────────────────────────────────────────┐
 │ libSDL3.so ◄─── libvox_tts.so (via LD_PRELOAD)              │
 │                   · Intercepts text input events            │
 │                   · Rebuilds chat messages via ChatTracker  │
 │                   · Mixes TTS audio into mic stream in-line │
 │                                                             │
 │ Vulkan API ◄─── libvox_tts.so (via Vulkan Layer Manifest)   │
 │                   · Intercepts vkQueuePresentKHR            │
 │                   · Renders Dear ImGui menu into swapchain  │
 │                   · Roblox Builder Sans font & BuilderIcons │
 └─────────────────────────────────────────────────────────────┘
```

Both halves reside within the exact same loaded shared library, sharing process memory, state structures, and thread synchronizations.

---

## 1. SDL3 Interception (`src/sdl_hooks.cc`)

Nightcap interacts with the host windowing and audio subsystem through libSDL3. By using `LD_PRELOAD`, `libvox_tts.so` wraps key SDL3 symbols before forwarding them to the real SDL runtime (`dlsym(RTLD_NEXT)`):

### A. Chat Message Tracking (`src/chat.cc`, `src/chat.h`)
Roblox chat text boxes open on keystroke (`/`) and commit on Enter.
- `ObserveChat()` intercepts `SDL_EVENT_TEXT_INPUT` and `SDL_EVENT_KEY_DOWN`.
- `ChatTracker` mirrors the active Roblox text input field:
  - Tracks character insertions, deletions, cursor navigation, home/end, and word jumps.
  - On `Return` / `Enter`, the completed message buffer is emitted.
  - If the message begins with `/` (and `skip_commands` is enabled in config), the message is ignored by TTS and passed through to Roblox (e.g. `/e dance`, `/w user`).
  - Otherwise, the message is dispatched to the speech synthesis worker thread via `nac::Say()`.
  - **Chat Interception (`intercept_chat`)**: When enabled, the `SDLK_RETURN` key event is dynamically transformed into `SDLK_ESCAPE` before reaching Roblox. This causes Roblox to close the chat box and discard the draft without sending it into the game chat, while Vox TTS speaks the message through TTS in voice chat.

#### VR keyboard (`src/vr_keyboard.cc`)
In VR, Nightcap's in-headset keyboard types into Roblox through Nightcap's own input queue (`QueueVirtualTextInput` / `QueueVirtualKeyPress`), so its keys never pass through the SDL event functions above. Roblox still focuses and unfocuses text boxes through `SDL_StartTextInput*` / `SDL_StopTextInput`, so the tracker still knows when a box is open.
- On the main thread, Vox TTS finds the keyboard's host callbacks (`mocktail::vr::g_host` and its mutex) through the static symbol table of `/proc/self/exe`, then swaps its own wrappers in for `commit_text` and `press_key`.
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

### C. Speaker Playback
- If the user selects output to speakers (`local` or `both`), a dedicated playback stream is created via `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)`.
- Playback volume is controlled independently via `local_volume`.

### D. Input Forwarding & Cursor Unlocking
- While the in-game menu is open:
  - Keystrokes, mouse clicks, and scroll events are consumed by the overlay and blocked from reaching Roblox.
  - When Roblox requests first-person mouse lock (`SDL_SetWindowRelativeMouseMode(true)`), Vox TTS temporarily disables relative mouse mode and shows the system cursor so the user can interact with the menu.
  - Upon closing the menu, previous mouse lock and cursor states are restored seamlessly.

---

## 2. Vulkan Overlay Subsystem (`src/layer.cc`, `src/overlay.cc`, `src/ui.cc`)

The in-game menu is rendered natively within the game's Vulkan rendering pipeline using an implicit Vulkan layer.

### A. Vulkan Layer Negotiation
- The Vulkan loader detects `vox_tts_layer.json` (or `nightcap_ac_layer.json`).
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
- Render loop operates at the game's full native refresh rate (e.g., 60–144+ FPS).

---

## 3. IPC & Control Socket (`src/control.cc`)

Vox TTS runs a UNIX domain datagram socket at:
`$XDG_RUNTIME_DIR/vox-tts/control-<pid>.sock`
(with a backward-compatible symlink at `$XDG_RUNTIME_DIR/nightcap-ac/control-<pid>.sock`).

When a user executes `vox-tts` from a terminal, application launcher, or `roblox://` link while Nightcap is already active:
- The launcher finds the running game's PID from Mocktail/Nightcap instance locks.
- It transmits an `open` datagram to the socket.
- The game immediately brings up the overlay menu and raises the window.
