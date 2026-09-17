using { /Godot.org/Godot }

# Which of Verse's own type names a script may write, asked of the compiler because the answer is
# not in `src/verse_keywords.h`: the reserved-word list carries `char8`, `char16` and `char32` and
# no `char`, though `char` is what every diagnostic prints and `string` is `[]char`.
#
# The editor's syntax highlighter is what needs the list. A reserved word colours from the lexer
# and a mirrored name from a generated table; a Verse type that is neither is drawn as plain text,
# which is how a name the editor does not know reads. `native_type_names` in
# src/verse_syntax_highlighter.cpp is the list this file is the evidence for.
#
# Run as written, the lines below compile: `char`, `event`, `cancelable`, `subscribable`,
# `listenable`, `signalable`, `comparable`, `error` and `weak_map` all resolve with the Godot
# package's `using` alone, because /Godot.org/Godot re-exports what it is built on.
#
# Two need an import of their own, and say so:
#
#     error 3506: Unknown identifier `awaitable`.  Did you forget to specify
#         using { /Verse.org/Concurrency }
#     error 3506: Unknown identifier `task`.       Did you forget to specify
#         using { /Verse.org/Concurrency }
#
# They are types wherever that import is written, so the highlighter carries them too -- colouring
# a name a *particular* file cannot resolve is the same trade the mirrored classes already make.
#
# One candidate is not a type here at all, and is the reason this file names the ones it does
# rather than every name UEFN's documentation lists:
#
#     error 3506: Unknown identifier `agent`.
using { /Verse.org/Concurrency }

stdlib_types_probe := class:
	A<public>(X:[]char):int = 0
	B<public>(X:event(int)):int = 0
	C<public>(X:cancelable):int = 0
	D<public>(X:awaitable(int)):int = 0
	E<public>(X:subscribable(int)):int = 0
	F<public>(X:listenable(int)):int = 0
	G<public>(X:signalable(int)):int = 0
	H<public>(X:comparable):int = 0
	I<public>(X:task(int)):int = 0
	J<public>(X:weak_map(int, int)):int = 0
	K<public>(X:error):int = 0
