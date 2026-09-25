# Phase 7.5 design: a Verse VM for the web

This phase builds an interpreter that runs Verse in a Godot web export. It answers R-PLAT-3 and
OQ-4 by construction rather than by argument. `verse-on-web.md` is the scoping record this starts
from: it closes running the UE host on wasm and extracting VerseVM, and leaves one path open, a
second execution path. This design takes that path, with one change the scoping record did not
assume: the interpreter runs **Epic's own VerseVM bytecode**, not an IR of ours.

Written 2026-09-24, before any of the work. Like every phase document, it ends with a section
written after the work (§14), which is where to look for what this body got wrong.

## 1. Requirements

These were settled in an interview with the project owner, and each is a decision rather than a
default.

| # | Requirement |
| --- | --- |
| W-1 | The cooker emits VerseVM bytecode, compiled by Epic's own backend, serialized by us into our own container. The interpreter executes that bytecode. |
| W-2 | The interpreter builds natively (Windows, into the GDExtension) and as wasm32. A project setting chooses it over the UE runtime host on desktop, so the existing export layer is its conformance suite. |
| W-3 | **Exit:** `dodge-the-creeps` plays in headless Chrome, and `tests/integration` passes on the interpreter in both the Windows export and the web export, every skip counted and justified. |
| W-4 | The web build is **nothreads** (`web_dlink_nothreads_*`), so a host needs no COOP/COEP headers. Proving that a GDExtension loads there without them is the first spike. |
| W-5 | Chrome is the only automated browser. Firefox and Safari are by-hand checks, recorded and not gating. |
| W-6 | No performance bar. Everything is measured with `host_bench`-style numbers beside the UE host's and recorded; nothing is gated on them. |
| W-7 | **Clean room** (§3). The interpreter is written without its authors reading VerseVM's source. |

Consequences of W-1 that are accepted rather than solved:

- **A web export ships Epic compiler output**, including the compiled `/Verse.org` library code the
  program reaches. `verse-on-web.md` §5.5's "no UE-derived bytes" benefit is therefore not claimed.
  The clean room (W-7) is about the interpreter's source, not the program it runs.
- **The bytecode is pinned to the engine commit** (`203d764`). A UE bump now includes re-checking the
  op set against the spec (§6.4), the way it already includes regenerating the mirror.

## 2. Shape

```
                         author's machine (UE checkout)                     player's machine
  .verse ──► verse_cook.exe ───────────────────────────────►  verse_data/  ──►  godot_verse (native or wasm)
             │ uLang → IR → Epic's bytecode backend              program.vbc        │
             │ (unchanged: CompileProject)                       verse_classes.json │ vm/ (clean room)
             └─ NEW: VbcWriter walks the linked program ──────►                     │   loader, heap, GC,
                (dirty room, host/Private/HostVbc*.cpp)                             │   interpreter, tasks,
                                                                                    │   stdlib natives,
                                                                                    │   vh_* and the 46
                                                                                    │   Godot natives
                                                                                    └─ src/: file bytes and
                                                                                        backend selection
```

Four pieces, three of them new:

1. **The cooker's writer** (new, dirty room). After `CompileProject` returns (`CookMain.cpp:301`), the
   whole program is linked in memory. `HostVbcWriter` walks every package reachable from the
   project, re-encodes every procedure op by op, and writes `program.vbc` beside the sidecar. It does
   not replace the IoStore cook: the UE runtime host keeps working, and a cook produces both.
2. **The sidecar** (existing, version 8, unchanged unless a reader proves it must change). It already
   carries everything the class-describing reads need, and the declared types the VM erases.
3. **`vm/`** (new, clean room). A godot-cpp-free C++20 library, in the tradition of the lexer and the
   module map: it loads a `.vbc`, owns a heap and a precise collector, interprets, runs tasks, and
   implements the Verse-library natives. It also implements the runtime subset of the `vh_*` ABI
   and the 46 natives of `Godot.native.verse`, because both speak only plain C — the ABI header and
   the `vh_godot_api` callbacks — and need nothing from godot-cpp. It builds two ways:
   - **`bin/verse_vm.dll`**, exporting the `vh_*` functions. It is a drop-in for
     `verse_host_runtime.dll`, so `tests/cooked_probe` runs against either **unchanged**, and the
     differential harness (§10.2) is one probe binary pointed at two DLLs.
   - **statically**, into the GDExtension, for the vm backend and for web.
