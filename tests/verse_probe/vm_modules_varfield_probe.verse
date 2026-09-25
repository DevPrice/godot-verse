using { /Godot.org/Godot }

# T1.4 (docs/web-vm/spec/modules.md): a module-level object whose class has a `var` field. Whether
# this is refused at compile time or raised while the module initializes is the question.
# Answer: compile time -- glitch 3512 at the field, "This mutable data definition has the
# 'allocates' effect, which is not allowed by its context." This file does not compile.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_modules_varfield_probe.verse --class vm_modules_varfield_probe

counter_holder := class<computes>:
	var Count:int = 0

GlobalCounter:counter_holder = counter_holder{}

vm_modules_varfield_probe := class(object):
	Read<public>()<transacts>:int = GlobalCounter.Count
