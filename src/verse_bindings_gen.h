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
///
/// **`p_inside_resource_load` is true when this is reached from inside a resource load**, where one
/// of the scripts to describe may be the one already being loaded further up the stack -- a cyclic
/// load, which Godot answers with ERR_BUSY, a null Ref and no sentence of its own, so the only thing
/// printed is the asking side's, naming the script asked for and never the one it collided with
/// (`by-hand-findings.md` B30). Only a script that *names a Verse class* can close that loop, and
/// those are the only ones held back.
///
/// **`p_previous` is what a held-back script's members come from**, and without it a generation
/// taken during a load *destroys* a roster that was complete. A held-back class used to be emitted
/// bare, so a Verse file calling one of its methods stopped compiling -- and a script that names a
/// Verse class names it on every load, so this is not a first-build state that passes: it is every
/// time the scene is instantiated. Describing it from the last generation is stale at worst, where
/// empty is wrong every time, and the class stays in `incomplete` either way so the corrective
/// build still runs (B36).
VerseBindings verse_generate_bindings(bool p_inside_resource_load = false,
		const std::vector<VerseBindingClass> *p_previous = nullptr);

#endif // VERSE_BINDINGS_GEN_H