4. **`src/`** (small changes, clean room). `VerseHostLibrary` gets a second way to fill its function
   pointers: assigned directly from `vm/`'s functions instead of resolved with `GetProcAddress`. The
   one thing `vm/` cannot do for itself is read a file out of a `.pck`, so `src/` hands it a reader
   over Godot's `FileAccess` before `vh_init`. Nothing else in `src/` changes for execution.

### 2.1 Why the seam is where it is

`src/`'s only contact with the host is `VerseHostLibrary`'s function pointers, typed by the ABI
header's `_fn` typedefs. A statically linked implementation of the same C functions slots in without
touching `verse_script_instance.cpp`, `verse_value.cpp`, `verse_callable.cpp` or anything else that
marshals values. That buys the whole consumer half — `Variant` ⇄ `vh_value`, the reference table,
script instances, placeholders, signals on the Godot side — for free, and it means a defect is either
in `vm/` or in something the UE host shares, which is a short list.

The runtime subset is exactly what a `WITH_VERSE_COMPILER=0` host answers today: the 21 execution
and class-describing entry points. The 12 compiler entry points answer `VH_ERR_UNSUPPORTED`, as
they already do in a runtime host.

### 2.2 What does not change

The editor, analysis, completion, hover, the debugger, the profiler, and every compile-time
behaviour stay on the UE host. The interpreter is an **export runtime**. In a Windows editor session
nothing loads it; an export chooses it (§9).

## 3. The clean room

The wall, as chosen:

| Room | Who | May read | Writes |
| --- | --- | --- | --- |
| Dirty | cooker-writer agents, spec agents | anything, VerseVM's `.cpp` files included | `host/Private/HostVbc*`, `docs/web-vm/spec/`, `docs/web-vm/ops.json` |
| Clean | interpreter agents | this document, `docs/web-vm/` (only the **reviewed** files of its `spec/`), `CLAUDE.md`'s Commands, Tests, Instruments, Generated files and Conventions sections only, this repo's `src/`, `vm/`, `include/`, `tests/`, `tools/` except `gen_vbc_writer.py`, `host/Verse/*.verse`, `host/Private/GodotMathLayout.gen.h` | `vm/`, `src/verse_vm_*`, `tests/vm_*`, `tools/*vm*`, `tools/*vbc*` |
| Lead | me | everything a clean agent may, plus `host/` (our code); **no VerseVM source** | this document, the task list, reviews |

Rules that make the wall real:

- **The spec is prose, tables and interoperability facts.** Op names, operand lists, encodings,
  value kinds and observable behaviour are in. Code, pseudo-code, quoted comments and descriptions of
  how Epic's interpreter is organized are out. Every spec section is reviewed by the lead for copied
  expression **before** a clean agent may read it, and the review is recorded in the section's
  header.
- **`host/Private/` is out of bounds for the clean room**, although it is our code: it is written
  against VerseVM's API, and reading it teaches VerseVM's structure. What a clean agent needs from it
  (the natives' contracts, the sidecar's fields) is restated in the spec.
- **Every spawn prompt states the room** and the reading rule. A clean agent that finds it needs a
  fact the spec lacks asks for a spec addition; it does not go looking.
- **`docs/web-vm/cleanroom-log.md`** records, per task, which room did it and what was read. It is the
  evidence, and it is appended to rather than rewritten.

The differential harness (§10.2) is how the clean room learns behaviour the spec got wrong: it
observes the UE runtime host's output, which is behaviour rather than source.

## 4. What the dirty-room survey established

A dirty-room survey (2026-09-24) mapped the bytecode in prose. Its findings that shape this design,
restated as requirements on the interpreter:

- **114 opcodes; about 90 matter.** Ten are inline-cache forms the compiler never emits and the
  cooker never serializes. Four more are never emitted at this commit. A handful (persistence,
  `LoadImport`) are unreachable from a Godot script. Live-variable `await` and `batch` are not: they
  compile in a script package (`spec/tasks.md` §5.7), so they are specified and implemented, if late.
- **It is a unification-based, lenient dataflow VM.** Results are *unified* into destination
  registers, and an op meeting an unbound logic variable parks rather than blocks. This, not
  concurrency, is the hardest part (§7.1).
