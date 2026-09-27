using { /Godot.org/Godot }

# doc_attribute_probe.verse without its `using { /Verse.org/Native }`, compiled rather than recorded
# as a comment, because it is half of tripwire/no_doc_comment_syntax (docs/tripwires.md).
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/doc_attribute_reject.verse
#
# `@doc` is /Verse.org/Native/doc_attribute, and a script reaches it only through that `using`: the
# answer is glitch 3506, "Unknown identifier `doc`", at the attribute. Verse has no doc-comment
# syntax either, so the comment above a declaration is what documents it (DocOf). The day this
# compiles, a script can document itself without an import -- which is worth knowing, because
# DocOf reads the attribute first.

doc_attribute_reject := class(object):

	@doc("A member documented through the attribute.")
	Run<public>():int = 1
