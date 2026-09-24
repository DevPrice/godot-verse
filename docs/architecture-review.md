# Architecture review

A ranked list of architecture and refactoring work, written on 2026-09-24 against `91153f0`. It is
ranked by what the defect record says goes wrong, not by what looks untidy: the first criterion is
**defect-prone designs** (the shapes that keep producing silent failures), then **detection**
(catching what still slips through), then **structure** (file size and coupling). The futures it
is weighed against are the three this project expects to meet: an engine drop moving VerseVM and
uLang underneath the host, Epic shipping tooling the bridge currently substitutes for, and Godot
moving its API dump and its ClassDB. A second execution path (`verse-on-web.md`) was deliberately
not weighed.

Nothing here is a design yet. Each item says what to build, why the record argues for it, what it
costs, and what order it goes in; the design document for an item is written when it is picked up.

## The evidence

### The defect corpus

Every defect this repository has written down — B1–B41, phase-4-gaps G1–G21, and the "where the
design was wrong" section of every phase document and `generated-bindings.md` §10 — was classified
by root cause, 131 rows in all. The classes:

| class | count | what it means |
| --- | --- | --- |
| MISSING-ROW | 33 | a table, list or switch needed a new entry and nothing forced it |
| FOREIGN-CONTRACT | 28 | Godot, UE or the Verse compiler behaves differently from what was assumed |
| WRONG-DEFAULT | 17 | a fallback or placeholder was wrong and looked plausible |
| LIFECYCLE-ORDER | 14 | something ran before or after the point at which it was valid |
| SILENT-DROP | 12 | an error path swallowed the failure |
| TWO-SITES-DISAGREE | 8 | one fact encoded in two places, and they diverged |
| NAME-IDENTITY | 8 | a name built, folded or compared wrongly |
| STALE-CACHE | 5 | a cache read past its validity |
| STRIDE-OR-LAYOUT | 3 | struct size or layout mismatch |
| THREAD-OR-REENTRANCY | 2 | wrong thread, or a call in an invalid state |

Two numbers decide most of the ranking below:

- **MISSING-ROW and TWO-SITES-DISAGREE are 41 of 131 (31%)**, and they are the same defect: one
  fact that has to be written in several places, with nothing checking that it was. The single
  largest (class, locus) cluster is MISSING-ROW in `src/verse_script_language.cpp` — B1, B22, B24,
  B25, B27, B31, B32, B33, B37, B40, B41 and G6 all have the shape "the mirror or the ABI grew
  something, and one of the editor's parallel 'does the editor know about X' tables did not."
- **61 of the 103 defects with a runtime symptom produced no diagnostic at all**, and 22 more
  produced a misleading one. Only 20 said what was wrong. Silence is not a property of one
  subsystem here; it is the default outcome of a defect.

### The repository

| measurement | value |
| --- | --- |
| commits in the last 300 touching `src/verse_script_language.cpp` (5,595 lines) | 100 (33%) |
| commits in the last 300 touching `host/Private/HostScript.cpp` (11,164 lines) | 89 (30%) |
| commits touching both | 36 (12%) |
| commits touching `include/verse_host_abi.h` | 47 (16%) |
| fix-shaped commits in the last 300 | 86 (29%) |
| direct tests of any function inside either of the two largest files | 0 |
| CI configuration, pre-commit or pre-push hook | none |
| `-Werror`, `/W4`, `-Wswitch-enum` in `SConstruct` or `VerseHost.Build.cs` | none |
| `static_assert` in the ABI header | 0 |
| `ERR_FAIL`/`CRASH_COND`/`DEV_ASSERT` in hand-written `src/` | 6 |
| `check`/`ensure`/`static_assert` in `host/Private/` | 4 |
| `default:` labels in the three marshalling files | 34 |
| references into UE internals (`uLang::`, `Verse::`, Solaris) in `HostScript.cpp` | 951, 68% of the host's |
| distinct `uLang::` / `Verse::` symbols the host uses | 61 / 189 |
| words in `CLAUDE.md`, distinct numeric claims in it | 17,632 / 94 |

The two largest files are the two most-changed files, and neither has a test that can reach inside
it. The project's invariants are written down carefully and at length — `CLAUDE.md` is longer than
most design documents — and almost none of them is something a build or a test can fail on.