- **Rollback is not in the bytecode.** Epic's VM undoes heap writes through AutoRTFM. The interpreter
  keeps an explicit undo log (§7.2).
- **Frames are heap-allocated, and a Verse-to-Verse call never recurses on the native stack.** That
  is what makes a nothreads wasm build straightforward: a suspended task is data.
- **The op stream in memory is 64-bit C++ structs with embedded pointers**, so the writer re-encodes
  every op; nothing is copied as bytes.
- **Most of the program is data built at compile time**: classes, archetypes and closures are cells,
  reached from constant pools, forming a cyclic graph. Cross-package references are already resolved
  to cells. The loader resolves a graph, not names.
- **Initialization has already run by the time the writer sees the program.** Compiling runs every
  package procedure and the global initializer, so the `.vbc` is a snapshot of the initialized
  program and a loader runs no Verse at all. This body first said the writer must capture the
  initializers for a loader to run; `spec/modules.md` §2 is why that would fail.
- **The Verse-library native surface a game reaches is about 70 natives** plus 10 VM intrinsics, on
  top of the 46 Godot natives — not the 25 the survey estimated, because a script package is
  `InternalUser` and so reaches `epic_internal` natives too (`spec/natives.md` §2). About 50 more are
  nameable and out of contract; they bind to a stand-in that raises.

## 5. The container: `program.vbc`

Our format, owned by this phase and specified in `docs/web-vm/format.md` — written by the lead, since
it is ours, from the op schema in `ops.json`. Its properties:

- **One cell table per program**, every cell addressed by index, forward references allowed, so the
  cyclic graph serializes without ordering tricks.
- **Procedures are re-encoded**: opcode, then each operand in the schema's order, with registers,
  constant indices, operand-pool ranges and labels as integers, and every immediate as a cell index.
  Labels become op indices within the procedure rather than self-relative byte offsets.
- **Source locations carried** (op index → line, plus the file path once per procedure), because a
  runtime error names a line.
- **Natives by binding key**, the package-definition key, because a name alone is ambiguous
  (`event.Await` and `task.Await`); the loader binds them.
- **Packages as named roots**, each with its definitions table. No initializer is carried, because
  none runs at load (§4).
- **Little-endian, varint-heavy, versioned**, with the build stamp the sidecar already carries, so a
  stale `.vbc` is refused with a sentence, not a crash.
- **A dumper** (`tools/vbc_dump.py`) prints any `.vbc` as text. It is how a cook is reviewed and how
  the writer and the loader are held to one format.

`ops.json` is the machine-readable op schema — name, number, operand roles and kinds — produced by the
dirty room from the op definitions. Both the writer's encoder and the interpreter's decoder are
generated from it (`tools/gen_vbc_ops.py`), which is the one way the two rooms share a fact without
sharing code.

## 6. The spec: `docs/web-vm/spec/`

Written by dirty-room agents, reviewed by the lead, read by the clean room. One file per area, each
headed with its review status:

| File | Covers |
| --- | --- |
| `values.md` | value kinds, ints (bignum, rational), floats (NaN, -0, printing), chars and strings, arrays, maps, options, tuples, equality's four answers |
| `unification.md` | placeholders, unify-into-destination, parking and re-execution, the effect token, lenient completion |
| `failure.md` | full and fast failure contexts, the trail, what is undone, runtime errors and their rollback |
| `calls.md` | procedures, registers 0 and 1, argument adaptation, named parameters and defaults, closures, scopes, `(super:)` |
| `objects.md` | classes, archetypes, layout and the override rule, construction protocol, fields, interfaces, the native-bound and native-representation flags |
| `tasks.md` | tasks, the task ops, `spawn`/`branch`/`sync`/`race`/`rush`, semaphores, cancellation, unwind edges and `defer`, terminate versus cancel |
| `modules.md` | packages, module ops, the package procedure, the global initializer, the sentinel |
| `natives.md` | the native calling convention's five outcomes, every `$BuiltIn` intrinsic, every Verse-library native a game reaches, `event(t)`, `task(t)`. `Sleep` is a Godot native and is `godot-natives.md`'s |
| `godot-natives.md` | the 46 natives of `Godot.native.verse` and what each asks of `vh_godot_api`, the `variant` lanes, which writes defer to commit — restated from our own host so the clean room need not read `host/Private` |
| `sidecar.md` | every field of `verse_classes.json` version 8 and which `vh_class_*` read answers from it. Our format, so the lead writes it from `HostSidecar.cpp`; no dirty agent is involved |
| `ops.md` | every op: operands, semantics, failure and parking behaviour, which spec section it leans on |

