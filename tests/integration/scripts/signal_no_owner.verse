using { /Godot.org/Godot }

# VH_SIGNAL_NO_GODOT_OWNER, which needs a file of its own because the thing being tested is the
# class's *base*: this one has none.
#
# A `godot_signal` binds to a handle, and a handle arrives exactly one way -- Godot attaching the
# script to an object it made. Instantiate refuses a class that does not derive from `object`, so
# this signal can never be registered, connected or emitted.
#
# GDScript has no equivalent of this because every GDScript class extends Object and so carries a
# signal table of its own; a plain Verse class is a VM object with no Godot counterpart at all.
signal_no_owner := class:

    Orphan<public>:godot_signal(int) = godot_signal(int){}

    Read<public>()<transacts>:int = 0