### Live defects found during the review

These are defects in `master` today, found while gathering the evidence above. They are listed
because each is an exhibit for an item below, and because each can be fixed in an afternoon on its
own.

| where | what | confirmed |
| --- | --- | --- |
| `src/verse_host.cpp:139`, `clear_function_pointers()` | `SetBindings` is resolved and never cleared. It is the newest entry point (11.1); every entry point is listed by hand at three sites, and the newest has already drifted. | yes |
| `src/verse_bindings.cpp:414` vs `tools/gen_verse_api.py:734` | The bindings' enumerator stripping has no `VERSE_STDLIB_NAMES`/reserved-word check. A GDScript enum `MODE_MIN, MODE_MAX` strips to `Min`/`Max`, which the mirror's generator refuses. | yes (by reading) |
| `host/Private/GodotBindings.cpp:848`, `:886` | `VhCallStatic` and `VhCallUtility` answer a zeroed value when their callback is null. `VhCallValue`, twenty lines above, raises in the same situation. | yes |
| `src/verse_script_language.cpp:2348`, `:2440` | The completion and signature caches are keyed on buffer text and caret only, with no generation. A build that changes a class declared in another file can leave a byte-identical buffer serving stale completions. | plausible, not reproduced |
| `host/Private/HostScript.cpp:3052` | `@export var X:<generated-binding class>` is refused `VH_EXPORT_UNSUPPORTED_TYPE`, because `ClassOriginOf` knows Mirrored, Script and Other and a binding is Other. Neither `spec.md` nor `generated-bindings.md` says whether it should work. | the refusal yes; whether it is a defect is a spec question |
| `docs/generated-bindings.md:103` vs `:549` | The body says a differential test runs the C++ classifier against the mirror; §10 says it was never built. `src/verse_bindings.{h,cpp}` has no unit test at all, although it was made godot-cpp-free for that purpose. | yes |
| `tools/gen_verse_api.py:594` | Its header says `MATH_LANES` agrees with `src/verse_value.cpp`'s marshalling. The only test of it compares `MATH_LANES` with a table derived from the same Python source. | yes |

## The ranked list

### 1. One registry per enumerable surface, and make the compiler count the rows

**Class it removes:** MISSING-ROW and TWO-SITES-DISAGREE — 41 of 131 defects, the largest share.

The pattern repeats across the tree. A new variant lane, ABI entry point, descriptor field, type
kind, class origin or editor-known type has to be added in several places, nothing lists those
places, and missing one is a silent wrong answer rather than a build failure. The surfaces worth
converting, with how many hand-maintained sites each has today:

| surface | sites that must agree | what happens when one is missed |
| --- | --- | --- |
| `vh_variant_tag` (40 lanes) | the ABI enum; switches in `verse_value.cpp`, `GodotBindings.cpp` (`LanesFor`, and separately `WireOf`/`FromWire`'s special-case lists) and `HostScript.cpp`; `gen_verse_api.py`'s `VARIANT_LANES`; the Verse `Tag*` constants | `AsRid[]` reading 0 (PHASE4B-e), a RID never crossing out (PHASE4B-f), `[]vector2` read as tuples (PHASE2-g) |
| ABI entry points (46) | the header; the host's `extern "C"` body; `VerseHostLibrary`'s member, `clear_function_pointers()` and `resolve_*` call | `SetBindings`, today |
| array-handed descriptors (`vh_method_desc`, `vh_param_desc`, `vh_complete_item`, `vh_binding_class`, …) | the ABI struct; the `HostScript.h` twin; five near-identical copy-out bodies in `VerseHost.cpp`; the consumer's reader | a stride mismatch reads as corruption; B32 was a missing field |
| "what kind of type is this" | `DescribeExportType`, `DescribeType`, `UserStructClass`, `IsVariantClass`, `IsRidClass`, then `ValueToWire`, `WireToValue`, `ReadSelfDescribingValue` with a load-bearing test order | `variant` asking for 22 arguments (PHASE4B-k); "Cannot convert argument 2 from RID to RID" |
| class origin | `ClassOriginOf`, `NativeClassOf`, `GodotPeerClassFor`, `MirroredClassForHandle`, `DeclaredReferenceClass`, and four copies of the base-first inheritance walk | the binding-class `@export` refusal above; PHASE2-i |
| "the editor knows this is a type" | `verse_api::classes`, `verse_api::types`, and three call sites that each take a different subset in a different order (`godot_classdb_class_for`, `godot_doc_class_for`, `mirrored_class_names`), plus a linear and a binary-search lookup of the same table | B23, B28 |
| naming rules shared by the mirror and generated bindings | `gen_verse_api.py` and `verse_bindings.cpp`: `split_pascal`, class, member, constant, enum and enumerator names, the predicate rule, container property types | the enumerator divergence above |
| rejection enums (`VH_EXPORT_*`, `VH_SIGNAL_*`, `VH_RPC_*`) | the presentation switch in `verse_script.cpp` and the message and code switches in `verse_script_language.cpp`, each with a `default:` | a new reason falls into a generic sentence |
| Verse type ⇄ Godot type | `verse_type_for_godot_type`, `verse_type_for`, `variant_type_for`, `builder_for`/`reader_for`, and the `NIL_IS_VARIANT` pairing at two call sites | a Godot refusal before the VM is entered |

**What to build**, in order, each step independently shippable:

1. **Turn the compiler on.** Enable `-Wswitch-enum` (clang, host) and `/w14061 /w14062` (MSVC,
   consumer) and promote those two to errors. Remove `default:` from every switch over an ABI enum
   that is meant to be exhaustive, so a new enumerator fails the build at each switch that has not
   heard of it. This is a day of work, it needs no design, and it converts part of the table above
   into build failures immediately.
2. **An X-macro list of ABI entry points** (`VH_ENTRY_POINTS(X)`) in the header, carrying name,
   type and required/optional. `VerseHostLibrary`'s members, `clear_function_pointers()` and
   `load()` become three expansions of it, and `host_smoke`'s resolver becomes a fourth.
3. **Generate the lane table from `VARIANT_LANES`.** The generator already holds the single source
   (`VariantLane` has the Godot type, Verse type, `Tag` constant and converters). Emit
   `vh_variant_tag` as a generated include that the ABI header pulls in, plus an X-macro the C++
   switches can `static_assert` their coverage against. The math lanes follow the same route:
   `verse_value.cpp` reads its component counts from `GodotMathLayout.gen.h`, as the host already
   does, instead of hand-coding them.
4. **Pin descriptor layouts.** Add `static_assert(sizeof(...) == N)` and `offsetof` asserts for
   every struct handed over as an array, in a header both toolchains compile. A layout change
   without a major bump then fails in whichever build runs first. Also fold a digest of those sizes
   into the `vh_init` handshake, so a layout mismatch between two binaries that each compiled
   cleanly is refused by name, not read as corruption.
5. **One type classifier.** A single function that answers a closed `EDeclaredKind` (scalar, math
   struct, `variant`, `rid`, user struct, reference by origin, container, option, array) for a
   uLang type. `DescribeExportType`, `DescribeType` and `UserStructClass` switch over its answer,
   and so do the three value converters. The "claimed in all three" rule in `CLAUDE.md` then stops
   being a rule and becomes a `switch` the compiler checks. Class origin gains a `Binding` member
   in the same step, and one base-first walk replaces the four.
6. **Shared test vectors for the naming rules.** The C++ side has no JSON parser, which is why the
   differential test was never built. It does not need one: have `gen_verse_api.py` write a flat
   file of (input, expected) pairs for every naming rule the bindings share, and have a new
   `verse_bindings_test` binary read it and compare. That is the missing unit test and the missing
   differential test in one step.

**Futures:** a Godot bump that adds a `Variant::Type` already fails loudly (`check_variant_lanes`);
after this item, the C++ halves fail with it. An engine drop that adds a uLang type kind shows up
as one unhandled enumerator instead of three describers that quietly disagree.

**Cost:** steps 1–2 take days, steps 3–4 take a week each, and step 5 is the largest (about a week
of host work plus the ABI-bump rebuild). Step 5 is also the prerequisite for item 5's split,
because the type model is what every cluster of `HostScript.cpp` reaches for.

### 2. A failure has to say which failure it was

**Class it removes:** the silence behind 61 of 103 runtime defects, plus most WRONG-DEFAULT (17),
where a fallback stood in for an error.

The host's describing and writing paths answer `bool`, and `VerseHost.cpp` turns every `false` into
`VH_ERR_NOT_FOUND`. So "no such class", "not analysed yet", "field exists but the value did not
typecheck" and "enum ordinal out of range" all reach the consumer as one code, and none of them
reaches a log. `VhCallStatic`/`VhCallUtility` answer a zero value where their sibling raises.
PHASE7B-d (`host_has_compiler` tested whether a symbol resolved, which it always does), PHASE7B-e
("waiting for analysis" confused with "can never analyse") and B39 (a pending build read as a null
default) all have the same root: a state that meant "I cannot answer" was indistinguishable from
an answer.

**What to build:**

1. **A reason-carrying result type on the host's internal surface.** For example,
   `TResult<T, EHostFailure>`, where `EHostFailure` is a closed enum (`NoSuchClass`, `NotAnalysed`,
   `TypeMismatch`, `Unconvertible`, `NoCompiler`, `CallbackMissing`, …). The ABI keeps its status
   codes, gains the narrower ones where a consumer would act differently (at minimum, separating
   "not analysed yet" from "not found"), and maps the rest in one function rather than at every
   entry point.
2. **Make "cannot answer" unrepresentable as a value.** Every placeholder, default and cache that
   can be served while its source is pending returns an optional-with-reason, not a value. B26 and
   B39 were each fixed by adding a cache; the structural fix is that the type of "the default"
   cannot be null-while-pending.
3. **A silence budget in development builds.** Every `return false` or `return {}` on a failure
   path either carries an `EHostFailure` or passes through one macro
   (`VH_UNREPORTED(reason)`) that logs once per site in a Development host and compiles away in
   Shipping. With a grep, the silent paths become an inventory that can be counted and brought
   down, where today they cannot be found.
4. **Assertions where an invariant is already written in a comment.** Most of the landmines in
   `CLAUDE.md` have an obvious `check`/`ERR_FAIL_COND` form. Examples: the game-thread-owns-this
   rule on `_frame` state, "a consumer that begins an analysis must poll it", `diagnostic_sink`
   never being re-entered, and `GCallbacks` touched only under its lock. Nine assertions in 20k
   lines of hand-written C++ is a choice, and the corpus says it costs more than it saves.

**Futures:** an engine drop's most common symptom in this record is silence (PHASE7B-a, -h, -j,
-k). A host that names its own failures shortens each of those sessions from hours to one line.

