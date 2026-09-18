using { /Godot.org/Godot }
using { /Godot.org/Bindings }

# R-INT-10, the one inheritance case that cannot work. **This file compiles**, which is the whole
# problem: `mob` is an ordinary Verse class and every method on it resolves, so Verse has no
# objection at all. What makes it wrong is Godot's rule, which no part of Verse knows -- an Object
# holds exactly one script instance, and this node's is `extends_binding.verse`. The inherited
# `Hit` would forward to `Call("hit")` on an object that has no `hit`, and the failure would arrive
# at the first inherited call, a long way from the line that caused it.
#
# So the bridge refuses it rather than the language: `_validate` reports at the class's own line,
# and `log_script_warnings` writes the same sentence to the log so a headless run can assert it.
#
# `boss := class(mob)` *between two bindings* is the case that does work, and it is not this one --
# that chain stays inside GDScript's own `base` pointers, where `GDScriptInstance::callp` walks it
# against the one instance the node really has.

extends_binding := class(mob):

	Whatever<public>()<transacts>:int = 1
