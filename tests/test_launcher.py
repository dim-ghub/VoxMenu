"""The launcher script's pieces, run by sourcing it in bash."""

import os
import shutil
import subprocess
import time
import unittest
from pathlib import Path

from helpers import LAUNCHER, ROOT, tempdir

NATIVE = """\
[Desktop Entry]
Type=Application
Name=Nightcap
Exec=env SDL_VIDEODRIVER=wayland,x11 /usr/bin/mocktail %u
MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;
X-Mocktail-Managed=true
Actions=settings;

[Desktop Action settings]
Name=Settings
Exec=/usr/bin/mocktail --settings
"""

JOIN = ("[I/Roblox] x [FLog::Output] ! Joining game "
        "'f0adcb34-df75-4e59-9a5e-0008641fedea' place 18667984660 at 10.0.0.1\n")
LEAVE = "[D/rbx.jni] onGameLeaveBegin() SessionReporterState_GameExitRequested placeId:1\n"


def bash(snippet: str, env=None) -> subprocess.CompletedProcess:
    script = f'source "{LAUNCHER}"\n{snippet}'
    return subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                          env=env or dict(os.environ), timeout=30)


class LauncherTest(unittest.TestCase):
    def test_current_game_from_the_session_log(self):
        with tempdir() as t:
            logs = Path(t) / "mocktail" / "logs"
            logs.mkdir(parents=True)
            env = dict(os.environ, XDG_STATE_HOME=t)
            (logs / "latest.log").write_text(JOIN)
            out = bash("current_game_uri", env).stdout.strip()
            self.assertEqual(out, "roblox://experiences/start?placeId=18667984660"
                                  "&gameInstanceId=f0adcb34-df75-4e59-9a5e-0008641fedea")
            (logs / "latest.log").write_text(JOIN + LEAVE)
            self.assertEqual(bash("current_game_uri", env).stdout.strip(), "")

    def test_link_in_args(self):
        self.assertEqual(bash("link_in_args roblox://x").stdout.strip(), "roblox://x")
        self.assertEqual(bash("link_in_args --launch-uri roblox-player:1").stdout.strip(),
                         "roblox-player:1")
        self.assertNotEqual(bash("link_in_args --settings").returncode, 0)

    @unittest.skipUnless(shutil.which("flock"), "needs util-linux flock")
    def test_finds_the_instance_lock_holder(self):
        with tempdir() as t:
            fake = Path(t) / "mocktail"
            shutil.copy(shutil.which("flock"), fake)
            lock = Path(f"/run/user/{os.getuid()}/mocktail.nightcap-ac-test.instance.lock")
            p = subprocess.Popen([str(fake), str(lock), "sleep", "20"])
            try:
                deadline = time.time() + 5
                found = ""
                while time.time() < deadline and str(p.pid) not in found:
                    found = bash("lock_holders").stdout
                    time.sleep(0.1)
                self.assertIn(str(p.pid), found.split())
                self.assertEqual(bash(f"exe_name {p.pid}").stdout.strip(), "mocktail")
                self.assertNotEqual(bash(f"is_hooked {p.pid}").returncode, 0)
            finally:
                p.kill()
                p.wait()
                lock.unlink(missing_ok=True)



class MakeInstallTest(unittest.TestCase):
    """make install / patch-desktop / unpatch-desktop, into temp dirs."""

    def setUp(self):
        self.tmp = tempdir()
        t = Path(self.tmp.name)
        self.source = t / "system" / "io.github.CoderDayton.nightcap.desktop"
        self.source.parent.mkdir(parents=True)
        self.source.write_text(NATIVE)
        self.prefix = t / "prefix"
        self.target = t / "data" / "applications" / "io.github.CoderDayton.nightcap.desktop"
        # Keep xdg-mime and update-desktop-database off the real system.
        stub = t / "stub"
        stub.mkdir()
        for tool in ("xdg-mime", "update-desktop-database", "pkill"):
            (stub / tool).write_text("#!/bin/sh\nexit 1\n")
            (stub / tool).chmod(0o755)
        self.env = dict(os.environ, XDG_DATA_HOME=str(t / "data"), PATH=f"{stub}:{os.environ['PATH']}")

    def tearDown(self):
        self.tmp.cleanup()

    def make(self, *targets, **variables):
        args = ["make", "--no-print-directory", "-C", str(ROOT), *targets,
                f"PREFIX={self.prefix}", f"DESKTOP_SOURCE={self.source}"]
        args += [f"{k}={v}" for k, v in variables.items()]
        r = subprocess.run(args, env=self.env, capture_output=True, text=True, timeout=600,
                           stdin=subprocess.DEVNULL)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_install_asks_and_patch_round_trips(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_text("[Desktop Entry]\nName=Mine\nExec=mine\n")

        out = self.make("install", VOX_MENU_DESKTOP="n")
        self.assertIn("Skipped the launcher patch", out)
        self.assertTrue((self.prefix / "lib/vox-menu/libvox_menu.so").exists())
        self.assertTrue((self.prefix / "bin/vox-menu").exists())
        # Older names still start Vox Menu.
        self.assertEqual((self.prefix / "bin/nightcap-ac").resolve(), (self.prefix / "bin/vox-menu").resolve())
        self.assertEqual((self.prefix / "bin/vox-tts").resolve(), (self.prefix / "bin/vox-menu").resolve())
        self.assertIn("Name=Mine", self.target.read_text())

        self.make("install", VOX_MENU_DESKTOP="y")
        lines = self.target.read_text().splitlines()
        self.assertEqual(lines[:2], ["[Desktop Entry]", "X-Vox-Menu=true"])
        self.assertIn(f"Exec=env SDL_VIDEODRIVER=wayland,x11 {self.prefix}/bin/vox-menu %u", lines)
        self.assertIn("Exec=/usr/bin/mocktail --settings", lines)
        self.assertNotIn("X-Mocktail-Managed=true", lines)

        self.make("unpatch-desktop")
        self.assertEqual(self.target.read_text(), "[Desktop Entry]\nName=Mine\nExec=mine\n")

        self.make("uninstall")
        self.assertFalse((self.prefix / "bin/vox-menu").exists())
        self.assertFalse((self.prefix / "bin/nightcap-ac").exists())

    def test_patch_from_an_older_version_is_recognised(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_text("[Desktop Entry]\nX-Nightcap-AC=true\nExec=old\n")
        self.make("install")  # no question asked: it's ours, so it is refreshed
        lines = self.target.read_text().splitlines()
        self.assertIn("X-Vox-Menu=true", lines)
        self.assertNotIn("X-Nightcap-AC=true", lines)
        self.make("unpatch-desktop")
        self.assertFalse(self.target.exists())


if __name__ == "__main__":
    unittest.main()
