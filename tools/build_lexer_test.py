#!/usr/bin/env python3
"""Compiles the standalone Verse lexer test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    build("build_lexer_test",
          ["src/verse_lexer.cpp", "tests/verse_lexer/verse_lexer_test.cpp"],
          ["src"], "verse_lexer_test.exe")
