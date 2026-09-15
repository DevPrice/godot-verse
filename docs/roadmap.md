# godot-verse — Roadmap

**Status:** Draft 16 · 2026-09-14 · **Phases 0–6 complete; Phase 7a built; 7b designed, not built.**
[`phase-7-design.md`](phase-7-design.md) is 7a's plan, written before the work from an interview
and three delegated reads of the engine sources: §1 is the decisions, §2 the spikes (S-1, OQ-10,
ran during the planning), §4–§10 the stages in build order, and **§13 is where it was corrected —
§13.7 is the wall that split the phase**. [`phase-7b-design.md`](phase-7b-design.md) is the other
half, and it is what a fresh agent picks up: §1 the decisions, §2 two spikes that run *before* any
stage is written, §3 the engine facts re-verified, §13 the section the implementing agent writes. Phase 6's design is [`phase-6-design.md`](phase-6-design.md), written before the work in the shape
Phase 4.5's and Phase 5's were, so **§13 is the part to read** — the six spikes' answers, and the
places §1 and §9 turned out wrong. The by-hand checks three phases owed
have been run, in one windowed session as each design asked — Phase 3's yardstick run and editor
session, Phase 4's Node-panel and `_make_function` flows, and Phase 5's five.
[`by-hand-findings.md`](by-hand-findings.md) is what they found: nine defects, all now fixed, and
two things that stay open because nothing can automate them. Phase 3's design is
[`phase-3-design.md`](phase-3-design.md); what Phase 2 built, and the four places its design was
wrong, are in [`phase-2-design.md`](phase-2-design.md) §11.

**Phase 5 is built**, and [`phase-5-design.md`](phase-5-design.md) **§14** is where it says what
the design got wrong. Concurrency is the headline feature and it arrived cheaper than planned: a
task scope per script instance, `Await()` on any Godot signal with a typed payload, `Sleep`, and a
frame budget readable in Godot's profiler. ABI **v6**. **`dodge-the-creeps`'s wall 3 is down** —
seven of its eight now are.

**Phase 4.5 is built.** [`phase-4.5-design.md`](phase-4.5-design.md) is the design and **§11 is
the part to read**: it was planned before the work, so §11 is where the plan turned out to be
wrong — two of the four spikes came back the opposite way, and §1's table has two bad rows.

**Phase 4a is built.** [`phase-4-design.md`](phase-4-design.md) is the design, and
[`phase-4-gaps.md`](phase-4-gaps.md) is where the implementation and that design disagree — twenty
numbered entries, read it before picking the phase back up. Its gate was the Dodge the Creeps port rewritten *idiomatically* rather than
merely without GDScript, and the port is: six of its eight walls are down, `vectors.verse` is gone,
and the 30 headless checks pass. What remains of the phase is **4b**, the editor's data model, which
the yardstick never touches.
**Companion to:** `docs/spec.md` (what must be true) and `README.md` (what is true now)

---

## 0. How to read this

Phases, not dates. Each phase states why it comes where it does, what it contains as a list of
spec requirement IDs, and what has to be observably true before the next one starts. A phase is a
dependency boundary, not a unit of time; two adjacent phases may be a week and a season.

Four decisions shape the whole thing:

- **De-risk before building.** Phase 0 answers the questions that could invalidate the
  architecture. It ships nothing.
- **One ABI redesign, early.** The call/marshalling core is designed once against the entire
  spec rather than grown a feature at a time.
- **Dodge the Creeps is the yardstick.** Godot's canonical first game, ported to Verse with no
  GDScript remaining. It is attempted repeatedly, not once at the end.
- **Break anything before 1.0.** No compatibility obligation to the current script shape, the
  Verse-facing API, the ABI, or the demo scripts.

**1.0 means: a Godot developer who is not the author can build a real project and not hit a wall.**
Whatever subset that turns out to require is the 1.0 scope; everything before it is 0.x.

One thing in here is not settled, and it is called out where it bites: **the 1.0 bar may require a
release process that no phase currently contains** (§"What this roadmap does not contain"). The
other open item in Draft 1 — the order of the iteration loop and the parity work — was Phase 0's to
decide, and it decided: **the iteration loop comes first**, and this draft is edited to match.

### Standing rules

- `demo/` runs at the end of every phase. Inside a phase, broken is fine.
- Every requirement a phase claims lands with a test (R-QUAL-2).
- A spike's output is a written answer in `docs/spec.md` §14, not a branch.
- When a phase's work contradicts the spec, the spec is edited in the same commit and the
  requirement's status changes with it.

---

## Phase 0 — Answer what could invalidate the design ✅

**Complete.** Findings: [`phase-0-spikes.md`](phase-0-spikes.md). Answers: spec §14.1. The three
questions closed as follows, and the sections below are kept as written so the questions asked can
be compared with the answers got:

| | answer | what it moved |
| --- | --- | --- |
| **S-1** / OQ-2 | Precompiled Verse, runtime-only host. The artifact is a **cook**, which needs an editor-class binary, so `host/` becomes three targets. | R-DIST-8, R-DIST-11; narrowed OQ-3 and OQ-4; **opened OQ-10**; reshapes Phase 7 |
| **S-2** / OQ-8 | Fresh package name per generation, with `IncrementalizeProjectSource` first. ~200 ms per reload, ~0.5 MB retained per generation. | All of §10; **moved the iteration loop ahead of parity** |
| **S-3** / OQ-5 | Submodules inside the one user package, from the directory tree. Prototyped: two same-named classes in two directories. | R-LANG-6; stays in Phase 2 |

No engine changes were needed for any of them, which also settled 0.1.

**Why now.** Three unknowns in spec §14 can each change the shape of the host. Building features
first means building them twice. Nothing here ships; the output is written answers and, where a
question needs one, a throwaway prototype.

Engine patching is unrestricted in this phase — patch the checkout freely to answer a question
fast. Nothing that ships may *require* a patched engine without a separate decision (below).

### 0.1 The engine dependency becomes tractable

Spikes mean carrying engine changes, and three phases later there will be more of them. Set this
up first so a patch is a commit rather than a lost afternoon.

- Decide between a documented patch set and a UE fork as a submodule. The fork is the stronger
  option for reproducibility — a spike result is worthless if nobody can reproduce which engine
  state produced it — at the cost of tracking `main`.
- `build_host.py` learns which engine revision it built against, and the smoke test reports it.

**Exit:** an engine change can be made, recorded, and reproduced by a second checkout.

**Result: a documented patch set, and no patches.** All three spikes ran against a stock
`ue6-main` checkout, including the one whose recorded cause was an engine `#if`. A fork imposes a
second multi-gigabyte remote and a permanent merge obligation to solve a problem that does not
currently exist. The reproducibility half was built anyway: `build_host.py` writes
`verse_host.build.txt` beside the DLL — engine commit, branch, count and paths of local engine
changes, ABI version, godot-verse commit — and `host_smoke` prints it first, so a test log carries
the engine revision it was produced against. **Revisit the fork** when a shipped feature requires a
patched engine, or when the patch set reaches three.

### 0.2 S-1 · Can VerseVM run precompiled Verse? (OQ-2)

The highest-leverage unknown in the spec. It decides the export story, Android/iOS, and web at
once, and it shares a likely answer with S-2.

**The question:** can the VM load and execute serialised Verse without the Solaris compiler and
without the engine's package source tree on disk? Today the host must load from
`Engine/Binaries/Win64` precisely because VNI records package source directories relative to the
module and the compiler reads those `.verse` files at runtime.

