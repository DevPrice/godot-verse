#!/usr/bin/env python3
"""Compiles the standalone Verse module-map test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    build("build_module_map_test",
          ["src/verse_module_map.cpp", "tests/verse_module_map/verse_module_map_test.cpp"],
          ["src"], "verse_module_map_test.exe")
