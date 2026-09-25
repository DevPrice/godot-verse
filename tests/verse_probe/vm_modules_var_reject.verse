using { /Godot.org/Godot }

# T1.4 (docs/web-vm/spec/modules.md): the refusals of module-scope mutable state. This file is
# meant NOT to compile; the text of each diagnostic is its result: glitch 3502 at the definition,
# "Module-scoped `var` must have `weak_map` type.", and glitch 3502 at the read, "Module-scoped
# `var` may only be partially read or written, e.g. `ModuleVar[Player]` or
# `set ModuleVar[Player] = ...`."
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_modules_var_reject.verse --class vm_modules_var_reject

var ModuleCounter:int = 0

vm_modules_var_reject := class(object):
	Read<public>()<transacts>:int = ModuleCounter
