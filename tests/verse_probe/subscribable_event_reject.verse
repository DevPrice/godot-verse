using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# subscribable_event_probe.verse's Q1, compiled rather than recorded as a comment, because it is half
# of tripwire/subscribable_event_unreleased (docs/tripwires.md).
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/subscribable_event_reject.verse
#
# The Verse book's `subscribable_event` is unreleased, and this drop does not have it under that
# name: glitch 3506, "Unknown identifier `subscribable_event`". The day this compiles, Epic's
# listenable-and-signalable event exists, and HostSignals' SignalVerseEvent is where adopting it as
# a member type would land (docs/signal-declaration.md 12).

subscribable_event_reject := class(object):

	BookSpelling<public>:listenable(int) = subscribable_event(int){}
