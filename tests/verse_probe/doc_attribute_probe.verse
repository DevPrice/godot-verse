using { /Godot.org/Godot }
using { /Verse.org/Native }

# Can a script write Verse's own `@doc("...")`?
#
# The attribute is `/Verse.org/Native/doc_attribute`, and it is how the standard library documents
# itself. If a user package may name it, the host's DocOf already reads it through
# GetAttributeTextValue, so a documented member here needs no new code -- only the answer.
#
# Without the second `using` above, `doc` is glitch 3506 -- "Unknown identifier `doc`. Did you
# forget to specify using { /Verse.org/Native }" -- so the answer is a `using` away.

@doc("A module-level function documented through the attribute.")
DocumentedHelper<public>():int = 1

doc_attribute_probe := class(object):

	@doc("A member documented through the attribute.")
	Run<public>():int = DocumentedHelper()