### 6.1 Three facts to measure before they are written

The survey flagged three disagreements between Epic's source and this repo's measurements. Each is
settled with `tests/verse_probe` or `tests/cooked_probe` before its spec line is written:

1. NaN against NaN under `<=` and `>=` — the source says true, `CLAUDE.md` says it fails.
2. Whether integer `Mod[]` floors or truncates.
3. Whether interpolated `ToString(float)` prints `1.000000` or the shortest round-trip form.

### 6.2 Spec by behaviour, where it can be

A spec line that can be checked by running something cites the probe that checks it. The spec is
where the clean room learns *what*; the probe is where anyone can confirm it.

### 6.3 Leniency is specified in full and implemented in stages

`unification.md` describes parking completely. §7.1 decides how much of it the interpreter builds
first.

### 6.4 Re-checking on an engine bump

`ops.json` is regenerated from the op definitions at the new commit and diffed. A changed op is a
spec change and a clean-room task; an unchanged diff means the bump costs this phase nothing.

## 7. The interpreter

`vm/` is organized by the spec's sections, not by Epic's. Its decisions:

### 7.1 Unification and leniency, staged

Placeholders, union-find binding and unify-into-destination are built in full from the start,
because the compiler binds ordinary definitions through them. **Parking** — an op meeting an unbound
placeholder at run time — is built in stages:

1. **Stage 1:** a runtime park is a fatal runtime error naming the op and line, and a counter. The
   writer also reports, per procedure, which ops *could* park, so the scale is known before it is
   paid for.
2. **Stage 2**, only if the suite or the yardstick parks: parking and re-execution within a failure
   context, then lenient completion.

Whether stage 2 is needed is a measurement, and the task list has a task that takes it.

### 7.2 Transactions: an undo log

Every mutation — var, field, mutable array element and append (`ArrayAdd` only when its
`bTransactional` flag says so), `FastAppendToArray`, `InPlaceMakeImmutable`, map insert with its
count — writes an undo record while a transaction is open, and so does every trailed register write
(`Reset`, `MoveTrailed`, `ReturnTrailed`, `EndTask`'s first-wins writes, a task's resume slot) and
every placeholder link (`spec/failure.md` §6). Task state is not transactional except
for a `spawn` inside a failure context (`spec/tasks.md` §12); joining or leaving a task group and
termination are never undone. A failure context opens a nested
log; success merges it into its parent; failure replays it backwards. The trail is the same log.
A runtime error unwinds to the outermost VM entry. Godot-side effects keep the protocol the UE host
already has: writes defer to the **root** commit at the end of the VM entry, not to each failure
context's commit, and the two immediate exceptions (`VhSignalEmit`, `VhRefSet`) stay immediate.

### 7.3 Values and heap

Our own value representation, chosen for wasm32: a tagged 64-bit word with small ints, doubles,
chars and 32-bit cell pointers inline, and cells for everything else. Bignums are our own (§11 lists
it as a risk). A **precise** mark-sweep collector, which is possible because we own every root:
package roots, live tasks and their frames, instance and callback handles held by the host, and an
explicit root stack for natives mid-call. Collection runs from `vh_tick` and `vh_collect_garbage`,
never mid-op. Releasing a `godot_ref` or a minted `vh_object` peer on sweep follows the rule the UE
host follows: only what was recorded as minted.

### 7.4 Dispatch

A `switch` loop over decoded ops. No tail-call threading, no computed goto on wasm. It is the
portable choice and W-6 sets no bar that argues otherwise.

### 7.5 Tasks without threads

A task is a heap object holding its frame chain and resume point, so a suspended task is data and
needs no native stack. There is **no scheduler queue**, though: whoever makes a task runnable — an
`EndTask`, an `event(t)` `Signal`, a synchronous `Cancel`, a write that wakes an `await`, a native
completing a call — runs it on its own native stack until it stops, and resumptions happen in the
fixed order `spec/tasks.md` §4.3 gives. An interpreter that queued them instead would reorder every
table in that spec. So the native stack deepens with *resumption* nesting, not with Verse call depth.
`Sleep` resumes from `vh_tick` on a monotonic clock, as the UE host's does.

