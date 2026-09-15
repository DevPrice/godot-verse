#!/usr/bin/env python3
"""Stages host/ into the UE engine tree and builds the VerseHost program target."""

import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

SKIP_DIRS = {"Intermediate", "Binaries"}

# The path build_host.py itself writes into the engine tree. Engine changes are what the
# provenance record is for, and our own staged copy is not one of them.
STAGED_PREFIX = "Engine/Source/Programs/VerseHost/"

PROVENANCE_NAME = "verse_host.build.txt"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def collect_src_files(src: Path) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for root, _dirs, filenames in os.walk(src):
        rel_root = Path(root).relative_to(src)
        for name in filenames:
            rel = name if rel_root == Path(".") else (rel_root / name).as_posix()
            files[rel] = Path(root) / name
    return files


def mirror_host(src: Path, dst: Path, extra: dict[str, Path] | None = None) -> tuple[int, int]:
    if dst.name != "VerseHost":
        raise RuntimeError(f"refusing to mirror into unexpected destination: {dst}")
    dst.mkdir(parents=True, exist_ok=True)

    src_files = collect_src_files(src)
    if extra:
        src_files.update(extra)

    copied = 0
    for rel, src_path in src_files.items():
        dst_path = dst / rel
        if not dst_path.exists() or src_path.stat().st_mtime > dst_path.stat().st_mtime:
            dst_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_path, dst_path)
            copied += 1

    removed = 0
    for root, dirs, filenames in os.walk(dst, topdown=True):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        rel_root = Path(root).relative_to(dst)
        for name in filenames:
            rel = name if rel_root == Path(".") else (rel_root / name).as_posix()
            if rel not in src_files:
                (Path(root) / name).unlink()
                removed += 1

    for root, dirs, _filenames in os.walk(dst, topdown=False):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        root_path = Path(root)
        if root_path == dst:
            continue
        try:
            next(root_path.iterdir())
        except StopIteration:
            root_path.rmdir()

    return copied, removed


def git(repo: Path, *args: str) -> str | None:
    try:
        result = subprocess.run(["git", "-C", str(repo), *args],
                                capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def engine_local_changes(engine: Path) -> list[str]:
    """Paths the engine checkout carries beyond its HEAD, excluding our own staged copy.

    This is the patch set a second checkout would have to reproduce, so an empty list is the
    claim that a stock checkout at `engine_commit` builds the same host.
    """
    status = git(engine, "status", "--porcelain")
    if status is None:
        return []
    paths = []
    for line in status.splitlines():
        path = line[3:].strip().strip('"')
        if path.startswith(STAGED_PREFIX) or path.endswith(PROVENANCE_NAME):
            continue
        paths.append(path)
    return sorted(paths)


def abi_version(repo: Path) -> str | None:
    header = repo / "include" / "verse_host_abi.h"
    try:
        text = header.read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r"^#define\s+VH_ABI_VERSION\s+(\d+)", text, re.MULTILINE)
    return match.group(1) if match else None


