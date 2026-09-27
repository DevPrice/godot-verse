#!/usr/bin/env python3
"""Compiles the standalone diagnostic-registry test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    build("build_diagnostics_test",
          ["tests/verse_diagnostics/verse_diagnostics_test.cpp"],
          ["include"], "verse_diagnostics_test.exe")
