#!/usr/bin/env python3
"""Stages host/ into the UE engine tree and builds the VerseHost program target."""

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_ENGINE = r"C:\UnrealEngine"
SKIP_DIRS = {"Intermediate", "Binaries"}


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


def mirror_host(src: Path, dst: Path) -> tuple[int, int]:
    if dst.name != "VerseHost":
        raise RuntimeError(f"refusing to mirror into unexpected destination: {dst}")
    dst.mkdir(parents=True, exist_ok=True)

    src_files = collect_src_files(src)

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


def run_ubt(engine: Path, config: str, clean: bool) -> None:
    build_bat = engine / "Engine" / "Build" / "BatchFiles" / "Build.bat"
    if not build_bat.exists():
        print(f"error: Build.bat not found at {build_bat}", file=sys.stderr)
        sys.exit(1)

    cmd = [str(build_bat), "VerseHost", "Win64", config]
    if clean:
        cmd.append("-Clean")

    print(f"[build_host] running: {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=str(engine))
    if result.returncode != 0:
        print(f"error: UBT build failed with exit code {result.returncode}", file=sys.stderr)
        sys.exit(result.returncode)


def collect_outputs(engine: Path, repo: Path) -> None:
    bin_dir = engine / "Engine" / "Binaries" / "Win64"
    dll_path = bin_dir / "verse_host.dll"

    if not dll_path.exists():
        print(f"error: {dll_path} not found after build", file=sys.stderr)
        matches = sorted(bin_dir.rglob("verse_host*.dll")) if bin_dir.exists() else []
        if matches:
            print("found these instead:", file=sys.stderr)
            for m in matches:
                print(f"  {m}", file=sys.stderr)
        else:
            print(f"no verse_host*.dll found anywhere under {bin_dir}", file=sys.stderr)
        sys.exit(1)

    out_dir = repo / "bin"
    out_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(dll_path, out_dir / "verse_host.dll")
    print(f"[build_host] copied {dll_path} -> {out_dir / 'verse_host.dll'}")

    pdb_path = dll_path.with_suffix(".pdb")
    if pdb_path.exists():
        shutil.copy2(pdb_path, out_dir / "verse_host.pdb")
        print(f"[build_host] copied {pdb_path} -> {out_dir / 'verse_host.pdb'}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", default=os.environ.get("UE_ROOT", DEFAULT_ENGINE))
    parser.add_argument("--config", default="Development", choices=["Debug", "DebugGame", "Development", "Shipping"])
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()

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

    copied, removed = mirror_host(src, dst)
    print(f"[build_host] staged {src} -> {dst} ({copied} copied, {removed} removed)")

    if args.stage_only:
        elapsed = time.time() - start
        print(f"[build_host] stage-only, done in {elapsed:.1f}s")
        return

    run_ubt(engine, args.config, args.clean)
    collect_outputs(engine, repo)

    elapsed = time.time() - start
    print(f"[build_host] done in {elapsed:.1f}s")


if __name__ == "__main__":
    main()