def write_provenance(engine: Path, repo: Path, config: str, destinations: list[Path]) -> Path | None:
    """Records which engine state produced this host, beside the binary it produced.

    A spike result is only worth as much as the ability to reproduce the engine it ran on, and
    the engine is not a dependency any manifest in this repo pins. The smoke test prints this
    file back so a test log carries the engine revision with it.
    """
    changes = engine_local_changes(engine)
    fields = [
        ("abi_version", abi_version(repo) or "unknown"),
        ("config", config),
        ("built_utc", datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")),
        ("engine_root", engine.as_posix()),
        ("engine_commit", git(engine, "rev-parse", "HEAD") or "unknown"),
        ("engine_branch", git(engine, "rev-parse", "--abbrev-ref", "HEAD") or "unknown"),
        ("engine_local_changes", str(len(changes))),
        ("godot_verse_commit", git(repo, "rev-parse", "HEAD") or "unknown"),
        ("godot_verse_dirty", "1" if git(repo, "status", "--porcelain") else "0"),
    ]
    lines = [f"{key}={value}" for key, value in fields]
    lines += [f"engine_change={path}" for path in changes]
    text = "\n".join(lines) + "\n"

    written = None
    for destination in destinations:
        try:
            destination.write_text(text, encoding="utf-8")
        except OSError as error:
            print(f"warning: could not write {destination}: {error}", file=sys.stderr)
            continue
        written = written or destination
    if written:
        print(f"[build_host] engine {fields[4][1][:10]} ({fields[5][1]}), "
              f"{len(changes)} local engine change(s)")
    return written


def run_ubt(engine: Path, target: str, config: str, clean: bool) -> None:
    build_bat = engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"
    if not build_bat.exists():
        print(f"error: Build.bat not found at {build_bat}", file=sys.stderr)
        sys.exit(1)

    cmd = [str(build_bat), target, "Win64", config]
    if clean:
        cmd.append("-Clean")

    print(f"[build_host] running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=str(engine))
    if result.returncode != 0:
        print(f"error: UBT build failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)


# UBT target -> the binary it leaves in Engine/Binaries/Win64. The cooker is not here: it is an
# executable the export plugin runs out of the engine tree, and nothing collects it.
TARGET_BINARIES = {
    "VerseHost": "verse_host.dll",
    "VerseHostRuntime": "verse_host_runtime.dll",
}


def binary_for(target: str, config: str) -> str:
    """What UBT names the target's binary. Only Development goes unsuffixed."""
    name = TARGET_BINARIES[target]
    if config == "Development":
        return name
    stem, _, suffix = name.rpartition(".")
    return f"{stem}-Win64-{config}.{suffix}"


def collect_outputs(engine: Path, repo: Path, config: str, target: str) -> None:
    bin_dir = engine / "Engine" / "Binaries" / "Win64"
    binary_name = binary_for(target, config)
    dll_path = bin_dir / binary_name

    if not dll_path.exists():
        print(f"error: {dll_path} not found after build", file=sys.stderr)
        matches = sorted(bin_dir.rglob("verse_host*.*")) if bin_dir.exists() else []
        if matches:
            print("found these instead:", file=sys.stderr)
            for m in matches:
                print(f"  {m}", file=sys.stderr)
        else:
            print(f"no verse_host* binary found anywhere under {bin_dir}", file=sys.stderr)
        sys.exit(1)

    out_dir = repo / "bin"
    out_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(dll_path, out_dir / binary_name)
    print(f"[build_host] copied {dll_path} -> {out_dir / binary_name}")

    pdb_path = dll_path.with_suffix(".pdb")
    if pdb_path.exists():
        shutil.copy2(pdb_path, out_dir / pdb_path.name)
        print(f"[build_host] copied {pdb_path} -> {out_dir / pdb_path.name}")

    # The only non-system dependency the monolithic host imports.
    tbb = bin_dir / "tbbmalloc.dll"
    if tbb.exists():
        shutil.copy2(tbb, out_dir / tbb.name)
        print(f"[build_host] copied {tbb} -> {out_dir / tbb.name}")
    else:
        print(f"warning: {tbb} not found; verse_host.dll will fail to load", file=sys.stderr)

    # The runtime host also ships: it is a .gdextension [dependencies] entry, so it has to sit
    # beside the GDExtension in the addon's bin/ for Godot's export to copy it (R-DIST-2). That
    # directory is git-ignored; scons copies the addon into each project from there.
    if target == "VerseHostRuntime":
        # demo/, not the repo-root addons/: demo is what SConstruct builds into and copies from,
        # so anything that has to reach the other projects' addons has to be there first.
        #
        # Staged under the unsuffixed name whatever the configuration, because the .gdextension's
        # [dependencies] rows name one file and VerseRuntime looks for one file. Which
        # configuration a game ships is therefore whichever was built last, and D12's "Development
        # for the debug template, Shipping for release" needs two names before it is true.
        # Shipping is 72.7 MB against Development's 112.4, so the choice is not academic.
        addon_bin = repo / "demo" / "addons" / "godot-verse" / "bin" / "windows-x86_64"
        addon_bin.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dll_path, addon_bin / TARGET_BINARIES[target])
        print(f"[build_host] staged {dll_path} -> {addon_bin / TARGET_BINARIES[target]}")
        tbb = bin_dir / "tbbmalloc.dll"
        if tbb.exists():
            shutil.copy2(tbb, addon_bin / tbb.name)
            print(f"[build_host] staged {tbb} -> {addon_bin / tbb.name}")

    write_provenance(engine, repo, config, [bin_dir / PROVENANCE_NAME, out_dir / PROVENANCE_NAME])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", default=os.environ.get("UE_ROOT"),
                        help="UE source checkout with the Verse toolchain; defaults to $UE_ROOT")
    parser.add_argument("--config", default="Development", choices=["Debug", "DebugGame", "Development", "Shipping"])
    parser.add_argument("--target", default="VerseHost",
                        choices=["VerseHost", "VerseHostRuntime", "VerseHostCooker"],
                        help="the UBT target under host/ to build: VerseHost (the editor host), "
                             "VerseHostRuntime (the one an exported game ships) -- both collected "
                             "into bin/ -- or VerseHostCooker, which is an executable the export "
                             "plugin runs and is left in the engine tree")
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()

    if not args.engine:
        print("error: no engine root; pass --engine or set UE_ROOT", file=sys.stderr)
        sys.exit(1)

    start = time.time()

    repo = repo_root()
    engine = Path(args.engine).resolve()
    src = repo / "host"
    dst = engine / "Engine" / "Source" / "Programs" / "VerseHost"

    if not src.exists():
        print(f"error: {src} does not exist", file=sys.stderr)
        sys.exit(1)
    if not engine.exists():
        print(f"error: engine root {engine} does not exist", file=sys.stderr)
        sys.exit(1)

    if args.clean and dst.exists():
        print(f"[build_host] removing staged copy at {dst}")
        shutil.rmtree(dst)

    # Both DLLs compile against the same ABI header, so it lives outside host/ and is staged in.
    shared_header = {"Public/verse_host_abi.h": repo / "include" / "verse_host_abi.h"}

    copied, removed = mirror_host(src, dst, shared_header)
    print(f"[build_host] staged {src} -> {dst} ({copied} copied, {removed} removed)")

    if args.stage_only:
        elapsed = time.time() - start
        print(f"[build_host] stage-only, done in {elapsed:.1f}s")
        return

    run_ubt(engine, args.target, args.config, args.clean)
    if args.target in TARGET_BINARIES:
        collect_outputs(engine, repo, args.config, args.target)
    else:
        print(f"[build_host] {args.target} built; its binaries are under "
              f"{engine / 'Engine' / 'Binaries' / 'Win64'} and are not collected into bin/")

    elapsed = time.time() - start
    print(f"[build_host] done in {elapsed:.1f}s")


if __name__ == "__main__":
    main()
