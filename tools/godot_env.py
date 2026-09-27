#!/usr/bin/env python3
"""Keeps every Godot process this repo launches out of Devin's real editor profile.

Godot keeps its editor settings, its recent-projects list and its per-project caches under a
per-user config/data directory -- %APPDATA%\\Godot and %LOCALAPPDATA%\\Godot on Windows,
$XDG_CONFIG_HOME/godot, $XDG_DATA_HOME/godot and $XDG_CACHE_HOME/godot everywhere else (Godot
follows the XDG basedir spec off Windows; nothing here is exercised there today, since only
Windows x86_64 is supported and runtime-tested). A headless test run passed none of that, so every
`godot --headless ...` this repo launched -- an integration project, an export, an import scan --
wrote into Devin's own `editor_settings-4.7.tres`, `projects.cfg` and shader/import caches for
real. `env_for()` builds an environment that points those at a throwaway directory instead
(`bin/godot_home/` by default); every launcher in `tools/` merges its own overrides (`UE_ROOT`,
`VERSE_HOST_TEST_FATAL`, ...) into what this returns rather than building an environment from
`os.environ` itself.

The directory is persistent across runs rather than wiped per invocation: it is not Devin's
profile, it is already excluded from git the way the rest of `bin/` is, and reusing it is what
saves a layer Godot's first-run hardware detection and shader-cache warmup on every single
`run_tests.py` call. Delete it by hand if it is ever suspected of holding something stale.

    python tools/godot_env.py -- <godot binary> --headless --editor --quit --path tests/integration

is the one-off a person or an agent reaches for instead of a bare `godot --headless ...`: the
import scan a fresh worktree's first integration run needs, run isolated the same way.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_HOME = REPO / "bin" / "godot_home"


def env_for(home: Path = DEFAULT_HOME, base_env: "dict[str, str] | None" = None) -> "dict[str, str]":
    """An environment dict that keeps Godot's user profile inside `home` rather than the real one.

    Windows reads APPDATA and LOCALAPPDATA (`EditorPaths::_create`, `core/os/os_windows.cpp`);
    every other platform reads XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME instead
    (`core/os/os_unix.cpp`'s fallbacks make the choice moot when they are unset, but this always
    sets them so a launch here never falls back to `$HOME/.config` either). `home` is created if
    missing; `base_env` defaults to a copy of the current process's environment, the way every
    caller here wants -- UE_ROOT and the rest still have to reach the subprocess.
    """
    home = Path(home)
    env = dict(base_env if base_env is not None else os.environ)
    if sys.platform == "win32":
        appdata = home / "AppData" / "Roaming"
        localappdata = home / "AppData" / "Local"
        appdata.mkdir(parents=True, exist_ok=True)
        localappdata.mkdir(parents=True, exist_ok=True)
        env["APPDATA"] = str(appdata)
        env["LOCALAPPDATA"] = str(localappdata)
    else:
        config = home / "config"
        data = home / "share"
        cache = home / "cache"
        for path in (config, data, cache):
            path.mkdir(parents=True, exist_ok=True)
        env["XDG_CONFIG_HOME"] = str(config)
        env["XDG_DATA_HOME"] = str(data)
        env["XDG_CACHE_HOME"] = str(cache)
    return env


def appdata_dir(home: Path = DEFAULT_HOME) -> Path:
    """Where `env_for()` points APPDATA on Windows -- the base an editor-settings file or a
    project's `user://` (`Godot/app_userdata/<project name>`) sits beneath. Windows-only, like the
    rest of what reads this: nothing here runs Godot off Windows today."""
    return home / "AppData" / "Roaming"


def godot_minor_version(godot: Path) -> "str | None":
    """"4.7" out of `godot --version`'s "4.7.stable.official.5b4e0cb0f", or None."""
    completed = subprocess.run([str(godot), "--version"], capture_output=True, text=True,
                               errors="replace")
    printed = (completed.stdout or "").strip().splitlines()
    parts = printed[-1].split(".") if printed else []
    return ".".join(parts[:2]) if len(parts) >= 2 else None


def write_editor_settings(home: Path, godot: Path, *, language: str = "en",
                          debug_port: "int | None" = None) -> None:
    """Pins the isolated profile's `editor_settings-<version>.tres` before the editor's first
    launch into it -- the language is read at startup, so a case hunting an English button label
    needs it written before Godot ever opens, and a fixed remote debug port keeps a Play case off
    6007, the port an open real editor listens on."""
    version = godot_minor_version(godot) or "4.7"
    settings_dir = appdata_dir(home) / "Godot"
    settings_dir.mkdir(parents=True, exist_ok=True)
    text = ('[gd_resource type="EditorSettings" format=3]\n\n[resource]\n'
            f'interface/editor/localization/editor_language = "{language}"\n')
    if debug_port is not None:
        text += f"network/debug/remote_port = {debug_port}\n"
    (settings_dir / f"editor_settings-{version}.tres").write_text(text, encoding="utf-8")


def ensure_export_templates(home: Path, real_template_dir: Path) -> None:
    """Makes the isolated profile see the same export templates Devin's real one has, without
    copying them (some hundred MB per version).

    There is no per-launch override for *where Godot looks* for export templates -- only for one
    preset's exact template file (`custom_template/release`/`custom_template/debug`), which would
    mean rewriting `export_presets.cfg` for every export this repo runs, including
    `dodge-the-creeps/export_presets.cfg`, which CLAUDE.md says is editor-owned and must gain no
    uncommitted line. A Windows directory *junction* (`mklink /J`) needs no elevated privilege,
    unlike a symbolic link, and is made once and reused -- `real_template_dir.name` is the version
    directory (`_export_template_dir` in run_tests.py already computed it against the real
    APPDATA), so a Godot upgrade that moves the version simply makes a new junction beside the
    stale one rather than reusing it wrongly.
    """
    if sys.platform != "win32":
        return
    link = appdata_dir(home) / "Godot" / "export_templates" / real_template_dir.name
    if link.exists():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        ["cmd", "/c", "mklink", "/J", str(link), str(real_template_dir)],
        capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"mklink /J {link} {real_template_dir} failed: "
                           f"{(completed.stdout or '') + (completed.stderr or '')}")


def main() -> int:
    raw = sys.argv[1:]
    if "--" not in raw:
        print("usage: python tools/godot_env.py [--home DIR] -- <godot binary> [args...]",
              file=sys.stderr)
        return 2
    split = raw.index("--")
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--home", default=None,
                        help=f"the isolated profile directory (default: {DEFAULT_HOME})")
    args = parser.parse_args(raw[:split])
    command = raw[split + 1:]
    if not command:
        print("usage: python tools/godot_env.py [--home DIR] -- <godot binary> [args...]",
              file=sys.stderr)
        return 2
    home = Path(args.home) if args.home else DEFAULT_HOME
    return subprocess.run(command, env=env_for(home)).returncode


if __name__ == "__main__":
    sys.exit(main())
