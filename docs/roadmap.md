# godot-verse — Roadmap

**Status:** Draft 6 · 2026-09-12 · **Phases 0, 1 and 2 complete; Phase 3 designed and not started.**
Its design is [`phase-3-design.md`](phase-3-design.md), which supersedes four of the bullets below
and gates the whole phase on a spike. What Phase 2 built, and the four
places the design was wrong, are in [`phase-2-design.md`](phase-2-design.md) §11. The Dodge the
Creeps port closed it and **plays** — the eight walls it hit, each mapped to a requirement, are
[`dodge-the-creeps.md`](dodge-the-creeps.md), and that list is Phase 4's scope.
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

## Phase 3 — What a project is: the source set, live

**Design:** [`phase-3-design.md`](phase-3-design.md) — the decisions, where they came from, the
spike that gates the phase, and the work order with the code anchors for each stage. **Read it
before starting anything here.** The bullets below are the brief the design was written against,
corrected in place where the design contradicted them — four of them were wrong, and each says so
rather than being quietly rewritten.

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

**Why now.** Everything here was blocked on Phase 1's dispatch and Phase 2's surface, and all of it
is faster to build behind Phase 3's reload loop. This is the phase the yardstick measures.

- **§5.3 in full** — signal declaration with typed arguments, emission, connection and
  disconnection, editor-side connection including the create-the-function flow (`_make_function`),
  and signals crossing to GDScript and C# (R-SIG-1 … R-SIG-4, R-SIG-6). R-SIG-5 (`await` a signal)
  waits for Phase 5.
- **R-NODE-7, R-NODE-8** — the complete virtual set and `_notification`, by a general mechanism: a
  virtual added by a future Godot version must not require a code change here.
- **R-NODE-3, R-NODE-4, R-NODE-5** — instantiation without a node, statics and constants,
  abstract classes.
- **R-EXP-6, R-EXP-7, R-EXP-8, R-EXP-9** — custom Resources, autoloads, icons, RPC config.
- **R-EXP-1** — the remaining `@export` surface, including enums and structs from R-LANG-2.
- **R-SCN-3 and OQ-11** — `@GlobalScope`'s utility functions, and the value-type methods and
  operators the math types still lack. Both moved here from Phase 2 because they are one problem: a
  free function has no handle to ride `VhCallValue` on, and neither does a `vector2`. One by-name
  dispatch entry point answers both, and deciding them together costs one rather than two. The
  naming is already decided — **Verse's own stdlib wins**, so of 114 utility functions only the ~28
  with no `/Verse.org` counterpart are mirrored (R-AUD-2).
- **§8 interop** — R-INT-1 … R-INT-4. R-INT-2 (calling a GDScript-defined method from Verse
  dynamically) was the one with no existing path; Phase 2's `Object` mirror is expected to deliver
  it through `callv`, so **verify it rather than scoping it**, and spend the time on R-INT-3 and
  R-INT-4 instead.

**Exit:** **Dodge the Creeps runs with no GDScript in it.** That is the phase gate, and it is
binary.

---

## Phase 5 — Concurrency

**Why now.** It needs Phase 1's ABI, Phase 4's signals, and a stable enough surface that the task
lifetime rules can be written down rather than discovered.

- **R-ASYNC-4** — per-script-instance task scopes. Today one `verse::FContentScope` serves the
  whole project, so one dead-object access terminates every suspended task in every script. This
  is first: it is a correctness bug, not a feature.
- **R-ASYNC-1** — `spawn`, `race`, `sync`, `branch`, `rush`, `loop`, `<suspends>` verified in
  script code across frames.
- **R-ASYNC-2, R-SIG-5** — await a Godot signal, a timer, a frame.
- **R-ASYNC-5** — tasks cancelled when a node leaves the tree or is freed, and on scene change.
- **R-ASYNC-3, R-ASYNC-6** — documented ordering against `_process`/`_physics_process`; the frame
  budget configurable and overruns reported.
- **R-ASYNC-7** — spawn the threading scoping document (OQ-6). It is not built here, but nothing
  in this phase may foreclose it.

**Exit:** a concurrency-heavy script is a reasonable thing to write. Dodge the Creeps port attempt
— this is where the second demo, the one that shows what Verse buys you, becomes writable.

---

## Phase 6 — Debugging and profiling

**Why now.** Deferred this long deliberately: R-DIAG-2 (errors with source locations) landed in
Phase 1 and covers most of the day-to-day need. The rest is large and benefits from a settled
surface.

- **R-DIAG-4** — Godot's own debugger: breakpoints, stepping, call stack, locals and members,
  expression evaluation. Note that the current state is *worse than absent* — every `_debug_*`
  virtual is declared and returns empty, so Godot believes the language supports debugging and is
  told there are no stack frames. Implementing or removing them is part of this phase, and the
  same audit applies to every other declared virtual.
- **R-DIAG-5** — the profiler, with the same declared-and-empty defect.
- **R-DIAG-3** — a script error never takes down the editor or the game.
- **R-DIAG-6** — the `SocketDebugger` DAP question (OQ-9), only if R-DIAG-4 turns out blocked.

**Exit:** a breakpoint in Godot's script editor stops a Verse script and shows its locals.

---

## Phase 7 — Platforms and export

**Why now.** After parity, before 1.0, as decided. Export and cross-platform are the same work
done once: R-DIST-9 builds the pipeline, and it should be built for every desktop platform
simultaneously rather than Windows-first and ported.

- **R-PLAT-1, R-PLAT-5** — Linux and macOS, editor and exported game, at the same feature level.
  Expect this to surface Windows assumptions; the codebase is portable in shape but has never been
  built elsewhere.
- **R-DIST-9, R-DIST-10** — exporting through Godot's ordinary dialog produces a game that runs on
  a machine with nothing installed.
- **R-DIST-11 and the host split** — precompiled Verse in exported games, per the S-1 answer
  (spec §14.1). `host/` becomes three targets over one set of sources: today's **editor host**, a
  **cooker** that is editor-class and runs only at export, and a **runtime host** with
  `WITH_VERSE_COMPILER=0`, which is what ships. Export cooks each Verse package to `.uasset` and the
  runtime host loads it. This is the largest single item in the phase and the reason R-DIST-9 and
  R-DIST-10 cannot be built before it.
- **R-PLAT-4** — an unsupported platform fails at export time, not on a player's device.
- **OQ-10, first** — whether an editor-class UBT Program target can be built at all. The cooker
  depends on it, and so therefore do R-DIST-9, R-DIST-10 and R-DIST-11. If the answer is no, the
  fallback is cooking through a real UE editor or commandlet process, and the phase is shaped around
  that instead. Nothing else in this phase should start before this is known.
- **R-PLAT-2, R-PLAT-3** — mobile and web, per Phase 0. These may land here, land later, or become
  written-down non-goals with a reason. They do not gate 1.0.
- **R-DIST-4, R-DIST-5** — the build and the version-mismatch message become things a stranger can
  survive.

**Exit:** Dodge the Creeps exports and runs on Windows, Linux and macOS.

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
  does that.
- **CI** (R-QUAL-4) — blocked on OQ-1, since a hosted runner cannot be given a licensed UE
  checkout. R-QUAL-3 is the substitute throughout.
- **The addon install story** (R-DIST-6, R-DIST-7) — blocked on OQ-1 and outside anyone's control
  here. If the Verse toolchain is open-sourced during the work above, it becomes a phase of its
  own; until then the project is source-only and every phase assumes it.
- **Threading** (R-ASYNC-7 / OQ-6) — gets a scoping document in Phase 5, not an implementation.