**Method:** compile a trivial package, serialise whatever the VM holds afterwards, and attempt to
load it in a second process with the source tree absent. Failure modes are as informative as
success — *what* it reaches for tells you what a runtime-only host would have to carry.

**What it changes:** R-DIST-8, R-DIST-11, R-PLAT-2, R-PLAT-3, and whether `host/` splits into
compiler and runtime halves. A negative answer does not close web and mobile by itself; it
converts them from "package it" into "build it", and that estimate is the actual deliverable.

**Exit:** OQ-2 answered in the spec, with the cost of the negative branch estimated well enough to
choose in Phase 7.

**Result: yes, and the cost is on the producing side.** A target built without editor-only data
gets `WITH_VERSE_COMPILER=0` and loads Verse from cooked `.uasset` files; `SavePackage2` carries a
VerseVM cell table for the write side. Producing that file is a **cook**, and a cook turned out to
need more than the `ITargetPlatform` the assert first pointed at: `TargetPlatform` and
`WindowsTargetPlatform` do link into the monolithic host and the manager does find Windows, but
`FSaveContext`'s constructor then requires `WITH_EDITOR` and an `IPackageWriter`. So `host/` becomes
three targets — today's **editor host**, a **cooker** that must be editor-class, and a **runtime
host** that ships. Whether an editor-class Program target is buildable is the new **OQ-10**, and it
is Phase 7's opening move.

### 0.3 S-2 · Which hot-reload mechanism? (OQ-8)

**The question:** of the three surviving candidates in spec §10 — fresh package name per
generation, out-of-process compilation, an engine change — which is buildable, and at what cost?

**Method:** prototype the fresh-package-name approach first. It is the smallest change that would
prove the constraint is escapable, and the engine-side cause is already understood down to the
`#if !WITH_EDITOR` in `NotifyCompiledVersePackage` (see `codegen-once-per-process`). Measure what
the leak actually costs: the previous generation's classes stay pinned for the life of the
process, and `IncrementalizeProjectSource` with `EBuildMode::All` already keeps native packages
out of a rebuild, so the bound is the project's own two packages.

**Evaluate S-1 and S-2 together.** Out-of-process compilation is a plausible answer to both. If
S-1 says a runtime-only host is viable, out-of-process stops being an expensive workaround and
becomes the architecture.

**What it changes:** all of §10, R-EXP-5 (`@tool`), and the position of the iteration loop below.

**Exit:** OQ-8 answered, with a chosen mechanism and the reason the other two lost.

**Result: fresh package name per generation.** The first attempt failed for a reason the spike
brief did not anticipate — the collision was in the *native* VNI packages, not ours, because
nothing told the second build they were already compiled. `IncrementalizeProjectSource` before each
build is the missing half; the `EBuildMode::All` note above was close but had the mechanism
backwards. With both, 25 generations ran in one process at 161–220 ms each, ~0.5 MB retained per
generation, and instances from earlier generations kept working against their own classes.

### 0.4 S-3 · How does a project escape one flat scope? (OQ-5)

**The question:** the whole project shares one `/user@localhost` scope and Verse forbids
shadowing, which is why a script's class must be named after its file. R-LANG-6 requires modules,
subdirectories and shared library code. Can the host publish more than one user package, and what
determines a package's scope at runtime?

**Method:** read how Solaris assigns package scopes; attempt a second user package alongside the
script package. The attribute package the host already adds at runtime is a partial precedent —
and its ordering constraint (it must exist before the first `AddDataSource`, because
`EnsureDataSourcePackageExists` snapshots dependencies exactly once) is the kind of thing this
spike exists to find more of.

**May slip** to Phase 2 if S-1 and S-2 prove large. It is the one Phase 0 question that changes
the language surface rather than the architecture.

**Exit:** OQ-5 answered, or explicitly deferred with the reason.

**Result: answered, and the question was aimed slightly wrong.** Publishing a second package is
possible and is not what modules are made of: a package carries a module tree, and the flat scope
is simply that the host adds every snippet to the root module. The recipe is Epic's own
`ResolveModuleForRelativeVersePath`. Prototyped and run: `gameplay/player.verse` and
`ui/player.verse` compiled together as `(/user@localhost/gameplay:)player` and
`(/user@localhost/ui:)player`, a third file read a member off each, and the smoke suite stayed at
247/247. Building it for real stays in Phase 2 as the brief allowed.

### Phase 0 exit criteria — met

- OQ-2, OQ-8 and OQ-5 have written answers in the spec (§14.1). ✅
- The decision points the negative branches imply are recorded with criteria, not left open — the
  fork-vs-patches revisit trigger under 0.1, and the editor-class-target question S-1 opened, which
  is now **OQ-10** with a fallback named and a phase to answer it in. ✅
- **The order of Phases 3 and 4 is decided.** The iteration loop moved ahead of parity, and this
  document is edited to match. ✅
- Engine changes are reproducible: `verse_host.build.txt`, printed by the smoke test. ✅
- No production code has changed. The spike prototypes were reverted; what remains is
  `build_host.py`'s provenance record and the smoke test's report of it, which are tooling. ✅

---

## Phase 1 — ABI v2: dispatch, marshalling, diagnostics, and the harness that proves them ✅

**Complete.** Design and the spikes behind it: [`abi-v2-design.md`](abi-v2-design.md). Two
requirements landed with a documented edge rather than whole, and both are named under the exit
criteria; nothing else is outstanding.

**Why now.** R-NODE-6 is the keystone: `call_func` dispatches three hardcoded names over an ABI
offering two call shapes, and signals, the full virtual set, `@tool`, custom resources and
two-way interop all queue behind it. Designed once, against the whole spec, informed by Phase 0.

### 1.1 Design ABI v2 ✅

**Done.** `VH_ABI_VERSION` is `MAJOR * 1000 + MINOR` at 2.0, the header is rewritten rather than
extended, and it carries a written compatibility policy (half of R-QUAL-5). Three spikes settled
what the design rests on, and two of the three answers were not what the brief assumed:

| | question | answer |
| --- | --- | --- |
| **wire shape** | what replaces the flat `variant` tuple, which cannot express a Dictionary, a nested Array, a `Callable` or a `Signal`? | **A fixed-width tuple of scalars.** godot-cpp reframes the question: it never marshals a reference type, so with `Array`/`Dictionary`/`Callable`/`Signal` as ids, *nothing on the wire ever nests*. Measured: it marshals across VNI, it is hashable (so `[variant]variant` is legal), and it allocates nothing. |
| **lifetime** | can a reference id be released when Verse drops the value? | **Yes, through a native class' `BeginDestroy`.** Measured, after a first attempt said no for the wrong reason. `VWeakCellMap` turned out unnecessary. |
| **names** | what does Godot call a Verse method? | Verbatim — `mover.Fire()` — with Godot's virtuals mapped from `extension_api.json`. |

One break. `VH_ABI_VERSION` goes to a v2 numbering and the header is rewritten rather than
extended. It must anticipate, whether or not it implements:

- marshalled calls with arguments and return values (R-NODE-6)
- the full `Variant` type set, typed arrays and dictionaries, `Callable` and `Signal`
  (R-TYPE-1, R-TYPE-2, R-TYPE-3)
- signal declaration, emission and connection (§5.3)
- per-instance task scopes (R-ASYNC-4)
- runtime errors carrying file, line and a Verse call stack (R-DIAG-2)
- whatever Phase 0 says about a compiler/runtime split

