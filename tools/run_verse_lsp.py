#!/usr/bin/env python3
"""Launches Epic's Verse language server (uLangLSP) for a VS Code client to drive over stdio.

See docs/editor-tooling.md for the research this is built on. Short version: uLangLSP
(Engine/Source/Runtime/Solaris/uLangLSP) is a message-type library, not a Program target, and
nothing in the UE checkout links it into an executable. Unlike verse_host.dll there is no build
command that produces a language server binary today, so this script's real job -- until that
changes -- is to say so clearly instead of crashing or inventing a binary that doesn't exist.

If a future engine drop (or an EXE_OVERRIDE below) does provide one, this script still does the
useful part: finds it, points it at this project's Verse source, and execs it with stdio passed
straight through.
"""

import argparse
import os
import sys
from pathlib import Path

DEFAULT_ENGINE = r"C:\UnrealEngine"

# Names a build of uLangLSP might plausibly produce. None of these exist in the checkout this
# script was written against -- see docs/editor-tooling.md -- but the check is cheap and this
# list is the one place to update if that ever changes.
CANDIDATE_EXE_NAMES = [
    "uLangLSP.exe",
    "VerseLanguageServer.exe",
    "VerseLSP.exe",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def find_lsp_exe(engine: Path) -> Path | None:
    bin_dir = engine / "Engine" / "Binaries" / "Win64"
    for name in CANDIDATE_EXE_NAMES:
        candidate = bin_dir / name
        if candidate.exists():
            return candidate
    return None


def verse_source_roots(repo: Path, project: Path) -> list[tuple[Path, str]]:
    # (source directory, Verse mount path). The plugin's own package is mounted at /Godot.org/Godot
    # by SetupVerse("/Godot.org/Godot", ...) in host/VerseHost.Build.cs; the project's own scripts
    # compile into the flat /user@localhost scope (host/Private/HostScript.cpp: ScriptVersePath).
    return [
        (repo / "host" / "Verse", "/Godot.org/Godot"),
        (project / "scripts", "/user@localhost"),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", default=os.environ.get("UE_ROOT", DEFAULT_ENGINE))
    parser.add_argument("--project", default=None, help="defaults to the repo's demo/ directory")
    parser.add_argument("--exe", default=None, help="path to a uLangLSP executable, bypassing the search in Engine/Binaries/Win64")
    args = parser.parse_args()

    repo = repo_root()
    engine = Path(args.engine).resolve()
    project = Path(args.project).resolve() if args.project else repo / "demo"

    if not project.exists():
        print(f"error: project directory {project} does not exist", file=sys.stderr)
        sys.exit(1)

    lsp_exe = Path(args.exe).resolve() if args.exe else find_lsp_exe(engine)
    if lsp_exe is None or not lsp_exe.exists():
        bin_dir = engine / "Engine" / "Binaries" / "Win64"
        print("error: no Verse language server executable found.", file=sys.stderr)
        print(f"       looked for {', '.join(CANDIDATE_EXE_NAMES)} under {bin_dir}", file=sys.stderr)
        print("", file=sys.stderr)
        print("       This is not a build-configuration problem: no UBT Program target in the", file=sys.stderr)
        print("       UnrealEngine checkout links uLangLSP into an executable. uLangLSP", file=sys.stderr)
        print("       (Engine/Source/Runtime/Solaris/uLangLSP) is a message-serialization library", file=sys.stderr)
        print("       only -- LSP.h/LSP.cpp/LSPUtils.h/LSPUtils.cpp, no main, no stdio loop -- and", file=sys.stderr)
        print("       its only in-tree dependents (VerseAssist, ProtoLem) are libraries too. There", file=sys.stderr)
        print("       is consequently no build command to run here, unlike verse_host.dll's", file=sys.stderr)
        print("       'python tools/build_host.py'. See docs/editor-tooling.md for the full", file=sys.stderr)
        print("       citation trail. Pass --exe to point at one if you have built one yourself.", file=sys.stderr)
        sys.exit(1)

    roots = verse_source_roots(repo, project)
    for source_dir, mount in roots:
        if not source_dir.exists():
            print(f"error: Verse source root {source_dir} (mounted at {mount}) does not exist", file=sys.stderr)
            sys.exit(1)

    # uLangLSP's own command-line interface could not be established (see docs/editor-tooling.md):
    # there is nothing to invoke, so nothing to read flags from. This is a best guess at the shape
    # a stdio LSP launcher would take, unverified against a real binary.
    argv = [str(lsp_exe), "--stdio", "--project", str(project)]
    for source_dir, mount in roots:
        argv.append(f"--verse-path={mount}={source_dir}")

    print(f"[run_verse_lsp] warning: uLangLSP's CLI is unverified; exec'ing best-guess args: {argv[1:]}", file=sys.stderr)
    print(f"[run_verse_lsp] exec: {' '.join(argv)}", file=sys.stderr)
    os.execv(str(lsp_exe), argv)


if __name__ == "__main__":
    main()
