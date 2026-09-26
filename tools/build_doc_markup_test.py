#!/usr/bin/env python3
"""Compiles the standalone Verse doc-markup test. No godot-cpp, no SCons."""

from unit_build import build

if __name__ == "__main__":
    # The comment reader lexes its way up to the declaration, so the lexer is linked in as well.
    build("build_doc_markup_test",
          ["src/verse_doc_markup.cpp", "src/verse_lexer.cpp",
           "tests/verse_doc_markup/verse_doc_markup_test.cpp"],
          ["src"], "verse_doc_markup_test.exe")
