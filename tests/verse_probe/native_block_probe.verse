using { /Godot.org/Godot }

# R-NODE-3, S-6 -- the assumption phase-4b-design.md §4 rests on and that S-4 and S-5 did not test.
#
# Both block-clause fixtures used a plain Verse class in the user's own package. §4 puts the clause
# on `vh_object`, which is `<native>` and compiled by VNI at UBT time, and expects it to fire for a
# user class in /user@localhost that reaches it through 1036 generated mirror classes. CLAUDE.md is
# explicit that the first does not imply the second: "A host build passing is not enough to know a
# `.verse` file compiles. VNI compiles host/Verse at build time against one package set, and the
# runtime compiler re-reads those same files against another."
#
# Paired with a temporary edit to host/Verse/Godot.native.verse -- `var Handle` plus
# `block: set Handle = 4242` -- and a public reader in GodotApi.native.verse, both marked SPIKE and
# both reverted afterwards. Rebuild the host before running:
#
#   python tools/build_host.py --engine <engine>
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/native_block_probe.verse --class native_block_probe
#
#   N1  does a user class derived from a mirrored class see the native root's block clause?
#       4242 means yes -- VNI accepted the clause and it fires across the package boundary.
#       0 means the clause compiled and did not run for a user class, which kills §4.
#   N2  the same for a class two deep in the user's own package, since a script may extend
#       another script.
#
# A build failure would itself have been an answer: VNI refusing a `block:` on a native class sends
# §4 back to the NewObject(helper{}) adopt shape its first draft carried.
#
# **What it answered. N1 = 4242, N2 = 4242.** VNI accepts a `block:` clause on a `<native>` class,
# and the clause on the native *root* fires for a user class in /user@localhost reached through the
# mirror -- both one level down from `ref_counted` and two levels down through another user class.
# §4 is proven rather than assumed.
#
# **What it cost on the way, which is the part the design did not predict.** Making `Handle` a `var`
# is not free on the C++ side: the shadow in host/Private/GodotClasses.h must become
# `verse::TPtr<int64> Handle;` rather than `TVal<int64> Handle;`, and the two host writes then fail
# to compile because `TPtr` has no `Init` -- `HostScript.cpp:3152` and `:7829` become
# `Handle.Set(Handle, Shadow)` (VersePointer.h:52-62). Reads through `.Get()` are unaffected. Four
# lines in three files, and the static assert in V_STATIC_ASSERT_HAS_DATA names the exact
# declaration it wants, so the failure is self-diagnosing.
#
# **This fixture does not compile against a committed host**, and that is expected: `VhProbeBlock`
# is unknown because the spike edits were reverted. Re-apply the two marked SPIKE edits and rebuild
# to re-run it. The `error 3506: Unknown identifier VhProbeBlock` it reports otherwise is the
# post-revert state, not a regression.
#
# **Half of that is now permanent rather than a spike edit.** R-NODE-3 shipped: `var Handle` and a
# `block:` clause are on `vh_object` in the committed host, and the four C++ lines above are in it
# too. What is still spike-only is `VhProbeBlock` itself, the public reader this fixture calls --
# the shipped clause writes the *real* handle, so what a fixture wanting to see the number needs is
# a reader, not the clause. `tests/host_smoke/objects.verse` is where the shipped behaviour is
# asserted; this file stays as the record of what was asked before it was built.

helper<public> := class(ref_counted):
	Tag<public>:int = 0

helper_deeper<public> := class(helper):
	Extra<public>:int = 0

native_block_probe := class(object):

	N1<public>()<transacts>:int = VhProbeBlock(helper{})

	N2<public>()<transacts>:int = VhProbeBlock(helper_deeper{Tag := 1, Extra := 2})

	Answer<public>()<transacts>:int = 42
