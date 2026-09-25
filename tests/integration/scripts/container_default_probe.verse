using { /Godot.org/Godot }

# T5.6 (docs/web-vm/tasks.md): does a container-typed data-member default -- `Items:godot_array =
# godot_array{}` -- mint a fresh Array per instance, the way the bare archetype spelling already
# does (marshal.verse's MadeArray), or does a host-built object reach it through a per-class default
# computed once? A bare archetype runs through the class's `block:` clause per construction
# (CLAUDE.md, "Objects that are not nodes"); a data-member default is a different path, and nothing
# had measured whether it is per-instance too. Two nodes of this script, one appended to and the
# other read afterwards, is the whole test; a third node made later is what would catch a default
# cached across constructions rather than one recomputed for each.
#
# `Table` and `Numbers` are the other two container kinds a default can mint (B42): the host picks
# which Godot container to make from the wrapper's class, and a parametric `typed_array(t)` is a
# class of its own.
@global_class
container_default_probe := class(node2d):

	var Items<public>:godot_array = godot_array{}

	var Table<public>:dictionary = dictionary{}

	var Numbers<public>:typed_array(int) = typed_array(int){Unpack := ContainerProbeUnpack, Pack := VariantInt}

	AppendValue<public>(Value:int)<transacts>:void = Items.AddInt(Value)

	ItemCount<public>()<reads>:int = Items.Length()

	PutEntry<public>(Key:string, Value:int)<transacts>:void = Table.SetInt(Key, Value)

	EntryCount<public>()<reads>:int = Table.Length()

	AddNumber<public>(Value:int)<transacts>:void = Numbers.Add(Value)

	NumberCount<public>()<reads>:int = Numbers.Length()

ContainerProbeUnpack(Value:variant)<decides><reads>:int = Value.AsInt[]