Anticipating is cheap now and expensive later; a shape that has to grow a second calling
convention has failed.

### 1.2 Build the dispatch core ✅

- **R-NODE-6** ✅ — any method, any argument types, return values. The three-name array in
  `verse_script_instance.cpp` is gone, and so are the two call shapes under it.
- **R-NODE-9** ✅ — the method list reports what the script defines, read out of the semantic
  program: parameters with their own names and types, result type, `<decides>`/`<suspends>`, and
  Godot's name for the virtual a method overrides.
- **R-TYPE-1 … R-TYPE-5** ✅ — the whole `Variant` set crosses. `variant` became a fixed-width
  native struct of scalar lanes; the sixteen math types cross as their components, generated from
  one layout that drives the Verse struct, the packers and the host's marshalling alike; and the
  reference types — `Array`, `Dictionary`, `Callable`, `Signal`, the ten packed arrays — cross as
  ids into a table the GDExtension owns, released when the Verse value wrapping one is collected.
  R-TYPE-4's rule is that nullability is a property of the *type*, which a scan of Godot's 5304
  documented value-typed returns settled: one real exception, editor-only.
- **R-TYPE-6, R-TYPE-7** ✅ — the fixed-width struct *removed* the allocations the old encoding
  made (three Verse arrays to carry one number); a reference costs a table entry, which is the
  price of not copying a container. Unmeasured, which is R-PERF-2's job. The plumbing stayed
  hidden, and that turned out to have teeth: because a script cannot spell a `variant`, every way
  into and out of a container had to be a typed accessor, and those are generated.
- **R-DIAG-2** ✅ — runtime errors report file, line and a Verse stack. Landed here rather than in
  Phase 6 because every phase after this one is easier to debug with it, and because it is an ABI
  shape, not a feature. It needed two engine hooks rather than one: the only callback handed the
  rendered callstack runs *before* the cascading abort, and the only one that reports runs after.

### 1.3 Build the integration harness ✅

- **R-QUAL-1** ✅ — the missing third layer: `tests/integration/` is a Godot project driven
  headless, attaching a `.verse` script to a node and asserting on what `node.call()` answers. Its
  coverage grows with the type set rather than with this requirement.
- **R-QUAL-3** ✅ — `python tools/run_tests.py` runs all three layers. A layer whose prerequisites
  are missing is reported **skipped**, never as a pass.
- Kept the existing shape: one line per case, non-zero on failure, no framework — in GDScript too.

### Phase 1 exit criteria — met

- ✅ A Verse script method taking and returning every `Variant` type is callable from GDScript,
  proven from GDScript in the integration layer. **One edge:** a Verse `[]float` names no single
  Godot type, so a script-defined method taking one declares `Array` (R-TYPE-1). R-TYPE-2's *Verse
  spelling* of a typed array is also still absent, though nothing is flattened: a typed container
  keeps Godot's own typing because it is never copied. Separately, an **upstream Godot defect**
  shows through `Callable` — a called GDScript lambda still referenced at `finish_languages()`
  crashes the engine at exit, reproducible in eight lines with no extension loaded (R-TYPE-3).
- ✅ A runtime error names a file and a line, and carries a Verse stack that reaches the script's
  own method.
- ✅ The integration harness runs headless and covers the marshalling matrix — 51 cases driving a
  `.verse` script attached to a node, through `node.call()`.
- ✅ `demo/` still runs.

**Found along the way, and worth not rediscovering:** every basis and transform3d that crossed used
to come back transposed, because the Godot side emitted rows and rebuilt from columns;
`operator'()'` is a reserved intrinsic, so `Data[Key]` cannot be given a meaning and container
lookup is `Data.GetInt[Key]`; and a `var` property whose type is a nested struct or a container
cannot exist, because Verse asks for a field-named accessor overload per nesting level that no
single signature satisfies — those stay ordinary getter and setter methods.

---

## Phase 2 — The whole engine API

**Design:** [`phase-2-design.md`](phase-2-design.md) — the decisions, the measurement the class set
is gated on, and the Verse language facts the shape rests on.

**Why now.** The surface a user writes against is cheaper to finish before parity features are built
on top of it, and it was blocked on Phase 1: reaching the whole type set needs the per-signature
marshalling Phase 1 built. Modules were in this phase in Draft 2 and moved to Phase 3, which already
rewrites the source-set machinery they would be built on. **The phase touches no ABI.**

- **R-SCN-1, R-SCN-2** — the mirror covers all 1022 classes rather than the curated list in
  `tools/verse_api_classes.txt`, **gated on a measurement**: full generation is 2.9 MB of Verse
  through VNI on every host build, and possibly through analysis on every keystroke. The rule that
  decides it is written down before the numbers arrive. The generator's type table is finished, and
  a skipped method is **visible with its reason, as an editor diagnostic** — the failure mode
  R-SCN-2 exists to prevent, "the method I need isn't there and I can't tell why", is the one that
  ends adoption. R-SCN-2 also gains a bar it lacks: only `virtual`, `static` and `vararg` may be
  skipped, and any other skip is a defect.
- **The type table, which is where the reachability actually is.** Of 17 150 method entries, 2362
  are unreachable by any spelling and 823 of those are this phase's, in four causes: a public
  `variant` (231), typed arrays (~250), the packed vector and colour arrays (207), and bare
  `Object` (60). Mirroring Godot's `Object` once those land drags **R-INT-2** and a piece of
  **R-SIG-3** in early, because `callv` and `connect` are ordinary methods the moment `Variant`
  crosses.
- **Objects in containers.** `GetChildren()` returns a container whose elements no script can read —
  unnamed in the spec, squarely inside R-SCN-1, and the most-hit gap in the mirror, because walking
  children is what scene code is. Two mechanisms: `typed_array(t)`, and a spike on Verse's own
  failable cast `type[Value]`, which would give `is`/`as` parity (**R-SCN-6**, new).
- **R-SCN-5 (new)** — Godot's 758 enums become real Verse enums rather than magic integers, with
  `@export` of one pulled forward out of Phase 4.
- **R-LANG-6, third clause only** — library files: a `.verse` with no class of its own stops being a
  broken script. It needs no modules, and it is most of what makes one flat scope livable while they
  wait.
- **R-LANG-1, R-LANG-2, R-LANG-3** — script-to-script inheritance, interfaces, structs, enums and
  parametric types get tests. Most of this is expected to work already; none of it is verified. A
  hole that is an afternoon gets fixed; anything larger is recorded as a wall.

**Exit:** a Verse script can reach any Godot class and any method on it, or find out why not;
`demo/` still runs. **First Dodge the Creeps port attempt** — not expected to complete, and committed
as a second project beside `demo/` so the next attempt starts where this one stopped. The walls are
`docs/dodge-the-creeps.md`, and that list is Phase 4's scope.

**Status: met.** All 1023 classes are mirrored, `unsupported_type` is zero, and a member the mirror
does not carry is explained *in the editor* rather than in a report file. The phase also delivered
part of R-SIG-3 as a side effect of mirroring `Object`, closed R-TYPE-2 and R-LANG-6's third
clause, and added R-SCN-5. The cost is recorded rather than hidden: per-keystroke analysis went
from 158 ms to 1190 ms, which §3.2 named in advance as the first thing to revisit.

