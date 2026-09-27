using { /Godot.org/Godot }

# verse_host_unit --tripwires' fixture (docs/tripwires.md): a limitation of Epic's that the bridge
# works around, asked of the adapter that works around it. Compiled only in that mode, so the abi
# layer's own run is unchanged by it.

# tripwire/attribute_takes_one_argument. A user package may not declare an attribute class (glitch
# 3552), but it may declare a constructor for one the bridge declares -- which is enough to put a
# tuple where GetAttributeTextValue looks, beside the one-string spelling it reads.
rpc_pair<public><constructor>(Mode:string, Locality:string)<computes> := rpc_attribute:
	Config := Mode

unit_tripwires := class(node2d):

	@rpc("any_peer")
	OneString<public>():int = 1

	@rpc_pair("any_peer", "call_local")
	TwoStrings<public>():int = 2
