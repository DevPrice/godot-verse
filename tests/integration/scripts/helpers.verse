using { /Godot.org/Godot }

# A library file: a `.verse` with no class of its own (R-LANG-6, third clause).
#
# Not a broken script. The whole project shares one flat `/user@localhost` scope, so every other
# `.verse` under res:// can call these by name with no import of any kind -- which is what makes one
# flat scope livable while modules wait for Phase 3. What it cannot do is go on a node, and Godot
# says so rather than attaching something that would never run: this file reports no instance base
# type, so the attach dialog refuses it.
#
# Deliberately holds no class at all. A file whose class is named something other than the file is a
# different thing and still a mistake.

# Called from marshal.verse, to prove a sibling file resolves it with no import.
HelperDouble<public>(Value:int):int = Value * 2

HelperGreeting<public>(Name:string):string = "hello, {Name}"

# A module-level constant, which is the other half of what a library file is for.
HelperAnswer<public>:int = 42
