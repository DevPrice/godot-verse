using { /Godot.org/Godot }

# Deliberately broken, like every other file in this project. What is under test is the sentence the
# editor attaches to `ErrSemantic_EffectNotAllowed`, which is the trap `dodge-the-creeps.md` wall 8
# describes: the compiler reports it at the **call**, and for the common shape the declaration that
# needs changing is somewhere else entirely.
effects := class(node2d):

    # No specifier at all, so this carries Verse's *default* effect set -- wider than `<transacts>`,
    # because it contains `no_rollback`.
    Helper():int = 7

    # Refused, and reported on this line rather than on Helper's, which is where the fix goes. The
    # bridge used to append a sentence saying so and no longer does (by-hand-findings.md B7); what
    # run_tests.py asserts on is the compiler's own text, so this file's job is to produce it.
    CallsHelper()<transacts>:int = Helper()

    # The other direction, where the callee is honest and the caller is what must widen: `QueueFree`
    # mutates the scene and says so.
    ReadsGodot()<reads>:void = QueueFree()
