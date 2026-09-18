#ifndef VERSE_BINDINGS_GEN_H
#define VERSE_BINDINGS_GEN_H

#include "verse_bindings.h"

// The half of the binding generator that needs Godot: which classes exist, and what each one has.
// `verse_bindings.h` is the other half, and it is godot-cpp-free so the naming and the emission are
// unit-testable without an engine.

/// Every class to bind, described and emitted.
///
/// Two sources, and they are keyed differently for a reason R-INT-7 spells out: a ClassDB class is
/// what `get_class()` answers, while a script class is not -- `get_class()` on a node carrying
/// `mob.gd` answers `Node2D`. The generated table carries whichever key applies.
///
/// A ClassDB class the mirror already carries is skipped, which in a stock Godot is almost all of
/// them: the mirror has all 1036 of `extension_api.json`'s. What is left are the classes
/// `GDCLASS` registers without the dump ever hearing of them -- `IPWindows`,
/// `GodotNavigationServer2D` -- plus whatever an addon brought.
VerseBindings verse_generate_bindings();

#endif // VERSE_BINDINGS_GEN_H
