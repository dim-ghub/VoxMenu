# Development & Testing Guide

This guide covers building Vox Menu from source, running the automated test suite, inspecting mock drivers, and debugging the runtime.

---

## 1. Prerequisites

To build Vox Menu, the following tools and development headers are required:

- **Compiler**: C++17 compliant compiler (`g++` or `clang++`).
- **Build System**: GNU `make`.
- **SDL3**: Development library headers (`sdl3` or `libsdl3-dev`).
- **Vulkan**: Headers and loader (`vulkan-headers`, `vulkan-icd-loader`).
- **Python 3**: For running the test suite.
- **TTS Backends (Optional for testing synthesis)**:
  - `espeak-ng`
  - `piper` or `piper-tts`

On Arch Linux / Artix Linux:
```bash
sudo pacman -S base-devel sdl3 vulkan-headers vulkan-icd-loader espeak-ng
```

---

## 2. Building

Build the shared library and helper tools:

```bash
make build
```

Artifacts generated in `build/`:
- `libvox_tts.so`: The core shared library (SDL hooks + Vulkan layer).
- `libvox_tts.so`, `libnightcap_ac.so`: symlinks to `libvox_menu.so` under the older names.
- `vox-menu-render`: Standalone CLI tool to render Animalese directly to a WAV file.

---

## 3. Running Tests

The test suite runs integration and unit tests without needing an active Roblox or Nightcap session:

```bash
make test
```

### Test Drivers
The test framework includes dedicated C++ stand-ins to exercise the hooks and overlay:

1. **`build/vk_driver` (`tests/vk_driver.cc`)**:
   - Spawns a headless Vulkan window with swapchain.
   - Loads `libvox_menu.so` as a Vulkan layer.
   - Feeds scripted mouse and keyboard inputs frame-by-frame (`key`, `click`, `move`, `text`, and
     `press` / `release` for drags).
   - Dumps framebuffer images (`.ppm`) to verify pixel-accurate menu rendering, hover states, and animations.

2. **`build/mic_driver` (`tests/mic_driver.cc`)**:
   - Opens an SDL3 microphone capture stream.
   - Simulates hardware microphone samples and calls `MixIntoMic()`.
   - Captures output PCM data to verify audio blending, volume scaling, and muting behaviors.

3. **`build/chat_test` (`tests/chat_test.cc`)**:
   - Unit tests the `ChatTracker` class (`src/chat.cc`) against complex typing sequences, Unicode strings, backspaces, text selections, and multiline pastes.

4. **`build/automation_test` (`tests/automation_test.cc`)**:
   - Unit tests the autoclicker and macro engine with input injection replaced by a log: click
     timing and limits, fixed points, hold mode, hotkeys, playback, looping, recording, and the
     macro file format.
   - Server tracking from Roblox's log lines, the packet-timing ping estimator, server-hop
     picking, and joining through a stand-in for Nightcap's launch broker (a real
     `MOCKTAIL-EXTERNAL-LAUNCH/1` socket).

5. **`build/xr_driver` (`tests/xr_driver.cc`)**:
   - Stands in for Nightcap's VR path: Vulkan made through the runtime, a session with the Vulkan
     binding, its own action set, and frames. `test_xr.py` runs it against Monado started headless
     (`XRT_COMPOSITOR_NULL=1`, a simulated headset, in a private runtime directory) and checks the
     panel and stats it renders. Skipped where Monado isn't installed. Monado's simulated
     controllers can't be pressed, so controller input is only covered by the unit tests of the
     panel geometry and bindings.

6. **Python Test Suites (`tests/`)**:
   - `test_inprocess.py`: Tests Vulkan overlay rendering, tab switching, keyboard selection, config migration, audio capture, the HUD overlays, the floating window's input routing, the autoclicker hotkey, layout-editor snapping, and the FPS cap.
   - `test_launcher.py`: Tests the shell launcher, lock detection, and desktop patching.
   - `test_synth.py`: Tests the audio synthesizer math, pitch autocorrelation, RMS power, and duration scaling across styles.

---

## 4. Debugging & Environment Flags

Vox Menu provides several environment variables for diagnostic output and inspection:

| Environment Variable | Description |
| --- | --- |
| `VOX_MENU_MENU_OPEN=1` | Starts the Vulkan layer with the overlay menu immediately open on the first frame. |
| `VOX_MENU_SHOW_BUTTON=1` | Forces the top-bar overlay button to render even outside of an active game. |
| `VOX_MENU_DUMP=/path/to/frame.ppm` | Dumps the composited Vulkan frame buffer to a PPM image file. |
| `VOX_MENU_DUMP_FRAME=N` | Specifies the frame number to dump (default: frame 30, allowing fonts and layouts to settle). |
| `VOX_MENU_LIB=/path/to/libvox_menu.so` | Overrides the library path searched by the launcher script. |

| `VOX_MENU_VR_OPEN=1` | Opens the headset panel on the first VR frame. |
| `VOX_MENU_VR_DUMP=/path/to/panel.ppm` | Saves the headset panel's texture (the `VOX_MENU_VR_DUMP_FRAME`-th rendered, default 10th), transparent parts over dark grey. |
| `DISABLE_VOX_MENU_XR_LAYER=1` | Turns the OpenXR (headset) layer off. |

The older `VOX_TTS_*` and `NIGHTCAP_AC_*` names still work. `DISABLE_VOX_MENU_LAYER=1` turns the overlay layer off.
