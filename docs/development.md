# Development

## Build and test

On Arch / Artix:

```bash
sudo pacman -S base-devel sdl3 vulkan-headers vulkan-icd-loader openxr espeak-ng
make build   # build/libvox_menu.so (plus legacy-name symlinks) and build/vox-menu-render
make test
```

Tests run without Roblox or Nightcap, using stand-ins:

| Driver | Stands in for |
| --- | --- |
| `tests/vk_driver.cc` | Nightcap's window and Vulkan presents; scripted input, frame dumps |
| `tests/mic_driver.cc` | Nightcap's mic stream and chat box (and its VR keyboard) |
| `tests/xr_driver.cc` | Nightcap's OpenXR path; run against headless Monado, skipped if absent |

Unit tests: `chat_test.cc`, `automation_test.cc` (also server tracking, ping, hop, launch broker),
`piano_test.cc`. Python suites: `test_inprocess.py` (overlay, input, HUD, FPS cap, piano socket),
`test_launcher.py`, `test_synth.py`, `test_xr.py`.

`build/vox-menu-render` renders Animalese to a WAV file.

## Environment variables

The `VOX_TTS_` and `NIGHTCAP_AC_` prefixes also work.

| Variable | Effect |
| --- | --- |
| `VOX_MENU_MENU_OPEN=1` | Start with the menu open |
| `VOX_MENU_SHOW_BUTTON=1` | Show the top-bar button outside a game |
| `VOX_MENU_DUMP=path.ppm` | Save a composited frame (`VOX_MENU_DUMP_FRAME`, default 30) |
| `VOX_MENU_VR_OPEN=1` | Open the headset panel on the first VR frame |
| `VOX_MENU_VR_DUMP=path.ppm` | Save the headset panel texture (`VOX_MENU_VR_DUMP_FRAME`, default 10) |
| `VOX_MENU_VR_CLICKS="x,y,down,up;..."` | Scripted trigger presses on the panel, in texture pixels |
| `VOX_MENU_LIB=path` | Library the launcher loads |
| `DISABLE_VOX_MENU_LAYER=1` | Disable the Vulkan layer |
| `DISABLE_VOX_MENU_XR_LAYER=1` | Disable the OpenXR layer |