This body first said suspension was "returning to the scheduler loop"; `spec/tasks.md` §4 measured
otherwise.

## 8. The ABI half, in `vm/`

- The runtime entry points over the interpreter, with `vh_init` taking the cooked directory as today.
- The class-describing reads served from the sidecar, reimplemented against the JSON (the UE host's
  reader is in `host/Private` and out of the clean room's bounds; `sidecar.md` is what the clean
  room reads instead).
- The 46 Godot natives over `vh_godot_api`. `VariantFromWire`/`VariantToWire`'s lane rules are
  restated in the spec from `GodotMathLayout.gen.h`, which is generated and readable by both rooms.
- Files are read through a reader the embedder may set before `vh_init`; the default uses the C
  library. The DLL build uses the default. The GDExtension sets one over `FileAccess`, so
  `verse_data` reads the same from a directory beside an executable and from inside a `.pck`.
- `verse_host.cpp` gains a static path: when the build carries the VM and the backend is `vm`,
  `VerseHostLibrary` is filled from `vm/`'s functions instead of a DLL.

## 9. Choosing the backend

- **Build:** `scons ... verse_vm=yes` compiles `vm/` into the library. Web implies it; the loader path
  is compiled out on web, where `LoadLibraryExW` does not exist.
- **Run:** a project setting, `verse/runtime/backend` = `host` (default) or `vm`, read by
  `VerseRuntime` in an exported game. An editor session always uses the host.
- **Export:** the export plugin cooks exactly as today and ships `program.vbc` in `verse_data`. With
  `vm`, it does not ship `verse_host_runtime.dll` or `tbbmalloc.dll`. On **Web** the setting defaults
  to `vm` through a `verse/runtime/backend.web` feature override that `VerseRuntime` registers, the
  way Godot defaults `rendering/renderer/rendering_method.web` to Compatibility; a Web export whose
  preset resolves it to `host` is refused, and so is a Web run. `web` leaves
  `UNREACHABLE_PLATFORMS`. The tag is `web` rather than `wasm32` because the constraint is the
  platform, not the architecture, and it is Godot's own convention. (This body first had the
  plugin refuse a Web export under the plain default, which made every author set a value that can
  only be one thing; a later commit ignored the setting on Web outright, which hid a choice the
  author had made.) On web, `verse_data` goes into the `.pck`
  rather than beside an executable, which is the one new path rule `verse_export_paths` gains.

## 10. Testing

### 10.1 Layers

| Layer | New or changed |
| --- | --- |
| units | `vm/`'s own unit tests: bignum, float printing, maps, equality, the undo log. godot-cpp-free, like the others. |
| abi | `vbc` case: cook `tests/host_smoke`, assert `program.vbc` loads, dumps and names every native. |
| **vm** (new) | the differential harness, §10.2. |
| export | runs twice: host backend (511 passed, 0 failed, 11 skipped, as today) and vm backend, asserting its own named counts. |
| **web** (new) | exports `tests/integration` for Web, serves it on localhost, runs it in headless Chrome with its own `--user-data-dir`, reads the summary line from the console log, asserts named counts. Skips with the reason when Chrome or the template is absent. |

`dodge-the-creeps` stays out of `run_tests.py`, as it is today. Its web run is a by-hand yardstick
with a script that performs it (`tools/run_dtc_web.py`), and the exit bar is that script passing.

### 10.2 The differential harness

`tests/vm_conformance/` holds small `.verse` fixtures, one behaviour each, grouped by spec section.
`tools/run_vm_conformance.py` cooks them once, runs every fixture's zero-argument methods on the UE
runtime host and on the interpreter — `tests/cooked_probe` pointed first at
`verse_host_runtime.dll` and then at `verse_vm.dll` — and diffs the printed transcripts. **A difference is a defect in one of them, and
the UE host is the reference.** This is the loop the clean room works in: it observes behaviour
without reading source, and it turns every surprise into a fixture.

### 10.3 Counting

Every layer prints one line per case and exits non-zero on failure, as the repo already requires.
A case the interpreter cannot run yet is a **skip with a reason**, counted, never a missing line.

## 11. Risks, largest first

