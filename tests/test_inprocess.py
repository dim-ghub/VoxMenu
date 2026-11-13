"""The library loaded into stand-ins for Nightcap: the in-game menu drawn by
the Vulkan layer, input routing, and speech mixed into the microphone."""

import json
import math
import os
import socket
import subprocess
import time
import struct
import unittest
from pathlib import Path

from helpers import BUILD, env_with_lib, pixel, read_ppm, run, tempdir

VK = str(BUILD / "vk_driver")
MIC = str(BUILD / "mic_driver")
F8 = "0x40000041"


# The menu slides up for ~11 frames after opening, so scripted input starts
# at frame 15, once it has settled.
def vk(tmp: Path, script: str, frames=45, **env):
    (tmp / "script").write_text(script)
    dump = tmp / "frame.ppm"
    e = env_with_lib(tmp, VOX_MENU_DUMP=dump, **env)
    r = run([VK, str(frames), str(tmp / "script")], e)
    return r, (read_ppm(dump) if dump.exists() else None)


class OverlayTest(unittest.TestCase):
    def test_menu_draws_over_the_game(self):
        with tempdir() as t:
            tmp = Path(t)
            r, img = vk(tmp, "", VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("overlay ready", r.stderr)
            self.assertIsNotNone(img)
            game = (33, 107, 33)  # the stand-in clears to this green
            corner = pixel(img, 5, 700)
            panel = pixel(img, 400, 590)
            self.assertLess(sum(corner), sum(game) * 0.7, "game is dimmed behind the menu")
            self.assertTrue(all(c <= 30 for c in panel), f"see-through black panel, got {panel}")
            close = pixel(img, 1000, 641)
            self.assertGreater(close[2], close[0] + 60, f"navy Close button, got {close}")

    def test_closed_menu_leaves_the_frame_alone_outside_a_game(self):
        with tempdir() as t:
            tmp = Path(t)
            r, img = vk(tmp, "")
            self.assertEqual(r.returncode, 0, r.stderr)
            # Nothing to draw: not in a game, menu closed, so no frame is dumped.
            self.assertTrue(img is None, "drew an overlay with nothing to show")

    def test_button_shows_in_a_game(self):
        with tempdir() as t:
            tmp = Path(t)
            logs = tmp / "state" / "mocktail" / "logs"
            logs.mkdir(parents=True)
            (logs / "latest.log").write_text(
                "[FLog::Output] ! Joining game '11111111-2222-3333-4444-555555555555' place 1 at x\n")
            r, img = vk(tmp, "", frames=90)
            self.assertTrue(img is not None, "no overlay in a game")
            # A solid #121215 round button (Roblox's top-bar fill) just right
            # of Roblox's top-bar buttons, with a white icon inside.
            self.assertEqual(pixel(img, 220, 34), (18, 18, 21))
            icon = max(sum(pixel(img, x, y)) for x in range(228, 250) for y in range(24, 44))
            self.assertGreater(icon, 600)

    def test_menu_takes_input_and_f8_hands_it_back(self):
        with tempdir() as t:
            tmp = Path(t)
            # Toggle "Speak my chat messages", close with F8, then input
            # should reach the game again.
            script = f"15 click 1012 214\n18 key {F8}\n22 click 600 400\n24 key 0x61\n"
            r, _ = vk(tmp, script, VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn("game got key 1073741889", r.stdout, "F8 never reaches the game")
            self.assertEqual(r.stdout.count("game got click"), 1, "only the click after closing")
            self.assertIn("game got key 97", r.stdout)
            cfg = json.loads((tmp / "cfg/vox-menu/config.json").read_text())
            self.assertFalse(cfg["enabled"])

    def test_tabs_switch(self):
        with tempdir() as t:
            tmp = Path(t)
            _, voice = vk(tmp, "", VOX_MENU_MENU_OPEN=1)
            _, output = vk(tmp, "15 click 434 104\n", VOX_MENU_MENU_OPEN=1)
            # The white underline moves from the first tab to the second.
            self.assertGreater(sum(pixel(voice, 300, 134)), 600)
            self.assertLess(sum(pixel(output, 300, 134)), 300)
            self.assertGreater(sum(pixel(output, 434, 134)), 600)

    def test_config_migration_from_legacy(self):
        for old in ("vox-tts", "nightcap-ac"):
            with self.subTest(old), tempdir() as t:
                tmp = Path(t)
                legacy_dir = tmp / "cfg" / old
                legacy_dir.mkdir(parents=True, exist_ok=True)
                (legacy_dir / "config.json").write_text(json.dumps({
                    "enabled": True,
                    "voice": "cranky",
                    "style": "nh"
                }))
                r, _ = vk(tmp, "15 key 0x40000041\n", VOX_MENU_MENU_OPEN=1)  # F8 toggle
                vox_cfg = tmp / "cfg" / "vox-menu" / "config.json"
                self.assertTrue(vox_cfg.exists(), "migrated config to the vox-menu directory")
                data = json.loads(vox_cfg.read_text())
                self.assertEqual(data.get("voice"), "cranky")
                self.assertEqual(data.get("style"), "nh")

    def test_button_hover(self):
        with tempdir() as t:
            tmp = Path(t)
            logs = tmp / "state" / "mocktail" / "logs"
            logs.mkdir(parents=True)
            (logs / "latest.log").write_text(
                "[FLog::Output] ! Joining game '11111111-2222-3333-4444-555555555555' place 1 at x\n")
            r, img = vk(tmp, "5 move 238 34\n", frames=90)
            self.assertEqual(r.returncode, 0, r.stderr)
            # Hovering lightens the circle to #2a2b30; nothing else changes.
            self.assertEqual(pixel(img, 220, 34), (42, 43, 48))
            self.assertEqual(r.stdout.count("game got"), 0)

    def test_click_outside_closes(self):
        with tempdir() as t:
            tmp = Path(t)
            # A click outside the panel closes the menu without reaching the
            # game; the next click does reach it.
            r, _ = vk(tmp, "15 click 60 600\n25 click 60 600\n", VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count("game got click"), 1)

    def test_keyboard_selection(self):
        with tempdir() as t:
            tmp = Path(t)
            # Tab selects the first row, ↓ moves to Speech Engine, → changes it to Animalese.
            script = "15 key 0x09\n17 key 0x40000051\n19 key 0x4000004f\n"
            r, img = vk(tmp, script, VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count("game got key"), 0, "keys stay in the menu")
            cfg = json.loads((tmp / "cfg/vox-menu/config.json").read_text())
            self.assertEqual(cfg["engine"], "animalese")
            # The white selection outline around that row.
            self.assertGreater(max(sum(pixel(img, x, 280)) for x in range(235, 242)), 600)

    def test_menu_slides_up_when_it_opens(self):
        with tempdir() as t:
            tmp = Path(t)
            _, early = vk(tmp, "", VOX_MENU_MENU_OPEN=1, VOX_MENU_DUMP_FRAME=3)
            _, settled = vk(tmp, "", VOX_MENU_MENU_OPEN=1)

            def tab_underline_y(img):
                for y in range(60, 300):
                    if sum(pixel(img, 370, y)) > 500:
                        return y
                return None

            self.assertIsNotNone(tab_underline_y(settled))
            self.assertGreater(tab_underline_y(early) or 10**6, tab_underline_y(settled))


GAME = (33, 107, 33)  # the stand-in game clears to this green
F6 = "0x4000003f"
ESC = "0x1b"


def vk_cfg(tmp: Path, script: str, config: dict, frames=45, **env):
    (tmp / "script").write_text(script)
    dump = tmp / "frame.ppm"
    e = env_with_lib(tmp, json.dumps(config), VOX_MENU_DUMP=dump, **env)
    r = run([VK, str(frames), str(tmp / "script")], e)
    return r, (read_ppm(dump) if dump.exists() else None)


def saved_config(tmp: Path) -> dict:
    return json.loads((tmp / "cfg/vox-menu/config.json").read_text())


class HudAndAutomationTest(unittest.TestCase):
    def test_fps_overlay_draws_outside_a_game(self):
        with tempdir() as t:
            r, img = vk_cfg(Path(t), "", {"hud_fps_on": True})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIsNotNone(img, "an overlay that is on is drawn")
            # A solid #121215 pill like Roblox's top bar, 16 px in, under it.
            self.assertEqual(pixel(img, 24, 90), (18, 18, 21))
            self.assertEqual(pixel(img, 640, 360), GAME, "nothing else is drawn")

    def test_floating_window_takes_its_own_clicks(self):
        with tempdir() as t:
            # The automation window's title bar, then the game twice (once
            # where ImGui keeps its hidden fallback window).
            script = "10 click 1000 128\n20 click 300 600\n25 click 200 200\n"
            r, img = vk_cfg(Path(t), script, {"tools_open": True})
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count("game got click"), 2, "only the clicks outside the window")
            self.assertLess(sum(pixel(img, 1000, 300)), sum(GAME) * 0.7, "the window is drawn")

    def test_autoclicker_hotkey_clicks_in_the_game(self):
        with tempdir() as t:
            r, _ = vk_cfg(Path(t), f"10 key {F6}\n", {"click_cps": 50, "click_hold_ms": 0}, frames=120)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("autoclicker on", r.stderr)
            self.assertNotIn("game got key 1073741887", r.stdout, "F6 is kept from the game")
            self.assertGreaterEqual(r.stdout.count("game got click"), 3)

    def test_autoclicker_keeps_clicking_with_the_mouse_still(self):
        with tempdir() as t:
            # Read the way Nightcap reads events; after F6, nothing else
            # happens (no mouse movement), and it must keep clicking.
            r, _ = vk_cfg(Path(t), f"10 key {F6}\n", {"click_cps": 50, "click_hold_ms": 0}, frames=120,
                          VK_DRIVER_PEEP=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertGreaterEqual(r.stdout.count("game got click"), 10)

    def test_autoclicker_hold_mode_clicks_only_while_held(self):
        hold = {"click_cps": 50, "click_hold_ms": 0, "key_clicker_mode": "hold"}
        with tempdir() as t:
            # Held for 70 frames: clicks the whole time.
            r, _ = vk_cfg(Path(t), f"10 keydown {F6}\n80 keyup {F6}\n", hold, frames=120)
            self.assertEqual(r.returncode, 0, r.stderr)
            held = r.stdout.count("game got click")
            self.assertNotIn("game got key 1073741887", r.stdout, "F6 is kept from the game")
            self.assertIn("autoclicker off", r.stderr, "letting go stops it")
        with tempdir() as t:
            # A tap is a moment's clicking, not a toggle that keeps going.
            r, _ = vk_cfg(Path(t), f"10 keydown {F6}\n11 keyup {F6}\n", hold, frames=120)
            tapped = r.stdout.count("game got click")
        self.assertGreaterEqual(held, 5)
        self.assertLess(tapped, held / 3)

    def test_piano_plays_a_song_loaded_over_the_socket(self):
        # C4 then C#4, 100 ms each, at 100 ticks a beat and 0.1 s a beat.
        track = bytes([0x00, 0xFF, 0x51, 0x03, 0x01, 0x86, 0xA0,
                       0x00, 0x90, 60, 100, 0x64, 0x80, 60, 0,
                       0x00, 0x90, 61, 100, 0x64, 0x80, 61, 0,
                       0x00, 0xFF, 0x2F, 0x00])
        song = (b"MThd" + struct.pack(">IHHH", 6, 0, 1, 100) +
                b"MTrk" + struct.pack(">I", len(track)) + track)
        with tempdir() as t:
            tmp = Path(t)
            (tmp / "songs").mkdir()
            (tmp / "songs" / "Scale.mid").write_bytes(song)
            (tmp / "script").write_text("")
            e = env_with_lib(tmp, json.dumps({"piano_folder": str(tmp / "songs"), "piano_88": False}), HOME=tmp)
            p = subprocess.Popen([VK, "150", str(tmp / "script")], env=e, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
            sock_path = tmp / ".vox-menu-piano.sock"

            def ask(command):
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.connect(str(sock_path))
                    s.sendall(command.encode() + b"\n")
                    return s.recv(4096).decode().strip()

            deadline = time.time() + 10
            while not sock_path.exists() and time.time() < deadline:
                time.sleep(0.05)
            self.assertEqual(ask("LIST_SONGS"), '["Scale"]')
            self.assertEqual(ask("LOAD_SONG 0"), "OK")
            self.assertIn('"song":"Scale"', ask("GET_TIME"))
            out, err = p.communicate(timeout=60)
            self.assertEqual(p.returncode, 0, err)
            T, SHIFT, LSHIFT = 23, 1, 225  # SDL scancodes / KMOD_LSHIFT
            lines = [l for l in out.splitlines() if l.startswith("game got scancode")]
            self.assertEqual(lines[:6], [
                f"game got scancode {T} down mod 0",           # C4: t
                f"game got scancode {T} up mod 0",
                f"game got scancode {LSHIFT} down mod {SHIFT}",  # C#4: Shift+t
                f"game got scancode {T} down mod {SHIFT}",
                f"game got scancode {LSHIFT} up mod 0",
                f"game got scancode {T} up mod 0",
            ])

    def test_layout_editor_drags_with_snapping(self):
        with tempdir() as t:
            tmp = Path(t)
            # HUD tab, Edit Layout, then drag the Ping overlay to a few pixels
            # off the screen's centre: it snaps there exactly.
            script = ("15 click 708 104\n20 click 790 572\n25 press 40 140\n27 move 300 250\n"
                      "29 move 612 361\n31 release 612 361\n35 key " + ESC + "\n")
            r, _ = vk_cfg(tmp, script, {"hud_ping_on": True}, frames=60, VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            cfg = saved_config(tmp)
            self.assertAlmostEqual(cfg["hud_ping_x"], 0.5, places=3)
            self.assertAlmostEqual(cfg["hud_ping_y"], 0.5, places=3)
            self.assertEqual(r.stdout.count("game got"), 0, "the editor keeps input from the game")

    def test_overlays_follow_robloxs_transparency(self):
        with tempdir() as t:
            tmp = Path(t)
            settings = tmp / "data/mocktail/android/data/files/appData/GlobalBasicSettings_13.xml"
            settings.parent.mkdir(parents=True)
            settings.write_text('<roblox><Item class="UserGameSettings"><Properties>'
                                '<float name="PreferredTransparency">0</float></Properties></Item></roblox>')
            # The automation window is as see-through as Roblox's menu: solid
            # at Background Transparency 0. (Its left padding, below the header.)
            _, solid = vk_cfg(tmp, "", {"tools_open": True}, XDG_DATA_HOME=tmp / "data")
            self.assertEqual(pixel(solid, 908, 300), (18, 18, 21), "Background Transparency 0: solid")
            _, normal = vk_cfg(tmp, "", {"tools_open": True})
            self.assertNotEqual(pixel(normal, 908, 300), (18, 18, 21), "see-through at Roblox's default")
            _, own = vk_cfg(tmp, "", {"hud_fps_on": True, "hud_match_roblox": False, "hud_background": 0.5},
                            XDG_DATA_HOME=tmp / "data")
            self.assertNotEqual(pixel(own, 24, 90), (18, 18, 21), "the HUD tab's own opacity when not matching")

    def test_joined_pills_stay_joined(self):
        with tempdir() as t:
            # Ping joined onto FPS: one pill right after it, whatever either
            # number is, and Ping's own position no longer applies.
            r, img = vk_cfg(Path(t), "", {"hud_fps_on": True, "hud_ping_on": True, "hud_ping_attach": "fps",
                                          "hud_ping_x": 0.9, "hud_ping_y": 0.9})
            self.assertEqual(r.returncode, 0, r.stderr)
            # Below the text and icons, the pill is one unbroken band from
            # the first pill well into the second (no gap where they meet).
            band = [pixel(img, x, 108) for x in range(40, 200)]
            self.assertTrue(all(p == (18, 18, 21) for p in band), "one continuous pill")
            self.assertEqual(pixel(img, 1180, 640), GAME, "nothing at Ping's own position")

    def test_dropping_beside_a_pill_joins_it(self):
        # With FPS on the HUD tab grows a row, so reach Edit Layout from the
        # keyboard: Tab, down five rows, Enter.
        open_editor = ("15 click 708 104\n17 key 0x09\n" + "".join(f"{19 + i} key 0x40000051\n" for i in range(5))
                       + "25 key 0x0d\n")
        on = {"hud_fps_on": True, "hud_ping_on": True}
        with tempdir() as t:
            tmp = Path(t)
            # Drag Ping (under FPS) up to roughly right of FPS (10 px past the
            # slot, 6 px low): it locks into the gap and joins.
            script = open_editor + "30 press 40 140\n32 move 120 100\n34 move 170 94\n36 release 170 94\n40 key " + ESC + "\n"
            r, _ = vk_cfg(tmp, script, on, frames=55, VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(saved_config(tmp)["hud_ping_attach"], "fps")
        with tempdir() as t:
            tmp = Path(t)
            # Joined, then dragged away: it leaves the pill and stays put.
            joined = dict(on, hud_ping_attach="fps")
            script = open_editor + "30 press 160 90\n32 move 400 300\n34 move 600 400\n36 release 600 400\n40 key " + ESC + "\n"
            r, _ = vk_cfg(tmp, script, joined, frames=55, VOX_MENU_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            cfg = saved_config(tmp)
            self.assertEqual(cfg["hud_ping_attach"], "")
            self.assertGreater(cfg["hud_ping_x"], 0.3)

    def test_pills_left_side_by_side_are_joined_once(self):
        # Ping beside FPS, as a version without joining left them (it
        # merged pills 8 px apart): joined on first start, so it looks the same.
        beside = {"hud_fps_on": True, "hud_ping_on": True, "hud_ping_x": 0.1158, "hud_ping_y": 0.1006}
        with tempdir() as t:
            tmp = Path(t)
            r, img = vk_cfg(tmp, "", beside)
            self.assertEqual(r.returncode, 0, r.stderr)
            cfg = saved_config(tmp)
            self.assertEqual(cfg["hud_ping_attach"], "fps")
            self.assertTrue(cfg["hud_joins_adopted"])
            self.assertTrue(all(pixel(img, x, 108) == (18, 18, 21) for x in range(40, 200)), "one pill")
        with tempdir() as t:
            tmp = Path(t)
            # Only once: after that, being side by side doesn't join them.
            r, _ = vk_cfg(tmp, "", dict(beside, hud_joins_adopted=True))
            self.assertEqual(saved_config(tmp).get("hud_ping_attach", ""), "")

    def test_fps_cap(self):
        with tempdir() as t:
            start = time.monotonic()
            r, _ = vk_cfg(Path(t), "", {"fps_cap": 20}, frames=30)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertGreater(time.monotonic() - start, 1.3, "30 frames at 20 FPS take 1.5 s")


def capture(tmp: Path, keep_mic: bool, message="Hello there, how are you?", engine=None, intercept_chat=False,
            vr=False):
    # The mic plays a 300 Hz tone so "keep my microphone" is observable.
    tone = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 300 * i / 48000)))
                    for i in range(48000 * 8))
    (tmp / "micin.raw").write_bytes(tone)
    cfg_data = {"enabled": True, "output": "vc", "mix_real_mic": keep_mic, "intercept_chat": intercept_chat}
    if engine is not None:
        cfg_data["engine"] = engine
    config = json.dumps(cfg_data)
    env = env_with_lib(tmp, config, SDL_AUDIO_DRIVER="disk",
                       SDL_AUDIO_DISK_INPUT_FILE=tmp / "micin.raw",
                       SDL_AUDIO_DISK_OUTPUT_FILE="/dev/null")
    r = run([MIC, str(tmp / "out.raw"), "4.5", message] + (["vr"] if vr else []), env)
    raw = (tmp / "out.raw").read_bytes()
    samples = struct.unpack(f"<{len(raw) // 2}h", raw)
    return r, [s / 32768 for s in samples]


def level(samples, start, end):
    part = samples[int(start * 48000):int(end * 48000)]
    return (sum(x * x for x in part) / max(1, len(part))) ** 0.5


# Stand-ins for PulseAudio's tools: one music app and Nightcap's own stream,
# and a recorder that plays a 440 Hz tone (logging what it was asked for).
FAKE_PACTL = """#!/bin/sh
printf '%s\\n' 'Sink Input #41' '  Properties:' '    media.name = "Song"' \\
  '    application.name = "Music"' '    application.process.binary = "music"' \\
  'Sink Input #42' '  Properties:' '    application.name = "Roblox"' \\
  '    application.process.binary = "mocktail"'
"""
FAKE_PAREC = """#!/usr/bin/env python3
import math, struct, sys, time
open(sys.argv[0] + ".log", "a").write(" ".join(sys.argv[1:]) + "\\n")
n = 0
while True:
    sys.stdout.buffer.write(struct.pack("<4800f", *(0.5 * math.sin(2 * math.pi * 440 * (n + i) / 48000)
                                                     for i in range(4800))))
    sys.stdout.flush()
    n += 4800
    time.sleep(0.1)
"""


class MicTest(unittest.TestCase):
    def test_an_apps_sound_goes_into_the_microphone(self):
        with tempdir() as t:
            tmp = Path(t)
            stub = tmp / "stub"
            stub.mkdir()
            for name, text in (("pactl", FAKE_PACTL), ("parec", FAKE_PAREC)):
                (stub / name).write_text(text)
                (stub / name).chmod(0o755)
            (tmp / "micin.raw").write_bytes(b"\0\0" * 48000 * 8)
            config = json.dumps({"enabled": False, "mix_real_mic": False, "media_on": True,
                                 "media_source": "*", "media_volume": 1.0})
            env = env_with_lib(tmp, config, SDL_AUDIO_DRIVER="disk",
                               SDL_AUDIO_DISK_INPUT_FILE=tmp / "micin.raw",
                               SDL_AUDIO_DISK_OUTPUT_FILE="/dev/null",
                               PATH=f"{stub}:{os.environ['PATH']}")
            r = run([MIC, str(tmp / "out.raw"), "4.5", "not spoken"], env)
            self.assertEqual(r.returncode, 0, r.stderr)
            raw = (tmp / "out.raw").read_bytes()
            audio = [x / 32768 for x in struct.unpack(f"<{len(raw) // 2}h", raw)]
            self.assertGreater(level(audio, 2.5, 4.0), 0.2, "the app's tone is in the mic stream")
            asked = (stub / "parec.log").read_text()
            self.assertIn("--monitor-stream=41", asked, "records the music app's stream")
            self.assertNotIn("--monitor-stream=42", asked, "never Nightcap's own")

    def test_speech_goes_into_the_microphone(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False)
            self.assertIn("speaking a chat message", r.stderr)
            self.assertLess(level(audio, 0.2, 0.9), 0.001, "mic muted when not kept")
            self.assertGreater(level(audio, 1.3, 3.0), 0.05, "speech is in the mic stream")

    def test_animalese_engine_in_microphone(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False, engine="animalese")
            self.assertIn("speaking a chat message", r.stderr)
            self.assertLess(level(audio, 0.2, 0.9), 0.001, "mic muted when not kept")
            self.assertGreater(level(audio, 1.3, 3.0), 0.05, "animalese speech is in the mic stream")

    def test_real_microphone_can_stay_on(self):
        with tempdir() as t:
            _, audio = capture(Path(t), keep_mic=True)
            tone = level(audio, 0.2, 0.9)
            self.assertGreater(tone, 0.1, "your mic still reaches voice chat")
            self.assertGreater(level(audio, 1.3, 3.0), tone, "speech is added on top")

    def test_commands_are_skipped(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False, message="/e dance")
            self.assertNotIn("speaking a chat message", r.stderr)
            self.assertLess(level(audio, 1.3, 3.0), 0.001)

    def test_intercept_chat_cancels_text_and_plays_speech(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False, message="Intercepted voice", intercept_chat=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("speaking a chat message", r.stderr)
            self.assertIn("driver received key: 8", r.stderr, "Backspace sent to clear chat box")
            self.assertIn("driver received key: 27", r.stderr, "Escape sent to close chat box")
            self.assertNotIn("driver received key: 13", r.stderr)
            self.assertGreater(level(audio, 1.3, 3.0), 0.05, "speech is in the mic stream")

    def test_normal_chat_sends_return_key(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False, message="Normal voice", intercept_chat=False)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("speaking a chat message", r.stderr)
            self.assertIn("driver received key: 13", r.stderr, "Return key reaches the game")


    def test_vr_keyboard_chat_is_spoken(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False, message="Hello there from my headset, how are you?", vr=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("following the VR keyboard", r.stderr)
            self.assertIn("speaking a chat message", r.stderr)
            self.assertIn("vr host got key: 40\n", r.stderr, "Enter still reaches Roblox")
            self.assertGreater(level(audio, 1.3, 3.0), 0.05, "speech is in the mic stream")

    def test_vr_keyboard_intercept_chat(self):
        with tempdir() as t:
            r, audio = capture(Path(t), keep_mic=False, message="Hello there from my headset, how are you?", vr=True,
                               intercept_chat=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("speaking a chat message", r.stderr)
            self.assertNotIn("vr host got key: 40\n", r.stderr, "Enter kept from Roblox")
            self.assertIn("vr host got key: 4 ctrl", r.stderr, "Ctrl+A selects the draft")
            self.assertIn("vr host got key: 42", r.stderr, "Backspace clears it")
            self.assertIn("vr host got key: 41", r.stderr, "Escape closes the box")
            self.assertGreater(level(audio, 1.3, 3.0), 0.05, "speech is in the mic stream")


if __name__ == "__main__":
    unittest.main()
