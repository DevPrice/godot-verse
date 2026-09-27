#!/usr/bin/env python3
"""Compiles the standalone class-name lookup test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    build("build_api_lookup_test",
          ["tests/verse_api_lookup/verse_api_lookup_test.cpp"],
          ["src"], "verse_api_lookup_test.exe")