**The port did more than it was asked to: it plays.** `dodge-the-creeps/` is the whole game in five
`.verse` files with no GDScript in it, and a headless check that presses Start and asserts on 29
things Godot sees. The expectation here was a half-broken attempt and a wall list, so the wall list
is the part worth reading: eight things a GDScript author writes without thinking, each with the
requirement that will give it a spelling. Six are Phase 4's — R-SCN-6, R-SIG-1/2/5 and R-SCN-3 via
OQ-11 — one is a permitted vararg skip, and one is not a feature at all but a trap in Verse's
effect defaults that a library file walks into. It also corrected two things this document and the
spec had recorded as done: **R-INT-2** is `part`, because a script cannot make the argument array
`callv` needs, and **R-SCN-6** was already known missing but is now measured — it is what seventeen
`@export` slots and three `Object.Set` calls are standing in for.

---

## Phase 3 — What a project is: the source set, live — **done**

**Design:** [`phase-3-design.md`](phase-3-design.md) — the decisions, where they came from, the
spike that gated the phase and its answer, and the work order with the code anchors for each stage.
**Read §1.1 and §10 of it before building on this.** The bullets below are the brief the design was
written against, corrected in place where the design contradicted them — four of them were wrong,
and each says so rather than being quietly rewritten.

**What it cost, measured rather than estimated.** The "~200 ms per reload" below is what S-2 saw on
a one-class prototype; against a real project it is **1.27 s per generation** and **~1.3 MB
retained** (spec R-PERF-2 and R-ITER-6). The second figure is why the leak is tolerable and the
first is why the trigger is Play. **OQ-12 came back positive** — the package name carries the
generation and the verse path is pinned — so nothing in the design was redrawn.

**Why now.** This phase and the next one swapped places, which is what Phase 0 was asked to decide.
S-2 closed OQ-8 in favour of the fresh-package-name mechanism and measured it — ~200 ms per reload,
about half a megabyte retained per generation, and instances from earlier generations still
working. At that price the optimistic placement the previous draft described is the one taken: hot
reload comes before parity, because parity is the longest phase in this document and building it
with a restart in the loop is the expensive choice.

**Modules joined this phase in Draft 3.** They were Phase 2's, and moved because both are the same
machinery: the host owning its script package rather than borrowing the IDE's, an `ISourceSnippet`
whose text can change, and a source set whose files come and go. "A file moved to another directory"
and "a file was renamed while the editor ran" are one problem, and building it against the borrowed
package first would have meant solving it twice.

- **R-LANG-6** — modules, subdirectories, shared library code, per the S-3 answer (spec §14.1).
  **Not** "each `res://` subdirectory becomes a module", which is what this bullet said before the
  design and is Epic's *default* rather than Epic's only model: a directory is a module only if it
  carries a `<name>.vmodule` marker, and unmarked directories are organisational — their files join
  the nearest marked ancestor, defaulting to root. That is `TryRenameModule`'s behaviour in
  `SourceFileProject.cpp`, and it is chosen because `res://` is an asset tree whose directory names
  were picked for sprites rather than as Verse identifiers, and because it changes no existing
  project's meaning. The two things S-3 left as design are settled in opposite directions: a Godot
  directory name never has to be a Verse identifier (the marker file's stem names the module), and
  **moving a file between directories is allowed to break its references** for now, with the fix
  deferred. `phase-3-design.md` §2 has the reasoning and the four diagnostics that carry it.
- **One top-level name per file retires entirely**, rather than narrowing to per-directory. A file
  may declare any number; only the class named after the file can be attached to a node. Uniqueness
  narrows from project-wide to per-module, and the collision diagnostic is what tells an author the
  marker exists.
- **R-TOOL-12 (new)** — **the editor maintains `using` statements.** A user never types a module
  path: completion offers symbols from modules not yet in scope, and the import materialises when
  analysis reports the unknown name, goimports-style, because Godot's completion API carries no
  edit-on-accept hook to hang it on. It is the prerequisite for ever splitting the Godot API itself
  into submodules. This bullet also claimed it is what makes moving a file cheap; that half is
  **deferred** — R-TOOL-12 inserts a missing `using`, and does not rewrite one when a file moves.
- **A name the user has written must survive a reload — and this now gates the phase.** The
  fresh-package-name mechanism changes the package name every generation, and module paths are
  user-visible text that R-TOOL-12 writes into the author's own file. S-2 varied the package
  *name*; whether `/user@localhost` held across those generations is not recorded, and
  `ScriptVersePath` is compiled into eight lookup sites in `HostScript.cpp`. **Stage 0 of the phase
  is a spike that answers it, and nothing else starts until it has** — a negative answer redesigns
  the module half rather than adding to it. `phase-3-design.md` §1.
- **R-ITER-1, R-ITER-2** — edit and run, indefinitely; files added, renamed and deleted live. The
  mechanism is settled (spec §14.1). What this phase builds is everything around it: the host
  owning its script package instead of borrowing the IDE's, an `ISourceSnippet` with settable text
  so analysis and completion survive the change, `IncrementalizeProjectSource` before each build,
  and Godot's side deciding *when* a reload happens. That last one is answered: **on Play, plus an
  explicit Build action — not on save.** It is what Godot already does for the one language whose
  compilation unit is the whole project like ours (C# builds from `EditorBuildCallback` on Play,
  blocking), and the hook needs no new API: `EditorNode::call_build()` calls `build()` on every
  editor plugin before a run and aborts the run if one returns false. Saving still refreshes
  analysis per keystroke, so diagnostics, completion and export *shape* stay live.
- **R-ITER-3 / R-EXP-4** — changed `@export` defaults refresh, which today requires generated code
  and therefore a restart.
- **R-ITER-4, R-ITER-5** — state preservation where Godot's contract allows; a failed reload leaves
  the working code running. R-ITER-4 starts from a good default: an instance keeps its own
  generation's class until something deliberately moves it.
- **R-EXP-5** — `@tool` scripts, unblocked by the above, and cheaper than this bullet assumed:
  `VerseScript::_can_instantiate` already routes a tool script to a real editor instance and
  everything else to a placeholder, so the work is a `tool` attribute and making `_is_tool()`
  truthful, not building an execution path. Ready and Process in the editor; **not** the
  editor-only virtuals, which are Phase 4's general mechanism and would otherwise be built twice.
- **The leak is measured and recorded, not fixed.** The earlier plan here — a bound with a test
  asserting it — was dropped by decision: a reaping mechanism is not worth building while the
  project is experimental, and the build-on-Play trigger above largely defuses it anyway, since a
  session spends generations in the tens rather than the hundreds. It gets a number taken against a
  real project and a new requirement (R-ITER-6) so the deferral is tracked rather than forgotten.

**Two things this phase deliberately does not do**, both recorded as requirements so they are
visible: a **running game** does not pick up an edit (R-ITER-7 — the game is a separate process with
its own host, so it is a second delivery mechanism rather than an extension of this one), and
**moving a file** does not rewrite the imports that referenced it.

**Exit:** no workflow requires restarting the editor — add, rename, delete, edit a body and press
Play to get the edited code, change an `@export` default, and break the build and have Play refuse
to launch rather than run something stale, all in one editor session. Two same-named classes in two
modules coexist and both attach. A `@tool` script runs in the editor. Dodge the Creeps is the
regression check, with its five files moved into a `scripts` module, and it is owed the **one
windowed run** Phase 2 never gave it. Full criteria: `phase-3-design.md` §9.

---

## Phase 4 — Parity: signals, virtuals, and the rest of Godot's model

**4a is built; 4b is not.** [`phase-4-design.md`](phase-4-design.md) is the design, written after
its three spikes rather than before them — **§2 is where they are**, because one spike retired the
design the phase would otherwise have been built around — and **§11 is what building it corrected**,
which is the part to read before trusting the rest.

**Why now.** Everything here was blocked on Phase 1's dispatch and Phase 2's surface, and all of it
is faster to build behind Phase 3's reload loop. This is the phase the yardstick measures.

**The phase splits.** 4a is what scene code touches every day and what the yardstick can measure; 4b
is the editor's data model, which Dodge the Creeps never touches.

### Phase 4a — what scene code touches

Ordered by dependency; the design document has the stage table and what each is done when.

- **R-SCN-6 first** — the failable cast, and object identity with it: a handle that carries a Verse
  script crosses as *that script's own object*, or `player[GetNode(…)]` can never succeed. Measured
  as the most expensive absence in the port: seventeen `@export` slots, three stringly-typed
  `Object.Set` calls, an `?option` unwrap per node lookup.
- **R-TYPE-2's other half, and R-INT-2** — a script can make a container rather than only pass one
  on. Small, and it unblocks `Callv` with arguments, `AddUserSignal`, and 173 mirrored parameters.
- **R-TYPE-3's other direction, R-INT-4** — a `Callable` backed by a Verse function. Ahead of
  signals, because `Subscribe` is one.
- **§5.3 in full** — R-SIG-1 … R-SIG-4 and R-SIG-6: a signal is a typed member (`Hit:signal()`),
  emission is `Hit.Signal(…)`, subscription takes a Verse function, and Godot's own 489 signals get
  typed accessors. R-SIG-5 (`await`) waited for Phase 5, and was the one wall 4a could not close.
- **R-NODE-7, R-NODE-8** — the complete virtual set, generated, spelled the way Godot spells it
  (`_Ready`, `_Process`, `_Draw`) because plain names collide with signals on Node, CanvasItem,
  Control and BaseButton. `_notification` turns out **not** to ride that mechanism — it is in no part
  of `extension_api.json` — so it is hand-declared, and the rest of that family becomes **R-NODE-10**
  in 4b.
- **R-SCN-3 and OQ-11** — `@GlobalScope`'s utility functions, Godot's 114 statics, its constants, and
  the math types' methods. **OQ-11 is closed**: Verse has type-based extension methods and definable
  operators, so the math is ordinary Verse with no handle and no ABI, which is also what C# does. What
  genuinely has no handle — the statics and the ~28 utilities with no `/Verse.org` counterpart — gets
  one by-name dispatch callback. The random family is the single exception to R-AUD-2, dispatched so
  `seed()` steers one stream.
- **R-NODE-4, R-NODE-5** — a script's own statics and constants (a module with a declared
  association), and abstract classes.
