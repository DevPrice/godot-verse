#!/usr/bin/env python3
"""Compiles the standalone Verse class-declaration scanner test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    # The scanner defers every comment and string decision to the lexer, so both translation
    # units are needed even though the test only calls into the scanner.
    build("build_class_decl_test",
          ["src/verse_lexer.cpp", "src/verse_class_decl.cpp",
           "tests/verse_class_decl/verse_class_decl_test.cpp"],
          ["src"], "verse_class_decl_test.exe")
