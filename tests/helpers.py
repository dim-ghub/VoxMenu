"""Shared bits for the tests: paths, and running the stand-in programs with
Vox Menu loaded the way the launcher loads it."""

import os
import subprocess
import tempfile
import zlib  # noqa: F401  (used by callers that save PNGs)
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
LIB = BUILD / "libvox_menu.so"
LAUNCHER = ROOT / "scripts" / "vox-menu"


def layer_dir(tmp: Path) -> Path:
    d = tmp / "layer"
    d.mkdir(exist_ok=True)
    template = (ROOT / "packaging" / "vox_menu_layer.json.in").read_text()
    (d / "vox_menu_layer.json").write_text(template.replace("@LIB@", str(LIB)))
    return d


def env_with_lib(tmp: Path, config: str | None = None, **extra) -> dict:
    cfg = tmp / "cfg"
    (cfg / "vox-menu").mkdir(parents=True, exist_ok=True)
    if config is not None:
        (cfg / "vox-menu" / "config.json").write_text(config)
    env = dict(os.environ,
               LD_PRELOAD=str(LIB),
               VK_ADD_IMPLICIT_LAYER_PATH=str(layer_dir(tmp)),
               VOX_MENU_LAYER="1",
               XDG_CONFIG_HOME=str(cfg),
               XDG_RUNTIME_DIR=str(tmp),
               # Keep the game-state watcher off the real Nightcap's log.
               XDG_STATE_HOME=str(tmp / "state"),
               SDL_VIDEO_DRIVER="offscreen")
    env.update({k: str(v) for k, v in extra.items()})
    return env


def read_ppm(path: Path):
    data = path.read_bytes()
    _, dims, _, pixels = data.split(b"\n", 3)
    w, h = map(int, dims.split())
    return w, h, pixels


def pixel(img, x, y):
    w, _, pixels = img
    i = (y * w + x) * 3
    return tuple(pixels[i:i + 3])


def run(cmd, env, timeout=60):
    return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)


def tempdir():
    return tempfile.TemporaryDirectory(prefix="vox-menu-test-")