**Cost:** step 1 is mechanical but wide (every `Get*`/`Write*`/`Read*` in `HostScript.h`), plus a
minor ABI bump for the narrower status. Steps 3–4 can proceed file by file. Do step 1 alongside
item 5, not before it, so each extracted unit takes the new type at the moment it moves.

### 3. Make detection automatic, structured and able to see inside the host

**Class it addresses:** every class, by moving detection earlier. Today a regression in
`HostScript.cpp` surfaces as "the export run passed 507, expected 511".

The suite is better than its shape suggests: 463 ABI steps, 609 integration checks, 254 generator
checks, and about 145 unit checks. But nothing runs it unless a person or an agent types the
command. It has already reported a failing layer green twice: PHASE2-j, where a third of
integration never ran, and `host_smoke`'s unread `ExportsOk` flag, which let two failures pass for
a session. And it cannot localize a failure inside either large file.

**What to build:**

1. **A gate that runs without being asked.** A GitHub-hosted runner cannot build this project,
   because a UE checkout and a Godot binary are machine-local. A self-hosted runner on this
   machine, or a pre-push hook that runs `run_tests.py --only units,abi` and the integration layer
   when the host is built, gives agents a gate they cannot skip. Record the result in the commit
   trailer or a log the agent reads. Of everything on this list, this has the best payoff per hour,
   because agents do most of the work and a gate turns "remember to run the tests" into something
   that cannot be forgotten.