1. **Leniency is needed early.** If ordinary scripts park at run time, stage 2 of §7.1 moves to the
   front and the schedule moves with it. The writer's per-procedure report measures this in M2.
2. **The nothreads web template cannot load a GDExtension without COOP/COEP.** Then W-4 is wrong and
   the fallback is the threads template with headers, which changes what the exit bar can say. M0
   measures this first because everything else is wasted if it fails.
3. **The Emscripten version.** Godot 4.7-stable's templates were built with Emscripten 4.0.11 and
   this machine has 6.0.0; a side module must match its main module closely enough to link. M0 pins
   it.
4. **Spec accuracy under the wall.** A wrong spec line costs a clean agent a day. The differential
   harness is the mitigation, and the spec cites probes wherever it can.
5. **The mirror's size in the container.** 57,000 generated lines of Verse becomes a lot of bytecode.
   `--classes-file` is the mitigation; the size is measured in M2.
6. **Bignums and float printing** are small, easy to get subtly wrong, and user-visible in every
   `Print`.

## 12. Milestones

Each has an exit that a command checks. The task list, `docs/web-vm/tasks.md`, breaks them down.

| | Milestone | Exit |
| --- | --- | --- |
| M0 | Web toolchain proven | A GDExtension built with the pinned Emscripten prints from `_ready` in a nothreads web export in headless Chrome, with no COOP/COEP headers. |
| M1 | Spec and format | `ops.json` generated; every spec file written and reviewed; `format.md` written; the three facts of §6.1 measured. |
| M2 | Cooker writes `.vbc` | The cooker writes `program.vbc` for `host_smoke`, `tests/integration` and `dodge-the-creeps`; `vbc_dump` reads all three; the park-risk report and the size are recorded. |
| M3 | Sequential VM | `cooked_probe` on `verse_vm.dll` prints the same transcript as on the UE runtime host for every non-concurrent conformance fixture. |
| M4 | Concurrent VM | Tasks, `race`/`sync`/`rush`/`branch`/`spawn`, `defer`, events and `Sleep` fixtures agree. |
| M5 | Windows export on the VM | The export layer passes on the vm backend with named counts. |
| M6 | Web | The web layer passes with named counts, and `tools/run_dtc_web.py` passes. |
| M7 | Close | §14 written; `spec.md` R-PLAT-3 and OQ-4 updated; `roadmap.md` Phase 7.5 marked; `CLAUDE.md` gains the map entries. |

M3 and M1's later spec sections overlap: the clean room starts on `values.md` and `calls.md` while
`tasks.md` is still being written.

## 13. Out of scope, by decision

- The debugger and profiler on the interpreter. An exported game has neither today.
- Firefox and Safari automation.
- Performance work beyond measuring it.
- The threads web template, unless M0 forces it.
- Mobile. The interpreter is platform-independent and should reach Android and iOS, but proving it
  is another phase.

## 14. After the work

Written 2026-09-25, when every milestone's exit was met. The body above has been corrected in place
where it was wrong, each correction saying so; this section is the record of what changed and what
the numbers came to.

### 14.1 What the exit bar measured

| Check | Result |
| --- | --- |
| `tests/vm_conformance` against the UE runtime host | 190 of 190 methods byte-identical, 17 classes, concurrency included; the same with a collection forced after every entry |
| `tests/integration` exported for Windows on the vm backend | 519 passed, 0 failed, 11 skipped — the host backend's own counts |
| `tests/integration` exported for Web, in headless Chrome | 517 passed, 0 failed, 13 skipped — the two extra skips are R-ASYNC-8's thread cases, which a build without threads cannot pose |
| `dodge-the-creeps` exported for Web, in headless Chrome | 30 of 30 checks (`tools/run_dtc_web.py`) |
| `vm/` unit cases | all pass (`verse_vm_test`) |

No fixture, export case or yardstick check ever parked at run time, so §7.1's stage 2 was never
built (T3.10 dropped with that evidence).

### 14.2 Where this body was wrong

- **A loader runs no Verse** (§4, §5). The cooker sees the program *after* initialization, so the
  `.vbc` is a snapshot; re-running initialization would fail. `spec/modules.md` §2 found it before
  any loader was written.
- **There is no scheduler loop** (§7.5). Whoever makes a task runnable runs it on its own native
  stack, in a fixed order; queueing would have reordered every measured table.