- **R-EXP-5's remainder** — the editor-only virtual surface, which rides R-NODE-7 rather than being
  built twice.
- **R-ASYNC-8** — refuse a call that enters the host from a foreign thread, with a message rather
  than corruption. Small, and it belongs here rather than in Phase 5 because *this* phase creates the
  exposure: a `Callable` made from a Verse function is a value an author can hand to a
  `WorkerThreadPool` task. VerseVM already asserts `IsInGameThread()` and then carries on, so the
  status quo is a logged callstack followed by undefined behaviour.

**Exit for 4a:** **Dodge the Creeps is idiomatic** — not merely free of GDScript, which it already
is, but free of the workarounds: casts instead of seventeen inspector slots, Verse-declared signals
instead of two scene connections to one engine signal, no `vectors.verse`, no `Object.Set` with
string property names. The port is rewritten in place, wall by wall, and the diff is the measurement.

**Met**, and the diff says so: `main` went from nine object-typed exports to one (`MobScene`, which
*should* be an export), `node_paths=PackedStringArray(...)` is gone from all four scenes, the two
connections that existed only because a script could not declare a signal are gone with it, and the
mob is configured through `set Mob.LinearVelocity = ...` behind a cast. The 30 headless checks pass
and `tools/run_tests.py` is green at 266 integration cases. What is **not** met is the by-hand
checklist, which needs a windowed editor and is owed.

**Everything `phase-4-gaps.md` found afterwards is now closed, answered or deliberately narrowed**,
and the ABI went to v5 doing it: signals are refused at the member rather than at the emission, a
struct payload crosses both ways, the thread guard covers every entry point, all sixteen math types
carry their operators and the methods scene code reaches, and all 114 `@GlobalScope` utilities are
classified with none unexplained. `_make_function` is written and has since been watched write a
handler. **The phase owes nothing**: its by-hand checks are done and what they found is closed
([`by-hand-findings.md`](by-hand-findings.md)). One `Control` virtual, `_CanDropData`, has still
never been exercised, and B11 there is the measurement saying no headless run can reach it.

### What 4a leaves open

Written here rather than only in `spec.md` §14, because the next phase's shape depends on them:

- **OQ-16** — a callback that is not a bound method has no owner. 4a accepts only bound methods,
  which is the half of Godot's own design that does not leak; library-level handlers wait.
- **OQ-17** — **C# has never been run against this bridge.** Four MUST requirements name it and every
  fixture in the repo is GDScript. 4a makes the claim bigger by adding signals to it.
- **OQ-14** — the analysis number after the mirror grows, recorded without a threshold by decision.
- ~~**R-SIG-5**~~ — `await` on a signal, and with it one-shot connections. **Done in Phase 5**, which
  is where the one yardstick wall 4a could not close fell.
- **The 1073** — methods that mutate Godot *and* return a value, so they can be neither deferred nor
  compensated. Signal emission joins them by decision. Phase 4.5 below is what audits them, and what
  counted them: the 1354 this line used to say included statics and methods the mirror does not emit.

### Phase 4b — the editor's data model

- **R-NODE-3** — instantiation without a node, `RefCounted` and `Object` both.
- **R-EXP-6, R-EXP-7, R-EXP-8, R-EXP-9** — custom Resources, autoloads, icons, RPC config.
- **R-EXP-1** — the remaining `@export` surface, type-driven where the Verse type can say it and
  attributes only where it cannot.
- **R-NODE-10** — the script-level hooks that are in no JSON: `_ToString`, `_Get`, `_Set`,
  `_GetPropertyList`, `_ValidateProperty`. Here rather than in 4a because `_Get`/`_Set` overlap the
  export machinery above them.

**Exit for 4b:** a Verse custom Resource is created, saved to `.tres`, edited in the inspector and
loaded back, and a Verse autoload answers from every scene. (4a's by-hand clause is met: the checks
were run and [`by-hand-findings.md`](by-hand-findings.md) is the record.)

---

## Phase 4.5 — Effects and transactions: what a rollback actually undoes

**Built.** [`phase-4.5-design.md`](phase-4.5-design.md) is the design, written before the work
rather than after it — which makes **§11 the part to read**, because that is where the plan is
corrected. The table below is §1's, kept as written; §11.2 says which two of its rows are wrong.

**Why now.** Parity is when the bridge stops being small enough to hold in one head, and concurrency
is when a second effect axis (`<suspends>`, task scopes, R-ASYNC-4) lands on top of this one. Between
those two is the only moment where the effect surface is both complete and still simple. It is a
phase rather than an open-question row because nothing else owns it, and an audit folded into a phase
with other goals is an audit that gets skipped.

**Numbered `4.5` rather than renumbering Phases 5–8**, so that the phase records already written —
Phase 0's spike log, Phase 2's and Phase 3's design documents — keep saying what their authors wrote.

