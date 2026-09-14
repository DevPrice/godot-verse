# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read first

`README.md` is the design document, not a quickstart. It carries the two-DLL rationale, the
`@editable` property-export story, the `_validate` threading story, and "Five constraints worth
knowing" — read the relevant section before changing anything in that area. `docs/property-export.md`
and `docs/editor-tooling.md` hold the full research and citations behind those sections.

**Phase 2 is complete.** `docs/phase-2-design.md` §11 says what it built and — more usefully — the
places the design in that same document turned out to be wrong. Read §11 before trusting §1 or §4
of it. The short version: Verse's overloading is far narrower than §1 claimed, Verse forbids
non-public struct fields (so `variant`'s lanes are public, R-TYPE-7), Verse has no anonymous
functions, and Godot's property metadata hides its own enums.

**Phase 3 is built.** `docs/phase-3-design.md` is the whole of it, and **§11 is the part to read**:
written after the code, it is where the design turned out to be wrong. The three corrections that
matter — a generation costs **1.27 s** and retains **~1.3 MB** against a real project rather than the
200 ms and 0.5 MB the design assumed; a `UClass`'s qualified name comes back out of its *mangled*
name because `PackageRelativeVersePath` is dead under VerseVM; and asking the *semantic* program for
one must not use `EPathMode::PackageRelative`, which is fatal for a class with no package. §1.1 has
OQ-12's answer (positive: the package name carries the generation, the verse path is pinned).
Its two by-hand checks — a windowed run of the yardstick, and an editor session — have since been
done; `docs/by-hand-findings.md` is what they found.

**Phase 4a is built; 4b is not.** `docs/phase-4-design.md` is the design, and unusually for this repo
its spikes ran *before* it was written — **§2 is where they are**. **`docs/phase-4-gaps.md` is the
part to read before trusting it**: twenty-one numbered entries saying where the implementation and
the design disagree, what is unbuilt, and what would close each. §13 of the design summarises the
phase and carries its measurements.

**Every one of those entries is now closed, built or answered** — G13 (the math bodies) and G11 (the
utilities) went with the rest, and the by-hand checks the phase owed have since been run
(`docs/by-hand-findings.md`), so **the phase owes nothing**. Closing them took the ABI to v5, Phase 5 took it to v6, the editor-performance work to 7.1 and the
editor last-mile work to 8.0 and Phase 6 to **8.1**; either way both DLLs must be
rebuilt and `run_tests.py --build` is how the test binaries follow.

The four ways a signal declaration could compile and not work (G1–G4) are one validation pass in
`GetClassSignals` now: `vh_signal_desc` carries a `Reject` the way `vh_export_desc` does, Godot is
never told about a signal nothing can emit, and `_validate` says why at the member's line. A struct
payload works in **both** directions — out as one Godot argument per field, named by the field, which
is what gives the connect dialog real names; back in through `InstanceCall`'s rule that N arguments
satisfy one struct parameter with N fields (**G21**). That path needs no Godot counterpart for a
struct, because a signal delivers the fields separately; a struct as a *method parameter* still has
none, and that half is R-LANG-2's, which the spec answers with a Dictionary. `signal()` is an
alias for `signal(tuple())` (G7), spelled the way `/Verse.org/Concurrency` spells
`listenable()`. The thread guard covers every entry point (G6), and the math tail is recorded rather
than silent (G12).

**`_make_function` is built and cannot be tested from here** (G5): it is a `ScriptLanguageExtension`
virtual with no ClassDB entry, and `Script.get_language()` is not in the public API, so GDScript can
reach neither — the editor's own C++ is its only caller. G19 said the same of `_HasPoint` and
`_CanDropData`; it was right about only one of them. `_HasPoint` is a case in `tests/integration`
now, because the engine asks it unprompted as soon as `Input.parse_input_event` supplies a click.
`_CanDropData` is the one Godot virtual that has still never been exercised.

Four things it settled are load-bearing everywhere else. A virtual is spelled the way Godot spells
it — **`_Ready`, not `Ready`**, and §7.1 counts the eight *signal* collisions that decided it.
`_notification` is in no part of `extension_api.json`, so `_Notification` is hand-written on the
native root and the rest of that family is R-NODE-10, in 4b. **OQ-11 is closed**: the math types are
ordinary Verse, with extension methods and definable operators and no ABI at all. And Verse's own
`signalable`/`subscribable` cannot be implemented here — their domains are `no_rollback` and every
Godot callback runs in a transaction — so `signal` has their *vocabulary* and not their
interfaces.

**One cost of the math is easy to trip over.** An extension method is a **module-level** definition:
`(V:vector2).Angle()` declares `operator'.Angle'`, and Verse resolves a bare `Angle` against it — so
a parameter or local named `Angle`, `Length`, `Dot`, `Cross`, `Normalized`, `Rotated`, `DistanceTo`
or `LengthSquared` is *ambiguous*, not shadowing. `gen_verse_api.py`'s `VERSE_STDLIB_NAMES` keeps the
generated mirror clear of them; a script has to avoid them by hand, the way it already avoids `Abs`
and `Clamp`. **`event` belongs on that list too** and is the one met first: the mirror spells
`_Input`'s parameter `Event`, and an override that writes `event` collides with
`/Verse.org/Verse`'s own `event` — glitch 3532, reported at the parameter with no hint that a
capital letter is the fix (`docs/by-hand-findings.md` B2).

**Phase 4.5 is built, and its `docs/phase-4.5-design.md` §11 is the one section of it to read** —
the design was written *before* the work, so §11 is where the plan is corrected rather than where it
is summarised. What it settled, all of which is load-bearing:

- **A method's effect is now Godot's `is_const`.** 3996 mirror methods carry `<reads>` where all
  9597 carried `<transacts>`, and the test is `const` **and answering a value** — Godot's `const`
  means "does not mutate the C++ object", so the 38 const-and-void methods are `OS.set_environment`,
  `CanvasItem.draw_string` and 36 more that plainly do something. A `<reads>` body dispatches through
  `VhCallValueConst`, not `VhCallValue`; the two have to move together.
- **An archetype instantiation carries the constructing class's own effect.** This is why `variant`
  and `godot_ref` are `<computes>` now, and why the singleton accessors stopped constructing:
  `GetInput()` is `input[VhObjectOf(VhSingleton["Input"])]`, a cast over what the host built, which
  is R-SCN-6's rule and what they should always have been. A mirrored class cannot be `<computes>`
  — it descends from the native `vh_object` — so anything a `<reads>` body must build has to be a
  cast rather than a construction.
- **`Verse::Stm::OnRollback` does nothing here.** It is the Solaris *interpreter's* STM and
  `VerseStm.h` says "Noop if StmActive() returns false". Phase 4's `Subscribe` compensation was
  written with it and had never run. The mechanism that works from this bridge is
  `AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>` — `SameAsClosed` is load-bearing,
  because every Godot callback reaches C++ inside `AutoRTFM::Open` and a plain `OnAbort` from open
  code is ignored.
- **`docs/nonatomic-methods.md` is generated** and is R-AUD-3's list: the **1073** emitted methods
  whose `<transacts>` promises a rollback the bridge cannot perform. Not 1354 — that count included statics
  and methods the mirror does not emit.
- **`CONST_OVERRIDES` is where Godot's flag is missing rather than too broad**, and nothing in it was
  judged: `tools/audit_const_overrides.py` reads the rows out of a Godot *source* checkout and takes
  a method only when its body is exactly `return <member>;`. Run it by hand against a newer Godot to
  revise the list; it needs `../godot`, which nothing else here does, so it is not in `run_tests.py`.
- **What a failure undoes is measured, not assumed**, and the rule is in `spec.md` next to R-AUD-1.
  A failure at any depth drops the deferred writes; a read does not see a write the same computation
  just made; a raise additionally halts every script until the next `vh_tick`, which is why
  `test_main.gd`'s transaction section used to run a step per frame — Phase 5 removed that
  constraint with it, and the section running raises back to back in one frame is now itself the
  test that a raise stops only the call that raised.

**`docs/dodge-the-creeps.md` is the one to read before adding a Verse-facing feature.** It is not a
progress report — it is the eight things a Godot author writes without thinking, each measured in a
real game rather than estimated, with the requirement that gives it a spelling. **Seven of the eight
are down**, the seventh at the close of Phase 5; the table says which, and §"After Phase 4" and
§"After Phase 5" say what each diff came to. The one still standing is the `<transacts>` trap, which
Phase 4.5 and Phase 5 **narrowed twice rather than removed**: reading Godot no longer starts the
cascade and a signal handler is no longer fixed at `<transacts>`. What it does *not* have any more
is an explanation: the appended diagnostic and the template's warning were both removed after the
by-hand session (`docs/by-hand-findings.md` B6, B7), because the sentence never checked which effect
had been refused and gave the opposite of correct advice on a `suspends` refusal. A helper that
*writes*, called from a body narrowed on purpose, still needs the word, and now says so in the
compiler's words alone.

**Phase 5 is built, and `docs/phase-5-design.md` §14 is the one section of it to read** — written
after the work, it is where the design turned out to be wrong. §2 was filled in the same way
*before* the work, from twelve questions put to the Verse compiler through `tests/verse_probe`
(eight committed fixtures), which is why so little of the rest needed correcting. Phase 5 took the
ABI to v6; the editor-performance work below took it to 7.1, the editor last-mile work to 8.0 and
Phase 6 to **8.1**, and either way both DLLs must be
rebuilt and `run_tests.py --build` is how the test binaries follow.

What it settled, all of which is load-bearing:

- **A task scope per script instance (R-ASYNC-4), made at `vh_instantiate` and terminated at
  `vh_release_instance`.** A raise terminates the *active* scope, so it costs that node's suspended
  work and nothing else's — and the instance gets a **fresh** scope at its next call rather than a
  revived one at the next tick. `GHaltedUntilTick`, `GTasksLostToError` and `ReviveContentScope` are
  gone, `VH_ERR_HALTED` is a narrow answer rather than what every other call got for a frame, and
  `vh_tick` is no longer where anything recovers. **D23 is retired**: "lazily at the first `spawn`"
  has no hook to hang on, and the guard has to be already active at that moment. It costs ~2.6 KB
  per scripted node and ~0.06 µs per call, both measured with `tools/build_bench.py`.
- **`Await` is ordinary Verse over `/Verse.org/Verse`'s `event(t)`.** `signal(t)` holds one
  and `Await<public>()<suspends>:t` forwards to it, which covers a script's own signals and all 489
  mirrored engine-signal accessors alike. The *host* half is smaller than the design budgeted for:
  **`verse::event` is a UObject with a public C++ `Signal`**, so the host reads the event off the
  signal object and signals it directly — Epic's code then does FIFO resumption, per-task scopes and
  dropping a cancelled awaiter. Neither of S-5's two proposed shapes was reachable, because
  `MakeCallableFor` accepts only a method bound to a *script instance* and a `signal` is not
  one.
- **`defer` runs when a task is cancelled, not only when it returns.** The whole connection lifetime
  of `Await` rests on it: `Await` connects with `CONNECT_ONE_SHOT`, holds the signal object, and
  disconnects in a `defer`. Measured in `tests/verse_probe/sleep_probe.verse` with a `race` whose
  loser sleeps for an hour.
- **An awaiting body cannot be narrowed, and neither can its caller.** `awaitable.Await` carries
  `no_rollback` exactly as `signalable.Signal` does. A mirrored virtual override is specifier-less
  and so, since this phase widened `signal.Subscribe`'s callback to match Verse's own
  `subscribable`, is a signal handler. **A `<reads>` body may not `spawn` at all** — where 4.5's
  narrowing and this phase collide. And **a virtual cannot be written `<suspends>`**: glitch 3532
  plus 3523, because the specifier makes it a different function. Both are said in the `.verse`
  template rather than diagnosed.
- **A foreign signal is named with `MakeSignal(Owner, Name)`** — Godot's own `Signal(object, "name")`,
  the analogue of `MakeCallable`. Nothing could produce a `signal_ref` before, so `signal_ref.Await()`
  and `.Subscribe()` would have been unreachable. Its payload is a **`godot_array`** of the emission's
  arguments (D17 amended: `[]variant` has no description on this bridge), and its `Subscribe` is the
  rollback-safe way to receive a foreign signal, which `Object.Connect` is not.
- **`Sleep(Seconds)` is a native `<suspends>` on `FPlatformTime::Seconds()`**, resumed from the pump,
  because it has to work where there is no scene tree. It ignores `Engine.time_scale` and keeps
  counting in a paused game; game timing awaits a Timer instead. `vh_tick`'s budget governs the
  queue and nothing else — a task resuming inside an emission is unbudgeted, exactly as GDScript's
  resume is — and `vh_tick` now fills a `vh_tick_stats` that becomes three Godot custom monitors.

**Phase 6 is built, and `docs/phase-6-design.md` §13 is the one section of it to read** — written
after the work, it is where the design turned out to be wrong. §2's spikes ran first, and S-1 was
the cliff the phase rested on: **a snippet-compiled procedure carries its file path verbatim**, the
absolute path `vh_compile_project` was handed, mixed separators and all, because a data package has
no `_DirPath` for `VVMLocationUtil.h`'s `GetPath` to relativize against. Everything else followed.

What it settled, all of which is load-bearing:

- **The host owns *which frame*, the consumer owns *which line*.** `EngineDebugger`'s breakpoint
  list and step state are never duplicated in the host; what only the host can see is frame
  ancestry, and it is needed because the bridge sees no Verse call, only a bytecode op, so Godot's
  depth counter would never move and step-over would behave as step-in. `DebugShouldBreak` carries
  a `vh_debug_frame_relation` — SAME, DEEPER, OTHER — and the consumer reads `get_depth()` as
  *which kind* of step is pending (-1 in, 0 over, 1 out) rather than as a count it maintains.
- **A step may not land where it started.** `Total := Helper()` reports its line **twice**, once
  before the call and once when the result lands, so a step-over with no memory of where it began
  stops on the line it began on. GDScript never meets this: its line opcode is per source line, a
  Verse location is per op. The consumer remembers the (source, line) it stopped at.
- **A line that emits no op carries no location.** Every statement line reports one and so does a
  function's declaration line, but a trailing bare expression that only reads a register does not —
  `Inner` as the last line of a body — so a breakpoint there never fires. Measured, and
  `tests/host_smoke` asserts both halves.
- **Re-entering a stopped VM is safe** (S-3, measured): an ordinary call or property read from
  inside Godot's debug loop runs and the outer frame resumes, so the remote inspector stays live
  while paused. `VH_ERR_STOPPED` is therefore narrow — the three that build or analyse, and not
  for re-entrancy: publishing a generation under a frame of the retiring one is incoherent, and an
  analysis resets the semantic program the stopped frame is about to resume into. `vh_tick` is
  refused silently.
- **The debugger attaches whenever Godot's is active, and that is affordable**: +1.6% of frame time
  on the yardstick (4.26 s → 4.33 s headless), though a single `vh_instance_call` goes from 0.27 µs
  to 2.79 µs. The polled breakpoint mirror the design held in reserve was never written. Attaching
  also suspends VerseVM's computation watchdog, which is what makes sitting on a breakpoint legal.
- **The two debuggers are mutually exclusive.** `SetDebugger` is one global pointer, so
  `verse/host/enable_debugger` (Epic's socket debugger) and Godot's own cannot both be attached;
  `vh_debug_set_enabled` refuses rather than overwriting and the consumer warns once.
- **The profiler is boundary instrumentation plus `profile{}` blocks, and not a sampler** — a
  sampler cannot produce a call count. **A Verse function called from another Verse function has no
  row of its own** unless the author writes `profile("tag"){…}`, which the compiler accepts in a
  `/user@localhost` package and the VM reports through `FVerseProfilingDelegates`.
- **`ScriptLanguageExtensionProfilingInfo` is a stride trap.** Godot's real `ProfilingInfo` has had
  five fields since 4.3; its `GDREGISTER_NATIVE_STRUCT` string still lists four, so godot-cpp's
  struct is 32 bytes for an array whose elements are 40 and `p_info_array[i]` corrupts for any
  `i > 0`. The stride comes from `Engine::get_version_info()`. Check the registration string
  against the header before trusting any other generated native struct.
- **`TArray::AddDefaulted_GetRef()` does not zero a POD** — it default-*initializes*, so a
  descriptor built that way carries whatever the previous answer left in the lane this one does not
  fill. That was a segfault four calls into the first working stop. Assign from a zeroed local.
- **OQ-13 closed as "nothing is bounded"**, and the defect it pointed at was elsewhere: the
  bridge's own unthrottled stack printing, which spent the shared character budget and silenced
  every other script. Rate limited per raise site, with the "n dropped" summary flushed from
  `vh_tick` — a script that raises sixty times and stops has no next occurrence to flush it.
  `verse/instance_tasks` is the custom monitor that stands in for a cap on the `spawn` runaway.

**The one thing Phase 6 owes is its editor session** (S-6), which is windowed and cannot be
automated; `docs/by-hand-findings.md` carries its steps. Everything from the ABI inward has
`host_smoke` cases; everything from `EngineDebugger` inward has none.

**The by-hand checks have been run, and `docs/by-hand-checklist.md` is deleted** — all twenty-two
of its entries were watched happen, and what is worth keeping is what they found rather than the
list. **`docs/by-hand-findings.md` is that**: B1–B9 are the defects, all fixed, B10–B11 are about
the list itself, B12 is a Verse fact, B13 is a later session's latency finding, its "What shipped"
table says what each change came to, and its "What is still open" section is where the two remaining
by-hand checks live. Four are load-bearing outside the editor:

- **B1** — override completion had offered nothing inside a class body since Phase 4 moved the
  virtuals onto the mirrored classes. `method_mapping` carries `is_virtual` now.
- **B4** — `_validate` and the two warning passes asked the host for a class by bare stem, so a
  script under a `.vmodule` marker got no method outline, no export warnings and no signal
  warnings. All three are module-qualified now.
- **B7** — `explain_effect_errors` is **gone**. It never checked which effect had been refused, so
  a `suspends` refusal took the `transacts` branch and gave the opposite of correct advice. The
  rule that replaced it: the bridge annotates a diagnostic only where the bridge is what the author
  is confused by.
- **B6** — the script template is a direct translation of GDScript's, two comments and `{}` bodies,
  and compiles as generated.

Those were re-checked by hand afterwards. Override completion and the connection gutter are
confirmed; the Attach Script dialog needed one more fix (unchecking its Template checkbox still
wrote the template — Godot matches a built-in named exactly `"Empty"`, and there is one now).
**One thing stayed broken and is documented rather than repaired**: adding `@tool` to a script that
did not have it needs the scene reloaded. With `_CanDropData`, that is the whole of what is known
to be open.

**The editor-performance work has no design document, by decision — the four commits are the
record.** `dcd517e` (every class-describing read answers from a snapshot instead of joining the
analysis thread, and the wait is measured), `40d72f4` (completion and signature help stop analysing
and stop waiting; ABI 7.0), `8bbba32` (the mirror is read from its digest, with a side table for the
two things a digest drops) and `1469dc1` (`vh_class_override_candidates`; ABI 7.1) each say what
changed and why, at more length than a summary would. The headline: a whole-project analysis is
**721 ms** where it was 1273, a generation **1.54 s** where it was 2.2, and **nothing on the
editor's thread waits for either** — reads that used to cost 1.7 s during an analysis cost 0.0 ms.
`docs/spec.md` R-PERF-2 has the whole table and the machine it was taken on.

**README predates Phase 1 and is stale on marshalling.** It still describes three hand-written
value types, a `variant` tuple, `object` as the only `<native>` class, and packed arrays crossing as
copies — all four now wrong — and it says nothing about general dispatch, the method list, or
runtime errors with stacks. It is awaiting a rewrite rather than a patch. Phase 4 made it staler still: all
1413 of Godot's virtuals are carried now and are spelled `_Ready`, not `Ready`; `@GlobalScope`'s
constants and statics are reachable through per-class `...Statics` modules; the math types have
methods and operators; and a script declares signals as typed members. Its editor-tooling, export and constraints sections are
unaffected. Where the two disagree, `docs/spec.md` and `docs/abi-v2-design.md` are the record.

Phase 2 makes it staler still, in ways worth knowing before reading it: every one of Godot's 1023
classes is mirrored now rather than a curated ~60, Godot's `Object` among them; its 758 enums are
real Verse enums; `typedarray::Node` is a `typed_array(node)` whose elements are objects a script
calls methods on; and the hand-written native root is `vh_object`, because `object` is now the
*mirror* of Godot's Object.

`docs/spec.md` is the requirements document — what the finished software must do, numbered so a
commit can cite one. README describes how the thing works; the spec describes what it must do, and
carries the per-requirement status of what it *does* do today — which is where to look now that
README has fallen behind. §14 holds the open questions that block the rest. Check a requirement's status there before assuming a gap is
unexamined. `docs/roadmap.md` sequences those requirements into phases and says which phase the
work in front of you belongs to. `docs/phase-0-spikes.md` is why three of those answers read
the way they do — read it before re-deriving anything about hot reload, the export pipeline,
or the flat scope. `docs/abi-v2-design.md` is the same thing for the wire: what godot-cpp does and
why the containers are references, plus the four spikes that settled the shape — the fixed-width
`variant`, whether VNI marshals a native struct, whether a dropped Verse value releases anything,
and how often a nullability rule would be wrong. Read it before changing `variant`'s lanes or
proposing a different encoding.

This file is the map and the working rules; the reasoning lives there.

## Two binaries, one C header

    verse_host.dll    host/       UBT + AutoRTFM clang, monolithic UE Program target.
                                  Boots FEngineLoop, owns VerseVM, compiles and runs .verse.
    godot_verse.dll   src/        SCons + MSVC against godot-cpp.
                                  Loads the host, feeds it Godot callbacks, pumps it per frame.

`include/verse_host_abi.h` is the only thing that crosses. Plain C — the two sides cannot share a
C++ ABI. It is staged into the host's `Public/` by `build_host.py`, so both compile the same file.

A change to that header means bumping `VH_ABI_VERSION` and rebuilding **both** sides: the mismatch
surfaces at `vh_init`, not at compile time. The version is now `MAJOR * 1000 + MINOR` with the
policy written at the top of the header: a major bump is a layout or meaning change and both sides
must be rebuilt; a minor bump adds something an older consumer can ignore behind a `StructSize`
check. The design argument for v2, and the spikes that settled it, are in `docs/abi-v2-design.md`.

### `src/` — the GDExtension

| file | owns |
| --- | --- |
| `register_types.cpp` | registration order: language before resource loader |
| `verse_host.{h,cpp}` | `GetProcAddress` loader over the ABI; no Verse logic |
| `verse_runtime.{h,cpp}` | the `VerseRuntime` singleton — `vh_init_desc`, the Godot callback table, `verse/host/*` project settings |
| `verse_value.{h,cpp}` | `Variant` ⇄ `vh_value`, arena-allocated |
| `verse_ref_table.{h,cpp}` | the id → `Variant` table the `Ref` lane names: Array, Dictionary, Callable, Signal and the packed arrays, which cross as references rather than copies |
| `verse_script.{h,cpp}` | a `.verse` file as a Godot `Resource`; valid only if it defines its own class |
| `verse_script_instance.{h,cpp}` | one script bound to one node; raw `GDExtensionScriptInstanceInfo3` vtable, not a `godot::Object` |
| `verse_script_language.{h,cpp}` | the `ScriptLanguage`: `_validate`, the analysis cache, `_complete_code`/`_lookup_code`, `_frame` (which pumps `vh_tick`, reaps `vh_check_project_poll` and attaches the debugger), and the `_debug_*`/`_profiling_*` surface |
| `verse_resource_format.{h,cpp}` | load/save, without which a `.verse` cannot be attached to a node |
| `verse_lexer.{h,cpp}` | resumable per-line lexer; no godot-cpp dependency, so it is unit-testable standalone |
| `verse_class_decl.{h,cpp}` | scans the top-level class **named after the file** and its `@global_class` attribute out of the text; defers comments and strings to the lexer, and shares its lack of godot-cpp |
| `verse_module_map.{h,cpp}` | which module each `.verse` is in, from the `.vmodule` markers; pure, and the third godot-cpp-free unit |
| `verse_module_menu.{h,cpp}` | editor-only: "Make Verse Module" in the FileSystem dock, because Godot's dock cannot create an empty file |
| `verse_syntax_highlighter.*`, `verse_editor_plugin.*` | editor-only (`TOOLS_ENABLED`) |

`VerseScriptLanguage` overrides only the virtuals it actually answers — godot-cpp binds a virtual
with Godot only when the subclass declares it, so **omitting one is how you say "unsupported."**
Adding an override you do not implement changes behaviour.

### `host/` — the UE Program target

`Private/` is the ABI implementation (`VerseHost.cpp`, `HostRuntime`, `HostScript`, `HostEventLoop`,
`HostDebug`, `GodotBindings`, `GodotClasses`). `HostDebug` is the `Verse::FDebugger` and the
profiler's accumulators; nothing else in the host knows either exists. `Verse/*.native.verse` is the `/Godot.org/Godot` package.

`GodotClasses.h` holds every C++ shadow a `<native>` Verse declaration needs, and there are three:
`vh_object` (a UObject, so a script's class has one to be instantiated and called through), `variant`
(a struct, the fixed-width lanes one Godot value crosses as) and `godot_ref` (a UObject whose
`BeginDestroy` is what releases a reference id when Verse drops the value holding it).

`vh_object`, not `object`: since Phase 2 `object` is the generated mirror of Godot's own `Object`
class, and it derives from `vh_object`. Verse cannot reopen a class, so Object's methods could not be
added to the hand-written root. Nothing a script writes should name `vh_object`. A native
Verse type without its shadow is an "incomplete type" build failure naming the generated header.
`VerseHost.Build.cs` and `.Target.cs` carry load-bearing comments — `SetupVerse(..., InternalUser)`
and the `VerseSimulationMetadata` dependency each exist for a reason spelled out inline.

## Commands

    python tools/build_host.py            # stages host/ into the UE tree, runs UBT
    scons target=editor                   # the GDExtension (also: target=template_debug)
    python tools/gen_verse_api.py         # regenerates the Verse mirror of Godot's API
    python tools/build_smoke.py           # ABI test binary
    python tools/build_lexer_test.py      # lexer test binary
    python tools/build_class_decl_test.py # class-declaration scanner test binary
    python tools/build_module_map_test.py # module-map test binary
    python tools/build_bench.py           # host benchmark (timings, not pass/fail)
    python tools/build_verse_probe.py     # the Verse probe (asks the compiler a question)
    python tools/audit_const_overrides.py # CONST_OVERRIDES, read out of a Godot source checkout

Run the tests:

    python tools/run_tests.py                    # all three layers; the one command (R-QUAL-3)
    python tools/run_tests.py --only units       # or one of units / abi / integration
    python tools/run_tests.py --build            # rebuild the test binaries first

It runs three layers and reports each: **units** (lexer, class-declaration scanner, module map,
generator — no Godot, no UE), **abi** (`host_smoke`, the whole C ABI with no Godot), and
**integration** — which
is two headless Godot projects, `tests/integration` for behaviour and `tests/coverage_diagnostic`
for the R-SCN-2 diagnostics. The second is its own project because its one script deliberately does
not compile, and one unresolvable name in the first would take every other case down with it. Its
assertions live in `run_tests.py` rather than in the project, because `ScriptLanguage` exposes
nothing a script can ask — so the only way to read what an author would see is to read what the
editor prints.

`tests/host_bench`, built by `tools/build_bench.py`, is not part of `run_tests.py`: it reports
timings rather than pass/fail, because R-PERF-2 asks for a recorded number and a threshold would
fail on a slower machine. It is what took the numbers in `phase-2-design.md` §3.1 and in
`spec.md` R-PERF-2, and how to take them again. Its arguments are the host DLL, the engine, **the
repo root** and an iteration count — the third is not a project path, it is where the bench finds
`tests/host_smoke`'s fixtures and `dodge-the-creeps/scripts`:

    bin/host_bench.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine . 10

`VH_TRACE_ANALYSIS=1` is the other instrument, and works against anything that loads the host: the
host prints a per-analysis trace to **stderr** — each package's role (Source or External), the
digest bytes read, parse and semantic milliseconds, and what the snapshot cost. stderr rather than
the diagnostic callback because a background analysis runs off the game thread and every ABI
callback is the game thread's alone. A layer whose prerequisites
are absent is **skipped and said to be skipped**, never counted as a pass. `UE_ROOT` names the
Unreal checkout and `GODOT` the Godot binary; both are guessed when unset.

`tests/verse_probe`, built by `tools/build_verse_probe.py`, is not in `run_tests.py` either, and for
a different reason: it asserts nothing. It compiles whatever `.verse` files it is handed as one
project, prints every diagnostic, and calls a class's zero-argument methods — which makes a language
question ("does a two-parameter function satisfy a tuple-parameter callback?") something you *run*
rather than something you read out of `SemanticAnalyzer.cpp`. Six of Phase 4's design decisions were
settled with it in one sitting, and **all twelve of Phase 5 §2's answers** came out of it; the eight
fixtures beside `example.verse` are kept so every claim can be re-run rather than recalled.
`async_reject.verse` compiles **nothing** on purpose — it is the file of refusals, and the *text* of
each refusal is its result. Take the path as absolute; the probe resolves nothing relative to `bin/`:

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine         tests/verse_probe/example.verse --class example

Run it **from `bin/`**, where `build_host.py` leaves `tbbmalloc.dll`: without it `LoadLibrary`
answers a bare 126 and names nothing.

The binaries still run standalone, which is what to reach for when bisecting one failure:

    bin/host_smoke.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine .
    bin/verse_lexer_test.exe
    bin/verse_class_decl_test.exe
    bin/verse_module_map_test.exe
    python tests/verse_api_gen/test_gen_verse_api.py

No test framework anywhere. Each test is a `main` (or a plain script) that prints one line per case
and exits non-zero on failure; keep new tests that shape. The integration layer is the same shape
in GDScript — `tests/integration/test_main.gd`, one line per case, `quit(1)` on failure.

`tests/integration` is a real Godot project, and two things in it are not committed but generated:
`run_tests.py` copies the built GDExtension into its `addons/`, writes `.godot/extension_list.cfg`
(outside the editor Godot loads extensions from that list rather than by scanning, and the editor is
what normally writes it), and rewrites the two `verse/host/*` settings from `UE_ROOT` — those name
one machine's engine checkout, so nothing portable can be committed. Adding a `.verse` fixture there
means adding it under `scripts/`; the host compiles every `.verse` under `res://` together.

`dodge-the-creeps/` is the third Godot project and the yardstick: the whole game in Verse, with no
GDScript in it but `headless_check.gd`, which is how to see it work without a window —
`godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd`, 30 checks, one
line each. `--fixed-fps` is not optional; headless, a `Timer` counts real seconds while the loop
runs flat out. It is deliberately **not** in `run_tests.py`: a yardstick that gates the build stops
measuring. `scons` copies the addon into it the way it does for `demo/`.

`tools/build_host.py` needs a UE source checkout with the Verse toolchain — `--engine`, or `UE_ROOT`.
Building the host and running the tests are fine to do unprompted, and so is **headless** Godot —
`tools/run_tests.py` drives one for the integration layer and it opens no window. **Ask before
launching the editor** (`godot --path demo` with no `--headless`), which does.

## Generated files — never hand-edit

| generated | by | from |
| --- | --- | --- |
| `host/Verse/GodotClasses.native.verse` | `tools/gen_verse_api.py` | `godot-cpp/gdextension/extension_api.json` |
| `src/verse_api_classes.h` | `tools/gen_verse_api.py` | same |
| `host/Private/GodotMathLayout.gen.h` | `tools/gen_verse_api.py` | same — the math types' field trees, so the host builds one the way the Verse struct declares it |
| `src/verse_api_skipped.h` | `tools/gen_verse_api.py` | same — every Godot member the mirror does not carry under its own name, and why, which is what `_validate` turns into a sentence (R-SCN-2) |
| `host/Private/GodotClassNames.gen.h` | `tools/gen_verse_api.py` | same — every Godot class and the mirrored Verse class an object of it crosses as, which is what R-SCN-6's cast is built on. Every class, not only the emitted ones: a `--classes-file` build still has to make a handle cross as *something*, so each row names its nearest emitted ancestor |
| `docs/nonatomic-methods.md` | `tools/gen_verse_api.py` | same — R-AUD-3's list: every emitted method that mutates Godot *and* answers a value, so its `<transacts>` promises a rollback the bridge cannot perform. Written by the pass that writes the mirror, so it cannot drift |
| `src/verse_keywords.h` | `tools/gen_verse_keywords.py` | the UE compiler's `ReservedSymbols.inl` |

**Every Godot class is mirrored by default.** `tools/verse_api_classes.txt` is a smaller curated
list kept for anyone who wants a smaller build, selected with `--classes-file`; there is no `--all`,
because all *is* the default. The reasoning is `docs/phase-2-design.md` §3: adding a class means
rebuilding `verse_host.dll`, which means a UE source checkout, so a subset is a wall rather than a
setting. It costs per-keystroke analysis latency, which is measured and recorded there.

`src/verse_api_skipped.h` also carries what the **math** file does not define, and that row source is
unusual: `gen_verse_api.py` *reads* `host/Verse/GodotMath.native.verse` to find out what is written
and records every other `builtin_classes` method and operator as a skip. So adding a method there
deletes its own skip row on the next generation, and the record cannot drift from the code. 410 rows
today, from 585 before the math was written.

`gen_verse_api.py`'s type table is the other half, and it no longer skips anything for a type it
cannot carry — `unsupported_type` is zero. Three small tables decide the awkward names, and each
says why in place: `VERSE_AMBIGUOUS_MEMBER_NAMES` (five names, compiler-confirmed, not guessed),
`PROPERTY_RENAMES` (`Min`/`Max` → `Minimum`/`Maximum`, the only invented names in the mirror) and
`FREE_FUNCTION_REPLACEMENTS` (`Object.to_string` is Verse's own `ToString`, which is also what
string interpolation desugars to).

`host/Verse/Godot.native.verse`, `GodotApi.native.verse` and `GodotMath.native.verse` **are**
hand-written: the first is the whole native primitive surface, the second the ordinary-Verse packing
layer above it, and the third the math types' methods and operators — which are ordinary Verse with
no handle and no ABI, because that is OQ-11's answer. `GodotMath` carries `.native.verse` despite
declaring nothing native: VNI refuses a plain `.verse` in a VNI-capable package. Mirroring
another Godot *class* still costs no C++ and no new native function — that rule held through ABI v2.
What did cost native functions was the reference types: the primitive surface went from 8 to 22
(24 since Phase 4.5 added the two `<reads>` dispatchers),
because a container has to be asked for its elements rather than decomposed.

The container wrappers (`godot_array`, `dictionary`) are **generated**, not hand-written, even
though they are not mirrored Godot classes. A script cannot spell a `variant` — the packers are
module-scoped by R-TYPE-7 — so every way into and out of a container has to be a typed accessor, and
ten element types against four key types is not a list to maintain by hand.

## Constraints that break things silently

- **The host must load from `Engine/Binaries/Win64`.** VNI records each Verse package's source
  directory relative to the loaded module and the compiler reads those `.verse` files at runtime.
  A copy elsewhere compiles against an empty package set and every identifier is unknown.
  `bin/verse_host.dll` exists for the smoke test only; Godot points at the engine tree through the
  `verse/host/dll_path` project setting.
- **A build is the whole project, and it happens on Play — not on save.** `vh_compile_project`
  publishes a *generation*: a package name no publish has used, with the verse path pinned at
  `/user@localhost` and the retiring generation removed from the source project first. Every build
  re-enumerates `res://`, so a file added, renamed or deleted lands without a restart. What a save
  does instead is refresh analysis, which keeps diagnostics, completion and the export *shape* live
  per keystroke — but a changed `@export` **default** is generated code and waits for a build.
  `VerseEditorPlugin::_build` is the trigger (`EditorNode::call_build()` before a run, the same
  hook C# uses), plus a "Build Verse" item in Project > Tools. A failed build publishes nothing and
  refuses the run, leaving the last good generation running. Instances adopt nothing: one made
  against generation N keeps generation N's class for life — which is why `VerseScript::_reload`
  *re-attaches* the script to every object holding it, destroying each instance and building
  another, exported values carried across by hand.
- **No call on the editor's thread may wait for an analysis.** Every read keyed by a class name —
  `vh_has_class`, the method, signal, static and member lists, abstractness, the export list with
  its Reject reasons and every export's declared default — answers from the **snapshot** the last
  analysis left, and 0.0 ms during one is the whole point: ~22 of these used to begin with a
  `std::thread::join` and cost the main thread 1.7 s apiece. The three that resolve a *position*
  cannot be snapshotted, because a position resolves against the AST the worker is rebuilding:
  `vh_lookup_symbol`, `vh_complete_symbol` and `vh_signature_at` answer `VH_ERR_STATE` while one
  runs, and the consumer's recourse is to queue that buffer and ask again. **Only the entry points
  that *execute* Verse still wait**, because Solaris blocks the VM for the length of any build. If
  you add an entry point, it belongs in one of those three groups and never in a fourth.
- **A consumer that begins an analysis must poll it to completion.** Nothing else reaps one: until
  `vh_check_project_poll` says finished, the next `vh_check_project_begin` is refused and `vh_tick`
  stays a no-op. The bench relied on a later wait to do the reaping and refused forever once the
  waits were gone.
- **The mirror is read from its digest after the first successful build**, which is half the
  per-keystroke cost, and a digest drops exactly two things: every definition's **file and line**
  (a digest is one synthetic snippet at a path no file is ever written to) and
  `CFunction::_bIsAccessorOfSomeClassVar` (DigestGenerator re-emits a class var without the
  `<getter>`/`<setter>` attributes the analyzer reads it off). A side table recorded at the first
  build's trailing analysis — the last program that reads the mirror's own files — restores both,
  keyed by qualified name plus the function type's code, because `GodotMath.native.verse` declares
  eight two-parameter `operator'+'` and a verse path alone is ambiguous. **Anything new that reads a
  mirror definition's location or accessor flag must go through that table**, `GetScopeName()`
  included: from a digest a top-level definition's Owner and its path are *both* the digest path,
  so an `Owner == DeclaredIn` test keeps passing while both are wrong.
- **The "user package" test is `InternalUser`, and the mirror passes it.** `SetupVerse(...,
  InternalUser)` in `VerseHost.Build.cs` sets it on `/Godot.org/Godot` and the attribute package
  sets it too, so "walk every InternalUser package" walks all 4.3 MB of the mirror's AST before
  reaching the two snippets that could hold a cursor — 97 ms per completion. Walk the package at
  `ScriptVersePath` and nothing else.
- **Adding `@tool` to an existing script needs the scene reloaded.** Editing a live `@tool` script
  takes effect on save; giving one `@tool` for the first time does not, because the node is holding
  a *placeholder* and the swap to a real instance does not happen. Known, small, and not fixed —
  `docs/by-hand-findings.md` B8 has what is ruled out and where to look. Nothing automated can see
  it: a placeholder only exists under `is_editor_hint()`.
- **The host module never unloads.** `vh_shutdown` tears the engine down; the DLL stays resident.
- **A stopped VM is re-entrant, but two entry points are still refused.** Godot's debug loop runs
  on the interpreter's own thread and goes on servicing the editor while stopped, so an ordinary
  call or property read from inside it works (measured). `vh_compile_project` and the two
  `vh_check_project` entry points answer `VH_ERR_STOPPED` — publishing a generation underneath a
  frame of the retiring one is incoherent, and an analysis resets the semantic program that frame
  is about to resume into — and `vh_tick` is refused silently, because resuming a slept task
  inside a VM stopped mid-op is not something any of this is designed for and `flush_output` can
  reach `_frame` from that loop.
- **Every Godot callback goes through `AutoRTFM::Open`,** and writes defer to `AutoRTFM::OnCommit`.
  The GDExtension was never instrumented by the AutoRTFM compiler, so calling into it from closed
  Verse code is a fatal "could not find function" at runtime, not a link error.
- **Calling *into* the VM must be open too.** `vh_instance_call` invokes through
  `VFunction::Invoke` inside an `AutoRTFM::Open` nested in its transaction, which is what
  `TVerseFunction::operator()` does for the same reason: a Verse runtime error raised from closed
  code trips `AutoRTFM::UnreachableIfClosed` in `FContext::RaiseVerseRuntimeError` and takes the
  process down instead of unwinding.
- **A raise terminates the *active* content scope, and `EnterVM` then declines to run anything in
  it** — silently, which is why this was invisible for a phase. Since Phase 5 that scope is the
  raising **instance's** (R-ASYNC-4), so the blast radius is that node's suspended work: another
  instance's next call runs, and so does the raising instance's, because a terminated scope is
  *replaced* at the next call rather than un-terminated at the next tick. Every entry into the VM
  goes through `EnterVerse` or `EnterVerseOn`, which is where the scope handling lives — never call
  `Context.EnterVM` directly. `VH_ERR_HALTED` is what an execution entry point answers when the body
  genuinely did not run, which is now rare.
- **Awaiting is the one thing that cannot be narrowed, and `spawn` is how a script starts it.**
  `event.Await` carries `no_rollback`, so a `<suspends><transacts>` body that awaits is glitch 3512
  and a `<reads>` body may not `spawn` at all. The caller must be specifier-less too: a mirrored
  virtual override is, and so is a `signal.Subscribe` handler. `_Ready<override>()<suspends>`
  is *not* a spelling — glitch 3532 plus 3523, because the specifier makes it a different function.
- **`defer` in a suspending body runs on cancellation as well as on return** (measured,
  `tests/verse_probe/sleep_probe.verse`). `signal.Await`'s Godot connection is taken away in
  one, which is the only reason a `race` whose loser never resumed leaves nothing behind.
- **`operator'()'` is a reserved intrinsic.** Verse rewrites `Data[Key]` on a non-function callee
  into a call to it, but refuses to let anything *define* one — as a class member or as a free
  function — so the bracket syntax cannot be given a meaning. Container lookup is
  `Data.GetInt[Key]`.
- **A `var` property cannot hold a nested struct or a container.** Verse asks a struct-typed `var`
  for a field-named accessor overload per nesting level, and `transform3d`'s two members have
  different types, so no one getter signature satisfies it. `gen_verse_api.py` leaves those as
  ordinary getter and setter methods; only the flat math types keep `set Node.Position = ...`.
- **Every top-level name must be unique within its module.** A directory is a module only if it
  carries a `<name>.vmodule` marker, and the **marker names the module**, not the directory —
  `res://my-stuff/gameplay.vmodule` is module `gameplay`. Unmarked directories are organisational:
  their files join the nearest marked ancestor, so a project with no markers has every file in the
  root module and nothing on disk changes meaning. Root is implicit: a module reads the root
  module's definitions with nothing written. A file may declare any number of top-level names, and
  only the class **named after the file** can go on a node — which is a bridge rule now rather than
  a Verse one. A file with no class at all is a library file (R-LANG-6). Every `ClassNameUtf8` in
  the ABI is module-qualified: `player` at the root, `gameplay/player` in a module.
  `src/verse_module_map.{h,cpp}` is the whole rule, and it is a unit test away from Godot.
- **The attribute package must be added before the first `AddDataSource`.** `@global_class` is
  declared in a source package the host adds at runtime, not in `host/Verse` — VNI compiles that
  at build time and rejects `class(attribute)`. `FSolarisIde::EnsureDataSourcePackageExists`
  snapshots the project's other packages as the script package's dependencies exactly once, so a
  package added after the first script is never depended on and the attribute stops resolving.
  Authorship comes from a `IPreSemAnalysisInjection`, which must stay registered for the life of
  the process: `CProgramBuildManager::Build` resets the semantic program on every compile *and*
  every analysis, so a one-shot grant is gone by the first build.
- **An explicit effect specifier narrows, and narrowing is contagious downward.** A function with no
  specifier carries the *default* set, which is wider than `<transacts>` — it contains
  `no_rollback`. So a `<transacts>` function may not call a specifier-less one, and anything that
  forces `<transacts>` on a method (a `Subscribe` handler; a failure context) forces it on
  everything that method calls, a file at a time. The compiler reports it at the **call** site, not
  at the declaration that needs changing — `ErrSemantic_EffectNotAllowed`, uLang glitch **3512**.
  The bridge used to append the fix to it and no longer does: the appender keyed on the code and the
  callee's package and never on which effect was refused, so a `suspends` refusal took the
  `transacts` branch (`docs/by-hand-findings.md` B7). This is `dodge-the-creeps.md` wall 8;
  Phase 4.5 narrowed it and did not remove it. **Reading Godot no
  longer starts the cascade** — a const-and-answering method is `<reads>` — but a helper that
  writes still needs the word, and effects are *contravariant*, so a `<reads>` callee satisfies a
  `<transacts>` caller and a `<transacts>` function type alike.
- **An archetype instantiation carries the constructing class's own effect.** `variant{Tag := ...}`
  inside a `<reads>` or `<computes>` function is *"This archetype instantiation constructs a class
  that has the 'transacts' effect"* unless the class says `<computes>`. `variant`, `godot_ref` and
  every container wrapper do; a **mirrored Godot class cannot**, because it descends from the native
  `vh_object`, so anything a narrowed body needs must be reached by a *cast* over what the host
  built rather than by construction — which is what the singleton accessors do now, and what
  R-SCN-6 says they should always have done.
- **A class member may not shadow an inherited mirrored one, and Godot's signals are members too.**
  `Hidden:signal(int)` on a `node2d` is *"Instance data member `Hidden` is already defined in
  `canvas_item`, did you mean to add the `<override>` specifier?"* — because `canvas_item` mirrors
  Godot's `hidden` signal as a `Hidden` accessor. Every one of the 489 signal accessors and 3232
  properties is a name a script cannot reuse, and the compiler reports it at the *declaration* with
  no hint that the collision is with generated code. Rename the member; there is nothing to override.
- **A module-level name and a local of that name are ambiguous, not shadowing** — and an *extension
  method* is a module-level name. `(V:vector2).Length()` makes `Length` unusable as a parameter name
  in any file that imports the Godot package, which is every script. See the Phase 4 note above for
  the list.
- **A bare `logic` in an `if` clause list is evaluated and thrown away.** `if (X)` alone is refused
  — *"Expected an expression that can fail in the 'if' condition clause"* — but as soon as *some*
  clause can fail, a `logic`-valued one beside it is accepted and **not tested**, so the body runs
  either way with no diagnostic. `if (Button := input_event_mouse_button[Event], Button.IsPressed())`
  runs on the release as well as the press; `Button.IsPressed()?` is the spelling that tests it.
  Measured in `tests/verse_probe` (`docs/by-hand-findings.md` B12) after it produced a passing test
  that was counting twice.
- **Verse silently drops a continuation line that begins with an operator.** An expression written
  as `0.5 * ((A * 2.0)` then `+ B * W` on the next line compiles, runs, and answers *the first line
  only* — no diagnostic, no warning. Two of `GodotMath.native.verse`'s formulas answered 0.0 that
  way. Keep arithmetic on one line or bind a term at a time.
- **Verse's float `=` is reflexive for NaN**, unlike IEEE and unlike C: both `X = X` and `X <> X`
  answer "equal", so the usual NaN test never fires. What does distinguish NaN is that it is
  *unordered* — it fails `<=` and `>=` against everything, itself included.
- **Float division is total** and answers `Inf`/`-Inf`/`NaN` exactly as C does; **integer division
  is `Quotient`, which floors**, where C and Godot truncate toward zero — so `Quotient[-3, 2]` is -2
  where Godot's `-3 / 2` is -1. `GodotMath`'s `TruncatedQuotient` is the bridge.
- **There is no `ToFloat`.** `X * 1.0` is the int-to-float conversion, and it works on a value and
  not only on a literal. `Floor`, `Ceil` and `Round` are `<decides>` and answer an `int`, so a
  float-valued floor is `if (V := Floor[X]) then V * 1.0 else X` — which is also the shape that
  makes `floor(inf)` agree with Godot.
- **A host build passing is not enough to know a `.verse` file compiles.** VNI compiles `host/Verse`
  at build time against one package set, and the *runtime* compiler re-reads those same files
  against another — a bare `Pi` passes the first and is an unknown identifier in the second. Run
  `tests/verse_probe` after touching anything in `host/Verse`.
- **Verse rejects mixed tabs and spaces.** Godot's script editor writes tabs; `.vscode/settings.json`
  matches that. Keep `.verse` files tab-indented.

## Conventions

- C++20 on both sides (the `SConstruct` swaps godot-cpp's `c++17` out).
- godot-cpp style in `src/`: `p_` parameters, `r_` out-params, `_`-prefixed Godot virtuals, tabs.
- UE style in `host/`: `FName`, `bFlag`, Epic copyright header, tabs.
- Comments in this repo explain *why* and are dense where the reasoning is non-obvious — match the
  neighbouring file rather than the average.