- **The native surface is about 70 Verse-library natives, not 25** (§4), because a script package
  reaches `epic_internal` natives; and a native binds by its package-definition key, not its name,
  since `event.Await` and `task.Await` share one. The Godot surface is 46 natives, not 39.
- **The ABI layer belongs in `vm/`** (§2, §8). It speaks only plain C, so `vm/` builds as
  `verse_vm.dll`, a drop-in for the runtime host, and the unchanged `tests/cooked_probe` became the
  differential harness. The planned `vm_probe` was never needed.
- **Live-variable `await`, `batch` and `set live` are reachable** from a script package (§4), and
  element, map-value and field reads register for `await` (`spec/ops.md` §15.1).
- **The collector cannot re-mark the program** (§7.3). A stop-the-world pass over the loaded mirror
  cost ~11 ms per collection unoptimized; the loaded program is now a permanent generation (T3.11),
  and an idle collection costs 0.02 ms.
- **The web boot "hang" was a missing command line** (§10). A Web export's page passes the engine
  no arguments, so the test driver's gate never opened; `run_web.py --godot-arg` supplies them.

### 14.3 What only a browser showed

- A build without threads runs a `WorkerThreadPool` task on the calling thread, so R-ASYNC-8's
  off-thread refusal cannot be posed there; `test_cases.gd` skips those two cases on such builds.
- Chrome renders, so a `VisibleOnScreenNotifier2D` fires for real, where Godot's `--headless`
  never computes visibility. `dodge-the-creeps`' checks were written for headless, so the web
  runner passes `--disable-render-loop` to restore those conditions.
- Godot offers an export plugin no way to withhold a `.gdextension`'s `[dependencies]`, so on the
  vm backend the plugin rewrites that file for the length of an export and restores it after.
  An export that dies in between leaves the section out until the next build regenerates it.
- The Emscripten risk (§11 risk 3) did not bite: a 4.0.11 side module links into the 4.7.2
  template's 4.0.20 main module.

### 14.4 Numbers

| Measure | Value |
| --- | --- |
| `program.vbc`, each of three projects | ~11 MB, almost all standard library and mirror (`web-vm/measurements.md`) |
| web library with the VM | 5.0 MB (`godot-verse.nothreads.wasm`) |
| Windows release library, the VM's share | ~565 KiB |
| `vh_init` on a cook | ~250 ms native, against the UE runtime host's ~420 ms; ~95 ms on web |
| a collection, idle, `/O2` | 0.01 ms; ~1 ms after 65,536 dead cells |
| `dodge-the-creeps` on web | 1.5× slower than real time at a fixed 60 fps — a correctness pass, not yet a playable one |

### 14.5 Defects this phase found in the existing UE path

All fixed on this branch, and all present on `master` before it: **B42**, where every scripted node
shared one Godot container per container member default, and held a dead reference in an export;
every method of a `task(t)` value crashing the runtime host; runtime-error frames that began with a
bogus frame in the editor and were missing in an export; and an uninitialised tag written into the
sidecar for every static value.

### 14.6 The clean room, as it actually held

`web-vm/cleanroom-log.md` is the record. Three gaps are in it, each logged when found. The clean
room was first allowed all of `docs/` and `CLAUDE.md`, which describe the UE host in Epic's terms;
the list was narrowed. Claude Code loads `CLAUDE.md` into every agent regardless, which no reading
list can prevent; the project owner decided to accept and log it. And one dirty report named Epic's
task-binding internals to the lead. None of it concerns how Epic's interpreter executes bytecode,
and no clean task's output shows it used. A web export ships Epic compiler output (§1); the
interpreter's source is what the wall was for.

### 14.7 Left open

- **Speed.** Nothing was optimized (W-6). The browser runs `dodge-the-creeps` at two-thirds of real
  time; making it playable is measurement and work this phase did not do.
- **Firefox and Safari** were never run, automated or by hand (W-5 made them by-hand checks).
- **Not implemented:** `vh_run_main` on the interpreter; `typed_array` and `typed_dictionary`
  *parameters*, which the sidecar cannot tell from `godot_array`; `classifiable_subset`'s
  `GetDiagnostic` text, which is a placeholder.
- **Wording that is ours**, where the reference had none: the unbound-native stand-in, the
  interpreter's refusal sentences, the `VH_ERR_THREAD` sentence and the signal-reject sentences.
