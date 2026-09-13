using { /Godot.org/Godot }

# R-NODE-7's behavioural half for a virtual whose *return value* the engine acts on.
#
# Most of the 1413 are void: a wrong one shows up as something not happening, and the tests for those
# assert that the body ran. This family is different. Godot asks the script a question and changes
# what it does with the answer, so a virtual that silently kept its generated default -- `false`,
# `vector2{}` -- would be a working script with wrong engine behaviour and nothing printed.
#
# `_GetMinimumSize` is the one of that family a headless run can reach, because it is the only one
# with a *public* caller: `Control.get_minimum_size()`. Its siblings `_HasPoint` and `_CanDropData`
# are reached only from pointer-input and drag paths, which need a window and a mouse, so they are on
# by-hand-checklist.md rather than here. What is asserted is the mechanism all three share.
@global_class
control_virtuals := class(control):

    # Not a round number, and not the default: `vector2{}` is what an unoverridden one answers, so a
    # test that expected zero would pass against a virtual that never ran.
    _GetMinimumSize<override>()<transacts>:vector2 = vector2{X := 73.0, Y := 31.0}
