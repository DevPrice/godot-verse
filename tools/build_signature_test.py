#!/usr/bin/env python3
"""Compiles the standalone Verse signature parser test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    build("build_signature_test",
          ["src/verse_signature.cpp", "tests/verse_signature/verse_signature_test.cpp"],
          ["src"], "verse_signature_test.exe")