**What it is.** Today the mirror labels every one of its 14,933 methods `<transacts>`, and the label
is a promise the C++ keeps three different ways — or does not keep at all:

| | count | how the promise is kept |
| --- | --- | --- |
| mutating, returns nothing | 6813 | **deferred** to `AutoRTFM::OnCommit`; an aborted transaction never performs it. Honest |
| const, returns a value | 6728 | nothing to undo. Honest, but mislabelled: a property read is already `reads`, so the method surface disagrees with the property surface |
| const, returns nothing | 38 | honest, trivially — **wrong**, see §11.2: these are `OS.set_environment`, `CanvasItem.draw_string` and 36 more that plainly do something |
| **mutating, returns a value** | **1354** | **not kept.** The result is needed now, so the call cannot be deferred, and nothing compensates it |

- **Decide `<reads>` for the const surface.** It is measured, mechanical (`is_const` in
  `extension_api.json`, which the generator does not currently read), and it costs one native
  primitive declared `<reads>`. It makes the mirror self-consistent and shrinks the dishonest set to
  exactly the 1354.
- **Decide what the 1354 do.** Compensation where an action is undoable — `Verse::Stm::OnRollback`,
  the mechanism Phase 4's `Subscribe` introduces and UE uses throughout; documented non-atomicity
  where it is not, which is what GDScript offers anyway.
- **Say it to the author.** `no_rollback` is the first sharp edge a library file hits
  (`dodge-the-creeps.md` wall 8), the message names an effect they never wrote, and it points at a
  caller rather than at the declaration. The `.verse` template and the R-SCN-2 diagnostic machinery
  are both places that could say it where the fix is.
- **Write the rule down** in a form R-AUD-1 can be tested against: what a Verse author may assume a
  failed expression undid, and what it did not.

**Exit:** every effect label in the mirror is either true or listed as knowingly untrue, with the
list in the spec; a failable expression's guarantees are one paragraph an author can act on; and
OQ-15 closes.

**What it came to.** 3996 mirror methods carry `<reads>` where all 9597 carried `<transacts>`; the
knowingly-untrue set is **1073**, generated into [`nonatomic-methods.md`](nonatomic-methods.md) by
the same pass that writes the mirror so it cannot drift; the rule is in `spec.md` next to R-AUD-1
and OQ-15 is closed. The spikes found a bug rather than an answer in one case — Phase 4's
`Subscribe` compensation was registered with `Verse::Stm::OnRollback`, which is the Solaris
interpreter's STM and had never run; it is `AutoRTFM::OnAbort<SameAsClosed>` now and
`tests/integration` aborts it three ways. Nothing in `dodge-the-creeps/` or `tests/` needed a
change: narrowing a callee is invisible to its callers.

---

## Phase 5 — Concurrency

**Built**, by-hand checks included. [`phase-5-design.md`](phase-5-design.md) is the
record, and **§14 is the section to read first** — written after the work, it is where the design
turned out to be wrong. §2 was filled in the same way *before* the work, from twelve questions put
to the Verse compiler through `tests/verse_probe`, which is why so little of the rest needed
correcting.

**What it built.** A `verse::FContentScope` per script instance, so a raise costs that node's
suspended work and nothing else's; `Await()` on any Godot signal, typed by the payload the
declaration gives; `Sleep` on the host's own real-time clock; a `signal_ref` that can be *named*
rather than only received, with a rollback-safe `Subscribe` beside its `Await`; and a frame budget
whose effect is readable in Godot's profiler. ABI **v6**.

**Three corrections worth carrying forward.** D23 is retired — a scope is made at `vh_instantiate`,
not at the first `spawn`, because there is no hook that fires when a task starts and the guard must
already be active at that moment; it costs **~2.6 KB per scripted node**, measured. D17 is amended
— a foreign signal's payload is a `godot_array`, not `[]variant`, which has no description on this
bridge. And S-4, which the design twice called design-retiring, retired nothing: re-entrant
resumption inside an open transaction is safe, because each resumption opens its own nested one.

- **R-ASYNC-4** — per-script-instance task scopes. **Done**, and first, as planned: cancellation
  hangs off it and building `Await` on a project-wide scope would have meant building its
  cancellation twice. `GHaltedUntilTick`, `GTasksLostToError` and `ReviveContentScope` are gone,
  and `phase-4-gaps.md` G9 closed with it — Epic's callback-drops-with-its-scope rule is coherent
  against per-instance scopes and was not against one revived process-wide one.
- **R-ASYNC-1** — `spawn`, `race`, `sync`, `branch`, `rush`, `loop`, `<suspends>` in script code
  across frames. **Done**, and most of it was already true; what the phase owed was the
  `host_smoke` tick-loop layer that keeps it that way.
- **R-ASYNC-2, R-SIG-5** — await a Godot signal, a timer, a frame. **Done.** `signal(t)`
  holds a `/Verse.org/Verse` `event(t)`; the host signals that event directly, because
  `verse::event` is a UObject with a public C++ `Signal`.