2. **Structured results.** Every layer writes one JSON line per case (`layer`, `case`, `status`,
   `got`, `expected`), and `run_tests.py` reports failures by case name rather than by layer. The
   export layer's hardcoded 511/11 then becomes a comparison against the editor run's own case
   list, filtered by the `editor` flag. That keeps the "a case must not silently stop" guarantee
   and removes the hand-edited integer.
3. **Assert diagnostics by ID, not by prose.** Twenty-four `COVERAGE_EXPLANATIONS` and eight
   integration `require_all` strings are substrings of English sentences, so a typo fix breaks a
   test and a longer future sentence can make one pass wrongly. Give each bridge-authored
   diagnostic a stable ID (`VG1042`) that is printed with it, and assert the ID. This also answers
   B7's lesson, because a test can then say which diagnostic fired.
4. **A white-box seam into the host.** Add a fourth UBT target, or a `host_unit` configuration of
   `VerseHost`, that links `Private/` together with a test `main`. Once items 1 and 5 have put the
   type classifier, the converters and the snapshot serializer behind headers, those can be tested
   against uLang types built in the test, without a Godot. The six godot-cpp-free units in `src/`
   show the pattern works here: each was extracted to be testable, and each is tested.
5. **The missing unit tests,** which cost almost nothing: `verse_bindings` (item 1, step 6), and
   the consumer's name-lookup helpers once item 6 extracts them.

**Cost:** step 1 takes a day or two. Step 2 is a rewrite of the reporting half of `run_tests.py`
and the two `.gd` drivers, about three days. Step 3 goes file by file. Step 4 depends on item 5.

### 4. A contract suite for foreign behavior, with tripwires for Epic's missing features

**Class it addresses:** FOREIGN-CONTRACT, 28 of 131. These were Godot, UE or the Verse compiler
doing something the design did not expect. All three futures this project expects arrive as
foreign contracts changing.

The repository has already measured most of what it relies on, and records the measurements in
`tests/verse_probe`'s fixtures and in `CLAUDE.md`'s prose. `verse_probe` asserts nothing, is not in
`run_tests.py`, and nothing re-runs it when the engine moves. The facts that matter most are the
*permissive* ones, because a change there fails silently rather than loudly: `<decides>` does not
narrow; `external{}` is allowed for accessors; a `defer` runs on cancellation; float `=` is
reflexive for NaN; an operator-leading continuation line is dropped; the vararg/tuple ambiguity
rules. The generator emits code that relies on each of these. If one of them changes, the
generated code still compiles and no longer means what its comments say.

**What to build:**

1. **Promote the probes into an asserted layer.** Give each `verse_probe` fixture an expected
   outcome: which diagnostics, which return values, and, for `async_reject.verse`, which refusal
   text. Then run them in `run_tests.py` as a `contract` layer. It needs only the host, so it is
   cheap.
2. **One test per measured claim.** Each "measured" or "verified against the engine" sentence in
   `CLAUDE.md` gets a contract test with an ID, and the sentence cites it. When an engine drop
   breaks a claim, the test names the paragraph that is now wrong. The 94 numeric claims (1036
   classes, 503 signal accessors, 3996 `<reads>` methods, and so on) are the easy half: have the
   generator emit them as a small data file, and have `CLAUDE.md` state them in a form a test can
   compare against. The prose can no longer rot silently.
3. **A Godot contract project.** A headless Godot project that asserts the Godot facts the bridge
   relies on: the `ProfilingInfo` stride (PHASE6-b), the lookup result's `script_path` field
   (B29), `ERR_BUSY` on a cyclic load (B30), `booleanize` on an empty Variant, and GDCLASS versus
   GDSOFTCLASS for the singletons. Run it on every `api_version` bump.
4. **Tripwires for the substitutes.** The bridge carries workarounds for missing Epic features: the
   one-string attribute split (SOL-972), comment-as-documentation, `subscribable_event_intrnl` not
   being used, and `tools/run_verse_lsp.py`'s search for an LSP binary. Put each behind one named
   adapter (for example, `AttributeArguments`, `DocOf`, and a single signal-delivery seam), and add
   a contract test that asserts the limitation *still holds*. When Epic fixes it, the test fails,
   names the adapter to retire, and names the design section that explains it. This is how
   "Epic shipping real tooling" arrives as a to-do list rather than as a surprise.
