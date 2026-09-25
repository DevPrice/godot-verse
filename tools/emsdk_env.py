#!/usr/bin/env python3
"""Run a command with the pinned Emscripten SDK on PATH.

The user's own emsdk (at ~/projects/emsdk) stays on whatever version they
last activated; this never touches it. It runs against a second, separate
emsdk checkout pinned to 4.0.11 (Godot 4.7-stable's web template toolchain),
without an `emsdk activate --permanent` that would rewrite shell rc files.

Usage:
    python tools/emsdk_env.py -- emcc --version
    python tools/emsdk_env.py -- scons platform=web arch=wasm32 threads=no

VERSE_EMSDK overrides the emsdk checkout path (default: sibling
"emsdk-4.0.11" beside this repo's parent directory).
"""

import os
import shutil
import subprocess
import sys

DEFAULT_EMSDK = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "..", "emsdk-4.0.11"
)


def emsdk_root():
    return os.path.normpath(os.environ.get("VERSE_EMSDK", DEFAULT_EMSDK))


def main(argv):
    if "--" not in argv:
        print("usage: emsdk_env.py -- <command...>", file=sys.stderr)
        return 2
    split = argv.index("--")
    command = argv[split + 1 :]
    if not command:
        print("usage: emsdk_env.py -- <command...>", file=sys.stderr)
        return 2

    root = emsdk_root()
    config = os.path.join(root, ".emscripten")
    if not os.path.isfile(config):
        print(
            f"emsdk_env.py: no {config}; run "
            f"'python {root}/emsdk.py install 4.0.11 && python {root}/emsdk.py activate 4.0.11' first",
            file=sys.stderr,
        )
        return 2

    env = dict(os.environ)
    path_dirs = [
        root,
        os.path.join(root, "upstream", "emscripten"),
    ]
    env["PATH"] = os.pathsep.join(path_dirs + [env.get("PATH", "")])
    env["EMSDK"] = root
    env["EM_CONFIG"] = config

    # CreateProcess appends only ".exe" to an extensionless name, so a bare
    # "emcc" silently falls through to another emsdk's emcc.exe earlier on
    # PATH even when this emsdk's emcc.bat comes first. Resolve explicitly.
    resolved = shutil.which(command[0], path=env["PATH"])
    if resolved is not None:
        command = [resolved] + command[1:]

    result = subprocess.run(command, env=env)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