- **R-ASYNC-5** — tasks cancelled when a node is **freed** (the wording is amended: leaving the
  tree does not cancel, which is GDScript's own rule), and on scene change. **Done.**
- **R-ASYNC-3, R-ASYNC-6** — the ordering documented against `_process`/`_physics_process` as a
  table that cites Godot's own source; the budget observable as three Godot custom monitors plus a
  rate-limited overrun warning. **Done.**
- **R-ASYNC-7** — the threading scoping document is **not** written and was never this phase's to
  write. What the phase owed OQ-6 was to foreclose nothing and to say what it settled: there is no
  scheduler whose thread affinity would have to be redesigned, because resumption is event-driven.
  OQ-6's row says so.

**Exit: met.** `dodge-the-creeps`'s **wall 3 fell** — `hud.verse` went from 55 lines of code to 44,
the `hud_phase` enum and the extra Timer node and both phase-reading handlers are gone, and all 30
headless checks still pass. The by-hand session ran too, covering all three phases that owed it;
[`by-hand-findings.md`](by-hand-findings.md) is what it found, and the game-over sequence and the
freed-node task were two of the entries it ticked.

---

## Phase 6 — Debugging and profiling

**Built.** [`phase-6-design.md`](phase-6-design.md) is the design,
written *before* the work, so **§13 is the part to read**. Its five automatable spikes all came
back and S-1 — does a snippet-compiled package carry a file path into its procedures? — was the
cliff the phase rested on: it does, **verbatim**, so nothing had to be restructured. ABI **v8.1**,
a minor bump: two callbacks appended to `vh_godot_api`, six entry points, three structs, one status
code and one `vh_tick_stats` field, all additive.

**Why now.** Deferred this long deliberately: R-DIAG-2 (errors with source locations) landed in
Phase 1 and covers most of the day-to-day need. The rest is large and benefits from a settled
surface.

- **R-DIAG-4** — Godot's own debugger: breakpoints, stepping, call stack, locals and members.
  Reachable: `Verse::FDebugger` is a public interface with a stack walk that hands back named
  registers, and `EngineDebugger` is a fully bound Godot singleton. **Two amendments** the design
  argues for: no expression evaluation (Epic's own Verse DAP client has none, and Godot's
  `evaluate` bails before it would ask us), and stepping follows the interpreter rather than the
  stopped task. The claim that the declared-and-empty `_debug_*` virtuals could be *removed* is
  wrong for four of them — godot-cpp binds those `_REQUIRED` and Godot errors when one is unbound
  — so the audit records *honestly empty* as a third outcome.
- **R-DIAG-5** — the profiler. Verse has no per-call hook, so this is boundary instrumentation
  (exact counts and times for every crossing the bridge makes) plus Verse's own `profile{}` blocks,
  and **not** a sampler, which cannot produce a call count.
- **R-DIAG-3** — a script error never takes down the editor or the game. **Part of this landed
  with Phase 3**, because a raise turned out to stop every script in the process rather than
  merely losing a return value; the spec has the three rules that replaced it. What is left here
  is bounding a script that raises every frame (**OQ-13**), and keeping a `@tool` script's error
  away from the scene the author is editing. Both are narrower than they read: Godot already drops
  errors past `max_errors_per_second`, and Phase 5's per-instance scopes already confine what an
  editor-time raise costs. **OQ-13's answer is that nothing is bounded** — what needs fixing is the
  bridge's own unthrottled stack printing, which eats the char budget and silences every other
  script.
- **R-DIAG-6** — the `SocketDebugger` DAP question (OQ-9), only if R-DIAG-4 turns out blocked.
  **Closed as not needed**: S-1 came back positive. The port still opens on
  `verse/host/enable_debugger`, and the two debuggers are mutually exclusive because `SetDebugger`
  is one global pointer.

**Exit: met.** All three requirements are **done**, both open questions are closed, `run_tests.py`
is green with 25 new `host_smoke` cases, and §7's audit covers all 59 declared virtuals with no row
found where a declaration had already been telling Godot something untrue. S-6's editor session was
run and found one defect — a `vector2` reaching the inspector as the *text* of one, because D7's
list of what a local crosses as had left the mirrored math structs off it — which is fixed and now
has a case. [`by-hand-findings.md`](by-hand-findings.md) has it, and keeps the session's steps:
everything from `EngineDebugger` inward has no other test.

Two numbers worth carrying forward. An attached debugger costs **+1.6%** of frame time on the
`dodge-the-creeps` yardstick (4.26 s → 4.33 s headless at `--fixed-fps 60`), which is what let the
always-attach path ship without the polled breakpoint mirror the design held in reserve; and a
single `vh_instance_call` goes from **0.27 µs to 2.79 µs** while attached, which is the same fact
stated where it looks worst.

---

## Phase 7a — Export, and what it can produce — **built**

**Why now.** Before 1.0, as decided — and **before 4b**, by the decision recorded in
[`phase-7-design.md`](phase-7-design.md) D1: export is the next thing worth having and shares
nothing with the editor's data model. 4b stays where it is above, unbuilt and owed before 1.0.

**The phase was split while it was being built, and this is the half that closed.** The design is
one document for both halves; **§13 is the part to read**, because it is where the design turned
out to be wrong, and **§13.7 is why there is a 7b at all**. Where the summary below and the design
disagree, the design is the record.

What 7a fixes:

- **The host split, and R-DIST-11 in full** — precompiled Verse, per the S-1 answer (spec §14.1).
  `host/` is three targets over one set of sources: the **editor host**, a **cooker** that is an
  editor-class *executable* the export plugin runs as a subprocess, and a **runtime host** with
  `WITH_VERSE_COMPILER=0` that ships with the game. One ABI header serves all three: the runtime
  host answers `VH_ERR_UNSUPPORTED` for the eleven compiler-side entry points and `vh_host_kind()`
  says which host was loaded, so a wrong DLL is one sentence rather than a crash. ABI **8.2**.
  The runtime host links **no Verse compiler at all** — the modules are behind
  `Target.bBuildWithEditorOnlyData` and `HostScript.cpp`'s compiler-side bodies behind the matching
  `#if`, so "no compiler ships" is a fact about the bytes rather than about the data directory.
- **OQ-10, closed** — an editor-class Program target builds, links and boots, *provided it also
  compiles against Engine*, is an executable rather than a DLL, and carries developer tools. Design
  §2 S-1 has the seventeen builds; §13.5 has what the body cost after that.
- **R-DIST-9** — Godot's ordinary export dialog produces the whole tree. Cooked packages and the
  class sidecar go in a `verse_data` directory beside the executable, which is
  .NET's layout and doubles as the host's engine directory; the runtime host rides as a
  `.gdextension` `[dependencies]` row; `.verse` sources are stripped to one-byte stubs the way C#
  strips `.cs`, and `.vmodule` markers are kept by hand because a module is half of a class's name.
  `verse/host/dll_path` and `engine_dir` are editor-only.
- **R-PLAT-4** — an export to Android, iOS or web fails at export with a sentence. §13.4 records
  why the plugin also withholds the data directory: `add_message(EXPORT_MESSAGE_ERROR)` reports but
  does not abort.
- **Godot 4.7 official** is the editor and the templates. godot-cpp is on the commit that carries
  `extension_api-4-7.json`, `SConstruct` exports `api_version` to pick it, and the mirror is
  regenerated: 1036 classes, 793 enums, 1437 virtuals, 503 signal accessors, 3312 properties.
- **An `export` layer in `run_tests.py`** exports `tests/integration` headless and asserts the tree
  it produced. It does not launch the result — that is 7b's — and it does not export
  `dodge-the-creeps`, which stays the by-hand yardstick it was always meant to be.

**Exit:** `run_tests.py` reports four layers; the fourth exports `tests/integration` from the 4.7
editor and asserts the data directory, the staged runtime host, the stubbed sources, the kept
`.vmodule` markers and the sidecar's contents; the abi layer drives `verse_cook.exe` over
`tests/host_smoke`'s fixtures and asserts what it wrote; an Android export fails with one sentence;
design §13 is written.

---

## Where the host lives is a machine's business, not a project's — **done**

Not a phase, and it was in no plan: it came out of the 7b interview, because 7a had quietly made it
worse by adding a third machine-specific path (`verse/host/cooker_path`) beside the two that were
already committed into three `project.godot` files.

**R-DIST-12** is what it answers: nothing a project commits names one machine. `verse_host_paths`
resolves the checkout, the host DLL and the cooker in one order — the environment (`UE_ROOT`,
`VERSE_HOST_DLL`, `VERSE_COOKER`), then the user's **EditorSettings**, then the legacy project
settings of the same names, which are read for projects that predate this, warned about once, and
never written. Only the checkout is normally set; the other two are derived from it, through a
per-platform table rather than a Windows path built at three call sites. `verse/host/enable_debugger`
stays a project setting, because which debugger a project wants *is* the project's business.

The environment comes first for a reason worth remembering: **`EditorInterface` does not exist in
an editor binary running `-s`**, which is how the integration layer runs — and `Engine::has_singleton`
is no guard, because Godot registers the *name* whatever the binary is doing and refuses the object,
so the check that works is `is_editor_hint()`. `tools/run_tests.py` exports `UE_ROOT` into every
Godot it launches and `point_at_engine` is deleted; the `[verse]` section is gone from all three
committed projects.

---

## Phase 7b — Loading what the cooker wrote — **built; exit met 2026-09-15**

**Done.** An exported game runs its Verse: `run_tests.py` reports four green layers, the fourth of
which launches what it exported (308 passed, 0 failed, 9 skipped), and `dodge-the-creeps` exported
outside the repo and run with `UE_ROOT` unset passes all 30 of its checks. It reaches its first Verse
`_Ready` in **0.54 s** against **4.08 s** compiled at startup, and ships **5.0 MB** of Verse against
7a's 68. **`phase-7b-design.md` §13 is what the work corrected**, and §13.8–§13.9 are the two walls
and the five further defects that no spike would have found — the rest of this entry is the plan as
it was written, kept because the route it chose is the one that worked.

**The wall, in one line:** `FLinkerLoad` has no `Verse::VCell` support, so a Verse package can be
cooked to a loose `.uasset` and cannot be *loaded* from one. Only the IoStore loader's
`FExportArchive` reads a cell (`AsyncLoading2.cpp:3184`), which is why the save writes its four
bytes (`LinkerSave.cpp:398`) and the load consumes none — the base `FArchive` operator's whole body
is `return *this;` (`Archive.h:1283`). An exported game therefore boots, loads the runtime host,
registers its mount points, starts loading `/GodotAttributes/_Verse` and dies on *"Missing VClass
for VerseClass … This class should have been created in-memory from a VClass, not loaded from a
cooked package."*

**[`phase-7b-design.md`](phase-7b-design.md) is the design**, written before the work from an
interview held against the Unreal sources. `phase-7-design.md` §13.7 is still the record of *how the
wall was found*; §3 of the new document is the same ground re-verified, and it corrects §13.7 in two
places.

**The route: the IoStore container, with the two spikes run first.** The cooker keeps writing loose
files — a legacy cooked header *does* carry the cells (`PackageFileSummary.h:153-168`,
`Linker.h:71-74`), which is what makes it the right input — and converts them with
`CreateIoStoreContainerFiles` (`IoStoreUtilities.h:19`), whose `FPackageStoreOptimizer` is the code
that already knows how to carry a cell across. The runtime host mounts the result. Both halves have
precedent, and the mounting half is more ordinary than the wall's discoverer feared:
`FPackageStore::Mount` is public `COREUOBJECT_API`, `FFilePackageStore` is a `PakFile` module
class rather than an editor one, and script imports resolve from **in-memory registration**
(`FGlobalImportStore::AddScriptObject`, called unconditionally) rather than out of a global
container's script-objects chunk. **S-8a and S-8b run before any stage is written**, which is 7a's
own lesson — its S-5 ran after stage 1 and a working cooker was built behind an unchecked wall.
**If both spikes fail, the phase stops and asks.** The engine patch, `FZenStoreWriter` and a
hand-rolled loader are each a separate decision.

**What 7b covers, and nothing else:**

- **The wall** — a cooked Verse package that loads in an exported game.
- **R-DIST-10** — the exported `dodge-the-creeps` runs sandboxed, outside the repo, with `UE_ROOT`
  unset and the UE checkout off PATH, recorded in `by-hand-findings.md` with what that cannot prove.
- **The export layer's second half** — `tests/integration`'s 1391-line `test_main.gd` split into a
  library two drivers share, the way `dodge-the-creeps/checks.gd` already is, so `run_tests.py`
  **launches** what it exported. A case that cannot run in an export is tagged `editor_only` and
  printed as skipped, and the layer asserts the skip count so none can quietly vanish.
- **A stamp and a refusal** — the sidecar carries the ABI version, the cooker's commit and the
  engine's, and a game whose Verse data is missing, unreadable or foreign refuses to start with one
  sentence rather than starting silently without scripts.

**Out, by decision:** Linux and the missing `dlopen` path; macOS; Shipping-versus-Development per
template; the debugger and profiler in an exported game, which stay untested and are said to be;
trimming the mirror, which is 67 of the cook's 68 MB and is recorded rather than fixed.

**Exit — met.** Four layers green with the fourth launching what it exported; dtc exported, run
sandboxed and green (`by-hand-findings.md` B14); R-DIST-9, R-DIST-10 and R-DIST-11 all **done** with
the Windows-only caveat written into each line; OQ-18 closed; design §13 written with §10's
measurements in §13.10.

**What the plan above did not anticipate, and what it cost:** both spikes passed and the phase was
stopped anyway, twice — first by a cooked native's C++ thunk, which is a function pointer and does
not serialise, and then by five more defects that only *launching* an export could show. The lesson
is narrower than "run the spikes first", which this phase did: **a spike that loads a cooked package
proves the loading, and only one that calls something proves the calling.**

---

## Phase 7.5 — Web

**Why a phase of its own.** R-PLAT-3 is SHOULD and blocked on **OQ-4**: UBT has no wasm Program
target, and Godot's web export is a constrained, single-threaded-by-default wasm environment.
Phase 7 removes nothing from that list and adds one thing to it — the runtime host now exists as a
target whose module set is the smallest Verse can run in, which is the only binary a web build would
ever need to reach. No design yet, by decision; when it is written it starts from OQ-4's row in
`spec.md` §14 and Phase 7's §2 S-2 (the runtime host's measured module set and size).