5. **Make the keyword and const tables re-derivable.** `gen_verse_keywords.py` transcribes
   `GRAMMAR_KEYWORDS` from `VerseGrammar.h` by line-number comment, and `CONST_OVERRIDES` is 127
   rows pasted from a run against a separate Godot checkout. Both go stale silently. Have the
   contract layer re-run both derivations when their inputs exist and fail on a difference, and
   have the generator report any hand table row that matched nothing (a renamed class leaves
   `CONST_OVERRIDES`, `PREDICATE_EXTRA` and `METHOD_RENAMES` rows inert today).

**Cost:** steps 1 and 5 take days. Step 2 is incremental and can be done one claim at a time. Step 3
takes about a week. Step 4 is small per adapter.

### 5. Split `HostScript.cpp` behind an engine-facing layer

**Class it addresses:** structure, and the cost of an engine drop. It is ranked below 1–4 because
splitting the file does not by itself remove a defect class. Items 1 and 2 are what make the split
worth doing, and the split is what lets item 3's white-box seam reach anything.

`HostScript.cpp` has thirteen identifiable clusters. The facts that decide how to cut it:

- **Cluster I (lookup, completion and signature, lines 7712–9509)** never enters the VM and reads
  only the live AST. It is the cleanest first extraction, and it is the code Epic's eventual LSP
  replaces. Isolating it now is what makes that retirement a deletion.
- **Clusters D (type and export description) and E (value marshalling)** are what every other
  cluster calls. Extract them together as the type model once item 1's single classifier exists.
  Before that, extracting them would copy the "must agree" hazard into two files.
- **Cluster J (sidecar JSON)** is already seamed. Cluster K (snapshot assembly) is the one place
  that calls into D, G, I and J together.
- **`GSnapshot`** is read by five clusters, and **`GScopeOuter`** by nearly every function that
  mints an object. Each needs an accessor header before anything can move.
- **`EnterVerse`/`EnterVerseOn`** must keep exactly one implementation (its own comment says why).
  It moves to a header the other units include, not into a copy.
- **`GodotBindings.cpp` is the model:** stateless, reached only through `GetHost()` and the public
  surface of `HostScript.h`.

**What to build:** in order: the lookup and completion unit; the type model (after item 1, step 5);
signals (after `GCallbacks` gets one owner, since today clusters H and L both write to it); instance
dispatch; the build and generation lifecycle. While doing this, route every `uLang::` and `Verse::`
reach that is not in the type model through a small set of engine-facing adapters. Examples are
"definition location" (the digest side table, `PrototypeOf`), "qualified name" (the `FName`
case-folding and mangled-name rules), "decorated name" and "native rebinding". Then an engine drop
means auditing the adapters, instead of 951 reference sites in one 11k-line file. Most of those
rules are written down in `CLAUDE.md` today as things every future caller must remember.
Afterwards, each is a function that callers cannot bypass.

**Cost:** about two to three weeks in total, done as five separate moves, each green on the full
suite before the next. None needs an ABI change.

### 6. Give the editor side one state owner and a generation epoch, then split it

**Class it addresses:** STALE-CACHE and LIFECYCLE-ORDER in the consumer (B13, B26, B27, B36, B39,
and the completion cache above). It also includes the structural fact that
`verse_script_language.cpp` cannot be split cleanly as it stands.

The consumer's state is about 30 `mutable` fields on `VerseScriptLanguage`. They are mutable because
Godot's virtuals are `const`, and they are driven from one `_frame()` that touches nearly all of
them. There is no single point at which "generation N is now reflected everywhere". Each cache
decides for itself when it has caught up. `analyzed_source_by_path` has three writers.
`module_by_script` has three invalidation triggers. `VerseScript`'s description caches have three
more. The completion cache has none.

**What to build:**

1. **A generation epoch.** One monotonically increasing counter, advanced when a build publishes
   and when an analysis lands. Every cache stores the epoch it was filled at, and a read compares
   epochs. That replaces "which of three triggers fired" with one comparison, and closes the
   completion-cache hole by construction.
