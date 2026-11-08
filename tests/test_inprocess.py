"""The library loaded into stand-ins for Nightcap: the in-game menu drawn by
the Vulkan layer, input routing, and speech mixed into the microphone."""

import json
import math
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
    e = env_with_lib(tmp, NIGHTCAP_AC_DUMP=dump, **env)
    r = run([VK, str(frames), str(tmp / "script")], e)
    return r, (read_ppm(dump) if dump.exists() else None)


class OverlayTest(unittest.TestCase):
    def test_menu_draws_over_the_game(self):
        with tempdir() as t:
            tmp = Path(t)
            r, img = vk(tmp, "", NIGHTCAP_AC_MENU_OPEN=1)
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
            # A solid #060608 round button just right of Roblox's top-bar
            # buttons, with a white icon inside.
            self.assertEqual(pixel(img, 220, 34), (6, 6, 8))
            icon = max(sum(pixel(img, x, y)) for x in range(228, 250) for y in range(24, 44))
            self.assertGreater(icon, 600)

    def test_menu_takes_input_and_f8_hands_it_back(self):
        with tempdir() as t:
            tmp = Path(t)
            # Toggle "Speak my chat messages", close with F8, then input
            # should reach the game again.
            script = f"15 click 1012 214\n18 key {F8}\n22 click 600 400\n24 key 0x61\n"
            r, _ = vk(tmp, script, NIGHTCAP_AC_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn("game got key 1073741889", r.stdout, "F8 never reaches the game")
            self.assertEqual(r.stdout.count("game got click"), 1, "only the click after closing")
            self.assertIn("game got key 97", r.stdout)
            cfg = json.loads((tmp / "cfg/nightcap-ac/config.json").read_text())
            self.assertFalse(cfg["enabled"])

    def test_tabs_switch(self):
        with tempdir() as t:
            tmp = Path(t)
            _, voice = vk(tmp, "", NIGHTCAP_AC_MENU_OPEN=1)
            _, output = vk(tmp, "15 click 640 104\n", NIGHTCAP_AC_MENU_OPEN=1)
            # The white underline moves from the first tab to the second.
            self.assertGreater(sum(pixel(voice, 370, 134)), 600)
            self.assertLess(sum(pixel(output, 370, 134)), 300)
            self.assertGreater(sum(pixel(output, 640, 134)), 600)

    def test_config_migration_from_legacy(self):
        with tempdir() as t:
            tmp = Path(t)
            legacy_dir = tmp / "cfg" / "nightcap-ac"
            legacy_dir.mkdir(parents=True, exist_ok=True)
            (legacy_dir / "config.json").write_text(json.dumps({
                "enabled": True,
                "voice": "cranky",
                "style": "nh"
            }))
            r, _ = vk(tmp, "15 key 0x40000041\n", NIGHTCAP_AC_MENU_OPEN=1)  # F8 toggle
            vox_cfg = tmp / "cfg" / "vox-tts" / "config.json"
            self.assertTrue(vox_cfg.exists(), "migrated config to vox-tts directory")
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
            # Hovering lightens the circle to #1b1c21; nothing else changes.
            self.assertEqual(pixel(img, 220, 34), (27, 28, 33))
            self.assertEqual(r.stdout.count("game got"), 0)

    def test_click_outside_closes(self):
        with tempdir() as t:
            tmp = Path(t)
            # A click outside the panel closes the menu without reaching the
            # game; the next click does reach it.
            r, _ = vk(tmp, "15 click 60 600\n25 click 60 600\n", NIGHTCAP_AC_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count("game got click"), 1)

    def test_keyboard_selection(self):
        with tempdir() as t:
            tmp = Path(t)
            # Tab selects the first row, ↓ moves to Speech Engine, → changes it to Animalese.
            script = "15 key 0x09\n17 key 0x40000051\n19 key 0x4000004f\n"
            r, img = vk(tmp, script, NIGHTCAP_AC_MENU_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count("game got key"), 0, "keys stay in the menu")
            cfg_file = tmp / "cfg/vox-tts/config.json"
            if not cfg_file.exists():
                cfg_file = tmp / "cfg/nightcap-ac/config.json"
            cfg = json.loads(cfg_file.read_text())
            self.assertEqual(cfg["engine"], "animalese")
            # The white selection outline around that row.
            self.assertGreater(max(sum(pixel(img, x, 280)) for x in range(235, 242)), 600)

    def test_menu_slides_up_when_it_opens(self):
        with tempdir() as t:
            tmp = Path(t)
            _, early = vk(tmp, "", NIGHTCAP_AC_MENU_OPEN=1, NIGHTCAP_AC_DUMP_FRAME=3)
            _, settled = vk(tmp, "", NIGHTCAP_AC_MENU_OPEN=1)

            def tab_underline_y(img):
                for y in range(60, 300):
                    if sum(pixel(img, 370, y)) > 500:
                        return y
                return None

            self.assertIsNotNone(tab_underline_y(settled))
            self.assertGreater(tab_underline_y(early) or 10**6, tab_underline_y(settled))


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


class MicTest(unittest.TestCase):
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