**Exit:** OQ-4 answered in `spec.md`, either way.

---

## Phase 8 — The manual, and 1.0

**Why now.** "Usable by someone else" is not testable without documentation, and the manual is
worth writing against a surface that has stopped moving.

- **R-QUAL-8** — the user manual: installing, a first script, the `@export` reference, the Godot
  API mapping, the concurrency model, the known limitations. Distinct from README, which stays the
  design document.
- **R-DIST-3** — the UE licensing consequence for a shipped game, stated where a user will see it
  before they need it.
- **R-AUD-1, R-AUD-3** — validated rather than asserted: can a Godot developer write a working
  script knowing only "class, member, method, `set`"?
- **The 1.0 test** — someone who is not the author builds a real project. Every wall they hit is
  either fixed or documented. The walls are the release notes.

**Exit:** 1.0.

---

## What this roadmap does not contain

Recorded so the omissions are visible rather than forgotten:

- **A release process** (R-QUAL-5, R-QUAL-6) — versioning policy, changelog, the tested Godot × UE
  matrix per release. Excluded by decision, and it is the one omission that sits in tension with
  the 1.0 bar: "usable by someone else" implies at least one release someone else can obtain, and
  no phase produces one. **This needs a decision before Phase 8** — either a minimal release step
  joins that phase, or the 1.0 bar is restated to mean something a source checkout satisfies.
  Left unresolved deliberately rather than quietly patched.
- **Benchmarks** (R-PERF-2) and the performance section generally (§13). Deferred by decision;
  R-PERF-1 still requires §13 to be replaced with measurements before 1.0, and no phase currently
  does that. **One of R-PERF-1's five numbers is no longer outstanding**: editor analysis latency
  per keystroke was measured and recorded in R-PERF-2 by the editor-performance commits (dcd517e
  through 1469dc1), which is work no phase owned and which has no design document by decision. The
  other four — the empty-`Process` comparison against GDScript, property read and write, a
  marshalled call, and compile time at 10/100/1000 scripts — and every *target* remain.
- **CI** (R-QUAL-4) — blocked on OQ-1, since a hosted runner cannot be given a licensed UE
  checkout. R-QUAL-3 is the substitute throughout.
- **The addon install story** (R-DIST-6, R-DIST-7) — blocked on OQ-1 and outside anyone's control
  here. If the Verse toolchain is open-sourced during the work above, it becomes a phase of its
  own; until then the project is source-only and every phase assumes it.
- **Threading** (R-ASYNC-7 / OQ-6) — Phase 5 neither implements it nor writes its scoping
  document: it updates the OQ-6 row with what it settled and what it left, and forecloses
  nothing. There turns out to be no scheduler whose thread affinity would need redesigning —
  resumption is event-driven, measured — only the pump, which runs where `_frame` does.