2. **A `VerseProjectState` object** that owns the build and analysis pump (`build_project`,
   `request_check`, `start_pending_check`, `poll_check`, `record_diagnostics`) and the flags it
   drives. Model its states as one enum, not as the combination of `project_built`,
   `provisional_build_allowed`, `corrective_build_pending`, `bindings_incomplete` and the two
   pending slots. The analysis report found this pump is the bus that every feature cluster is
   wired to, so extracting any feature first only moves the coupling somewhere else.
3. **Then the splits, which become mechanical:** the `verse_api` lookup helpers (lines 75–452,
   already shared with `verse_script.cpp` and misplaced); the debugger and the profiler (each owns
   a disjoint field set today); diagnostic and rejection prose (pure string building); completion;
   hover.

**Cost:** step 1 takes a few days. Step 2 takes about a week and is the risky one, because the
by-hand findings (B13, B20, B27, B38, B39) all live in exactly this sequencing. Re-run their
by-hand checks after it, in addition to the suite. Step 3 takes about a week and is low risk.

### 7. Retire the prose invariants as each is enforced

**Class it addresses:** the cost of keeping all of this true, and the next agent's odds of
knowing it.

`CLAUDE.md` is 17,632 words and loaded into every session. Much of it is the rulebook this list
converts into code: "claimed in all three", "never write a second one", "must go through that
table", "add a fifth construction path and it leaks". The phase documents also contradict their own
bodies; `generated-bindings.md` did so while this review was being written.

**What to build:** nothing by itself. As items 1–6 enforce a rule, replace its paragraph with one
line that names the enforcing mechanism and its test ID, and move the history to the phase
document where it belongs. When an invariant is enforced mechanically, its paragraph can become
one line. When it is not, the paragraph has to remain, because it is the only guard.

**Cost:** it rides along with the other items.

## What is deliberately not on this list

- **Shrinking the generated files.** `GodotClasses.native.verse` (57k lines), `verse_api_classes.h`
  (17k) and `verse_api_skipped.h` (7.7k) are large because the mirror is complete, which is a
  decision (`phase-2-design.md` §3). They are not where defects come from; the generator that
  writes them is, and it appears above.
- **Replacing the hand-rolled test style with a framework.** The one-line-per-case shape is not the
  problem, because it is what makes a layer readable in a log. Item 3 keeps it and adds structure
  beside it. What a framework would have prevented, a green layer with a printed FAIL, is fixed by
  counting failures globally, which `host_smoke` now does, and by item 3's per-case results.
- **A second binding generator in Python** to remove the C++/Python duplication. The C++ side has
  to run inside the editor, where the roster lives. Shared test vectors (item 1, step 6) remove
  the risk without making the editor depend on Python.
- **Splitting `gen_verse_api.py`.** It is long, but it is one linear pipeline with a single-source
  lane table and 254 white-box checks. The only structural fix it needs is to make its implicit
  call-order dependencies explicit (`record_math_skips` has to run before
  `render_skipped_header`, and nothing enforces that), which is small.

## Summary

| rank | item | defect classes | cost | depends on |
| --- | --- | --- | --- | --- |
| 1 | One registry per surface; compiler counts the rows | MISSING-ROW, TWO-SITES (31%) | 3–5 weeks, staged; step 1 in a day | — |
| 2 | Failures carry their reason | SILENT-DROP, WRONG-DEFAULT, silence (59% of runtime defects) | 2–3 weeks, rides with 5 | alongside 5 |
| 3 | Automatic, structured, white-box detection | all, earlier | step 1 in a day; rest 2 weeks | step 4 needs 5 |
| 4 | Foreign-contract suite and tripwires | FOREIGN-CONTRACT (21%) | 2–3 weeks, incremental | — |
| 5 | Split `HostScript.cpp` behind engine adapters | structure; engine drops | 2–3 weeks | 1 (step 5) |
| 6 | Editor state owner and generation epoch, then split | STALE-CACHE, LIFECYCLE-ORDER | 2–3 weeks | — |
| 7 | Retire prose invariants as they are enforced | upkeep | rides along | 1–6 |

If only three things are picked up before the next feature, make them item 1 step 1 (compiler
warnings), item 3 step 1 (a gate that runs without being asked), and the seven live defects above.
Together they cost less than a week, and each makes every later item cheaper to verify.
