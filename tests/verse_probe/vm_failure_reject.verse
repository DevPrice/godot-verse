using { /Godot.org/Godot }

# T1.5 (docs/web-vm/spec/failure.md §9.1): the file of refusals. It compiles nothing on purpose;
# the text of each refusal is the result. Each is a source spelling that would reach a runtime error
# the VM can raise while a module initializes, or a write failure.md would otherwise have to specify.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <abs path>/tests/verse_probe/vm_failure_reject.verse
#
# Expected, at this commit:
#   line 20: error 3502, Module-scoped `var` must have `weak_map` type.
#   line 26: error 3512, This archetype instantiation constructs a class that has the 'transacts'
#            effect, which is not allowed by its context.
#   line 30: error 3607, Structs may not contain mutable members.
#
# Line 20 is the only spelling of a module-scoped `var` that is not a weak_map, and line 26 the only
# way to allocate a `var` field while a module's data initializes; both are what the VM's
# ErrRuntime_UnimplementedGlobalVariable guards, so that runtime error is unreachable from a script.

var ModuleCounter:int = 0

reject_holder := class:
	var V:int = 0

# A module-level object whose class has a `var` field.
ModuleHolder:reject_holder = reject_holder{}

# A struct with a mutable member (so there is no struct field for `SetField` to write).
reject_pair := struct:
	var A:int
