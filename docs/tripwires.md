# Tripwires

The bridge works around four things Epic's toolchain does not have yet. Each workaround sits behind
one named adapter, and each has a contract-layer tripwire (`tools/run_tripwires.py`, run by
`python tools/run_tests.py --only contract`) that asserts the limitation **still holds**. When Epic
lifts one, its tripwire fails and the FAIL line names the adapter to retire and the section below.
That is how new Epic tooling shows up here as a to-do item, not as a surprise
(`docs/architecture-review.md` item 4 step 4).

A check whose input is missing (no UE checkout, no built host, no `bin/verse_probe.exe`, no
`verse_host_unit.exe`) is skipped and says why. It never counts as a pass.

| tripwire | limitation | checks | adapter | explained in |
| --- | --- | --- | --- | --- |
| `attribute_takes_one_argument` | An attribute takes one argument. `GetAttributeTextValue` refuses an argument that is a `MakeTuple` (`@HACK: SOL-972`), and an overloaded attribute constructor cannot be referenced ("not yet implemented"). | `overloaded_constructor_unreferenceable` (probe: `rpc_attribute_probe.verse` still says 3502); `getattributetextvalue_skips_a_tuple` (source: `Attributable.cpp` still tests `!= Invoke_MakeTuple`); `one_string_reads` and `tuple_unreadable` (unit: `AttributeArgument` reads `@rpc("any_peer")` and not `@rpc_pair("any_peer", "call_local")`) | `AttributeArgument` in `host/Private/HostEngineAdapters.h`, the only caller of `GetAttributeTextValue`, plus the readers that split its one string: `ReadRpcConfig` and `@export_flags` | `phase-4b-design.md` "Stage 6's `@rpc`, and an attribute that may not be overloaded"; `property-export.md` "What the semantic program will and will not give"; CLAUDE.md "An attribute may take only one argument" |
| `no_doc_comment_syntax` | Verse has no doc-comment syntax. The parser keeps four comment kinds, and `@doc` needs `using { /Verse.org/Native }`. | `comment_kinds` (source: `Vst::Comment::EType` is still `{block, line, ind, frag}`); `doc_attribute_needs_using` (probe: `doc_attribute_reject.verse` still says 3506 for `doc`) | `DocOf` in `host/Private/HostEngineAdapters.h`, whose exhaustive switch over `EType` also fails the host build on a fifth kind; `src/verse_doc_markup`'s `verse_doc_comment_above` on the consumer's side | `by-hand-findings.md` B38, B41; CLAUDE.md "Verse has no doc-comment syntax" |
| `subscribable_event_unreleased` | The Verse book's `subscribable_event` is not in this drop. `subscribable_event_intrnl` is `<epic_internal>` and marked for deletion. | `book_spelling_unknown` (probe: `subscribable_event_reject.verse` still says 3506); `intrnl_still_epic_internal` (source: `Event.native.verse`'s declaration) | `SignalVerseEvent` in `host/Private/HostSignals.cpp`, the one place the host signals a Verse event, and the `signal(t)`/`event(t)` member types | `signal-declaration.md` §12; CLAUDE.md "The Verse book's `subscribable_event` does not exist in this drop" |
| `no_verse_lsp_binary` | Nothing links uLangLSP into an executable. It is a message-type library. | `no_executable` (run_verse_lsp's `find_lsp_exe`, plus any `*lsp*`/`*languageserver*` exe in `Engine/Binaries/Win64`); `no_program_links_ulanglsp` (no Program module's `.Build.cs` names it) | `tools/run_verse_lsp.py`'s `find_lsp_exe`, and the host's own lookup and completion (`host/Private/HostLookup.cpp`), which Epic's server would replace | `editor-tooling.md` "uLangLSP" |

## When one fires

Read the explaining section first. It says what the workaround cost and what shape the real feature
should take here. Then move the adapter onto the new feature. Most moves are additive: the
one-string attribute form keeps working beside a several-argument one, and `DocOf` can read a doc
comment before it reads a plain one. After that, delete the tripwire's checks in
`tools/run_tripwires.py`, its row in `tests/claims/test_claims.py`'s `TRIPWIRES`, and its
`(contract: tripwire/<id>)` citation in CLAUDE.md. `test_claims` fails if any one of those three is
removed without the other two.
