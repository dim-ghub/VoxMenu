"""The OpenXR layer in a headset session, against Monado running headless
(a simulated headset and its null compositor): the panel and stats are
rendered and added to the frame, and the game's input is left alone."""

import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from helpers import BUILD, LIB, ROOT, pixel, read_ppm, tempdir

XR = str(BUILD / "xr_driver")
MONADO_RUNTIME = Path("/usr/share/openxr/1/openxr_monado.json")
GREY = (40, 40, 40)  # the dump puts transparent parts over this


@unittest.skipUnless(shutil.which("monado-service") and MONADO_RUNTIME.exists() and Path(XR).exists(),
                     "needs Monado and build/xr_driver")
class HeadsetTest(unittest.TestCase):
    def setUp(self):
        # Monado's socket path must be short: a private dir under /tmp.
        self.rt = tempfile.mkdtemp(prefix="vmxr.")
        os.chmod(self.rt, 0o700)
        self.monado = subprocess.Popen(
            ["monado-service"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=dict(os.environ, XDG_RUNTIME_DIR=self.rt, XRT_COMPOSITOR_NULL="1"))
        deadline = time.time() + 10
        while time.time() < deadline and not (Path(self.rt) / "monado_comp_ipc").exists():
            time.sleep(0.1)

    def tearDown(self):
        self.monado.terminate()
        self.monado.wait(timeout=10)
        shutil.rmtree(self.rt, ignore_errors=True)

    def run_xr(self, tmp: Path, config: dict, frames=40, **env):
        vk_layer, xr_layer = tmp / "vk", tmp / "xr"
        vk_layer.mkdir()
        xr_layer.mkdir()
        (vk_layer / "vox_menu_layer.json").write_text(
            (ROOT / "packaging/vox_menu_layer.json.in").read_text().replace("@LIB@", str(LIB)))
        (xr_layer / "vox_menu_xr_layer.json").write_text(
            (ROOT / "packaging/vox_menu_xr_layer.json.in").read_text().replace("@LIB@", str(LIB)))
        (tmp / "cfg/vox-menu").mkdir(parents=True)
        (tmp / "cfg/vox-menu/config.json").write_text(json.dumps(config))
        e = dict(os.environ, XDG_RUNTIME_DIR=self.rt, XR_RUNTIME_JSON=str(MONADO_RUNTIME),
                 XR_API_LAYER_PATH=str(xr_layer), XR_ENABLE_API_LAYERS="XR_APILAYER_VOXMENU_overlay",
                 VK_ADD_IMPLICIT_LAYER_PATH=str(vk_layer), VOX_MENU_LAYER="1",
                 VOX_MENU_VR_DUMP=str(tmp / "panel.ppm"), XDG_CONFIG_HOME=str(tmp / "cfg"),
                 XDG_STATE_HOME=str(tmp / "state"))
        e.update({k: str(v) for k, v in env.items()})
        r = subprocess.run([XR, str(frames)], env=e, capture_output=True, text=True, timeout=90)
        dump = tmp / "panel.ppm"
        return r, (read_ppm(dump) if dump.exists() else None)

    def test_menu_panel_in_the_headset(self):
        with tempdir() as t:
            r, img = self.run_xr(Path(t), {}, VOX_MENU_VR_OPEN=1)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("headset panel ready", r.stderr)
            self.assertIn("controller input for Vox Menu ready", r.stderr)
            self.assertIsNotNone(img, "the panel was rendered")
            # 864 x 736 logical at 1.5x: the dark menu, the white underline
            # under the first tab (of five: no Automation in VR).
            self.assertLess(sum(pixel(img, 640, 300)), 60, "the dark panel")
            self.assertGreater(sum(pixel(img, 150, 87)), 600, "TTS tab underlined")
            self.assertNotIn("game trigger: 1", r.stdout)

    def test_stats_without_the_menu(self):
        with tempdir() as t:
            r, img = self.run_xr(Path(t), {"hud_fps_on": True, "hud_ping_on": True}, frames=30,
                                 VOX_MENU_VR_DUMP_FRAME=2)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIsNotNone(img)
            self.assertEqual(pixel(img, 640, 300), GREY, "no menu")
            # The stats pill under the panel area: Roblox's top-bar fill,
            # #121215. (ImGui's 8-bit vertex colours round it a few levels in
            # the sRGB texture the headset wants.)
            fill = pixel(img, 12, 1104 + 45)
            self.assertTrue(all(abs(a - b) <= 5 for a, b in zip(fill, (18, 18, 21))), f"pill fill, got {fill}")
            self.assertEqual(pixel(img, 12, 1104 + 8), GREY, "transparent around it")

    def test_nothing_on_means_no_rendering(self):
        with tempdir() as t:
            r, img = self.run_xr(Path(t), {"vr_stats": "off"}, frames=20)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("frames: 20", r.stdout)
            self.assertIsNone(img, "nothing to show, nothing drawn")


if __name__ == "__main__":
    unittest.main()
