# godot-verse — Specification

**Status:** Draft 3 · 2026-09-14 · targets no release yet
**Supersedes:** nothing. **Companion to:** `README.md`

---

## 0. About this document

`README.md` is the *design* document: it explains why the code is shaped the way it is, and
everything in it is true of the software as it exists today. This is the *requirements* document:
it describes the finished thing. Where the two disagree, README describes the present and this
describes the target; neither is a plan, and §14 is the only place that admits what is unknown.

Requirement levels are RFC 2119: **MUST**, **MUST NOT**, **SHOULD**, **MAY**. A MUST is a
condition of calling the project done; a SHOULD is a strong default that a documented reason may
override; a MAY is permission, not obligation.

Every requirement carries a status:

| | meaning |
| --- | --- |
| **done** | implemented and exercised by something |
| **part** | implemented for a subset, or implemented but not reachable |
| **none** | nothing exists |
| **blocked** | cannot be built until something in §14 is resolved |

A requirement is numbered so it can be cited in an issue or a commit. Numbers are never reused.

---

## 1. Scope

### 1.1 Goal

Verse is a first-class scripting language for Godot. Two bars, both of which must be cleared:

1. **Parity with GDScript.** Anything a GDScript author can express about a Godot project, a
   Verse author can express. Not "most things" — the gaps are where a language integration goes
   to die, because a project that hits one has to rewrite a file in another language and then the
   whole premise is gone.
2. **Core Verse.** The language's own features work — concurrency, failure contexts, parametric
   types, transactional semantics — not a hand-picked subset that happened to be easy to bridge.

Anything past those two bars is a bonus and is marked MAY.

**One stated exception to bar 1, and it is permanent: background threads.** A GDScript author can
run *their own code* on a `Thread` or a `WorkerThreadPool` task. A Verse author cannot, and this is
not a gap this project can close. VerseVM asserts the game thread at its top-level entry —
`ensure(IsInGameThread() && …)` in `VVMEnterVMInline.h`, above the comment "Verse bytecode and
AutoRTFM transactions must run on the game thread" — and Verse's own concurrency is *cooperative*
rather than parallel: `spawn`, `race`, `sync` and `branch` interleave tasks on one thread, they do
not distribute them across cores. So no Verse code ever executes off the game thread, a long Verse
computation blocks the frame, and there is no Verse spelling of "do this in the background".

What a Verse author keeps is the ability to *ask the engine* for threaded work and observe the
result on the main thread — threaded resource loading, a `WorkerThreadPool` task whose callable is
not Verse — and the whole of Verse's concurrency for everything that is about waiting rather than
about cores. What changes is only who runs the work.

Recorded here, against the bar it fails, rather than buried in §7. **R-ASYNC-8** makes a
foreign-thread call refuse with a message instead of corrupting the VM; **OQ-6** holds the part that
could still change, which is whether such a call is *marshalled* to the game thread rather than
refused. Neither would let Verse code run on another thread; only an engine change could.

### 1.2 Target user

**A Godot developer who wants a better language than GDScript.** They know Godot's model —
nodes, scenes, signals, resources, the inspector — and they do not know Verse. This ordering
resolves conflicts:

- **R-AUD-1 (MUST)** Every Godot concept a GDScript user relies on has an obvious Verse spelling,
  discoverable from the completion list without reading Verse documentation.
  *Rationale:* the user is porting a mental model, not learning one.
- **R-AUD-2 (MUST)** Where Verse and GDScript differ in *spelling*, Verse's spelling wins —
  `set Position = …`, PascalCase members, `Print`. Where they differ in *model*, Godot's model
  wins: a node is a node, a signal is a signal, a Resource is a Resource.
  *Rationale:* a Verse-shaped API over a Godot-shaped engine is learnable; a Verse-shaped *engine
  model* would mean the user's Godot knowledge stops transferring, and that knowledge is the only
  thing they brought. Status: **done** as a principle — the mirror already reads `node2d`,
  `Position`, `GetParent()`.

  Godot's `Object.to_string` is the one place the principle had teeth. It cannot be a method named
  `ToString`: Verse's own `ToString` is ambiguous with it whatever the arity. Renaming it would have
  been the obvious escape and the wrong one — Verse's spelling wins, so it is reachable as
  `ToString(Value)`, a module-level overload of Verse's own name. That buys more than it costs:
  string interpolation *desugars* to `ToString`, so `"{MyNode}"` prints what Godot prints, with
  nothing written to make it.

  Exactly four other members across 1023 classes are ambiguous with a Verse name, all of them `Min`
  or `Max` as **data** — a `var` has no signature to be told apart by, where a *method* of the same
  name is fine, which was confirmed both ways. Those four stay properties under one word of English:
  `set X.Maximum = 1.0`. Both spellings an author might reach for are recorded as skipped and point
  at it, so `X.Max` and `X.GetMax()` are each answered by the editor with the one that works.

  **The random family is the one exception to the principle, and it is a deliberate one.** `Randf`,
  `RandiRange`, `SeedRandom` and their five siblings are dispatched to *Godot's* implementation
  rather than answered by Verse's own, which R-AUD-2 would otherwise decide the other way. The
  reason is that they are not really a spelling difference: `seed()` and `randomize()` name a stream,
  and a Verse-side RNG would ignore both, so a project that seeds for a replay would get a different
  game and nothing would say why. That is a *model* difference wearing a spelling's clothes, and
  Godot's model wins. C# makes the same exception for the same reason. The integration suite proves
  it is one stream: GDScript seeding and Verse asking see the same number. See R-SCN-3.
  **What a failed expression undoes** (OQ-15's answer, Phase 4.5). A Verse *failure* — an
  `if (X := F[])` that declines, a `<decides>` method that declines, an option unwrap, a failed cast
  — **undoes every Godot write the failing computation had made**, at any depth. A write is deferred
  to the transaction's commit, so a failure discards it and the scene never sees it; a **raise** does
  the same and additionally cancels the raising *instance's* suspended tasks (R-ASYNC-4). It used to
  stop every script in the project until the next frame; Phase 5's per-instance task scopes replaced
  that, so the rest of the project never notices. Three
  things that follow are sharp edges rather than defects, and an author has to know them. **A read
  does not see a write the same computation just made**: the write has not happened yet, so
  `set Position = X` followed by reading `Position` answers the old value. **A method that both
  mutates Godot and answers a value is not undone** — the answer is needed now, so the call cannot be
  deferred, and the bridge forwards it to Godot rather than performing it, so it has no inverse to
  register. That is the shape, and `docs/nonatomic-methods.md` is the generated list: 1073 methods,
  plus signal emission, which runs its handlers immediately by decision. **A `<reads>` function
  guarantees the first two do not arise**: it may call Godot's const-and-answering methods and read
  properties, and the compiler refuses it any write at all.

- **R-AUD-3 (SHOULD)** Verse's less familiar features — `<decides>`, structured concurrency,
  parametric types — are available but never on the critical path of a first script. A user must
  be able to write a working script knowing only "class, member, method, `set`".

  **Effect labels.** Every method in the mirror carries an effect that is either true or on a
  written list. Godot's 6728 const-and-answering methods carry `<reads>`, so a helper that only
  looks at the scene can be `<reads>` too and stops forcing `<transacts>` onto everything that calls
  it; 3996 of them survive into the mirror under their own names, the rest being superseded by
  properties. Everything else carries `<transacts>`, which is honest for the 6813 mutating-void
  methods (deferred to commit) and **knowingly untrue for 1073**, enumerated in
  `docs/nonatomic-methods.md` and generated by the same pass that writes the mirror so the list
  cannot drift. Godot's `const` decides that split, and it means "does not mutate the C++ object"
  rather than "has no effect": the 38 methods that are const and return *nothing* are
  `OS.set_environment`, `OS.delay_msec`, `CanvasItem.draw_string` and 35 more of that shape, so the
  test is const **and** answering. The flag is also applied unevenly in the other direction —
  `Tween::is_running` is `bool is_running() { return running; }` and is not marked const — so
  `CONST_OVERRIDES` carries 127 exceptions, none of them judged: `tools/audit_const_overrides.py`
  reads them out of Godot's own source and accepts a method only when its body is exactly
  `return <member>;`, rejecting a literal return or any name declared `virtual` anywhere. The one mutate-and-answer call the bridge performs itself rather
  than forwards — `signal.Subscribe` — is compensated with an
  `AutoRTFM::OnAbort<SameAsClosed>` that disconnects, and `tests/integration` aborts it three ways
  and checks the connection came back.

### 1.3 Non-goals

Stated so they are not re-litigated:

- **UEFN / Fortnite compatibility.** No mirror of `/Fortnite.com` or `/UnrealEngine.com`, no
  compatibility shims, no attempt to make a UEFN device snippet compile. The common ground with
  UEFN is the language core and `/Verse.org` only, and §4 says how much of that we commit to.
- **Verse as a general embedding.** `verse_host_abi.h` is a means, not a product. It is not
  documented, versioned, or supported for use by anything other than this GDExtension.
- **Replacing GDScript or C#.** Mixed-language projects are a first-class supported case (§8), not
  a migration path away from anything.
- **Godot 3.x.**
- **Running Godot inside Unreal**, or the reverse. The host boots `FEngineLoop` to own VerseVM;
  no Unreal rendering, gameplay framework, asset pipeline, or editor is exposed to a script, ever.

---

## 2. Distribution and licensing

The install story has two states separated by an event outside this project's control: whether
Epic ships the Verse compiler toolchain under terms that permit redistributing binaries built
from it. The spec commits to both states rather than waiting.

**Today — source only.**

- **R-DIST-1 (MUST)** The project is distributed as source. A user builds `verse_host.dll` from
  their own licensed UE source checkout. Status: **done**.
- **R-DIST-2 (MUST NOT)** Ship, host, or attach any binary built from UE source — including in a
  release asset, a CI artifact, or the repository itself. `bin/verse_host.dll` is a local build
  product and stays git-ignored.
  *Rationale:* current UE terms permit distributing source derived from the engine but not
  binaries built from it to parties without their own license.
- **R-DIST-3 (MUST)** Documentation states plainly, where a user will see it before they build a
  game, that a game shipped with the host is subject to Unreal Engine's terms — including
  royalties — and that this is a property of the engine's licensing, not of this project.
  Status: **none**.
- **R-DIST-4 (MUST)** Building from a clean checkout is one documented command per binary, with
  a precise error when a prerequisite is missing — not a stack trace from UBT. Status: **part**
  (`build_host.py` exists and takes `--engine`/`UE_ROOT`; failure modes are not curated).
- **R-DIST-5 (MUST)** A version or ABI mismatch between the GDExtension and the host is reported
  at load as a single actionable sentence naming both versions and the command that fixes it,
  never as a crash or a silent no-op. Status: **part** (`VH_ABI_VERSION` is checked at `vh_init`;
  the message is not curated, and a missing DLL path is not distinguished from a stale one).

**End state — addon, gated on licensing.**

- **R-DIST-6 (MUST, blocked)** When the Verse toolchain is separately licensed such that
  redistribution is permitted, godot-verse installs the way any Godot addon installs: download,
  unzip into `addons/`, restart. No UE source checkout, no UBT, no Visual Studio, no engine tree
  on disk. Blocked on **OQ-1**.
- **R-DIST-7 (MUST, blocked)** In that state, prebuilt hosts are published for every platform in
  §3 that has one, and building from source stays supported for contributors and for platforms
  that do not. Blocked on **OQ-1**.
- **R-DIST-8 (MUST)** Whatever the compiler needs at runtime travels with the addon. Today the
  host must load from `Engine/Binaries/Win64` because VNI records each package's source directory
  relative to the loaded module and the compiler reads those `.verse` files at runtime
  (README, "Five constraints"). An addon that must sit inside an engine tree is not an addon.
  Status: **none**. **OQ-2** resolves this for the shipped half: the runtime host reads `.uasset`,
  not `.verse`, so it carries nothing from the engine tree. The editor host still does, and it is
  the one an addon user would install — so this requirement is unchanged for the case it was
  written about.
- **R-DIST-12 (MUST)** Nothing a project commits names one machine. Where this developer's Unreal
  checkout is, and which host and cooker binaries to run out of it, are properties of the machine,
  not of the project — a `project.godot` carrying them is one contributor's local state in every
  other contributor's clone, and it is rewritten under them the first time they open the editor.
  Status: **done**. `src/verse_host_paths.{h,cpp}` resolves each of the three in one order:
  the environment (`UE_ROOT`, `VERSE_HOST_DLL`, `VERSE_COOKER`), then the user's EditorSettings
  (`verse/host/engine_dir`, `dll_path`, `cooker_path`), then the project settings of the same
  names — which are read for projects that predate this, with a warning naming the move, and never
  written. Only the checkout is normally set; the two binaries are derived from it. The environment
  comes first because it is the only store a headless run has: `EditorInterface` does not exist in
  an editor binary running `-s`, which is how the integration layer runs, so `tools/run_tests.py`
  exports `UE_ROOT` into every Godot it launches instead of editing three `project.godot` files.

  `verse/host/enable_debugger` stays a project setting, because which debugger a project wants is
  a property of the project. An exported game reads none of the four: it derives everything from
  its own executable (`phase-7-design.md` D8).

**Exporting a game.**

- **R-DIST-9 (MUST)** Exporting a Godot project that uses Verse produces a runnable game through
  Godot's ordinary export dialog, with no manual copying of DLLs or engine directories. The
  export plugin collects everything the game needs. Status: **done, on Windows** — 7a built the
  export and 7b made the result run. An export runs the cooker, puts an IoStore container and a
  class sidecar in a `verse_data` directory beside the executable, carries the
  runtime host as a `.gdextension` `[dependencies]` row and strips every `.verse` to a stub, with no
  manual copying of anything. `run_tests.py`'s `export` layer exports `tests/integration`, asserts
  the whole tree, **launches it and asserts its counts** — 317 passed, 0 failed, 9 skipped — and
  `dodge-the-creeps` exported and run outside the repo passes all 30 of its checks
  (`by-hand-findings.md` B14). Windows only: no Linux or macOS export has ever been attempted
  (R-PLAT-1).
- **R-DIST-10 (MUST)** An exported game does not require the user who *runs* it to have anything
  installed. Status: **done, on Windows, with the limit named.** `dodge-the-creeps` was exported to
  a directory outside the repository and run with `UE_ROOT`, `VERSE_HOST_DLL` and `VERSE_COOKER`
  unset and `PATH` cut to `C:\Windows\system32;C:\Windows`: 30 checks, 0 failures, exit 0
  (`by-hand-findings.md` B14). The game finds its host, its engine directory and its container by
  where it is running and reads no setting and no environment variable. **What that cannot prove**,
  and the finding says so in its own words: a sandboxed run on the build machine cannot rule out a
  machine-wide dependency such as a VC redistributable. That needs a machine that has never built
  anything.
- **R-DIST-11 (SHOULD)** An exported game ships compiled Verse rather than `.verse` source plus a
  compiler. This is the difference between a game that ships a language toolchain in its data
  directory and one that ships a program; it also removes compilation from startup time and is a
  precondition for §3's mobile and web targets, where shipping a compiler is not viable.
  Status: **part**, and the two halves of it now have different answers.

  *No `.verse` source ships*, and the `export` layer asserts it by reading the `.pck`: every one of
  `tests/integration`'s twenty scripts is in the pack at exactly one byte. *No compiler ships* is
  true of the data directory and **false of the bytes beside it**, and cannot be made true from
  this side: `Solaris` lists `VerseCompiler` and `VerseVMCodeGen` in its own public dependencies
  unconditionally (`Solaris.Build.cs:14-29`) and `uLangUE` in its private ones, and Solaris is the
  module that runs Verse. Dropping all five from the runtime host's own list compiles, links, and
  leaves every one of them in the graph and the binary at 112.4 MB (`phase-7-design.md` §13.2).

  What the configuration buys instead: Shipping drops `ScriptDisassembler`, Solaris's one
  conditional dependency, and weighs **72.7 MB** against Development's 112.4. Whether a game gets
  it is not yet decided by anything — the `.gdextension` names one file — which is D12's remaining
  half, and is **out of Phase 7b by decision** (`phase-7b-design.md` D2). What 7b does flip is the
  first half from "nothing loads it" to "a game runs on it": the cooked Verse a game ships is only
  worth the name once it can be read back, and it is read back now.

  Measured on `dodge-the-creeps` (the machine R-PERF-2 names): the shipped Verse payload is
  **5.0 MB** — a 4.9 MB container and a 248 KB sidecar — against 7a's 68 MB of loose cook, and the
  container step costs **0.40 s** inside a **14.2 s** headless export.

---

## 3. Platforms

Two distinct questions per platform: can the *editor* run Verse there, and can an *exported game*
run Verse there. They are not the same requirement — the editor needs the compiler and the
analysis pipeline, the game may need only the VM (R-DIST-11).

| platform | editor | exported game | status |
| --- | --- | --- | --- |
| Windows x86_64 | MUST | MUST | editor **done**, game **none** |
| Linux x86_64 | MUST | MUST | **none** |
| macOS arm64 + x86_64 | MUST | MUST | **none** |
| Android arm64 | MAY | MUST | **none** |
| iOS arm64 | MAY | MUST | **none** |
| Web (wasm) | MAY | SHOULD, aspirational | **blocked** |

- **R-PLAT-1 (MUST)** Windows, Linux and macOS desktop are supported for both editor and exported
  games, at the same feature level. A feature that works on one desktop platform and not another
  is a bug, not a platform limitation, unless §14 records why.
- **R-PLAT-2 (MUST)** Android and iOS run exported games. Authoring on mobile is not required.
  *Known unknowns:* binary size of a monolithic UE Program target, and whether VerseVM's execution
  strategy is compatible with iOS's prohibition on JIT. Recorded as **OQ-3** — which OQ-2 has
  narrowed to the **runtime** host, carrying no compiler.
- **R-PLAT-3 (SHOULD, blocked)** Web export runs Verse. This is the stated ideal and it is
  currently not known to be reachable: UBT has no wasm Program target, and Godot's web export is a
  constrained single-threaded-by-default wasm environment. OQ-2 removed the third obstacle — the
  runtime host reads `.uasset`, not `.verse`, so it does not need a filesystem full of sources —
  but the first two stand. Blocked on **OQ-4**, and deferred to **Phase 7.5** by decision
  (`phase-7-design.md` D16).
- **R-PLAT-4 (MUST)** A platform that is not supported fails at export time with a clear message,
  not at game startup on a user's device. Status: **done** (Phase 7a) — `VerseExportPlugin::
  _export_begin` refuses `android`, `ios` and `web` with one sentence naming the platform. It also
  withholds the data directory, because `add_message(EXPORT_MESSAGE_ERROR)` reports without
  aborting the export (measured, `phase-7-design.md` §13.4), so the sentence at export time is
  what the author reads and a game that will not load is what they get if they ignore it.
- **R-PLAT-5 (MUST)** Nothing in the GDExtension assumes Windows. Status: **part**, and weaker
  than it reads — `verse_host.cpp` is a `GetProcAddress` loader with `#else return "unsupported
  platform"` and no `dlopen` path at all, so the portability is in the shape rather than in the
  code. **Out of Phase 7b** with the rest of Linux (`phase-7b-design.md` D2) — the `dlopen` branch
  itself needs no cross-toolchain and could land any time, but exporting to a platform that cannot
  load what it ships is not worth verifying, so it waits for the phase that does Linux.

---

## 4. The Verse language surface

The commitment is the *language*, not a dialect of it. Godot-specific additions are attributes
and packages, never syntax.

- **R-LANG-1 (MUST)** Classes, inheritance, abstract classes, interfaces and interface
  implementation work in user code, including a Verse script class extending another Verse script
  class. Status: **done.** Tested in Phase 2, which is what it needed: script-to-script inheritance,
  interfaces and interface dispatch all worked, and the testing found one hole on the *bridge* side
  rather than in the language. An `@export` declared by a base script class was invisible on the
  derived instance, in two places that made the same assumption — the export list harvested only a
  class' own members, and a field read built the shape key from the object's class rather than the
  class that *declares* the member. Both were true while the only thing above a script was generated
  API. Both now walk the chain, stopping at the first class outside the script package.
- **R-LANG-2 (MUST)** Structs and enums work in user code, and both are expressible at the Godot
  boundary where Godot has a counterpart: an enum member is `@export`-able as a Godot enum
  property, a struct member as a Godot struct or dictionary. Status: **part.** Both work in user
  code, nested structs included, and an enum member exports as a dropdown of its own enumerators
  (R-EXP-1). A **struct** member at the boundary is **part**: a project's own struct now crosses as a
  `signal` payload in both directions — out as one Godot argument per field, named by the
  field, and back in through `InstanceCall`'s packing rule and `WireToValue`'s user-struct branch
  (R-SIG-1). That path needs no Godot counterpart at all, because a signal delivers the fields as
  separate arguments and the host reassembles them.

  What is left is the general case: a struct as a method parameter or return value, and as an
  `@export`. Those need a Variant to *be*, and this requirement already names it — **a dictionary**,
  keyed by field name, which is GDScript's own idiom for a record and is order-independent where a
  positional encoding would misread every saved scene after a field was reordered, exactly as
  reordering a Verse enum does. Keep it separate from the signal path when it lands: a Dictionary is
  a reference on this wire since ABI v2 and costs a ref-table entry per crossing, which a signal
  emitted every frame should not pay. Tracked as `phase-4-gaps.md` G21.
- **R-LANG-3 (MUST)** Parametric types work — generic functions, generic classes, and the
  parametric spellings in `/Verse.org/Simulation`. Status: **part.** A parametric class in user code
  and a generic function over it both work, instantiated at two different types in one script, and
  the bridge itself now rests on one: `typed_array(t)`. What is still untested is
  `/Verse.org/Simulation`'s own parametric spellings, which nothing in the bridge reaches yet.
- **R-LANG-4 (MUST)** Failure contexts are fully usable: `<decides>` functions, `if`/`for`
  conditions, `option`, `?` and `or`, and user-authored failable functions. Status: **part** —
  the mirror uses `<decides>` where absence is real (`GetParent()` at the root, an editor
  singleton), and the semantics of a *freed* object were deliberately made a runtime error rather
  than a failure (README). That decision stands.
- **R-LANG-5 (MUST)** Arrays, maps, tuples and their comprehensions work, and each maps to a Godot
  `Variant` in both directions per §6.
- **R-LANG-6 (MUST)** A project is many files organised in directories, with real modules. A Verse
  file can import and use definitions from another file in the project; two files may define
  names that would collide at top level; shared library code lives outside any script.
  *Current state:* **done.** The third clause was done in Phase 2: a `.verse` with no class of its
  own is a library file rather than a broken script — it compiles, every other file in the project
  resolves its module-level definitions with nothing written to import them, and it reports no
  instance base type, so Godot refuses to attach it and says why. The first two clauses are Phase
  3's, and they are what modules bought: two files in different modules may declare the same name
  and both go on a node, uniqueness narrows from project-wide to per-module, and a file may declare
  any number of top-level names — only the one named after the file can be attached, which is a
  bridge rule now rather than a Verse one. **OQ-5** is closed (§14.1): the escape is submodules
  built from the project's directory tree, inside the one user package. Phase 3's
  design ([`phase-3-design.md`](phase-3-design.md) §2) settles which directories become modules,
  and it is **not** all of them: a directory is a module only if it carries a `<name>.vmodule`
  marker, and unmarked directories are organisational — their files join the nearest marked
  ancestor, defaulting to root. Epic's toolchain supports both models and this is the second of
  them; it is chosen because `res://` is an asset tree whose directory names are not Verse
  identifiers, and because it changes no existing project's meaning. Two rules follow: a file may
  declare **any number** of top-level names, only the one named after the file being attachable to a
  node; and uniqueness narrows from project-wide to **per-module**. One consequence is worth naming
  because it is silent otherwise: Godot asks `_get_global_class_name` from `EditorFileSystem`'s scan
  thread before anything is built, so a script's base class is resolved from *text* by matching the
  file stem across `res://` — which modules make ambiguous. The decision is to report the ambiguity
  and fall back to `Node`, then let the build supply the real base; the alternative, teaching the
  text scan to follow `using`, would create a second resolution path guaranteed to disagree with the
  compiler somewhere.
- **R-LANG-7 (MUST)** Transactional semantics have a defined meaning at the Godot boundary, and
  it is documented as a language rule rather than as an implementation note. Today: every Godot
  callback is invoked through `AutoRTFM::Open` and writes defer to `AutoRTFM::OnCommit`, so a
  failed Verse expression does not leave the scene half-written. Status: **done** mechanically,
  **none** as user-facing documentation.
- **R-LANG-8 (MUST)** Concurrency — see §7.
- **R-LANG-9 (MUST NOT)** Introduce syntax. Every godot-verse-specific thing a user writes is an
  attribute (`@export`, `@global_class`), a function, or a class in a package we ship. A user's
  Verse knowledge must remain valid Verse knowledge.
- **R-LANG-10 (SHOULD)** The subset of `/Verse.org` that is guaranteed to work is enumerated and
  tested. A pure-logic Verse module that uses only that subset and no engine API is portable to
  any other Verse host; we do not break that, and we promise nothing beyond it (§1.3).
- **R-LANG-11 (MUST)** Verse's indentation and formatting rules are respected by every tool we
  ship. Concretely: Verse rejects mixed tabs and spaces, Godot's script editor writes tabs, and
  anything we generate or reformat writes tabs. Status: **done**.

---

## 5. Godot integration

This is where parity is won or lost. Each item below is something a GDScript author does without
thinking about it.

### 5.1 Scripts and classes

- **R-NODE-1 (MUST)** A `.verse` file is a script, attachable to a node of the type its class
  extends. Status: **done**.
- **R-NODE-2 (MUST)** A script class is registered as a global Godot class name, usable from the
  node-creation dialog, from `class_name`-style references in other languages, and as an
  `@export` type. Status: **done** via `@global_class` — `_handles_global_class_type` and
  `_get_global_class_name` answer from the source text so the name survives without compilation.
- **R-NODE-3 (MUST)** A Verse class can be instantiated without being attached to a node — a plain
  object a script creates, holds, and passes around, including one that extends
  `RefCounted`/`Object` rather than `Node`. Status: **done** (Phase 4b stage 2).

  The spelling is Verse's own archetype and there is nothing to learn: `H := helper{}` where
  `helper := class(ref_counted)`. The whole of it is one `block:` clause on the native root, which
  runs per instance, runs for every class derived from it, and sees `Self` already at the derived
  type — so the host resolves the concrete Verse class, walks up to its nearest mirrored ancestor,
  and asks Godot for an object of that Godot class. `docs/phase-4b-design.md` §2 is the seven
  spikes behind that sentence and §4.5 is why the obvious alternative cannot work.

  **Lifetime is Godot's, not this bridge's.** A `ref_counted` and below dies with its last holder;
  an `object` is freed by hand; a `node` is owned by the tree once it is parented, and one that
  never enters the tree **leaks deliberately**, exactly as GDScript's `Node.new()` does — Godot's
  orphan report at exit names both the same way. Release rides on the collection that finds the
  Verse value unreachable, so a peer outlives its value by a cycle or two; `vh_object::BeginDestroy`
  is the hook, and it releases only what the host recorded as minted, because every object crossing
  *from* Godot is a `vh_object` too.

  **Two things a mirrored archetype now does that it did not.** `node2d{}` is a live Node2D rather
  than the handle of 0 it used to be, which closes that footgun by construction and takes with it
  the "reach through a handle of 0" idiom three fixtures used to spell a deliberate raise; the
  replacement is an archetype of a class Godot will not instantiate — `viewport{}` — which raises
  with a sentence naming the class. And an archetype of an abstract or singleton-only class is that
  raise rather than a silent dead object.

  Not reachable the other way: `MyVerseClass.new()` from GDScript. `new()` is bound on `GDScript`
  and `CSharpScript` and **not on `Script`** (`gdscript.cpp:1093`), so a GDExtension language has
  none; the Godot spelling is `RefCounted.new()` followed by `set_script(...)`. That is R-INT-1's
  business and a Godot limitation with a citation rather than a gap here.
- **R-NODE-4 (MUST)** A script's own static functions and constants are callable and readable.
  Status: **done** (Phase 4 stage 6).

  Verse has no `static` keyword. What it has is a **module**, and a module inside a script file is
  ordinary Verse: `PlayerStatics.MaxSpeed` and `PlayerStatics.Describe()` work with nothing from
  the bridge at all. So what was actually missing was the *link* Godot needs to answer
  `get_script_constant_map()` and `has_static_method()`, and `@statics` is it:

  ```
  @statics("player")
  PlayerStatics<public> := module:
      MaxSpeed<public>:float = 400.0
      Describe<public>()<transacts>:string = "the player"
  ```

  A **declared** association rather than a naming convention, and the interview's reason still
  holds: a typo in a convention produces a silently empty statics module where a module naming a
  class that does not exist can be told about. The one deviation from
  `docs/phase-4-design.md` §8.4 is that the class is named as a **string**: uLang hands back an
  attribute's *text* value (`GetAttributeTextValue`), and there is no equivalent for a `type`
  argument, so `@statics(player)` would have meant walking the attribute's own AST. The string is
  checked rather than trusted, so the failure mode the design was avoiding is avoided either way.

  **Both checks the attribute was chosen for now report**, at the module's own line: an association
  naming a class no script declares, and two modules claiming one class. They run once per analysis
  rather than from `GetClassStatics`, which is asked about one class at a time and so could never
  have seen the first of them. Without them the attribute bought nothing over the convention it
  replaced, which is worth saying plainly — the argument for it was entirely that a mistyped
  association is *checkable*.

  A constant's *value* is read out of the published package by its decorated path, the way an
  enum's name is built — so a class that has never been built reports its statics by name with no
  value, which is the same bargain an `@export` default already makes.
- **R-NODE-5 (SHOULD)** Abstract Verse classes report as abstract so Godot refuses to instantiate
  them. Status: **done** (Phase 4 stage 6). Verse has `class<abstract>`, so `_is_abstract` answers
  from the semantic program instead of returning false unconditionally.

### 5.2 Calling into a script

This is the sharpest single gap in the current implementation and everything in §5.3–§5.6 depends
on closing it.

- **R-NODE-6 (MUST)** Godot can call *any* method a Verse script defines, with any argument types
  §6 covers, and receive the return value. Status: **done for the types §6 currently carries.**
  `vh_instance_call` is one entry point over any signature; the three hardcoded names and the two
  call shapes are gone. Arguments convert against the declared parameter types
  `vh_class_method_list` reports, a `<decides>` method that declines is `VH_ERR_FAILED` rather than
  a missing method, and wrong arity is refused without running anything. **What is left is §6, not
  §5.2**: a method taking a `Dictionary` is uncallable because no `Dictionary` crosses, not because
  dispatch cannot reach it.
- **R-NODE-7 (MUST)** A Verse script overrides the full set of Godot virtuals its base class
  declares — `_enter_tree`, `_exit_tree`, `_input`, `_shortcut_input`, `_unhandled_input`,
  `_unhandled_key_input`, `_gui_input`, `_draw`, `_notification`, `_get_configuration_warnings`,
  `_to_string`, and the rest — not a curated list of three. The mechanism must be general: a new
  Godot virtual in a future engine version must not require a code change here.
  Status: **done** (Phase 4 stage 5), and all 1413 are emitted rather than most of them: the two
  return kinds that had no default body -- an object, and a parametric container -- both have
  one now, an option's `false` and the container's generated maker (`by-hand-findings.md` B37).
  All **1413** of `extension_api.json`'s virtuals are generated
  onto the class that declares them — `_Input` and `_Ready` on `node`, `_Draw` on `canvas_item`,
  `_GuiInput` on `control` — with a default body a script says `<override>` over, and Verse's own
  redeclaration rules supply the error when a signature is wrong. The mechanism is general by
  construction: a virtual a future Godot adds arrives by regenerating the mirror.

  **The name keeps Godot's leading underscore: `_Ready`, not `Ready`.** That was counted rather than
  preferred. A virtual collides with a *method or property* of the same PascalCase name 834 times,
  almost entirely on server-extension classes nobody derives from — survivable on its own. What
  decides it is the **eight** collisions with a **signal**: `Node.ready` vs `_ready`,
  `CanvasItem.draw` vs `_draw`, `Control.gui_input` vs `_gui_input`, `BaseButton.pressed` vs
  `_pressed`, `Range.value_changed`, `CollisionObject2D/3D.input_event`, `BaseButton.toggled` — on
  the classes an ordinary script derives from, and both are things a script touches. Godot's names
  are already disambiguated by the underscore; keeping it dissolves every collision of both kinds by
  construction. It is also what a Godot developer types in GDScript and what C# generates
  (`public override void _Ready()`), so R-AUD-1 is served rather than strained.

  The rename landed all at once — `demo/`, `tests/`, `tests/host_smoke/` and the yardstick in one
  commit, old names gone rather than deprecated. Pre-1.0, no compatibility obligation.

  **74 virtuals are skipped, each with a recorded reason.** An unoverridden virtual has to answer
  *something*, and there is no zero value to write for an object return or for a typed container
  (which would mint a Godot Array on every call Godot makes to a virtual nobody overrode). That is
  R-SCN-2's machinery, so the gap says so in the editor rather than being silently absent.

  **One of the 1413 has still never been exercised: `_CanDropData`.** The behavioural risk is
  confined to the handful whose *return value* the engine acts on, and those are covered —
  `_GetMinimumSize` through `Control.get_minimum_size()`, and `_HasPoint` by injecting a click with
  `Input.parse_input_event`, which makes the engine's own picking path ask. `_CanDropData` is reached
  only from the drag path, and `Viewport` gets there through `gui.target_control`, which
  `_update_mouse_over` leaves unset for a native window because the dummy display server sends no
  window-enter event — so no headless run can make the engine ask. `by-hand-findings.md` B10 and B11
  measure both halves; the second says how to check it with a window.

  Two bugs were found on the way and are worth recording. `GodotVirtualNameOf` turned `_Ready` into
  `__ready`, because it prepended a separator before every capital and Godot's name already starts
  with one. And `InstanceHasFunction` — which decides whether Godot puts the node in the process
  list at all — compared *function cells* to tell an override from an inherited body; a method is
  stored once per shape and `Bind` makes a fresh cell on every field load, so every method looked
  overridden. It compares **procedures** now, and walks the superclass chain rather than asking one
  fixed root, because the declaring class is `node` or `control` rather than the native root.
- **R-NODE-8 (MUST)** `_notification` reaches a script, with the notification constant, so
  `NOTIFICATION_WM_CLOSE_REQUEST` and friends are handleable. Status: **done** (Phase 4 stage 5),
  through the `notification_func` the vtable already carried and with **no new ABI**. It does
  **not** ride R-NODE-7's general mechanism, which was Phase 4's first assumption: `_notification` is absent from
  `extension_api.json` entirely — `Object` declares no virtuals there — so it is hand-declared on the
  native root, and the constants it takes are R-SCN-3's (`NodeStatics.NotificationReady`) until
  Phase 4 stage 6 lands them. See `docs/phase-4-design.md` §7.3.

  The rest of that set is **R-NODE-10**, and `_Get`, `_Set`, `_GetPropertyList` and
  `_ValidateProperty` are hand-declared on the native root beside it, for the same reason and
  reached the same way. `_ToString` is not among them and never was: see R-NODE-10.
- **R-NODE-9 (MUST)** A script method list (`_get_script_method_list`, `_get_method_info`,
  `_has_method`) reports what the script actually defines. Status: **done**. `vh_class_method_list`
  reads the class's own declarations out of the semantic program — names, parameters with their own
  names and types, result type, `<decides>`/`<suspends>`, and Godot's name for the virtual it
  overrides — and the script instance answers `has_method`, `get_method_list` and
  `get_method_argument_count` from it. A parameter of an object type also reports **the class it
  declares** (ABI 12.0), as does a result and a signal argument, because a `PropertyInfo` with no
  `class_name` is what Godot draws as `Object` however specific the declaration was. A script
  class Godot has not registered reports its nearest mirrored ancestor instead of a name nothing
  can resolve, which is the rule an exported member of that type already followed
  (`by-hand-findings.md` B32).
- **R-NODE-10 (SHOULD)** The script-level hooks Godot offers a script rather than registering in
  ClassDB are reachable: `_to_string`, `_get`, `_set`, `_get_property_list`, `_validate_property`.
  Status: **done** (Phase 4b stage 5). Named
  by Phase 4 once `_notification` proved that none of this family is in
  `extension_api.json` and so none of it can be generated. `_to_string` is what makes `print(node)`
  in GDScript show something a Verse author chose; `_get`/`_set` overlap §5.4's export machinery,
  which is why the set is scheduled with it (roadmap, Phase 4b) rather than with R-NODE-7.

  **`_to_string` is the one of the five that is not a Verse method at all, and that is decided.**
  Verse already has a spelling for "what this value prints as", so the bridge adds no `_ToString`
  virtual to the native root; an author writes the **extension method**, which is the idiomatic form
  and — measured — the only form the compiler accepts:

      (X:my_class).ToString<public>()<transacts>:string = "..."

  Both alternatives are refused, each by a different definition. A class *member* named `ToString`
  is glitch 3532 against `/Verse.org/Verse`'s own `ToString`; a module-level *overload* for the
  script's class is 3532 against **the mirror's** `ToString(:object)`, since every script class
  derives from `object` and Verse does not prefer the more specific overload. That second one is the
  bridge's own doing — `FREE_FUNCTION_REPLACEMENTS` maps Godot's `Object.to_string` onto it.

  The mechanism that makes this cover both directions is that **`X.ToString()` and `"{X}"` are not
  the same lookup**: interpolation desugars to the free `ToString(X)`, which reaches
  `ToString(:object)` and so Godot's `to_string()`. Implementing `to_string_func` therefore serves
  `print(node)` from GDScript *and* Verse's own interpolation, which arrives at the same override by
  going out to Godot and back. `tests/verse_probe/tostring_probe.verse` carries all five rounds.

  What the host has to look up is **module-level, not a class member** — which is why
  `vh_class_method_list` does not carry it and `vh_instance_call` cannot reach it unaided:
  `operator'.ToString'(:my_class, :tuple())`, receiver first and the call's own arguments as a tuple
  second. So it has its own entry point, `vh_instance_to_string` (ABI **8.5**), and the analysis
  records the name per class because an exported game has no semantic program to search in
  (sidecar **5**).

  Two things about that lookup were measured rather than derived, and both were wrong first time.
  The extension method is found in the class's **enclosing scope** rather than among its own
  definitions — a module's scope, so every file in the module sees it. And the key a `VPackage`
  holds it under is *not* a class's shape: it is the scope prefix wrapped around the function's
  **whole decorated name**, which already repeats that scope and carries the signature, so the
  parameters are part of the key and two overloads are told apart by them. Reusing a class's key
  found nothing, and the only symptom was `to_string` quietly keeping Godot's own text.

  **An extension method may be no more accessible than the type it extends.** A script class written
  the ordinary way is internal, so `(X:my_class).ToString<public>()` is uLang glitch 3593 — whose
  message talks about subpaths of `/user@localhost` and never says `<public>`. Omit the specifier.

  **The other four are ordinary Verse methods on the native root, with empty bodies and no new ABI
  entry point of their own.** They are `_Notification`'s shape exactly — Godot offers them to
  *scripts* rather than registering them in ClassDB, so nothing generates them — and they are
  reached through the same call path every other method takes:

      _Get<public>(Property:string):variant = variant{}
      _Set<public>(Property:string, Value:variant)<decides>:void = false?
      _GetPropertyList<public>():godot_array = MakeArray()
      _ValidateProperty<public>(Property:dictionary):void = {}

  `_Set` is the one bool among them, so it is spelled the way every other bool virtual is — a test
  rather than a value, `<decides>` and nothing else beside it. An override's arms already end in
  failable reads, so the `true` each used to close with was restating what the `if` had decided.

  **None of the four costs a script that overrides none of them anything**, which is the reason they
  can be declared on the root at all: `vh_class_method_list` reports a class's **own** declarations,
  an `<override>` is one and an inherited empty body is not, so the consumer's `resolve` finds
  nothing and never enters the VM. Without that, `_Get` would be a VM call on every property miss of
  every scripted node in the project.

  **None of the four carries an effect specifier**, for the reason `_Notification` carries none: the
  default set is the widest one, and narrowing here would narrow every helper an override calls, a
  file at a time (R-AUD-2). The consequence to know is the other direction — the hooks carry
  `no_rollback`, so a script's own `<transacts>` code may not call its own `_Get`. Godot is the
  caller, which is what the declaration is for.

  **How they compose with `@export` is Godot's rule, not a new one**: `_get` and `_set` are consulted
  only for a name the property list did not already carry, so an exported member is never routed
  through them and an `@export` and a `_Get` of the same name cannot race. `_GetPropertyList`'s
  entries are appended *after* the exports for the same reason.

  **What this stage had to build is `variant` on the script-call wire** (ABI **8.6**, a new
  `VH_TYPE_VARIANT` enumerator, which the policy at the top of the header makes a minor). `_Get` and
  `_Set` carry a value whose Godot type is not known until it arrives, and `variant` is the only
  thing in this bridge that can hold one — but before this it could be named only in a *native*
  declaration. Described as a user struct it asked Godot for 22 arguments per parameter; described
  as nothing it was refused before the call reached the VM. `VH_TYPE_VARIANT` is a **declaration**
  type and never a payload: the value crosses as whatever the variant holds, and what the type says
  to the consumer is "describe this to Godot as accepting anything", which Godot spells `Variant::NIL`
  with `PROPERTY_USAGE_NIL_IS_VARIANT`. No sidecar change — a declared type's number is already
  carried. The reward is general rather than local: **any** script method may now take or answer a
  `variant`.

  Two things a script writing these hooks has to know, both measured in
  `tests/verse_probe/hooks_probe.verse`. A class that does not derive from `object` has none of the
  four, and the diagnostic for that is glitch 3523 blaming the *access specifiers*. And Godot's
  `Variant::Type` numbers — which is what a property dictionary's `"type"` key wants — are not a
  script's to write: the mirror's `TagInt` and its 38 siblings carry no access specifier, so naming
  one is glitch 3593. The public spelling is the generated `ToInt(:variant_type)`, which is also the
  closer analogue of the `TYPE_INT` a GDScript author writes.

### 5.3 Signals

No script *declares* a signal today: `_has_script_signal` returns false and
`_get_script_signal_list` returns empty. Signals are how Godot programs are wired together, so this
is parity-critical. What already works is receiving one — a scene-file connection reaches a Verse
method with its arguments (R-SIG-4), which is what let the Dodge the Creeps port be wired at all.

- **R-SIG-1 (MUST)** A Verse script declares signals with argument types, and they appear in the
  editor's Node panel where a designer connects them. Status: **done** for the declaration and the
  list (Phase 4 stage 4); the Node panel itself is R-SIG-4's by-hand check. **Two member types
  declare one**, neither deprecated — see below and `docs/signal-declaration.md`, which carries the
  measurements and §11's table of which to reach for.

  **The member's name is the signal's name**, whichever spelling declares it, so there is no second
  place to spell it and nothing to drift.

  **`@export_signal` is what registers a member with Godot**, and it is required on both spellings —
  the same bargain `@export` makes for the inspector. Status: **done**.

  ```
  player := class(area2d):
      @export_signal
      Hit<public>:signal(tuple()) = signal(tuple()){}
      @export_signal
      Struck<public>:signal(tuple(int, string)) = signal(tuple(int, string)){}
  ```

  What the two member types differ in is what *silence* means, and the difference is the whole
  reason they are told apart rather than folded into one test. An `event(t)` is useful purely
  between Verse tasks, so one without the attribute is not a signal and not a complaint — it is
  absent from the list. A `signal(t)` has no purpose but Godot, so one without the attribute is far
  likelier to be a forgotten line than a decision: it is **listed and refused** with
  `VH_SIGNAL_NEEDS_ATTRIBUTE`, which puts the sentence at the member's own line instead of leaving
  the Node panel empty for no stated reason. `tests/integration/scripts/signal_rejects.verse`'s
  `Forgotten` is the fixture, well formed in every other way so that it tests the rule rather than
  the ladder above it.

  The attribute's *name* is not the obvious one, and that is a collision rather than a preference: a
  bare marker attribute is a class, the attribute package shares `/Godot.org/Godot`'s verse path so
  a script's existing `using` reaches it, and a third definition of `signal` beside `signal(t)` and
  its `signal()` alias is glitch 3532. It joins the `@export*` family instead, which is what it
  does. `docs/signal-declaration.md` §3 has the measurement.

  The attribute is read out of the semantic program rather than out of the text, so
  `vh_class_signal_list` refreshes per keystroke the way the method and export lists do — a bridge
  attribute is read from the *text* only where the text is the only source, which is `@global_class`
  and `@icon`, because Godot asks about files it has only scanned. A signal declared in a script that
  has never been built appears after the next Build, which is the same bargain an `@export`
  *default* already makes.

  **A member may also be declared as an ordinary `event(t)`**, so that it is the type Verse's own
  concurrency vocabulary is built on rather than a bridge type shaped like it. Status: **done**.
  Godot cannot tell the two apart — same signal list, same argument names, same reassembly inbound,
  same GDScript interop — and the choice between them is about what the *Verse* side gets:

  | reach for | when |
  | --- | --- |
  | `event(t)` | the member should satisfy `awaitable(t)` for Verse code that has never heard of Godot, or you want the spelling that ages into Verse's own `subscribable_event` when it ships |
  | `signal(t)` | you want the connection scoped to the wait, no bypass hazard, or `listenable(t)` conformance |

  Neither is deprecated, and that is a correction: the plan deprecated `signal(t)` and was wrong to,
  because it is the only one of the two that satisfies `listenable(t)`, keeps R-SIG-5's scoped
  connection, and cannot have its emission bypassed. `docs/signal-declaration.md` §11 has the whole
  comparison. The event spelling:

  ```
  player := class(area2d):
      @export_signal
      Struck<public>:event(struck_payload) = event(struck_payload){}
  ```

  The attribute is a bare marker with no argument — the name stays the member's, which is also what
  C# does (it registers under the delegate's own spelling, PascalCase and all). What does **not**
  move is the emit verb: `signalable.Signal` carries `no_rollback`, so a `<transacts>` body may not
  call it (measured, uLang glitch 3512, `tests/verse_probe/event_probe.verse`), and Godot has to be
  the dispatcher for a GDScript connection to fire at all. So the author emits with a bridge-owned
  `Struck.Emit(...)` and the field's own `Signal` is left alone. C# reached the same split for the
  second reason alone.

  `signal(t)` was described as shaped like Verse's own `listenable` **without implementing it**, and
  that was over-drawn: the reasoning is `signalable.Signal` being `no_rollback`, and `listenable` is
  `awaitable` + `subscribable` and does **not** extend `signalable`. **It implements `listenable(t)`
  now**, which gives a script's declared signals and all 503 engine-signal accessors the same
  interop without touching their connection model. `signal(t)` keeps its place as the accessors'
  return type, because a script's own signal always has a receiver while a foreign object's has none
  until someone asks — C# ships that same asymmetry, a local delegate list for declared signals and
  a real per-handler `Connect` for engine ones.

  **The payload is one type, and tuples carry arity above one.** A multi-parameter Verse function
  satisfies a one-tuple-parameter callback, because a function's parameter *is* its tuple, so
  `OnStruck(Damage:int, By:string)` subscribes to a `signal(tuple(int, string))` and Godot
  still sees two arguments. What Godot is told, per `docs/phase-4-design.md` §6.2:

  | payload | Godot arguments |
  | --- | --- |
  | `tuple()` | none |
  | a bare type | one, named for the type — `Int`, `Float`, `Text` |
  | `tuple(a, b)` | two, `Arg0` and `Arg1` |
  | a struct | one per top-level field, **named by the field** |

  Verse tuples cannot name their elements (`tuple(Damage:int, ...)` is "Expected a type, got data
  definition instead"), which is why the tuple names are positional — and why the struct row exists
  at all: it is the one spelling that carries names to the connect dialog and to what
  `_make_function` writes.

  **The struct row decomposes one level**, and a field that is itself a user struct is refused at the
  member (below), because Godot has no argument shape for a struct and there is nothing for a second
  level to flatten into. A field of one of the mirrored *math* types is fine — a `vector2` is one
  Vector2 argument.

  **It crosses both ways, and the two directions are not each other reversed.** Outbound the struct
  is taken apart into N arguments. Inbound Godot invokes the handler with those N while
  `Subscribe(Callback(:t))` declares one value, so the host builds the struct back: `InstanceCall`
  packs N arguments into one tuple when the single declared parameter is a user struct with N fields,
  and `WireToValue` constructs it from the semantic field list. Verse already reads a
  multi-parameter function as satisfying a one-tuple-parameter callback for the same reason — a
  function's parameter *is* its tuple — so this is that rule reaching the one spelling that carries
  names. A *direct* `Object::call` cannot do the same: Godot checks arity against the script's method
  list, which reports the one declared parameter, before the host sees the call at all.

  **A signal the bridge cannot carry is refused at the member**, reusing R-EXP-3's shape rather than
  failing at the emission — `vh_signal_desc` carries a `Reject` the way `vh_export_desc` does,
  `_get_script_signal_list` drops a rejected signal so Godot is never told about one nothing can
  emit, and `_validate` turns the reason into a warning at the member's own line. The four reasons,
  all decidable from the declaration:

  | `vh_signal_reject` | the declaration |
  | --- | --- |
  | `IS_VAR` | a `var` member — a signal is an identity, and the binding is minted once |
  | `NO_GODOT_OWNER` | a class that does not derive from `object`, so nothing ever hands it a handle |
  | `PAYLOAD_UNSUPPORTED` | an argument with no Godot type |
  | `PAYLOAD_NESTED_STRUCT` | a struct payload whose field is itself a struct |

  **A member's access level is not among them**, and `NOT_PUBLIC` is retired. It read as a fifth
  reason on the understanding that connecting is done from outside the class — but connecting is not
  done from Verse at all. A designer connects in the Node panel and GDScript connects by string name,
  and neither consults a Verse specifier; nothing else in the bridge tested one either, so a
  non-public `@export` member has always reached the inspector and a non-public method has always
  been callable from Godot. What a specifier governs is which *Verse* code may name the member.

  The consequence is worth saying plainly, because the specifier does not say it: **a registered
  signal is connectable and emittable by anything holding the node**, `<private>` included.
  `Object::emit_signal("Own")` from GDScript reaches it. The privacy is from Verse callers and from
  nothing else. `tests/verse_probe/signal_access_probe.verse` is the measurement, and
  `signal_shadow_probe.verse` is why binding by name stays unambiguous: the compiler refuses a member
  that shadows an inaccessible one of the same name (glitch 3593), so two members of one name cannot
  reach the list. The enumerator keeps its value until the next major ABI bump, because removing it
  renumbers the three codes after it.

  `NO_GODOT_OWNER` is the one with no GDScript counterpart, and the asymmetry is worth stating: every
  GDScript class extends Object, so every instance carries a signal table of its own and a
  `RefCounted` subclass can declare signals freely. A plain Verse class is a VM object with no Godot
  counterpart at all — there is no table to register on and no object to connect to.

  Emitting or subscribing to a refused signal reports its own reason rather than the generic "names
  nothing", because the editor warning is the only report a *tools* build makes and a game running
  outside one would otherwise get nothing useful.

  Signals **inherit**: a script class deriving from another has that class's signals, and both the
  list and the construction-time binding walk the whole chain. Phase 2 shipped exactly this bug once
  already, for `@export` on a base script class.
- **R-SIG-2 (MUST)** A script emits a declared signal with arguments. Status: **done** (Phase 4
  stage 4). `Hit.Signal(())`, `Struck.Signal((9, "spike"))`. Under R-SIG-1's `@export_signal` spelling the
  verb is `Hit.Emit(())` — a bridge-owned extension method on `event(t)`, because the field's own
  `Signal` carries `no_rollback` and is refused from the transaction a Godot callback runs in.
  Either way the emission reaches Godot rather than the Verse event directly, which is what makes a
  GDScript connection fire.

  **Emission is immediate, and that is a stated exception.** Every other void mutation in the mirror
  defers to `AutoRTFM::OnCommit`, which is what makes `<transacts>` literally true for 6813 methods.
  Emission does not: handlers run synchronously, as they do in GDScript, so "emit, then read what the
  handler changed" behaves the way a Godot author expects and a Verse emission is indistinguishable
  from a GDScript one. The cost is stated rather than hidden — if the emitting transaction later
  aborts, the handlers have already run. It joins the container write as the second member of the
  set **Phase 4.5** audits.

  Underneath, a `signal(t)` member is bound at construction: the host walks the class's data
  members for the ones whose declared type reaches the native `vh_signal`, mints an id per member
  per instance, and writes it into the member's own object exactly where `Handle` is written. The
  payload's decomposition is read off the *instantiation* — the declared type comes back as the
  generic `signal(t)`, and the type argument is on it as a substitution table.
- **R-SIG-3 (MUST)** A script connects to any signal on any object, with a Verse function or
  closure as the target, and disconnects. Status: **done** for a Verse function bound to a script
  instance, which is the only shape 4a accepts (R-TYPE-3 says why, and OQ-16 carries the rest).

  `Hit.Subscribe(OnHit)` answers a `cancelable`, and `Cancel()` disconnects and is **idempotent**,
  as `event_subscription::Cancel` is in UEFN. Subscription goes **through Godot**, not through a
  list on the Verse side: it costs a Callable per subscription and it is the only arrangement in
  which a signal emitted from GDScript reaches a Verse subscriber, which R-SIG-6 requires. Two
  subscriptions of one handler are two connections with two independent cancels, because equality
  is by reference. `Subscribe` takes no flags in 4a; one-shot belongs with `Await` in Phase 5.

  **Godot's own 489 signals are generated too**, one accessor per signal per class, as C# generates
  an `event` per engine signal: `Timer.Timeout().Subscribe(OnMobTimerTimeout)`. A method rather
  than a data member, for the reason C# gives — a mirror wrapper is built per crossing, and a
  member would have to be filled on each one. The payload follows the same rule a declared signal's
  does, and the payload *type* is read off the accessor's own return type, so nothing had to be told
  what a Godot signal carries. The eight accessor names that would have collided — `Node.ready`,
  `CanvasItem.draw`, `Control.gui_input`, `BaseButton.pressed` among them — collide with a
  **virtual**, and R-NODE-7's underscore dissolves all eight, so no signal needed an invented name.
  This is also what makes Phase 5's `Timer.Timeout().Await()` a spelling rather than a wish.

  **`Subscribe` is compensated**, where emission is immediate and every other mutation defers. It
  mutates Godot *and* returns a value, so it can be neither queued for commit nor ignored, and the
  native registers an `AutoRTFM::OnAbort<SameAsClosed>` to disconnect — without it a failed
  transaction leaves a live connection the script believes it never made. It is the only rollback
  compensation in the host and the shape to copy for anything later that mutates Godot and cannot
  defer.

  Phase 4 wrote it as `Verse::Stm::OnRollback` and Phase 4.5's S-3 measured that doing nothing:
  that is the Solaris *interpreter's* STM, and `VerseStm.h` says "Noop if StmActive() returns
  false". `SameAsClosed` is the load-bearing half of the replacement — every Godot callback reaches
  C++ inside `AutoRTFM::Open`, and a plain `OnAbort` from open code is documented to be ignored.
  `tests/integration` aborts a `Subscribe` three ways and checks the connection came back.
- **R-SIG-4 (MUST)** A connection made in the editor to a Verse script's method works — including
  the editor's "connect and create the function for me" flow, which is what `_make_function` is
  for. Status: **part**, and the part that works is the load-bearing one. The Dodge the Creeps port
  is wired by eight connections in its scene files — five `Timer.timeout`s, the player's own
  `body_entered`, `StartButton.pressed` and `screen_exited` — and every one reaches its Verse
  method, with the signal's arguments marshalled to the declared parameter types.

  Two of the ten it used to carry are gone, and their going is Phase 4's: `Area2D.body_entered` and
  `Button.pressed` were each wired to *two* scripts because neither script could declare a signal
  of its own. They are now the player's `Hit` and the HUD's `StartGame`, subscribed to from code.
  The rest stayed deliberately: connecting a node's own signal through the Node panel is what a
  Godot author does, and it keeps this path covered.

  `_make_function` is **built**: "Make Function" writes a tab-indented `<public>` handler whose
  parameters carry the Verse spelling of each argument's type, and `<transacts>` on the signature,
  which is load-bearing rather than decoration — `Subscribe` fixes its callback at that effect, so a
  specifier-less stub would hand the author wall 8 on their first generated line.

  What is still untested is the **editor-side flow**, and it is untestable here rather than merely
  untested: `_make_function` is a `ScriptLanguageExtension` virtual with no ClassDB entry and
  `Script.get_language()` is not in the public API, so GDScript can reach neither — calling it by
  name answers "Nonexistent function", measured. The editor's own C++ is its only caller. Both it
  and the Node panel connection have since been watched by hand and work; the stub it wrote did not
  compile the first time, which is `by-hand-findings.md` B3.
- **R-SIG-5 (MUST)** A script `await`s a signal from a concurrent context: the Verse spelling of
  GDScript's `await button.pressed`. Status: **done** (Phase 5). `signal(t)` holds a
  `/Verse.org/Verse` `event(t)` and `Await<public>()<suspends>:t` forwards to it, which is ordinary
  parametric Verse with **no native on the Verse side and no parametric ABI**. One method covers a
  script's own declared signals and all 489 mirrored engine-signal accessors alike, because every
  Godot signal is already a mirrored accessor of that type:

      MessageTimer.Timeout().Await()
      GetTree[].CreateTimer[1.0].Timeout().Await()
      Hit.Await()                                    # the script's own

  **The connection lives exactly as long as the wait.** `Await` connects to Godot with
  `CONNECT_ONE_SHOT`, holds the signal object for the duration, and takes the connection away in a
  `defer` — which runs whether the task resumed or was cancelled, by `race`, by the node being
  freed, or by a rebuild. `tests/integration` asserts the connection count is back to zero on both
  sides of a race.

  **That stays true everywhere a `signal(t)` is awaited** — a script's own declarations and all 503
  engine accessors alike — because `signal.Await` is the bridge's own method and `VhSignalAwait` is
  the hook it connects from. The one case that cannot work that way is R-SIG-1's `@export_signal` spelling:
  a bare `event(t)`'s `Await` is Verse's native, so there is nothing to connect from and the member
  gets **one connection held for the instance's life** instead, made at the first entry into the
  instance. Not earlier: until Godot has installed the script instance on the object it refuses the
  connect as naming a nonexistent signal, and that point is after `_instance_create` has returned,
  which is not a moment this side can name. First entry is early enough because a Verse awaiter can
  only exist once Verse code has run on the object. That is the only place the property above is
  traded away, and it is traded for the only thing that buys it.
  `docs/signal-declaration.md` §7 has why two mechanisms is the right answer rather than an
  inconsistency, and C#'s prior art for the same split.

  **How an emission reaches the event** is the host's half and is smaller than the design budgeted
  for: `verse::event` is a UObject with a public C++ `Signal`, so the host reads the event off the
  signal object and signals it directly, and Epic's own code does the FIFO resumption and the
  per-task content scope. The payload is rebuilt from the emission's Godot arguments against the
  same shape the signal descriptor was generated from, so a tuple payload comes back a tuple and a
  struct payload comes back a struct.

  **One line of R-SIG-1's surface changed with it**, and it is a correction rather than an addition:
  `signal.Subscribe`'s callback is now specifier-less, matching Verse's own
  `subscribable<native>(t:type) := interface: Subscribe<public>(Callback(:t):void)<transacts>`.
  Phase 4's `Callback(:t)<transacts>:void` was chosen on the reading that Verse's own fixes its
  callback at a domain that could not touch Godot; the lattice is the other way up — the default
  effect set *contains* transacts — and the narrower spelling was what stopped a handler from
  starting a task, which is what a game-over sequence is. Measured: an existing `<transacts>`
  handler still satisfies the widened parameter, so no script broke.
- **R-SIG-6 (MUST)** Signals declared in Verse are connectable and emittable from GDScript and C#
  with no knowledge that Verse is involved (§8). Status: **done for GDScript** (Phase 4 stage 4).
  `Object::connect` validates against `has_script_signal` and `emit_signalp` refuses a name neither
  ClassDB nor the script knows, so answering `_has_script_signal` and `_get_script_signal_list` is
  the whole of it: GDScript connects to `Hit` by name and receives it when the Verse script emits.
  The reverse direction works too, through the same Callable: a Verse handler runs when a GDScript
  object emits. **C# has never been run** — that is OQ-17, which this requirement is one of the
  four that assert it.

### 5.4 The inspector and the editor's data model

- **R-EXP-1 (MUST)** `@export` covers the surface GDScript's `@export` covers: every exportable
  type, ranges, enums, flags, file/dir pickers, multiline text, node paths, resource types,
  groups, subgroups and categories. Status: **done for the set an author reaches**, with the
  deliberate omissions named below — `@export_group`, refusal of types the inspector cannot draw,
  and range constraints derived from the Verse type's own `where` clause so the slider and the type
  cannot disagree (README).

  **The rule is type-driven where the Verse type can say it, and an attribute only where it
  cannot.** Everything a bounded number, an enum or a mirrored class implies is read off the
  declaration and needs nothing written. Phase 4b stage 6 added the five where the declared type is
  `string` or `int` and says nothing about what the value is for — the only place an attribute earns
  its keep:

  | attribute | the hint it becomes | the type it describes |
  | --- | --- | --- |
  | `@export_file("*.png,*.jpg")` | `PROPERTY_HINT_FILE` | `string` |
  | `@export_dir` | `PROPERTY_HINT_DIR` | `string` |
  | `@export_multiline` | `PROPERTY_HINT_MULTILINE_TEXT` | `string` |
  | `@export_flags("Fire,Water,Earth")` | `PROPERTY_HINT_FLAGS` | `int` |
  | `@export_node_path("Node2D")` | `PROPERTY_HINT_NODE_PATH_VALID_TYPES` | `string` |

  Each hint string is already in Godot's own spelling and passes through untranslated — the flags
  names are comma separated because that is what `PROPERTY_HINT_FLAGS` wants, and the file filter is
  `*.png` because that is what `PROPERTY_HINT_FILE` wants. Each takes **one string or none**, for
  the reason `@rpc` does: the toolchain refuses a several-argument attribute today and says so in
  words that read as unfinished (R-EXP-9 carries both refusals and what to move to when either
  lifts). `@export_flags` is the one that would most obviously want several, and one comma-separated
  string is what Godot wanted anyway.

  **The pairing is checked, because nothing else could check it.** The attribute exists precisely
  because the type says nothing, so `@export_flags` on a `string` is a mistake with no other
  detector: the attribute compiles, and Godot draws a plain field — which is also what no attribute
  at all draws. It is `VH_EXPORT_HINT_WRONG_TYPE`, refused at the member with a sentence naming the
  attribute the author wrote, and the export carries a new `vh_export_hint` value per attribute
  (ABI **8.8**, a minor: an older consumer that does not know one draws the plain field it drew
  before the attribute existed).

  **Deliberately not in the set**, and recorded so the omission is visible rather than looking like
  an oversight: `color_no_alpha`, `exp_easing`, `global_file`/`global_dir`, `placeholder_text`, and
  enum-on-`string`. They are real parts of GDScript's surface and none is reached by a first
  project.

  **Enums** landed in Phase 2, pulled forward with R-SCN-5, and for Godot's own as much as for a
  script's: an exported `node_process_mode` is a dropdown of the enumerators, and what is stored is
  the ordinal — the same thing GDScript and C# store, carrying the same trap that reordering
  enumerators reinterprets scenes already saved. Consistent because the enumerator is the identity on
  the Verse side: whatever the ordinal names, `ToInt` of it is the number Godot receives.

  Making that testable turned up a hole with nothing to do with enums: a *live* script instance's
  `get_property_list` was a stub, so an exported member was invisible to `Object.get_property_list`,
  to `PackedScene.pack` and to anything reflective, even though get and set worked. The inspector
  never noticed because a non-tool script is drawn from a placeholder, which was told. It now answers
  with the same list the placeholder gets.
- **R-EXP-2 (MUST)** An exported value edited in the inspector persists into the scene and is
  present when the script runs. Status: **done**.
- **R-EXP-3 (MUST)** A member that cannot be exported is reported with a reason, at the member,
  not silently dropped. Status: **done**.
- **R-EXP-4 (MUST)** The inspector reflects a change to a script's exports without restarting the
  editor; a changed *default value* lands at the next build, per §10's trigger. Status: **done**,
  and the clause above is a restatement rather than the original — which said a changed default
  "requires code generation and therefore a restart".

  **The restart was already gone and the clause was what had gone stale.** The export *shape* — the
  member list, the hints, the Reject reasons — refreshes per keystroke, because
  `vh_class_export_list` answers from the snapshot the last analysis left. What waits is a changed
  default, and it waits for a **build** rather than a restart, because a default is generated code
  and §10's trigger is build-on-Play-or-Build-action. That is the same bargain C# makes, and it is
  a decision rather than a limitation — which is why this is recorded as a restatement with a reason
  rather than as a quiet edit to a MUST.
- **R-EXP-5 (MUST)** `@tool` scripts run in the editor: gizmo drawing, procedural generation,
  scene validation, `_get_configuration_warnings`. Status: **part**. `@tool` exists — a bridge
  attribute beside `@global_class` and `@export`, read out of the text the same way because Godot
  asks `is_tool` of scripts it has only scanned — and `VerseScript::_can_instantiate` already did
  the rest, handing the editor a real instance for a tool script and a placeholder for everything
  else. So `_Ready` and `_Process` run in the editor. The editor-only virtual surface came with
  R-NODE-7 rather than being built twice for one attribute: `_GetConfigurationWarnings` and the
  gizmo virtuals are among the 1413, so what would have been a feature is a *declaration* that
  needs no code of its own. What it is **not** yet is a test. `_GetConfigurationWarnings` is
  exercised only by direct call on a non-tool script, which proves the method resolves and says
  nothing about the editor acting on it. That check needed a window and has been done: the warning
  triangle and its tooltip appear on the node and clear with the condition (`by-hand-findings.md`).
  A tool script runs the **last built** generation, per §10's trigger — the same bargain a C# `[Tool]`
  script makes today, and the workflow most likely to send an author looking for the Build action.
  **Stated risk:** Verse now runs against the scene the author is editing, and R-DIAG-3 does not
  land until Phase 6 — the defect recorded there, where a raised runtime error empties every later
  method result, is one a tool script can now reach without running the game.
- **R-EXP-6 (MUST)** A Verse class can be a custom `Resource`, saved to and loaded from `.tres`,
  with its exported properties serialised. Status: **done at runtime; the editor half is a by-hand
  check** (Phase 4b stage 3).

  It needed no code. `_get_instance_base_type` already answered `Resource` for a `class(resource)`,
  `_instance_create` already took any `Object *`, and the export list, the property path and
  `get_property_list` were already keyed on the class rather than on nodehood — so the stage that
  `phase-4b-design.md` §13 called "most likely to cost more than it looks" cost seventeen test cases
  and nothing else. What it did need was R-NODE-3 underneath it: a Resource is a `RefCounted`, and
  the block clause on the native root adopts the object Godot made rather than minting a second.

  Asserted in `tests/integration`, in the editor run and in an exported game both: a `Resource`
  with a Verse script attached, its exported defaults read, written, saved to `.tres`, loaded back
  with its values and its methods intact, plus a `.tres` the project **ships** — which is the only
  shape that says anything about an export, where the `.verse` it names is a one-byte stub and
  `ext_resource path=` resolving at all is the claim.

  **A non-`@tool` Verse resource is a placeholder in the editor, and that is parity rather than a
  gap.** `_can_instantiate` gates on `is_editor_hint()`, which stands in for GDScript's own
  `ScriptServer::is_scripting_enabled()` — the editor sets it false (`editor_node.cpp:8476`), so a
  non-`@tool` GDScript resource is a placeholder there too. The inspector edits the placeholder's
  stored values and `ResourceSaver` writes them, which is what placeholders are for.

  The five editor steps — the New Resource dialog, the inspector, and a save from it — are in
  `by-hand-findings.md` under "R-EXP-6's editor half", because nothing a script can ask reaches any
  of them. Out of scope, as §5 scoped it: binary `.res`.

  **A Verse resource held as an `@export` of another class works, and §5 was wrong to defer it.**
  That is `VH_EXPORT_HINT_SCRIPT_CLASS`, and it was broken for any class inside a module: the
  descriptor carries the module-qualified name every `ClassNameUtf8` in the ABI carries, and the
  inspector filters a slot by the name *Godot* knows the class as — which is flat, because ClassDB
  has one namespace and `@global_class` registers the file stem PascalCased. The two disagreed and
  the editor said *"Cannot get class 'Gameplay/myResouce'"*. Reported by hand against a real project;
  `by-hand-findings.md` B18. The consumer now takes the leaf of the qualified name, and the case is
  asserted against `Script.get_global_name()` rather than against a constant, because the invariant
  is that the two agree.

  **A class that is not named after its file cannot register at all** -- Godot collects one global
  class per script *path* -- so a member typed as one has no name to filter its slot by. What
  GDScript does in that position is measured in `property-export.md` §"A second class in one file",
  along with the staged plan; the short version is that its inner classes lose the class entirely on
  save, so parity is the export hint and the write check, not the serialisation. The hint falls back
  to the nearest mirrored Godot class, the write is refused by class on both ABI paths, and an inert
  `@global_class` is now a `_validate` warning instead of silence.

  **The rule that follows, which is R-EXP-6's boundary: a Verse class that is to be authored as a
  `.tres` or persisted inside one lives in its own `.verse` file.** Measured rather than asserted --
  a second class's member saves as an *empty* sub-resource and reads back as the empty option, where
  a mirrored member beside it (`?gradient`) round-trips intact. The reason is structural and is what
  makes the rule non-negotiable rather than stylistic: a Verse object's members live in the VM and
  its Godot peer carries none of them, so what bridges the two is *being a script* -- and by
  R-LANG-6 only the class named after the file can be one. A second class's peer is a bare
  `Resource` with nothing on it to write. Stage B's warning says this at the attribute, and the one
  alternative -- addressing a second class as `res://x.verse::second` so it could carry a script --
  was spiked and is dead: `::` is how Godot spells "internal to a file", so such a path can be
  loaded and never referenced (`property-export.md` §"Stage C").
- **R-EXP-7 (MUST)** A Verse script can be registered as an autoload singleton. Status: **done in a
  running game; the two editor-side halves are by-hand checks** (Phase 4b stage 4).

  It needed no code, for the second time in this phase. Godot's rules decide the whole of it and the
  bridge already satisfied them: `_create_autoload` refuses a script whose `get_instance_base_type()`
  is not a Node (`editor_autoload_settings.cpp:354-355`), and `VerseScript` has answered `Node` for a
  `class(node)` since Phase 2 -- including in an export, where reading it off the stripped source
  would fail and `vh_class_base_type` answers instead (ABI 8.4, added by stage 3).

  `tests/integration` registers `scripts/game_state.verse` as `GameState` and asserts it in the
  exported run: the singleton is in the tree, carries its script, answers its methods, and a write
  through it is what the next lookup reads. Plus the structural half of "from every scene" -- it is a
  child of the *root*, beside the current scene rather than inside it, which is what a scene change
  does not touch.

  **The editor-side run cannot test any of that, and not for a Verse reason**: `--script` replaces
  the main loop before Godot sets up any autoload, so under the test driver `/root` has no children
  at all -- measured, and not even this suite's own `VerseExportCheck` is there. Those five cases are
  skipped with that reason and counted, which is the same discipline the export-side skips follow,
  pointing the other way.

  What the gate refuses is asserted through the predicate itself rather than by naming a bad autoload
  in `project.godot`, which would stop the project rather than test it: a `class(resource)` reports a
  base type `ClassDB::is_parent_class(..., "Node")` rejects.

  Two halves are by-hand and are in `by-hand-findings.md`: that the **editor's own dialog** refuses a
  non-Node class with Godot's sentence rather than crashing, and that a **`@tool`** autoload is
  instantiated in the editor too -- `in_editor` is `scr->is_tool()` (`editor_autoload_settings.cpp:390`),
  and `is_editor_hint()` is false in every headless run, so nothing automated can reach either.
- **R-EXP-8 (SHOULD)** A script declares an editor icon. Status: **done** (Phase 4b stage 7):
  `@icon("res://art/player.svg")` over the class, and `_get_class_icon_path` answers it.

  **Read out of the source text rather than from the host**, which is the whole of what this cost:
  Godot asks `get_class_icon_path` of a script it has merely *scanned*, from the filesystem thread,
  before anything has been built — the same question `_get_global_class_name` answers and for the
  same reason, so `verse_scan_class_decl` reads both. The attribute is still declared in the runtime
  attribute package, because a script carrying one has to compile.

  **Its editor half is a by-hand check and always was**, and its *testable* half is not where a
  reader expects. `Script::get_class_icon_path` is a pure virtual with no ClassDB entry — its one
  caller is `EditorData::get_script_icon_path` — so GDScript can no more call it than it can call
  `_make_function`, and no integration case can assert it. What the units layer asserts instead is
  the scanner, which is where the logic is: the path, an `@icon` on another class in the same file,
  a bare one, and one whose argument is not a literal. Whether Godot *draws* it is the editor
  session's, which no headless run could have seen anyway.
- **R-EXP-9 (SHOULD)** `_get_rpc_config` reports RPC annotations so a Verse script participates in
  Godot's high-level multiplayer. Status: **done** (Phase 4b stage 6), except for the half that
  needs two processes — see below.

  **Receiving** is `@rpc` on a method, and the config it builds is Godot's own shape exactly:
  `Script.get_rpc_config()` answers a Dictionary keyed by method name, each value carrying
  `rpc_mode`, `call_local`, `transfer_mode` and `channel` with Godot's own numbering. That shape is
  written down nowhere but in the code that reads it — `SceneRPCInterface::_parse_rpc_config` — and
  it is bound in ClassDB, which is what makes the whole receiving half assertable from a test
  without a second peer.

      @rpc("any_peer call_local unreliable_ordered 2")
      TakeDamage<public>(Amount:int)<transacts>:void = ...

  **All four of Godot's arguments are one string, and that is a compiler constraint rather than a
  preference — a *temporary* one, by both of the compiler's own accounts.** Two separate things in
  the toolchain refuse the four-argument spelling today, and each says in its own words that it is
  unfinished rather than decided:

  - an attribute site *references* its constructor before calling it, and Verse refuses to reference
    an overloaded function at all — *"Referencing an overloaded function without immediately calling
    it is **not yet implemented**"*, naming every candidate — so four arities of `rpc` cannot exist;
  - `GetAttributeTextValue` refuses any attribute whose argument is a `MakeTuple`, which is every
    attribute of more than one argument. The comment above it is `@HACK: SOL-972, We need full
    proper support for **compile-time evaluation of attribute types**`.

  **So this is a thing to come back to.** When a future engine drop lands either fix — overloaded
  function references, or compile-time attribute evaluation — `@rpc("any_peer", "call_local",
  "reliable", 2)` becomes writable and is the spelling to move to, because it is GDScript's and
  because the compiler would then check the argument *count* and the channel's *type* where the
  bridge checks them by hand today. The change is additive on the authoring side: the words already
  separate on spaces *or* commas, so a GDScript author writing Godot's own `"any_peer",
  "call_local"` inside one pair of quotes already gets what they meant, and the parser that reads
  them would keep working unchanged beside a tuple-reading one. The same constraint governs every
  multi-argument attribute this bridge might add, `@export_flags` included — see R-EXP-1.

  **There is no bare `@rpc`** either, for the sibling reason: an attribute with no argument has to be
  the attribute *class*, and the class is what the constructor builds; the two cannot share a name.
  GDScript's default is spelled out instead, as `@rpc("authority")`.

  The words are matched rather than positional and a number among them is the channel. Godot's
  defaults — authority, not call-local, reliable, channel 0 — are applied in the **host**, so that
  there is one statement of what a partial `@rpc` means rather than two that can drift. A
  configuration the bridge refuses is **dropped** rather than registered with whatever survived
  parsing, and `_validate` says why at the method's line: an `@rpc` with a misspelled word is a
  method the author believes is remote-callable, and half-applying it would make that belief nearly
  true, which is worse than not at all.

  It rides a new entry point, `vh_class_rpc_list` (ABI **8.7**), rather than fields on
  `vh_method_desc` — a new entry point is a minor under the header's own policy where a struct's
  layout is not — and a sidecar field (version **6**), because an exported game has no semantic
  program to read an attribute out of.

  **Sending** needed nothing of its own: `Node.Rpc` and `Node.RpcId` are two of the 15 vararg
  methods the same stage generated, and `Callable.Rpc`/`Callable.RpcId` two of the six that ride
  `VhRefCall`.

  **What is not asserted is the call that arrives at a second peer**, which needs two processes.
  A single-process run can see that the config is what Godot reads and that a send leaves Verse and
  comes back as one of Godot's Error ordinals, and it cannot honestly see more: with no peer
  connected the editor-side driver and an exported game stop at different guards inside Godot, for
  reasons that are Godot's rather than this bridge's. That last half is a by-hand check.

### 5.5 The scene

- **R-SCN-1 (MUST)** The full node API is reachable: `GetNode`, `FindChild`, groups, `AddChild`,
  `QueueFree`, signals, `SceneTree` access, `ChangeScene`, instantiating a `PackedScene`.
  Status: **done**, to the limit of what the phases below it allow. All **1023** classes are
  emitted, Godot's own `Object` among them, and `gen_verse_api.py`'s type table no longer skips a
  single method for a type it cannot carry — `unsupported_type` is **zero**. Of what remains
  unreachable, every entry is one of the categories R-SCN-2 permits: `virtual` (1413, R-NODE-7,
  Phase 4), `static` (114, R-NODE-4, Phase 4), `vararg` (15, no requirement), a raw C pointer (3,
  which GDScript cannot call either), and the property skips, each of which leaves Godot's own
  getter and setter standing.

  A subset was chosen against, with the measurement behind it in `phase-2-design.md` §3: adding a
  class to the mirror means rebuilding `verse_host.dll`, which means a UE source checkout, so a
  curated list is a wall rather than a setting.
- **R-SCN-2 (MUST)** Every Godot class and every method on it is reachable, or the reason it is
  not is recorded per-method and surfaced to the user rather than silently missing.
  *Rationale:* "the method I need isn't there and I can't tell why" is the failure mode that ends
  adoption. Status: **part.** The coverage side is done and has a bar: the permitted skip categories
  are named exhaustively — `virtual`, `static`, `vararg`, `unmarshallable_pointer`, and the property
  skips that leave Godot's own accessors standing — and any *other* skip is a defect. That makes the
  requirement testable where "a reason is recorded" alone did not: 8984 recorded reasons would have
  satisfied the old wording. `unsupported_type` is zero today, and a new one after a Godot version
  bump is impossible to miss.

  The surfacing is done too, so the status is **done**. A script that names a member the mirror does
  not carry gets the compiler's own diagnostic with a sentence appended: *"Godot has
  Node2D.get_position, but it is reachable as the property `Position`."* Both of Verse's wordings are
  answered — `Unknown member X in Y`, and `Unknown identifier X` for the unqualified call written
  inside a class body, which is the spelling an author reaches for first. For the unqualified one the
  class is not known, so the answer is given only when every class that skips that name skips it
  alike; where they disagree it says nothing, because a confident wrong class is worse than silence.

  In the editor rather than in a report file, because a report file in this repository is read by
  whoever wrote the generator and by nobody else.

  **The math types were the largest hole in that promise and are now inside it.** They are ordinary
  Verse rather than a mirror over an ABI (OQ-11), so `tools/gen_verse_api.py` never enumerated
  `builtin_classes[*].methods` and 367 methods and 261 operators across the sixteen types were absent
  with nothing recorded. The generator now **reads `host/Verse/GodotMath.native.verse`** to find out
  what is written and records everything else as `math_not_written` / `math_operator_not_written` —
  585 rows when it landed, against 43 written, and 410 today after the math was written. Reading the
  file rather than maintaining a list is the whole
  point: adding a method makes its skip disappear on the next generation, so the record cannot drift
  from the code. The sentence says the truth about these, which is different from every other skip:
  not "the bridge cannot carry this" but "nobody has written it yet, and here is the file it goes in".

  Resolving one needed a second path in the editor: `ClassDB` has never heard of `Vector2` — it is a
  Variant type, not a class — so the chain walk that answers for `node2d` answers nothing for
  `vector2`, and the lookup matches on the Verse name the skip row carries instead.

  **410 of those skips remain, from 585**, and what closed the difference was writing the math rather
  than recording it: **all sixteen types** carry their operators and the methods scene code reaches,
  along with the `@GlobalScope` scalars Verse has no name for. What is left is deliberate rather than
  pending — the *general* inverse of a `basis` or a `transform3d` (a rotation's inverse is its
  transpose, which is written, and that is the case scene code has), the Euler and axis-angle
  conversions, and the `projection` constructors.
- **R-SCN-3 (MUST)** Godot's `@GlobalScope` utility functions and constants are reachable under
  names that do not collide with `/Verse.org/Simulation`. Status: **done** (Phase 4 stage 6), and
  in three pieces because the question turned out to be three questions.

  **Constants and statics go in an inline module per owner** — `NodeStatics.NotificationReady`,
  `Vector2Statics.Up`, `TweenStatics.InterpolateValue(...)`. Verse has no constant and no static
  *on* a type (Epic hit the same wall and wrote `Zero2()` with a TODO wishing for `vector2.Zero`),
  and an inline module needs no `using`. **352 constants and all 114 statics** are emitted. The
  `...Statics` suffix is load-bearing rather than decoration: Verse refuses a *local* that resolves
  ambiguously against a visible definition, so a module named `Tree` would break
  `if (Tree := GetTree[])`, which the yardstick writes today.

  **The statics need one by-name call that carries no object**, `vh_godot_api::CallStatic`, over
  Godot's own `ClassDB.class_call_static` — generic, so all 114 arrive with no per-method code.

  **All 114 are now classified, with none unexplained.** 92 are answered by a Verse
  spelling, and the editor names it — a script that types `floor(x)` is told to write `FloorF(X)`,
  where before it got "utility_not_dispatched" and nothing else. 14 have a `Variant` parameter or
  result, which R-TYPE-7 keeps a script from spelling, so they have no signature to be given and
  that is their recorded reason. 11 more are dispatched because their *behaviour* is the engine's —
  `push_error` and `push_warning` reach the editor's Debugger panel where a `Print` reaches stdout,
  `print_rich` carries BBCode, `type_string` and `error_string` are engine tables,
  `instance_from_id` is the object registry. Nothing is left over.

  The dispatched wrappers are hand-written rather than generated, because their Verse signature is
  deliberately not Godot's: the print family is `vararg` there and one argument here, which is what
  a script writes.

  **The random family is R-AUD-2's one exception, and only it.** `Randf`,
  `RandiRange`, `SeedRandom` and their five siblings are *dispatched* through `CallUtility`, because
  a Verse-side RNG would silently ignore `seed()` and `randomize()` and a project that seeds for a
  replay would get a different game. C# makes the same exception for the same reason, and the
  integration suite proves it is one stream: GDScript seeding and Verse asking see the same number.
  The other 106 keep Verse's spelling where Verse has one, and are recorded as skips where it does
  not — the GDExtension interface offers no by-name utility call that takes Variants, only a
  ptrcall wanting a signature hash.

  Two renames, both recorded so either spelling is answered in the editor: `seed` is `SeedRandom`
  (the `Seed` *property* on six classes has no signature to be told apart by) and `Color.TAN` is
  `TanColor` (Verse's `Tan` is the trigonometric function). Those are the only two collisions across
  the whole surface, and the utilities live in a `GodotStatics` module because a module-level
  `Randf` would be ambiguous with `random_number_generator.Randf`.
- **R-SCN-4 (MUST)** Referencing a freed object is an error with a diagnosable message and does not
  corrupt the scene. Status: **done** — it raises a Verse runtime error, unwinds to the root
  failure context, and discards queued writes. *But* see R-ASYNC-4: today it also kills every
  suspended task in the project.
- **R-SCN-6 (MUST)** A Godot object a script is handed can be narrowed to what it actually is, as a
  failable cast: `if (Sprite := animated_sprite2d[GetNode("Pic")])`. A cast that does not hold
  **fails** rather than raising, so it composes into a guard the way every other question does.
  Status: **done** (Phase 4 stage 1).

  Identity is part of the requirement rather than an optimisation of it. Every mirrored method
  declares the class Godot's own API declares -- `GetNode` returns `node` -- so a Verse object built
  from the *signature* is a `node` whatever the handle names, and no downcast could ever succeed.
  The host therefore builds the object at the class the handle **is**: the Godot class name comes
  back through `vh_godot_api::GetClassOf` and is matched against a generated table of every Godot
  class (`host/Private/GodotClassNames.gen.h`), which also names the nearest mirrored ancestor for a
  class a `--classes-file` build left out.

  And a node carrying a Verse script crosses as **that script's own object**, out of a
  handle-to-instance registry the host keeps from `vh_instantiate` to `vh_release_instance`.
  Without it `player[GetNode("Player")]` fails on exactly the case the cast exists for. Two
  crossings of a handle with no script are two mirror wrappers: equality is by handle, and caching
  them would need a rule for what happens across a hot-reload generation that nothing yet needs.
  The class lookup itself *is* cached per handle -- Godot does not reuse an instance id within a
  run, which is what makes that safe.

  A handle Godot has already freed, or one of a class outside the mirror, crosses as a bare
  `vh_object`, and every cast then declines. That is the shape the caller asked for: they wrote a
  failable cast, so a failure is an answer.

---

## 6. Types and marshalling

- **R-SCN-5 (MUST)** Godot's per-class and global enums are reachable as named values rather than as
  magic integers: `SetProcessMode(node_process_mode.Always)` compiles and `SetProcessMode(2)` does
  not. Status: **done.** All 758 of them are real Verse enums, generated with a pair of converters
  each in ordinary Verse over `case` — a `<native>` Verse enum would need a hand-written C++ shadow,
  and 758 of those is not a thing anyone writes. The wire is unchanged: an enum still crosses as the
  int it is.

  The type is class-qualified (`node_process_mode`) because 96 bare enum names repeat across Godot's
  classes and the project shares one flat scope. Enumerators drop the prefix their own names share,
  all or nothing per enum, and not at all when a stripped name would be an illegal identifier, a
  duplicate, a reserved word, or ambiguous with a Verse stdlib function — which is why Variant::Type
  reads `variant_type.TypeInt`, `Int` being one of Verse's own. 680 of 765 strip; the 85 that do not
  are almost all `VisualShaderNode*`. Deriving the prefix from the *enum's* name instead would have
  failed for 357 of 736 class enums, because Godot's prefixing is only half consistent while the
  enumerators always agree with each other.

  Two kinds of enumerator are dropped, and each would otherwise be a lie: a `_MAX` sentinel, which is
  a count rather than a value and the one thing Godot renumbers between releases (twelve moved between
  4.6 and 4.7), and an alias, a second name for a value another enumerator already has.

  A **bitfield**'s parameters stay `int`, because a combination of flags is not an enumerator and no
  enum value could hold one. Its enum is still declared so the flags have names, and Verse's own
  bitwise intrinsics combine them: `BitOr(ToInt(key_modifier_mask.MaskCtrl), ToInt(...))`. `ToInt` is
  the one public name the enum machinery adds, overloaded across all 758.

  The case that mattered most was nearly missed: Godot's property metadata reports an enum-typed
  property as a plain `int`, and only the getter says which enum. That is true of **515 of its 994**
  int properties, `Node.process_mode` among them — so the generator takes the accessor's word over the
  property's, and `set Node.ProcessMode = node_process_mode.Always` is the spelling R-SCN-5 is
  written about.

  This is the phase's one deliberate break of existing scripts, and it is the R-AUD-1 win: the
  reader sees the enumerator rather than the number.
- **R-TYPE-1 (MUST)** Every Godot `Variant` type crosses in both directions: the numeric and
  string types, all packed arrays, all math structs (`Vector2/2i/3/3i/4/4i`, `Rect2`, `Transform2D`,
  `Transform3D`, `Basis`, `Quaternion`, `AABB`, `Plane`, `Projection`, `Color`), `StringName`,
  `NodePath`, `RID`, `Callable`, `Signal`, `Dictionary`, `Array`, and `Object`.
  Status: **done, with two edges.** `variant` is a fixed-width native struct of scalar lanes; all
  sixteen math types cross as their components, generated from one layout that drives the Verse
  struct, the packers and the host's marshalling alike; and the reference types — `Array`,
  `Dictionary`, `Callable`, `Signal` and the ten packed arrays — cross as ids into a table the
  GDExtension owns, released when the Verse value wrapping one is collected.

  Phase 2 closed the last four: `PackedVector2Array`, `PackedVector3Array`, `PackedVector4Array` and
  `PackedColorArray` now convert element by element rather than being skipped, and a Godot container
  can finally carry a value whose type the script does not know — `godot_array.GetVariant` and
  `dictionary.GetVariant`. Two things were quietly broken before anything tested them, and both were
  the same mistake in opposite directions: a Verse `[]vector2` reached Godot as one tuple per
  element while the GDExtension read it as a flat run of floats, so a three-element array arrived
  with one element; and a packed array *carried as a reference* was decoded by its Variant tag
  before its carrier was looked at, so `packed_variant` found no sequence and answered with an empty
  container — which is every Verse array passed to a Godot method taking one. Both now have tests.

  The one edge worth naming here. A Verse `[]float` names no
  single Godot type — it is equally a `PackedFloat32Array`, a `PackedFloat64Array` and an `Array` —
  so a *script-defined* method taking one declares `Array`, which Godot builds from any of them
  where the reverse conversion does not exist. (R-TYPE-3 records an upstream Godot defect that
  shows through `Callable`; it is not a limitation of this bridge.) A mirrored Godot method is unaffected: the generator
  knows which packed type it wants and tags it.
- **R-TYPE-2 (MUST)** Typed arrays and typed dictionaries preserve their element type across the
  boundary, so a `TypedArray[Node2D]` is not flattened to an untyped array. Status: **done.** An
  `Array` and a `Dictionary` cross as references into the container Godot already owns, so nothing
  is flattened and a typed one keeps whatever typing Godot gave it. Phase 2 added the Verse side of
  the *type*: `typedarray::Node` is a `typed_array(node)` and `typeddictionary::int;String` a
  `typed_dictionary(int, string)` — ordinary parametric Verse classes over the same reference id,
  carrying their element conversion as a function value rather than needing a generated wrapper class
  per element type. All 70 element types and both key/value pairs the API spells are covered.

  Phase 4 added the half that was missing: **a script can now make one.** Until then a container
  could only be held, never built — `godot_array{}` compiles, because the wrappers are public so
  they can be named in a signature, and holds reference 0, which crosses as `Nil`.

  That fix carried a semantic change worth naming, because it is the second stated exception to
  "a write defers to commit": **a container write happens immediately.** Deferring it was never
  consistent — `VhRefGet` and `VhRefSize` were always immediate, so a container disagreed with
  itself inside one expression — and it made a freshly built container useless, since the append
  that fills one is a write at the current size and the call that consumes the result runs before
  the commit that would have filled it. Signal emission is the other exception, and **Phase 4.5**
  audits the set.

  What this unblocked is larger than the requirement. `GetChildren()` returned
  `typedarray::Node` and was therefore skipped entirely, along with `GetNodesInGroup` and
  `GetOverlappingBodies`: a `godot_array` offers ten typed element accessors and not one of them is an
  object, so walking children — which is most of what scene code does — had no spelling at all.

  Two Verse facts were found the hard way and are recorded because they shape the design. A required
  data member may be **no less accessible than its class**, so the element converter had to be public,
  which forced `variant` public with it (R-TYPE-7). And Verse has **no anonymous functions**, so the
  converter cannot be an inline lambda: it names the element type's own `VhUnpack<GodotType>` reader
  where one exists — the plain-function half of `V.As<GodotType>[]`, which exists precisely because
  an extension method is receiver-plus-tuple and cannot be a function *value* — and a generated
  per-class function where the element is a class, because
  `VhFromObject` takes the base `object` and a Verse function type is not satisfied by one that
  merely accepts a supertype.

  **The archetype is the maker now, which closes wall 7 rather than working around it.** Phase 4's
  `MakeArray()` gave a script a way to originate a container and left `godot_array{}` — the
  spelling a Godot author reaches for first — compiling and holding reference 0, which crosses as
  `Nil`. A container mints from its own **data-member default** instead, so `godot_array{}` and
  `dictionary{}` are live, empty Godot containers; the typed forms mint the same way, from a maker
  that supplies their two converters and no `Ref`. `MakeArray()` and `MakeDictionary()` remain as
  the published names and do no work.

  Three measurements decided that shape, all in `tests/verse_probe/ref_block_probe.verse`. A field
  the **archetype supplies wins** over an overridden default, so `VhToArray` pays nothing for a mint
  it would immediately overwrite. A **`block:` clause cannot do it**: a `var` member needs
  `allocates` and an assignment is `transacts` outright, glitch 3512 twice over, so a container with
  a minting block would be a `transacts` class and would drag the mirror's 379 container-answering
  `<reads>` methods across with it. And a **`<converges><native>` call is accepted as a default**,
  which is the question `ctor_delegate_probe.verse` left open when glitch 3582 refused an ordinary
  one — `<converges>` being native-only is what makes this the mirror's to spell and not a
  script's. The class carries `<reads>`, which is exactly as wide as the two calls in it: the
  default, and a `block:` that only adopts.

  Adoption is why that block exists. A container the archetype minted reached no converter, so
  nothing passed it to a native, so it had no `UObject` shadow — and the shadow is the only thing
  that releases the table entry when Verse drops the value (`docs/abi-v2-design.md` §1a).
  `VhRefNewDefault` answers 0 under a reading device, for the reason `VhAdoptOrMint` does: the
  throwaway instance the export defaults are read off runs its members' initializers in full.
- **R-TYPE-3 (MUST)** `Callable` is a Verse value a script can hold, invoke, and hand back to
  Godot — this is what makes R-SIG-3 and any callback-taking engine API work. Status: **done**,
  both directions since Phase 4 stage 3. A `callable` is held, passed back, and invoked with
  arguments; and `MakeCallable(t, F)` turns a Verse function into one Godot can call.

  The Verse-function direction is the reference table pointing the other way, and it accepts
  **only a method bound to a script instance**. That is Godot's own design rather than caution:
  `GDScriptLambdaSelfCallable` reports the captured object and dies with it, while
  `GDScriptLambdaCallable` reports the *script resource*, overrides `is_valid()` to "the function
  exists", and outlives the object — which is Godot's own known leak (GH-102327, the same defect
  this repo met from the other end). The bound half needs no lifetime tracking at all:
  `get_object()` is the node, `is_valid()` is ObjectDB's answer, and Godot drops a freed object's
  connections itself. The unbound case is **OQ-16**.

  `MakeCallable` is parametric over the payload rather than one maker per arity, because a Verse
  function's parameter *is* its tuple: `MakeCallable(tuple(int, int), OnCalledTwice)` reaches a
  handler written with two parameters, and Godot emits two arguments. Equality is by reference,
  which both prior arts agree on — Godot's lambda callables compare by pointer, and UEFN's event
  inserts one entry per subscribe — so two subscriptions of one handler are two connections.

  **An upstream Godot defect shows through this, and is not ours.** A GDScript *lambda* that has
  been called, and that is still referenced when `ScriptServer::finish_languages()` runs, segfaults
  Godot at exit. Reduced to eight lines with no GDExtension loaded at all:

  ```gdscript
  # repro.gd, run as `godot --headless --path . --script res://repro.gd`. Godot 4.7.stable: exit 139.
  extends SceneTree
  func _init() -> void:
      var maker = load("res://maker.gd").new()   # maker.gd: func make() -> Callable:
      var fn: Callable = maker.make()            #               return func(n): return n * 2
      Engine.set_meta("keep", fn)                # any engine component outliving the language
      print(fn.call(21))                         # 42, then exit 139
      quit(0)
  ```

  Godot's own `GDScriptLanguage::finish` names this case twice — "referenced from another engine
  component, which shuts down later (e.g. an instance is stored in the metadata of `Engine`)", and
  a TODO at its third pass: *"We might want to clean up `GDScriptLambdaCallables` at this point, to
  prevent leaks from set & forget lambda setups. See GH-102327."*

  The reference table is one such component, so a lambda a Verse script holds hits it. **Nothing in
  a GDExtension can avoid it:** `Main::cleanup` runs `finish_languages()` 25 lines before
  `deinitialize_extensions(SCENE)`, and `ScriptLanguage::finish` is never delivered to an extension
  language, so there is no earlier hook — verified, not assumed. Releasing sooner does not help,
  and neither does abandoning the entries instead of destroying them; both were tried.
  `Callable(object, "method")` is unaffected, which is what `connect` and every callback-taking
  engine API use, and what the integration suite covers.
- **R-TYPE-4 (MUST)** The absence of a value has exactly one spelling at the boundary, and it is
  documented. Verse has no null; the existing decision — a stale object handle is a runtime error,
  a legitimately-absent object is `<decides>` — is the rule, and it extends to every other type
  a Godot API may return as `null`. Status: **done, and the rule is that nullability is a property
  of the *type*.** Only an `Object` return is `<decides>`; every other return is total, and a nil
  arriving where a value was declared raises as a bridge bug rather than failing quietly. Measured
  rather than assumed: across Godot's 5304 documented value-typed returns, five mention returning
  null and four are false positives on inspection -- one names its *parameter*, one describes its
  Array's *elements*, one returns an editor class the rule already covers, and one is a virtual the
  generator skips. The single real exception, `EditorProperty.get_edited_property`, is editor-only.
  A hand-maintained list would be 5303 entries of ceremony to catch it.

  **The argument direction is the same rule read off Godot's own metadata.** An object argument
  the API dump marks `"meta": "required"` is declared as the class; one it does not is declared
  `?class`, because Godot accepts null there and a class has no value that spells it -- so
  before this the call could not be made at all (`by-hand-findings.md` B37). The polarity is
  Godot's: `RequiredParam<T>` (godotengine/godot#86079) marks the arguments that refuse null,
  and 112 of the mirror's 1020 are marked. They are the ones an author writes most, so
  `AddChild(Child)` is unchanged and `dodge-the-creeps` needed no edit. A plain value does not
  coerce to an option, so the remainder cost their callers an `option{}`; options *are*
  covariant, so one `VhFromMaybeObject(?object)` packs all of them. A generated binding has no
  such metadata in either of its sources -- GDScript has no annotation and
  `class_get_method_list` carries none -- so every object argument of one is optional.

  **And the result direction reads the same metadata to take `<decides>` away.**
  `RequiredResult<T>` marks a return that cannot be null, and 40 of the mirror's 759 object
  returns carry it -- the whole Tween builder chain, `SceneTree.GetRoot`, `CreateTimer`,
  `CreateTween`, `GetMultiplayer`. Those are total, so a caller spends no failure context on a
  case that does not arise. They are not infallible: the *cast* can still refuse if Godot
  answers a class outside the mirror, and a required return raises through `Err` there, which
  is what the 39 total singleton accessors below already do.

  **A virtual reads the same metadata, because Godot's declaration is a promise the override
  keeps.** Its parameters follow the argument rule -- Godot is what passes one -- and its
  result follows the return rule, which is what finally gave the 43 object-returning virtuals a
  default body: all of them used to be skipped, since no value of a class stands in for "nobody
  overrode this". The 41 Godot does not mark are `?class` defaulting to `false`; the 2 it marks
  keep the class and default to `Err`, which nothing reaches, because a class's *own*
  declarations are what `vh_class_method_list` reports.

  **The singleton accessors are not this rule**, and there is nothing to read for them: the
  dump's `singletons` table carries a name and a type and no metadata at all. Which two can be
  absent comes from the class's `"api_type": "editor"`, which is a stronger answer than
  `required` would be -- it names them rather than marking the rest.

  The **singleton accessors** are the second `<decides>` family, and the rule applies to them the
  same way: 39 of the 41 are registered during `Main::setup`, so no run that can execute Verse at
  all can find one absent, and only `EditorInterface` and `GDScriptLanguageProtocol` — the two
  whose class says `"api_type": "editor"` — can fail in a game. **Those two are failable and the
  other 39 are total**, which is this rule and not an exception to it: a singleton that cannot be
  absent is not a nullable type, and the `<decides>` the 39 used to carry said otherwise.
  What makes a total accessor awkward to write is still **V3564**, *"class engine used as a
  parameter/result in a native function must also be native"* — a native cannot answer a mirrored
  class — so the accessor handles the cast's failure itself and raises through
  `/Verse.org/Verse`'s `Err`, whose result type `false` is uninhabited. That is the same answer
  R-LANG-4 already gives a stale object handle.

  What a raise costs is recorded rather than assumed, because it is more than a failure costs: the
  call answers `VH_ERR_RUNTIME`, everything the body deferred before it is rolled back with the
  transaction, and the raising instance's content scope is terminated, so that node's suspended
  work dies and the next call into it gets a fresh scope (R-ASYNC-4, R-DIAG-3).
  `tests/verse_probe/singleton_effect_probe.verse` is the measurement: that `Err`'s `diverges`
  effect is allowed in every body narrow enough to reach Godot at all, and that a failure context
  over the two that remain failable does not force `<transacts>` on its caller. It survives a cook,
  which was not free: `Err` is a module-level native in `/Verse.org/Verse`, and a cooked
  `VNativeProcedure` with no rebound thunk is a jump to address 0. `RebindVniModuleNatives` covers
  it, and `tests/cooked_probe` over a cook of one class is the reading — `VH_ERR_RUNTIME` and the
  message, with no callstack, because a runtime host has no compiler runtime to build one.

  The two failable ones are also where this bridge is *better* than GDScript rather than merely
  different. `EditorInterface` is an identifier only a `TOOLS_ENABLED` build registers, so a
  GDScript that names it fails to compile in an export template and the node loses every method on
  it. Here the script compiles, the accessor reports the absence at the call, and the method goes
  on to answer — asserted in `tests/integration`. The consumer checks `Engine::has_singleton`
  before `get_singleton` for the same reason: Godot's own miss is an `ERR_FAIL_COND_V_MSG`, and an
  absence the script is handling should not print an engine error per call.
- **R-TYPE-5 (MUST)** A type mismatch at the boundary is a compile error wherever the typed layer
  can see it, and a diagnosable runtime error with both type names where it cannot.
  Status: **part** (`VhTypeMismatch` exists).
- **R-TYPE-6 (MUST)** Marshalling does not allocate per-call on the hot path in a way that makes
  per-frame script code unusable. No target number (§13), but the design must not make one
  impossible to reach later. Status: **improved on, not measured.** The encoding this replaced
  built up to three Verse arrays to carry one number; a fixed-width struct builds none, so every
  value type is allocation-free at the boundary now. A reference type costs a table entry, which is
  the price of not copying a container. Still unmeasured — R-PERF-2 is what would say.
- **R-TYPE-7 (MUST)** The plumbing stays hidden. The `Vh…` primitives and `object`'s `Handle` carry
  no access specifier and stay out of completion lists; a user cannot accidentally hold a raw handle
  that outlives what it names.

  Amended in Phase 2, and **weakened further than intended** — recorded here because the first
  attempt at this paragraph claimed a guarantee the language does not allow.

  The `variant` **type** had to become public: a script cannot call any of Godot's 231
  Variant-typed methods without naming it in a signature. The intent was that the *lanes* stay
  module-scoped. They cannot. Verse forbids a non-public field on a struct outright —
  `Verse::Version::StructFieldsMustBePublic`, "Access level internal is not allowed in structs" — so
  a public struct has public fields, and `V.I0` and `variant{Tag := 24, Ref := N}` both compile from
  a script. Verified, not assumed: both were put in the coverage-diagnostic project expecting
  errors, and neither produced one.

  What that costs is bounded, and it is the one thing worth being precise about. A script can lift
  the raw instance id out of an object-tagged variant and fabricate one back later. What it gets is
  an `object` naming a possibly-dead Godot object — which is **exactly** what it already gets by
  holding an `object` across a `queue_free`, and which is already a runtime error rather than memory
  unsafety (README, and R-LANG-4's note that a freed object was deliberately made an error rather
  than a failure). So the wording "a user cannot accidentally hold a raw handle that outlives what it
  names" survives on *accidentally*; the deliberate route now exists and lands where the accidental
  one already did.

  Two things would restore it, neither of them Phase 2's: `variant` as a **class** rather than a
  struct, which is an ABI change and would give up the fixed-width no-allocation property the whole
  encoding was measured for (`abi-v2-design.md` §1a); or Verse growing non-public struct fields back.
  The normal route is unaffected — `V.As<GodotType>[]`, `VariantKind(V)`, `MakeVariant[V]` and the
  named `Variant<GodotType>` builders —
  and is what every generated body and every example uses. Status: **part.**

---

## 7. Concurrency

Verse's structured concurrency is the headline reason to prefer it to GDScript, and **Phase 5 is
where it arrived**: a task scope per script instance, `Await()` on any Godot signal, a real-time
`Sleep`, and a frame budget whose effect is readable in Godot's own profiler.

- **R-ASYNC-1 (MUST)** `spawn`, `race`, `sync`, `branch`, `rush`, `loop` and `<suspends>` functions
  work in script code, with tasks resuming across frames. Status: **done** (Phase 5), and most of it
  was already true before the phase started — `phase-5-design.md` §2 put it to the compiler and the
  runtime through `tests/verse_probe` and measured the whole cycle working with no host change: a
  call spawned a task, the task suspended, the call returned `VH_OK`, `vh_tick` ran, and the task
  resumed *synchronously* inside the next call that signalled it, with the member it wrote readable
  afterwards (F9). What the phase added is the scope work R-ASYNC-4 asks for and the tests that keep
  it passing: `tests/host_smoke`'s tick-loop layer drives a spawn, a suspend, a tick and a resume
  with no Godot in the way, and `tests/integration` does the same with signals behind it.

  **Two constraints the same measurement found**, and they are language facts rather than bridge
  choices. An *awaiting* body cannot be narrowed — `awaitable.Await` carries `no_rollback` exactly as
  `signalable.Signal` does — so it carries no effect specifier, and `spawn` reaching it must come
  from a caller that carries none either. A mirrored virtual override is such a caller, and since
  Phase 5 so is a signal handler; a `<reads>` body is not, and may not `spawn` at all. And a virtual
  **cannot** be written `<suspends>`: the specifier makes it a different function, so the compiler
  answers *"must have a distinct domain"* and *"could not find a parent function to override"*
  rather than an effect error. Both are said in the `.verse` template rather than diagnosed, because
  the compiler already refuses them at the author's own line.
- **R-ASYNC-2 (MUST)** A script awaits a Godot signal, a timer, or a frame from a concurrent
  context. Without this, Verse's concurrency cannot observe the engine and is decorative. Status:
  **done** (Phase 5) — `signal(t).Await()<suspends>:t`, which covers a script's own declared
  signals and all 489 mirrored engine-signal accessors alike because every one of them answers a
  `signal(t)`. `GetTree[].ProcessFrame()`, `GetTree[].PhysicsFrame()` and
  `GetTree[].CreateTimer[1.0].Timeout()` are accessors like any other, so a frame and a timer need
  nothing of their own. `Sleep(Seconds)` is the eventless case and is real time, not engine time —
  see R-ASYNC-3's table.
- **R-ASYNC-3 (MUST)** Verse tasks run on Godot's main thread and are pumped deterministically
  relative to `_process` and `_physics_process`; the ordering is documented, not emergent.
  Status: **done** (Phase 5). The rule is that **there is almost nothing to invent**: a task
  awaiting a Godot signal resumes *inside* that emission, in connection order, which is exactly
  where GDScript resumes a coroutine (`GDScriptFunctionState::_signal_callback` is an ordinary
  `Callable` that calls `resume()`). So the ordering is Godot's own:

  | you await | you resume | where |
  | --- | --- | --- |
  | `GetTree[].PhysicsFrame()` | before that step's `_physics_process` pass | `scene_tree.cpp:649`, then `_process(true)` at `:655` |
  | `GetTree[].ProcessFrame()` | before that frame's `_process` pass | `scene_tree.cpp:713`, then `_process(false)` at `:719` |
  | a `SceneTreeTimer` timeout | after `_process`, in `process_timers` | `scene_tree.cpp:729` (idle) / `:660` (physics) |
  | any node's signal | inside that `emit_signal`, in connection order | — |
  | `Sleep` | at the pump: `ScriptLanguage::frame()`, end of `Main::iteration` | `main.cpp:5107` |
  | a task on a `queue_free`d node | keeps running until the delete queue flushes | R-ASYNC-5 |

  The one row this bridge owns is the fifth, because nothing in Godot fires it. Within it,
  resumptions are **FIFO** — the earliest deadline first — and the queue behind it is a `TQueue`,
  which has no other order to offer. `ScriptLanguage` has no per-physics-step hook at all: `frame()`
  is the only one (`script_language.h:332`) and `Main::iteration` calls it last, which is why the
  pump is where it is and why resumption had to be event-driven rather than pumped.
- **R-ASYNC-4 (MUST)** Task scopes are per-script-instance, not per-project. A runtime error in
  one script's task must not terminate tasks belonging to another script. Status: **done**
  (Phase 5). Each `vh_instance` owns a `verse::FContentScope`, made at `vh_instantiate` and
  terminated at `vh_release_instance`, and every entry that runs that object's code pushes its
  guard — which is what decides the task group a `spawn` inside it joins
  (`FRunningContext::EnterVM_Internal` reads the active scope). A raise terminates the **active**
  scope, so it costs that node's suspended work and nothing else's, and `tests/host_smoke` asserts
  exactly that: two instances with a task each, one raises, the other's task still resumes.

  **Two things the design expected to have to build were already there**, and both are recorded in
  `phase-5-design.md` §14.1. `FContentScopeGuard` is a *stack*, so scopes nest and the guard is a
  thread-local pointer swap — measured at 1.09 µs per `vh_instance_call` against 1.03 µs without,
  which is inside the noise. And every live scope is batched behind one `FGCObject` registration by
  Epic's own `FContentScopeGCReferencer`, so N scopes are not N registrations. What a scope does
  cost is **~2.6 KB per scripted node**, measured the same way.

  **`phase-4-gaps.md` G9 closes with it, and not as tidiness.** Epic's rule — a callback is dropped
  when the scope it was subscribed in *terminates*, and a terminated scope is replaced rather than
  revived (`VerseEvent.cpp:169-188`, `ContentScopeRepository.h:80-92`) — was incoherent against one
  process-wide revived scope and is the obvious rule against per-instance ones. The bridge now
  follows it: `ReviveContentScope` is gone, a terminated instance scope is replaced at that
  instance's next call, and `TVerseCall::Return` declines for a scope that was terminated, which is
  what drops a sleeping task on a freed node with no work from us.

  What made it load-bearing is that **terminating a task group does not unwind the tasks in it**: a
  `defer` covers a task cancelled by `race` or by completing, and covers nothing when the node dies.
  So an `Await` registers its Godot connection on `FContentScope::OnContentScopeCleanup` — Epic's
  own hook, the one `event::SubscribeInternal` uses — and without it freeing an awaiting node left a
  live connection behind. The case that found it is in `tests/integration`.
- **R-ASYNC-5 (MUST)** A node's tasks are cancelled when the node is **freed**, and a scene change
  cancels the tasks of everything it unloads. Structured concurrency whose structure does not match
  the scene tree's lifetime is a leak with extra steps. Status: **done** (Phase 5) —
  `vh_release_instance` terminates the instance's scope, which terminates its task group. A scene
  change falls out of it: the nodes are freed.

  **Amended by Phase 5 (`phase-5-design.md` D9, D22), and the original wording said "leaves the tree
  or is freed".** Leaving the tree is struck because Godot's own answer is the free and not the
  removal: `GDScriptInstance::~GDScriptInstance` clears `pending_func_states`
  (`gdscript.cpp:2069-2073`), and nothing consults the tree. Pooling and re-parenting remove and
  re-add nodes constantly — Dodge the Creeps' mobs are that shape — and a GDScript coroutine
  survives it; what actually stalls such a coroutine is the *source* it awaits going quiet, which
  is the author's own choice of what to await. Cancelling on removal would make Verse's concurrency
  behave differently from every other scripting language in the engine for no requirement's sake.

  **The trigger is the real free, not `queue_free`.** Godot flushes the delete queue on the next
  idle (`_flush_delete_queue`), so a queued node's task keeps running until then and may emit or
  write in that window. That is the window Godot leaves open for its own scripts, and it is a
  documented row in R-ASYNC-3's table rather than a defect.

  **A rebuild cancels everything**, which needs no mechanism of its own: a new generation replaces
  every instance, and an instance released takes its scope with it.
- **R-ASYNC-6 (MUST)** The per-frame Verse time budget is configurable and observable, and
  overrunning it is reported rather than silently dropping frames. Status: **done** (Phase 5).
  `verse/runtime/frame_budget_ms` was already configurable and defaults to 4.0; what the phase
  added is the observable half. `vh_tick` now fills a `vh_tick_stats` — jobs run, jobs still
  queued, tasks sleeping, seconds spent, and whether the budget stopped it with work left — and the
  GDExtension turns that into three Godot **custom monitors** (`verse/queued_jobs`,
  `verse/pump_ms`, `verse/sleeping_tasks`), which draw in the profiler's Monitors tab beside the
  engine's own. An overrun is reported as a warning, rate-limited to one per 600 frames, because a
  project that is over budget is over budget every frame and one line each would bury everything
  else.

  **What the budget governs is the queue, and only the queue.** A task awaiting a Godot signal
  resumes inside the emission and is not budgeted — exactly as a GDScript coroutine's resume is not
  — and a `Sleep` whose deadline has passed is woken before the queue and is not budgeted either. A
  budget that could hold a due deadline over would be a frame of drift an author cannot see.

  **After a gap in the pump, the sleepers are drained rather than spread**, which is the one thing
  Phase 5 left open (`phase-5-design.md` §7.1, closed in its §14.4). In the editor `vh_tick` no-ops
  for the length of a background analysis — ~520 ms — so a `@tool` script's tasks stall and come
  back together. That burst cannot grow with the gap: a sleeper's deadline is stamped when `Sleep`
  is called, not accrued while the pump is stopped, so one task asleep 0.1 s across that gap has one
  deadline due and not seven, and the whole burst is bounded by the number of sleeping tasks that
  `vh_tick_stats.Sleeping` reports. What the resumes enqueue is budgeted as any other job is.
  Spreading them would preserve no ordering that draining loses, since both wake earliest deadline
  first; it would only add drift to tasks already late.
- **R-ASYNC-7 (deferred)** Interaction with Godot's own threading — `WorkerThreadPool`, threaded
  resource loading, calling into Verse from a non-main thread, and running Verse tasks off the
  main thread — is **out of scope for this document and requires its own scoping**. It is not
  dismissed: the design of R-ASYNC-3 and R-ASYNC-4 must not foreclose it, and §14 **OQ-6** holds
  the questions that scoping has to answer. Phase 5 forecloses nothing and removes one of the
  worries: there is no scheduler whose thread affinity would have to be redesigned, because
  resumption is event-driven. What runs on the Verse thread is the pump, where `_frame` does.

- **R-ASYNC-8 (MUST)** A call that enters the host from any thread other than the one that called
  `vh_init` is **refused with a diagnosable error, and nothing runs**. Status: **done** (Phase 4
  stage 3). `vh_init` records its thread; **every** entry point compares against it and
  answers `VH_ERR_THREAD` having run nothing, reporting through the diagnostic callback with the
  entry point named and what to do instead. The GDExtension turns that into an invalid call, so
  GDScript sees it where it made the mistake. It landed with the Callable because that is the stage
  that enlarges the exposure: a Callable is a value, and an author may hand one to a
  `WorkerThreadPool` task.

  Phase 4a guarded only the two entry points that *execute* Verse; the guard now covers all of them.
  The read-only ones were never harmless — they read a semantic program the analysis thread
  replaces. Three are deliberately unguarded, each argued in place: `vh_abi_version` (answered
  before `vh_init`, so there is no thread to compare against), `vh_init` itself, and
  **`vh_callback_release`** — a Godot `Callable` is destroyed on whatever thread dropped its last
  reference, and refusing that would leak the row rather than protect anything. Releasing never
  enters the VM, so allowing it is safe; the `TMap` it mutates is what needed protecting, and it is
  now under a lock, with `vh_callback_invoke` copying its target out before running anything rather
  than holding a pointer across a call that runs Verse.

  Four entry points return `vh_bool` and have no error value, so a refused call answers `0` — which
  reads as "no such class" rather than "refused". The diagnostic carries the difference; widening
  those four is a major ABI change nobody has needed.

  *Rationale, and why a lock is not the answer.* VerseVM does not merely prefer the game thread, it
  asserts it: `VVMEnterVMInline.h` opens the top-level VM entry with
  `ensure(IsInGameThread() && (!IsInAsyncLoadingThread() || …))` above the comment "Verse bytecode and
  AutoRTFM transactions must run on the game thread." That is thread *identity*, so a mutex around
  entry does not satisfy it — a single uncontended call from a `WorkerThreadPool` task is already a
  violation, and AutoRTFM's transaction state is per-thread besides. Because it is an `ensure` rather
  than a `check`, the current failure mode is the worst available: a logged callstack in a development
  build and then execution *continues* into undefined behaviour.

  Godot can reach us this way today — a `Thread` or `WorkerThreadPool` task calling a method on a node
  that carries a Verse script, or a signal emitted from a worker thread, whose handlers run on that
  thread. (Threaded *resource loading* is not one of these: `verse_resource_format.cpp` makes no host
  calls.) Phase 4 enlarges the exposure, because a `Callable` made from a Verse function is a value the
  author may hand to anything.

  **Threaded physics is not one of the hazards, which is worth recording because it is the first
  thing anyone asks.** `physics/2d/run_on_separate_thread` and its 3D twin exist and default to
  false, but they move the *simulation*, not the callbacks: `PhysicsServer2DWrapMT::step` is pushed
  to the physics thread's command queue while `flush_queries()` calls straight through on the calling
  thread, and `Main::iteration` runs `sync()` → `flush_queries()` → `physics_process()` →
  `end_sync()` → `step()` from the main thread. So `_physics_process`, `body_entered` and
  `_integrate_forces` all arrive on the main thread with the physics thread parked, enabled or not.
  This is a property of Godot's current design rather than a guarantee we hold, which is one more
  reason to have the guard: if it ever changes, the bridge says so instead of corrupting.

  What this requirement asks for is the refusal, not a solution: the id of the `vh_init` thread,
  checked at every entry point, answering a distinct status with a message that names the script and
  method. Serving such a call — by marshalling to the game thread, blocking or deferred — is
  R-ASYNC-7's and **OQ-6**'s, and the deadlock that a blocking hand-off invites is recorded there. The
  host already has the discipline in one place: while a background analysis owns the program, `vh_tick`
  is a no-op and the entry points that read the semantic program block, because proceeding anyway trips
  `ensure(!bBlockAllExecution)` and takes the process down.

---

## 8. Interop with GDScript and C#

The requirement is full, two-way, first-class: to the rest of the engine a Verse class is
indistinguishable from a GDScript one.

**R-INT-7 to R-INT-12 are the other direction of the same sentence**, and they are newer than
the rest of this section: a GDScript class is *not* indistinguishable from a Godot one to a
Verse script, because the mirror is generated from `extension_api.json` and that file describes
core Godot and nothing else. `docs/generated-bindings.md` is the design, and its §10 is the
section written after the spikes.

- **R-INT-1 (MUST)** GDScript and C# can instantiate a Verse class by its global name, call its
  methods with arguments and return values, read and write its properties, and connect its
  signals — with no Verse-specific API and no knowledge that Verse is involved. Depends on
  R-NODE-6 and §5.3.

  **The other direction — a Verse script reaching a signal *GDScript* declared — closed in Phase 5.**
  A signal the mirror has no accessor for, because a script or `add_user_signal` made it, is named
  with `MakeSignal(Owner, "name")` and answers a `signal_ref` with `Await()` and `Subscribe()` on
  it. The payload is Godot's own Array of the emission's arguments rather than a typed `t`, and it
  has to be: there is no declaration to read a type off, so the author unpacks it with the `Get*`
  accessors a `godot_array` already has and a wrong expectation fails at the unpack. `Subscribe`
  there is also the **rollback-safe** way to receive a foreign signal — it compensates on abort the
  way `signal.Subscribe` does, which `Object.Connect` cannot (see `nonatomic-methods.md`).
- **R-INT-2 (MUST)** A Verse script calls methods on, and reads properties of, an object whose
  script is GDScript or C#, dynamically. Status: **done** (Phase 4 stage 2 closed the argument
  array). The dispatch itself arrived as a side effect rather than as work of its own: `Object.callv(StringName, Array) -> Variant` is an ordinary concrete method, and Godot's
  `Object` became mirrorable the moment Variant and Array both crossed (Phase 2 §4.4). A script
  writes `Target.Callv("_double", Args).AsInt[]` and the GDScript method runs. `get`, `set`,
  `has_method` and `get_class` came with it.

  **The argument array** was what was missing, which the Dodge the Creeps port found and
  `docs/dodge-the-creeps.md` records as its wall 7: a script could not make a `godot_array` of its
  own, so dispatch by name worked and *originating* such a call did not. `MakeArray()` and
  `MakeDictionary()` now mint one through `VhRefNew`, `Make<Element>Array()` and
  `Make<Key><Value>Dict()` do the same for the typed forms — which no script could ever have
  spelled, since their converters are module-scoped — and `Add<Element>` appends. This is
  R-TYPE-2's other half and landed with it.

  **`CallConst` is the `<reads>` half, and it is done.** Every `Call` and `Callv`
  overload is `<transacts>`, so reading a third-party property from a `<reads>` function started
  wall 8's cascade for no reason — the call is const and answers a value, which is exactly the
  test the mirror's own 3996 `<reads>` methods pass. `CallConst` is an extension method on
  `object` over the `VhCallValueConst` native that was already there, so it costs no new native
  and no ABI change. What it does cost is that the honesty is the caller's: nothing checks that
  the method named is const, exactly as nothing checks it for the mirror's own — there the
  generator reads Godot's `is_const`, and for a binding it reads the addon's
  `METHOD_FLAG_CONST`. For a hand-written call it reads nothing.

  Proving it turned up a rule that had leaked out of the place it belonged: an object-typed
  *exported member* must be optional, because the inspector can leave a slot empty, and that rule was
  being applied to method *arguments* too — where it is not merely unnecessary but wrong. A parameter
  declared `node2d` was handed a `?node2d`, so the first `.GetName()` on it died inside the
  interpreter instead of failing to compile. A bare parameter now receives the object itself, and a
  null handle for one is refused as a bad argument rather than becoming an empty option.
- **R-INT-3 (MUST)** Signals cross in both directions (R-SIG-6).
- **R-INT-4 (MUST)** A `Callable` produced by any language is invocable from any other, including
  a Verse function handed to a GDScript API (R-TYPE-3). Status: **done** (Phase 4 stage 3).
  Proved from both ends in the integration suite: GDScript calls a Callable a Verse script made,
  and a Verse function connected to a Godot signal runs when Godot emits it.
- **R-INT-5 (SHOULD)** A scene may mix Verse, GDScript and C# scripts on different nodes with no
  ordering caveats beyond Godot's own. Status: **done** for the subset that works.
- **R-INT-6 (MAY)** A Verse class extends a class *defined by a GDScript or C# script*. Godot does
  not generally support cross-language script inheritance, so this is permission to do it if the
  engine ever makes it cheap, not an obligation. Composition and signals cover the need.

  **Status: refused, and the refusal is now specified rather than incidental** (R-INT-10). The
  engine does not make it cheap and the reason is structural at both ends: `GDScriptFunction::call`
  takes a `GDScriptInstance*` and indexes members by slot, so it cannot run against a
  `VerseScriptInstance`; `GDScript::base` is a `Ref<GDScript>`, so the chain cannot hold a Verse
  class either; and an `Object` holds exactly one `script_instance`, so there is no seam to add
  one. Generated bindings make the *spelling* available — `player := class(mob)` compiles, because
  `mob` is an ordinary Verse class — which is what turns this from a gap into a trap, so the
  bridge refuses it at the class's own line.

- **R-INT-7 (MUST)** Every class ClassDB carries that the mirror does not, and every script class
  with a `class_name`, is reachable from a Verse script as a **declared type** — completion,
  hover, an argument hint, and a compile error at the call rather than an empty `variant` at
  runtime. A script with no `class_name` has no global name to bind and is skipped; it is *said*
  to be skipped rather than silently absent. The dynamic route (R-INT-2) is unaffected and stays
  the answer for anything a binding cannot describe. Status: **done for a script class, and for a
  GDExtension class as far as this harness can see one.** A GDScript `class_name` is a Verse type:
  `tests/integration/mob.gd` is reached as `mob`, and the cast, the declared result types and the
  `logic` are all asserted. What a binding carries is still only methods and signals — properties,
  enums, constants and statics are R-INT-9's remainder.

  **The editor's four answers are four separate pieces of work and completion was the only one
  that came free.** Hover, ctrl+click and syntax highlighting all answered nothing for a binding,
  because a binding's declaration is a synthetic snippet read back from a digest in the engine
  tree rather than a file under `res://`. They answer for what the author actually wrote instead:
  the GDScript for a script binding, Godot's class documentation for a GDExtension one, and a
  member under its Godot name. `generated-bindings.md` §11 is the record, and
  `tools/probe_hover.py`'s H5 is what fails if it regresses.

  The shape is a **subclass of the mirrored base**, not a wrapper holding one: a wrapper cannot
  be passed to `AddChild` and reads `M.Target.GetName()` at every call site. A binding therefore
  inherits all 3312 mirrored properties and 503 signal accessors, and `mob[SomeNode]` is Verse's
  own downcast over R-SCN-6's cast with no new machinery. Measured: a `/user@localhost` class
  crossed in through `ObjectForHandle`, downcast and ran a method, with `NewMirroredWrapper`
  unmodified (`generated-bindings.md` §10.1).

- **R-INT-8 (MUST)** Bindings are generated into a Verse package of the project's own, at
  `/Godot.org/Bindings`, regenerated whenever the class roster changes, and never committed:
  they live under `.godot/`, which Godot regenerates routinely, so generation runs on project
  open as well as on a roster change. The trigger is language-agnostic —
  `EditorFileSystem.script_classes_updated` for script classes and `GDExtensionManager`'s three
  signals for ClassDB — so C# costs no new code (OQ-17 still says no test here has ever run it).
  Status: **done.** The consumer regenerates on `script_classes_updated` and before every build, and
  writes the package to `res://.godot/verse/bindings.verse` for the author to read when a diagnostic
  names it. **Only what a GDExtension registered is bound**, which was a correction rather than a
  decision: the editor's ClassDB carries every editor-only class, and binding those produced 1677
  lines of Verse for classes an exported game does not have.

  **A generation may be reached from inside a resource load, and there one script may not be
  loaded.** "Before every build" includes the build a `.verse` load performs to make the script
  valid, so a GDScript that names a Verse class reaches its own load through it — `main.gd` →
  `mover.verse` → build → generate → `main.gd` — which Godot refuses as a cyclic load with
  `ERR_BUSY`, a null `Ref` and no sentence of its own (`by-hand-findings.md` B30). The generation is
  still made there, and still loads every other script, because a build whose bindings have no
  members refuses every Verse file that *calls* one: what is held back is the scripts whose text
  names a Verse class, which are the only ones that can close the loop. **The class list is enough
  for a declaration** — it records what each script extends, followed through the list where that is
  another script class — so the type is never lost, only its members, and only until the next
  frame.

  **A package of its own, rather than rows in the mirror.** The mirror is one package in the
  engine tree, shared by every project on the machine, and these classes are per-project and
  change when someone installs an addon. The build cost is *not* the reason: a whole new `.verse`
  file staged into the mirror with `build_host.py --stage-only` — no UBT run, no VNI — compiles
  and runs (`generated-bindings.md` §10.4).

  The package is **generational**, named afresh per roster change with the retiring one removed
  from the source project first, for the reason R-ITER-1's generations are (OQ-8): the assembler
  publishes every Source package the program carries, and publishing one name twice is
  `!ObjectItem->HasAnyFlags(EInternalObjectFlags::LoaderImport)` inside `AsyncLoading2.cpp`,
  which is a crash and not a diagnostic. Measured across two builds in one process, with the
  roster changed between them (`generated-bindings.md` §10.3).

- **R-INT-9 (MUST)** A binding's members carry the same classifications the mirror's do:
  `<decides>` for an object return, `<decides>:void` for a predicate, `logic` for a method that
  answers a value rather than a test, `<reads>` where the source says the method is const *and*
  it answers something, properties as writable members except where a nested struct or a
  container forces a getter/setter pair. Enums, constants and statics are bound too — a
  third-party physics class is unusable without its enums. Status: **`CallConst`, `<reads>`,
  `logic` and `<decides>` for an object return are done; predicates, properties, enums,
  constants and statics are not.** `CallConst` and `CallvConst` are in
  `GodotApi.native.verse` and asserted in the integration and export layers, and the emitter
  reads Godot's own `METHOD_FLAG_CONST` for `<reads>`. An object return is `<decides>` over
  `AsObject[]` and a downcast, which is what lets a binding name a class at all — the mirror's
  or another binding's (`by-hand-findings.md` B35). What is left is Phase 7c's.

  **`<reads>` needs `CallConst` and could not exist without it.** Verse's internal access is
  scoped by verse path, so a package at `/Godot.org/Bindings` reaches neither
  `VhCallValueConst`, nor the packers, nor `vh_object.Handle` — and `object.Call` is
  `<transacts>` in every overload. The three ways out were all run and the other two were
  rejected: sharing `/Godot.org/Godot` makes a name collision glitch 3532 against a generated
  file the author cannot edit, and dropping `<reads>` makes every binding method a wall-8
  cascade source. See `generated-bindings.md` §10.6.

  A GDScript class has no const methods, so script-class bindings are `<transacts>` throughout
  and never reach this.

- **R-INT-10 (MUST)** Binding-to-binding inheritance mirrors the source hierarchy, and the one
  case that cannot work is refused where the author can see it. Three cases, and only the third
  is a problem: `boss := class(mob)` between two bindings stays inside GDScript's own chain and
  works; `player := class(rapier_character_body)` is ordinary Godot inheritance and works; and
  `player := class(mob)` — a *Verse script* extending a *script-class* binding — cannot, because
  the inherited methods forward to `Call("hit")` on an object whose script is `player.verse`.
  Status: **done** for the refusal, which is the half that needed building — the two cases that
  work need no code and get none. One sentence written twice, the way `inert_global_class_message`
  is: an error `_validate` draws at the class's own line, and a warning `log_script_warnings` pushes
  so a headless run can assert it. `tests/integration/scripts/extends_binding.verse` is the fixture
  and it **compiles**, which is what makes it a test of the rule rather than of the compiler.

  It is refused in `_validate`, at the class's own line, naming this requirement and the
  one-script-per-node rule. Not by `<final>`, which is available in a user package and refuses
  with glitch 3569 (`tests/verse_probe/final_probe.verse`): marking a binding final would also
  refuse the generated `boss := class(mob)` that a GDScript hierarchy forces, and would report it
  against a generated file rather than against what the author wrote.

- **R-INT-11 (MUST)** An exported game runs a script that uses a binding. A game shipping a
  physics addon needs its bindings at runtime, so the class-to-binding mapping joins the sidecar
  beside the declared types and the 503 signal payloads (R-DIST-11), and the export layer asserts
  a case that calls through one. Status: **half done.** The export plugin generates the package and
  hands it to `verse_cook.exe` as `-bindings=<file>`, so an exported game's Verse compiles against
  its bindings and the export layer asserts that. What is missing is the **table**: the
  class-to-binding map lives only in the editor host's memory, so a handle in an exported game
  cannot be keyed on it. The four cast cases are printed as skips in the export run, with that
  reason, rather than dropped.

- **R-INT-12 (SHOULD)** A script constructs a bound class by the Godot spelling: a ClassDB class
  through `ClassDB.instantiate(name)`, a script class by minting its base and then `set_script`,
  which R-INT-1 already names. Status: **done.**
  `GodotPeerClassFor` answers a binding's own Godot class and `host_smoke` asserts it mints that
  rather than the mirrored ancestor; `api_instantiate_class` makes a script class by loading it
  out of the global class list, instantiating `get_instance_base_type()` and attaching the script,
  which is what `MainScript.new()` does underneath. Reaching for the class list rather than for
  the binding roster is deliberate: the roster is the editor's, and `project.godot` carries the
  class list into an exported game. **Minting one is legal where attaching is not** — R-INT-10
  refuses a Verse class that extends a script binding *on a node*, because that node's one script
  instance would be the Verse one; a minted object is fresh and holds exactly the script the
  binding's methods call through.
  Constructing a binding mints the *bound* Godot class and not its
  nearest mirrored ancestor, which is the trap: `GodotPeerClassFor` walks to the nearest ancestor
  in `/Godot.org/Godot` to decide what to mint, so left alone it hands back a `RigidBody2D` where
  a `RapierBody2D` was meant — silently, while a working scene looks entirely normal.

---

## 9. Editor and tooling

**Godot's built-in script editor is the primary authoring surface** and gets full support. An
external editor is secondary.

- **R-TOOL-1 (MUST)** Syntax highlighting, including members, enums, strings, comments and
  interpolation. Status: **done**. The member set comes from the analysis where there is one — the
  class's own data members plus every property and signal accessor it inherits — and from a scan of
  the buffer's indentation only for a file that has never been built. Comment markers (the editor's
  own critical/warning/notice word lists) colour the way GDScript's do. The **type** set is every
  name the mirror exports — the 1036 classes, the 793 enums, the sixteen value types, `rid`,
  `variant` and the containers — plus Verse's own type names that its reserved-word list does not
  carry, which is `char` and the concurrency vocabulary, plus every generated binding (R-INT-7)
  and every type the open file declares at top level. Three of those groups reached it through
  one table and the rest through none, so `vector2` coloured and `vector2i` did not; the units layer
  now checks the tables against the mirror's own source, which is the half of this a headless run
  can see. **A type draws in one of the editor's three type colours**, split GDScript's way
  (`gdscript_highlighter.cpp`): engine for a mirrored class ClassDB knows, a mirrored enum and a
  GDExtension binding; user for the project's own classes, a binding for a script `class_name` and
  anything the open file declares; base for the value types and the exported ones. Only the class a
  file is *named after* is project-wide — a second class in a file is coloured in that file alone,
  which is the same trade the member scan already makes. Whether the editor *draws* the colour stays a by-hand check
  (`by-hand-findings.md` B23): the highlighter registers at Godot's editor initialization level, so
  nothing a `--script` run can reach ever constructs one.
- **R-TOOL-2 (MUST)** Inline diagnostics as you type, from real semantic analysis rather than a
  local parse. Status: **done** — analysis re-runs per keystroke, off the main thread, and is not
  subject to the single-generation rule. The editor's thread does not wait for one: what `_validate`
  answers is the analysis that last landed, and the fresh one replaces it when it does. R-PERF-2 has
  the latency. A build is the whole project, so a file that fails to compile elsewhere used to
  refuse Play with no sign in the editor until that moment; now it is reported either way, but not
  at the same price. Godot reads its per-file `depended_errors` partition **only when `_validate`
  answers invalid**, and answering invalid clears the connection gutter, freezes the method outline,
  marks the tab errored in the script list and leaves a stale error bar uncleared. GDScript pays
  that for a file the script *depends on*; here every file is a dependency, so paying it for an
  unrelated one would degrade every open tab. The split: a file that **already has its own errors**
  also carries the other files' (free @EM@ all of the above is being paid anyway, and the sections are
  clickable), and a **clean** file gets one row in the warnings panel naming them instead, which
  Godot reads whatever `valid` says and which costs none of it.
- **R-TOOL-3 (MUST)** Code completion: members, locals, types in scope, imported package contents,
  and keywords. Status: **part** — the popup opens at once from what the last analysis left, and
  refines in place when the analysis this buffer needs lands. At a bare identifier that first answer
  is the enclosing class's own members, the class names and the keywords; inside a class body it
  also carries the inherited members a subclass could still declare with `<override>`, which are
  what an author reaches for there and used to arrive ~0.7 s late. After a `.` the first answer is
  whatever the receiver can be read off the text as — `Self`, a member of the class with a declared
  type, or a mirrored class named outright — and the class chain's members from the snapshot;
  anything else (a call's result, a local, a dotted chain) still waits, because a wrong member list
  is worse than a late one. Inside a string literal the answer is a node path, a `res://` path, an
  input action or a signal name, chosen by the call the literal is an argument to. Ranking is by
  inheritance distance since ABI 8.0: the host says how many hops separate an item's owner from the
  class asked about, so a class's own members sort above its parent's above Object's.
  **A `?` at the head of an argument answers the callee's named parameters and nothing else** —
  `Input.IsActionPressed("jump", ?ExactMatch := true)` — off the same signature the argument hint
  is drawn from, so it is answerable on exactly the keystrokes the hint is and costs no second
  question. It is the `@` case again: every name in the enclosing scope is *refused* at that
  position rather than merely unlikely, so the whole scope is the wrong answer there. The `?` the
  author typed is left alone and the option inserts `Name := `, because a bare `?Name` is an option
  *type* and not an argument. Telling the three spellings of `?` apart is done from the buffer: a
  named argument's `?` follows the call's own bracket or a comma, where the postfix unwrap of
  `Target?` follows an expression and the `?node2d` of an option type follows a `:`.
  **Whether a parameter is named crosses the ABI as a flag** (`vh_complete_item::IsNamed`, ABI
  **9.0**, a major because the items are an array and a field at the end moves the stride). It has
  to: the compiler records the `?` on the function *type*, not on the parameter's definition —
  `AnalyzeParam` types the definition with the value type and wraps it in a `CNamedType` afterwards
  — so `?ExactMatch:logic` reaches a consumer as `ExactMatch:logic`, which is a call Verse refuses.
  The argument hint spells the `?` for the same reason.
  **The buffer the host is asked about is repaired before it is sent.** A half-written line is a
  *parse* error and uLang keeps no partial snippet, so nothing maps a VST node to the file and
  `vh_complete_symbol` answers `VH_ERR_NOT_FOUND` — the author was left with the class names and
  keywords this side appends and nothing else: no local, no member, no `Print`. Two shapes do it,
  and one of them is every keystroke of every condition, because Godot's auto-brace completion
  supplies the `)` and never the `:`: `if (X)` short of its `:` is *"Expected block, got end of line
  following `if`"*, and an unclosed bracket is *"Block starting in `(` never ends"*. So
  `verse_repair_completion_buffer` closes what the buffer leaves open and gives a bare `if` its `:`,
  appending only past the caret so no position anything was measured against moves.
  **Nothing a class var's `<getter>`/`<setter>` names is offered.** The mirror carries 7344 of them,
  two per Godot property, and none is spellable: the `accessor` parameter has no spelling and only
  the compiler ever names one, where it rewrites a read or a write of the var the attributes are on.
  Offered, they were **2816 of 13114 options — a fifth of every popup** over `dodge-the-creeps`
  (`tools/probe_complete.py`). `DescribeCompletion` refuses one the way it already refused a
  compiler-generated constructor, off the flag `IsOverridable` was reading for a narrower purpose,
  which puts the refusal below both paths a completion arrives by — the snapshot answer, whose walk
  passes no access scope at all, and the live one.
  **Whether Godot opens the popup is the editor's decision and not the language's.** The trigger
  characters are hard-coded in `CodeTextEditor`'s constructor — `.`, `,`, `(`, `=`, `$`, `@`, `"`,
  `'` — and a caret with nothing typed behind a character outside that list is cancelled by
  `CodeEdit::_filter_code_completion_candidates`, which `force` does not exempt. Two of the
  positions above sit behind a character Godot never needed, so `vector2{` and `Foo(?` were
  answering correctly and being closed before they drew; a second field already worked, because `,`
  is in the list. The list is a per-`CodeEdit` property, so `VerseEditorPlugin` widens it with `{`
  and `?` on the Verse editor alone. Not `:` or `<`: those decline an empty prefix in
  `_complete_code` itself, so a popup there would have nothing to draw.
  **The whole surface has an instrument now**, `tools/probe_complete.py` over
  `VerseScriptLanguage::probe_complete` — `_complete_code` is a virtual, so no script can call it,
  which is why a fifth of the popup could be unwritable names for a phase with nothing to say so.
  It reports the answer rather than the popup; the trigger half stays by hand. `if` alone:
  `for` and `case` recover from the missing `:` on their own, measured, and a balanced buffer is
  never touched, so nothing that parses today can be broken by it.
- **R-TOOL-4 (MUST)** Hover and ctrl-click: type, signature, doc comment; go to definition, for
  both user code and the mirrored Godot API. Status: **part** — a known defect is that ctrl-hover
  inside a string interpolation underlines the whole string. A position resolves against the AST an
  analysis is rebuilding, which no snapshot describes, so a hover during one **declines** rather
  than waiting; Godot treats that as "no result" and the next hover answers. Every one of those
  refusals first tries the symbol as a mirrored class name against the generated table, which needs
  no AST and no host — so a hover on `node2d` answers during an analysis, before a first build, and
  on a buffer the analysis has not caught up with. It never preempts a resolved answer. A type the
  mirror exports that is not a mirrored class answers Godot's page for what it carries — `variant`
  is Variant, `godot_array` is Array, `char` is String because `string` *is* `[]char` — and the two
  that stand for nothing of Godot's, `signal(t)` and `connection`, keep the mirror's own comment
  instead of borrowing a page that describes something else. The same rule reaches the mirror's
  *functions*: the 159 math extension methods (`(V:vector2).Length()` is `Vector2.length`) and the
  50 hand-written globals (`Smoothstep`, `LerpAngle`, `ToString`) each name the page Godot
  documents them on. An extension method arrives as a module-level definition named
  `operator'.Length'`, so its owner is the file rather than the receiver, and the receiver is read
  off the first parameter of its declared type — which is also what tells `V.Snapped(Step)` from
  the scalar `Snapped(X, Step)`, two different pages behind one name. **What stops here is what
  mirrors nothing**: `MakeVariant`, `godot_array.GetInt`, Verse's own `event`, and a second class
  in a file. Each keeps the local result, because it is the only one that carries the comment above
  the declaration, and there is no Godot page to prefer over it.
  **What the comment above the declaration cannot reach, the host now hands over** (ABI **11.0**,
  `vh_lookup_desc::DocUtf8`). Verse's own library documents itself with a `@doc("...")` *attribute*
  — 132 of them across `/Verse.org/Verse` — so the text is in no line above the declaration and no
  reading of the source produces it; and a definition in a package the project does not own has its
  file in the engine tree, which is not one an editor should open on a hover keystroke. `Sqrt`,
  `event` and `Concatenate` drew a type and an empty box until the host read the attribute with
  `GetAttributeTextValue` and reported it. The consumer still prefers its own reading of a file it
  holds the buffer for: that one is current with an unsaved edit and this is not.
  **11.0 is a major because `vh_lookup_desc` had no `StructSize`** — nothing a consumer could check
  before reading a field appended after the version it was built against, and no way to make the
  addition ignorable. The field is there now, so the next one can be a minor.
  **The prose reaches Godot as its own doc BBCode.** Godot draws a local result's description with
  `_add_text_to_rt` — every `\n` a paragraph, every `[` a tag, and nothing read for a backtick — and
  both readers joined comment lines with `\n`, so a five-line comment was five paragraphs with its
  backticks printed and `Floor[X]` half-parsed. `src/verse_doc_markup.{h,cpp}` converts at the
  boundary, in every place a description is handed over (the lookup result, and a script doc's
  members and class text): consecutive lines join with a space and a blank line is a paragraph, which
  is GDScript's own `_process_doc_line` rule; a backtick span is `[code]`; an indented or fenced
  block is `[codeblock lang=verse]`, named so Godot does not run the GDScript highlighter over it;
  `**bold**` and `*emphasis*` under Markdown's rule; a bullet breaks the line; a tag Godot's own
  documentation accepts passes through, and any other bracket is `[lb]`/`[rb]`. The readers keep a
  line's indentation now — only the delimiter and the one space after it come off — because an
  indented sample stripped flat is a sentence. The rules are the units layer's
  (`tests/verse_doc_markup`), the whole converted string of one member is asserted in
  `tests/integration`, and what the tooltip draws with it is by hand.
- **R-TOOL-5 (MUST)** Signature help while typing a call. Status: **part** — declines during an
  analysis for R-TOOL-4's reason, and queues that buffer so the next ask answers. It is asked about
  the same repaired buffer R-TOOL-3 describes, off the same analysis, so an unclosed call has a hint
  again. It no longer asks about a block macro's head at all: `if (` looks like a call to the
  backward scan that finds a callee, and there is no signature for one, so every keystroke inside a
  condition used to spend a `vh_signature_at` that could only answer `VH_ERR_NOT_FOUND`.
- **R-TOOL-6 (SHOULD)** Find references and rename across the project. Status: **none**.
- **R-TOOL-7 (SHOULD)** The script editor's outline/member list is populated. Status: **done** —
  `_validate`'s `functions` key is what `ScriptTextEditor::get_functions()` builds the outline from
  and what places the connection gutter icon, and `_get_member_line` answers from the same snapshot,
  which is what `ScriptEditor::script_goto_method` needs for the Connections dock's "Go to method"
  and for an animation method track.
- **R-TOOL-8 (SHOULD)** Doc comments on a script's classes and members reach Godot's own
  documentation panel, so a Verse class is documented the way an engine class is.
  Status: **part** (`_get_documentation`, `_supports_documentation` exist).
- **R-TOOL-9 (MUST)** Auto-indent, comment toggling and the new-script templates produce Verse that
  compiles and is tab-indented. Status: **part**.
- **R-TOOL-10 (SHOULD)** A language server exists and speaks LSP over the same host analysis, so
  VS Code and other editors get R-TOOL-2 through R-TOOL-5. Today this is not possible: `uLangLSP`
  in the UE checkout is a message-type library, not a Program target, and nothing links it into a
  binary. Writing our own server over `verse_host_abi.h` is the alternative. Related: **OQ-7**.
- **R-TOOL-11 (MAY)** A formatter. Where it will plug in is decided for us: see the
  `EditorLanguage` note below.
- **R-TOOL-12 (MUST)** The editor maintains `using` statements; a user never types a module path.
  Status: **part**, and both halves of it work. The import materialises when analysis reports the
  unknown name — goimports-style, reacting to the *diagnostic* rather than to the keystroke,
  because Godot's completion API carries no edit-on-accept hook to hang it on. `vh_resolve_unknown_name`
  answers which of the project's modules declare the name; one answer is inserted, more than one is
  reported and left to the author, because two modules declaring one name is legal and only the
  author knows which was meant. Insert only: nothing is ever removed, because removing a line the
  author may have written by hand is a different and worse promise.
  **The buffer unknown is closed**: `ScriptEditor::get_current_editor()->get_base_editor()` reaches
  the active `CodeEdit` from a GDExtension, and the insertion happens from `_frame` rather than
  from inside the validate that found the name. The **fallback ships as well as the mechanism**
  rather than instead of it: the diagnostic always names the exact `using` line, which is what
  reaches an author whose file is not the one on screen.
  What is **missing** is the completion half — offering symbols from modules not yet in scope.
  Typing a name you already know, from another file, is the flow this covers; discovering one you
  do not is not. *Rewriting* imports when a file moves is explicitly **not** part of this either: a
  move is allowed to break its references and report them.

**Godot's `EditorLanguage` split, and what it means for the two above.** Godot master has moved the
editor-facing half of `ScriptLanguage` into a separate interface, `core/object/editor_language.h`,
reached through `ScriptLanguage::get_editor_language()`. Five methods: `validate`, `complete_code`,
`lookup_code`, `find_function` and a new `format_code`. The header says the design is meant to keep
the LSP spec in mind, because the GDScript language server uses the same API, and that the goal is
to move all editor functionality out of `ScriptLanguage` over time.

A GDExtension reaches it through `ScriptLanguageExtension::EditorAdapter`, which forwards each of
the five to the virtuals we already implement — `format_code` onto `_auto_indent_code`, the rest
one-to-one. So nothing here has to change to keep working, and nothing here *can* change yet: our
godot-cpp (4.6) has no `EditorLanguage` in `extension_api.json`, so the interface is not
implementable from an extension at all, only reachable through the adapter.

Two things follow, and they are why this is written down rather than acted on:

- **R-TOOL-11's formatter belongs on `format_code`**, not on `_auto_indent_code`. Today the adapter
  makes them the same call; when the split reaches extensions they stop being the same call, and a
  formatter written into `_auto_indent_code` would be a formatter that runs on Enter.
- **R-TOOL-12's missing half gets a hook.** `CompletionOption` carries a `TextEdit` — "optional
  server side calculated insertion", a range plus replacement text the editor applies without
  matching preexisting text. That is exactly the edit-on-accept hook whose absence is the reason
  the `using` insertion reacts to the diagnostic instead of to the accepted completion. The header
  notes the builtin editor does not support it yet; the language server does.

Also worth knowing: `r_force` is documented as possibly going away, in favour of showing a signature
hint and a completion list together. `_complete_code`'s `force` key is ours to stop relying on for
anything but "open the popup".

---

## 10. The iteration loop

**Full hot reload is required.** Edit a script, and the new code runs — no editor restart, ever.

This was the hardest requirement in the document and it is **met** as of Phase 3: R-ITER-1 through
R-ITER-5 are done. It is left standing at full strength because the reason it was written has not
changed — an authoring loop with a restart in it is not a tool people use — and because two of its
neighbours are still open: a **running game** does not pick up an edit (R-ITER-7) and retained
memory is unbounded across a session (R-ITER-6).

What it replaced, for the record: any change to a script's *code* (as opposed to its shape, which
analysis has refreshed live since Phase 1) used to require restarting the editor, and a script added
while the editor ran was not seen at all.

- **R-ITER-1 (MUST)** Editing a script and running the project executes the edited code, in the
  same editor session, indefinitely many times. Status: **done**. `VerseEditorPlugin::_build` runs
  before every Play and publishes a generation; `Project > Tools > Build Verse` does the same on
  demand. Proven in `host_smoke` (a second generation runs the edited code, and the rest of that
  suite then runs against it) and in the integration project (a node attached before a build goes
  on answering after it).
- **R-ITER-2 (MUST)** A `.verse` file added, renamed or deleted while the editor runs is picked up
  without a restart. Status: **done**, and it needed no mechanism of its own: every build
  re-enumerates `res://` rather than building a remembered list.
- **R-ITER-3 (MUST)** A changed default value on an `@export` member refreshes in the inspector
  (R-EXP-4). Status: **done**, on a **build** rather than on a save, because a declared default is
  evaluated by generated code. `VerseScript::generation_published` re-reads the export list off the
  new generation and calls `notify_property_list_changed`. The two halves of an export move at
  different speeds and that is the price of the trigger: a newly declared member appears as soon as
  analysis sees it, while its default value is whatever the last build generated.
- **R-ITER-4 (SHOULD)** Reloading preserves the state of a running game where Godot's own
  `reload(keep_state)` contract allows it. Status: **done**, with its meaning stated rather than
  assumed: **an instance keeps its own generation**. Nothing is invalidated under the engine and no
  state is transferred, because an instance made against generation N goes on running generation
  N's class for life. Adopting a new class is a deliberate act nobody has asked for, and it would
  need a state-transfer path across the ABI that does not exist.
- **R-ITER-5 (MUST)** A compile error during reload leaves the previously-working code running and
  reports the error; it does not leave the project in a half-loaded state. Status: **done**. A
  failed build publishes nothing, so the last generation that succeeded is still what runs, and
  `_build` returning false makes Godot abandon the run rather than launch a game whose code the
  author has already been told does not compile — the same thing C# does with the same hook. One
  consequence decided with Phase 3's design: the
  inspector keeps showing the *analysed text's* shape rather than the last good generation's, so it
  can briefly show a property no running code has. Shape has come from analysis since Phase 1 and
  refreshes live; this is the lesser of the two surprises and the one that is already true today.
- **R-ITER-6 (SHOULD)** Retained memory across a long editor session is bounded. Status: **none**,
  deliberately, and now with a **measured figure** rather than an estimate. `tests/host_bench`
  builds ten generations of `dodge-the-creeps` — five files, a class each, the whole 1023-class
  mirror behind them — and reports **~1.3 MB retained per generation** (median of ten; the first is
  larger). That is the previous generation's `VPackage`, its `UPackage` and their pinned exports,
  and nothing reclaims it. The build-on-Play trigger keeps a session's generations in the tens
  rather than the hundreds, which is most of why this is tolerable: tens of megabytes across a long
  day, next to the ~110 MB the compiler and native packages cost once. Had the trigger been
  per-save it would not be. Accepted while the project is experimental; no reaping mechanism is
  built. This requirement exists so the deferral is tracked rather than rediscovered.
- **R-ITER-7 (SHOULD)** A **running game** picks up an edit without being restarted. Status:
  **none**, deferred out of Phase 3 by decision. R-ITER-1 is satisfied without it, because a run is
  a fresh process that compiles the current source; a game already running has its own host in its
  own process, so reaching it means a second delivery mechanism — Godot's remote-debugger channel —
  rather than an extension of the editor's. Wanted eventually; not a gate on anything above.

**The mechanism is settled (OQ-8): a fresh package name per generation**, with
`FSolarisModule::IncrementalizeProjectSource` called before each build so that everything already
compiled in this process — the native packages above all — is marked external and skipped. The
obstacle README describes ("The project is the compilation unit") is real but is only reachable by
publishing the same package twice, which nothing obliges us to do.

Measured at 25 generations in one process: 161–220 ms per reload in steady state, ~0.5 MB retained
per generation, and each generation running the edited code. §14.1 has the numbers and
[`phase-0-spikes.md`](phase-0-spikes.md) the method. Two things that follow for the requirements
above:

- R-ITER-4 comes close to free. An instance created by an earlier generation keeps working against
  its own generation's class, so a reload invalidates nothing under the engine's feet and adopting
  the new class becomes a deliberate act.
- The host must own its script package rather than borrow the IDE's, whose name is fixed. That
  costs a small `ISourceSnippet` implementation with a settable text, without which analysis and
  completion regress.

*Out-of-process compilation* lost here but is the shape of the export pipeline under OQ-2. *An
engine change* lost because none is needed.

**What Phase 3's design added on top of the settled mechanism** ([`phase-3-design.md`](phase-3-design.md)):
a generation is built **on Play and on an explicit Build action, not on save** — the trigger is the
one thing the mechanism does not imply, and this is what Godot already does for the one language
whose compilation unit is the whole project like Verse's: C# is built by
`BuildManager.EditorBuildCallback` → `BuildProjectBlocking`, invoked on Play. GDScript reloads
synchronously on save and gets away with it only because its unit is one file at about a
millisecond. The hook needs no new Godot API — `EditorNode::call_build()` calls `build()` on every
editor plugin before a run and aborts the run if one returns false, which is R-ITER-5's reporting
path as well as the trigger. Saving still refreshes analysis, so diagnostics, completion, lookup and
the export *shape* stay live per keystroke; only generated code waits for a build.
**Instances adopt nothing**: a live instance keeps its own generation's class, which is what
R-ITER-4 asks for and costs no state-transfer path across the ABI.

**The last unknown about the mechanism is now closed.** S-2 gave each generation its own package
*name* but did not record whether the *verse path* `/user@localhost` held across those generations
— which decides whether a user-written module path, the sort R-TOOL-12 writes into the author's own
file, survives the author's next save. It does: **the name changes and the path is pinned**, and the
one thing that has to happen besides is that the retiring generation's package leaves the *source
project* before the next build, or every class in it is declared twice at one path. **OQ-12**, closed
in §14.1 with what the run also confirmed about root being implicit from a submodule.

---

## 11. Diagnostics, debugging and profiling

- **R-DIAG-1 (MUST)** A compile error appears in Godot's script editor at the right line with the
  compiler's message. Status: **done**. The output log is the *build's* alone: an analysis — per
  keystroke, on save — writes nothing to it at any severity, because the log cannot retract a line
  and the script editor replaces its own list on the next validate; a build writes every diagnostic
  it filed, every time, since it is something the author asked for. The compiler's warnings
  (glitches 2000–2007) reach the script editor's warning list through `_validate` the same way its
  errors reach the error list.
- **R-DIAG-2 (MUST)** A *runtime* error — a failed unrecoverable expression, a stale object access,
  a division by zero — reports the Verse file, line, and a Verse call stack into Godot's output and
  errors panel, and is clickable to the source. Status: **done**. `vh_init` takes an
  `OnRuntimeError` callback carrying the message and the frames; the host binds
  `RuntimeErrorTextProvider` (which is handed the rendered callstack while the Verse stack is still
  standing) and `OnVerseRuntimeError` (broadcast after the abort), because neither hook alone
  carries both. The innermost located frame is what `push_error` is given, so Godot makes it
  clickable, and the rest of the stack follows it. *Known shape:* a raise inside the generated
  mirror reports the mirror's line as the site, with the script's own frame further out — which is
  where it was raised, and the stack is what carries the author's line.
- **R-DIAG-3 (MUST)** A script error never takes down the editor or the game process. Status:
  **done**, after being narrowed three times.
  A raised runtime error calls `Terminate()` on the active `FContentScope` (`VVMRuntimeError.cpp`),
  and `FRunningContext::EnterVM_Internal` then returns *without invoking its functor* for every
  later entry into that scope (`VVMEnterVMInline.h`). Until Phase 3 the host made one scope that
  lived for the process, so the first raise anywhere stopped everything: a call reported `VH_OK`
  having not run, and a read reported "no such member". Phase 3 made that survivable with
  `ResetTerminationState()` at the next `vh_tick`, which is the API Epic's own `VerseNativeTests`
  use after deliberately raising.

  **Phase 5 replaced that rule rather than refining it**, because R-ASYNC-4 changed what the scope
  *is*. Three rules hold now, each pinned in `host_smoke`:
  1. **A raise stops the call that raised, and nothing else.** The scope it terminates is the
     raising instance's, so another instance's next call runs in the same frame — and so does the
     raising instance's, because a terminated scope is **replaced** at that instance's next call
     rather than un-terminated at the next frame boundary. That is Epic's own policy:
     `ContentScopeRepository` hands out a fresh scope and never resets a terminated one.
     `GHaltedUntilTick`, `GTasksLostToError` and `ReviveContentScope` are all gone with the old
     rule, and `vh_tick` is no longer where anything recovers.
  2. **Nothing pretends to have run.** `vh_instance_call` checks, after the fact, that the VM
     actually ran the body and answers `VH_ERR_HALTED` when it did not — it was that confusion
     between "did not run" and "ran and found nothing" that kept this invisible for a phase.
     `VH_ERR_HALTED` is now a narrow answer rather than what every other call got for a frame.
  3. **The author is told what it cost** — when there is something to tell. One line when a raise
     cancels suspended work, saying that it was *this instance's* and that others are unaffected,
     because that is the part the error message cannot carry. Sampled in the runtime-error handler,
     which is the last moment the task group can be asked, and silent otherwise: a script that
     raises every frame already reports its error every frame.

  **Phase 6 closed the two things that were left**, and both turned out narrower than they read.

  4. **The bridge's own stack printing is rate limited.** This was the actual defect behind
     OQ-13, and it was not the one the question asked about. Godot already drops *errors* past
     `network/limits/debugger/max_errors_per_second` and says once that it did; what it does not
     throttle is the Verse stack the bridge prints underneath each one, which is ordinary output
     counted against `max_chars_per_second` — so a script raising at 60 Hz with a six-frame stack
     emitted ~360 lines a second into a budget every other script shares, and silenced them. Keyed
     on the raise site (the innermost located frame's path and line) plus the message: the first
     prints in full, repeats inside a one-second window print nothing, and the window closing says
     how many were dropped, in Godot's own wording for the same thing. The summary is flushed from
     `vh_tick` rather than by the next occurrence, so a script that raised sixty times and then
     stopped is still told what was swallowed.
  5. **The `@tool` clause is met by rules 1 and 3, not by a new mechanism.** It was written in
     Phase 3's world, where one raise stopped every script in the process. Since R-ASYNC-4 an
     editor-time raise costs the raising instance's suspended work and nothing else's, and no Godot
     write survives the failed transaction — so the scene the author is editing is unharmed, and
     the error is reported rather than swallowed. The same rate limit applies in the editor, which
     is the whole of what the editor needed that the game did not.

  **OQ-13's answer is that nothing is bounded**, and that is a decision rather than an omission.
  Godot does not bound GDScript either; per-instance scopes already confine the cost to the raising
  node; and disabling an instance is a policy an author cannot see coming and cannot undo without a
  reload. What was chosen instead is **observability**: `verse/instance_tasks` is a Godot custom
  monitor carrying the largest number of live tasks any one instance's scope holds, which is what a
  `spawn` in `_Process` runs away with and what no other number here can separate from many
  instances with one task each. Status: **done**.
- **R-DIAG-4 (MUST)** Godot's own debugger works on Verse: breakpoints in the script editor, step
  in/over/out, the call stack, and local and member inspection. Status: **done**, with two
  amendments written into the requirement rather than left in a design document.

  **Built on `Verse::FDebugger`**, the four-method interface `SetDebugger()` installs, not on
  Epic's `SocketDebugger` — which is one *implementation* of it and unusable here for a structural
  reason: it parks the mutator on its own condition variable, whereas `RemoteDebugger::debug()`
  loops on the thread that called it and services the editor from there. So the host asks Godot,
  Godot blocks on the interpreter's own thread, and Godot re-enters the host through new reads
  while that outward call is still on the stack.

  **The division is: the host owns which frame, the consumer owns which line.** The breakpoint
  list and the step state are `EngineDebugger`'s and are never duplicated in the host. What only
  the host can see is frame ancestry, and it is needed: the bridge never sees a Verse call, only a
  bytecode op, so Godot's depth counter would never move and step-over would behave as step-in.
  `DebugShouldBreak` therefore carries a `vh_debug_frame_relation` — same frame, deeper, or
  neither — and the consumer reads Godot's depth as which *kind* of step is pending rather than as
  a count.

  **The dedup is not optional.** `Notify` fires per bytecode op, so the host filters by
  (frame, file, location) before asking anything, exactly as `VSocketDebugger::UpdatePrevLocation`
  does. Without it the ABI would be crossed millions of times a second.

  *Amendment 1 — no expression evaluation.* Three independent reasons, any one sufficient. Epic's
  own Verse DAP client handles no `evaluate` and no `setVariable`, so there is nothing in Verse's
  tooling to build on. Godot never asks: its `evaluate` command bails when
  `debug_get_stack_level_instance` is null, which is permanent here — Godot calls a C++ virtual on
  what that returns, and a GDExtension script instance is not a `ScriptInstance`, so returning
  anything is a type-confused virtual call. And evaluating would mean compiling an expression
  against a stopped frame's scope and running it in a VM paused mid-op.
  `_debug_parse_stack_level_expression` is **removed** rather than left empty: it is `EXBIND`, so
  unbound is silent, and it is the one debug virtual where "omitting one is how you say
  unsupported" applies cleanly.

  *Amendment 2 — stepping follows the interpreter, not the task.* **Known shape.** Verse tasks are
  not OS threads and Godot's debugger has no concept of them, so a step over a line that suspends
  is handed to whatever the interpreter reaches next, which may be a different script's
  `_Process`; and a second instance of the same script reaching the same line can take a step
  asked for in the first. This matches the only Verse debugger that exists and diverges from
  GDScript, whose coroutines each carry their own stack.

  *Known shape — a line that emits no op carries no location.* Measured: every statement line
  reports one, and a function's declaration line does too, but a trailing bare expression that
  only reads a register (`Inner` as the last line of a body) does not, so a breakpoint there never
  fires. `tests/host_smoke` has it as a case so a change in the compiler would move the answer
  rather than go unnoticed.

  *Known shape — self and its fields are the members list.* `Self` appears under
  `_debug_get_stack_level_members` because `_debug_get_stack_level_instance` cannot answer, and the
  list is filtered to *data* members: a script class inherits ~52 of Godot's own methods through
  its shape, and showing them would bury the three the author wrote.

  A local arrives as a real Variant when the bridge carries its type — an `int`, a `float`, a
  `string`, a `logic`, a Godot object, and any of the mirrored math structs — and as
  `VValue::ToString` otherwise, which covers a tuple, an option, a map, a class instance of the
  author's own and every container wrapper. The math structs are in the first list rather than the
  second because they are the one shape a value *can* name itself: a `vector2` says so through its
  class, where an empty array cannot say what it holds and a `false` cannot say whether it is a
  logic or an empty option. Without that a `vector2` reached the inspector as the *text* of one
  rather than as a `Vector2` slot, which is what the editor session found. A register outside its live range
  at the stopped op is reported as `<not yet in scope>` rather than dropped: the name is in scope in
  the source the author is reading, and its absence would read as a bug.

  **The debugger attaches whenever Godot's is active, and detaches when it stops being** — which is
  unconditionally correct rather than conditionally cheap, and the measurement is what allowed it.
  Attaching turns `CheckForHandshake` from a relaxed load and a compare into the slow path and a
  virtual call on *every bytecode op*: `tools/build_bench.py` reports a one-line method's call
  going from **0.27 µs to 2.79 µs**, ten times. At frame level that is **+1.6%** — the
  `dodge-the-creeps` yardstick, headless at `--fixed-fps 60`, runs in a median 4.26 s plain and
  4.33 s under `--debug`. The polled breakpoint mirror the design held in reserve (sweep
  `is_breakpoint` over the lines each script reports, attach only when one exists) is therefore
  **not built**; it would have bought back the per-op cost at the price of a breakpoint that arms
  on a delay. Attaching also suspends VerseVM's computation watchdog, which is what makes sitting
  on a breakpoint for a minute legal rather than an `ErrRuntime_ComputationLimitExceeded`.

  **Re-entering a stopped VM is safe** (measured, `tests/host_smoke`): an ordinary
  `vh_instance_call` or `vh_instance_get_field` from inside Godot's debug loop runs, and the outer
  frame resumes correctly afterwards — so the remote inspector stays live while paused. What is
  refused while stopped is `vh_tick` (silently; resuming a slept task inside a VM stopped mid-op is
  not something any of this is designed for) and the three that build or analyse — publishing a
  generation underneath a frame belonging to the retiring one, or resetting the semantic program
  the stopped frame is about to resume into.
- **R-DIAG-5 (MUST)** The same applies to profiling: Verse functions appear in Godot's profiler
  with their own timings, and the per-frame Verse tick cost is visible. Status: **done**.

  **Boundary instrumentation plus Verse's own `profile{}` blocks, and not a sampler.** Godot's
  `ProfilingInfo` wants a call count, a total time and a self time per function, and Verse offers
  no per-call hook of any kind — the only thing that could produce one is `FDebugger::Notify`,
  which fires per bytecode op. `FSamplingProfiler` was considered and rejected because it cannot
  produce a call count, so every row it contributed would carry a fabricated one.

  What the bridge knows exactly is every crossing it makes, so those rows are true: one per script
  method Godot calls, one per Verse callback invoked through a `Callable`, and one synthetic
  `<verse>::0::vh_tick` for queued work. Self time is the total less the time spent in *nested*
  boundary entries, which is what makes a Verse method that emits a signal that calls another Verse
  method attribute correctly.

  **The limit, written here rather than left for a user to discover: a Verse function called from
  another Verse function has no row of its own** unless the author wraps it in `profile("tag"){…}`,
  which the compiler accepts in a `/user@localhost` package and which the VM reports through
  `FVerseProfilingDelegates::OnEndProfilingEvent` with an exact count and an exact time.

  A row's signature is GDScript's three-part shape, `res://scripts/player.verse::12::player._Process`,
  because the editor's profiler splits on it. Off until Godot turns it on; with it off a boundary
  crossing pays one relaxed load and a predicted branch, and a `profile{}` block costs a delegate
  that is not bound.

  **Landmine.** The array Godot hands `_profiling_get_accumulated_data` is *not* laid out the way
  godot-cpp thinks it is: `ScriptLanguage::ProfilingInfo` has carried a fifth field since 4.3
  (`internal_time`) that its `GDREGISTER_NATIVE_STRUCT` registration string still does not mention,
  so godot-cpp generates a 32-byte struct for an array whose real elements are 40. Indexing past
  element zero writes into the wrong offsets and eventually past the end. The bridge takes the
  stride from the engine's version instead, and leaves `internal_time` as Godot zeroed it.
- **R-DIAG-6 (SHOULD)** The Verse debugger the host already links (`Verse::SocketDebugger`, port
  1963) is usable from an external editor. Known: the port is real and the flag that opens it
  works. Unknown: whether any DAP client can speak its 4-byte-length-prefixed JSON framing, which
  is not the `Content-Length` transport standard DAP uses. Status: **not needed**, and **OQ-9 is
  closed with it** — the roadmap made both conditional on R-DIAG-4 turning out blocked, and it did
  not. Still reachable: `verse/host/enable_debugger` opens the port exactly as before. The two
  cannot both be attached, because `SetDebugger` is one global pointer, and the one asked for at
  `vh_init` wins; the bridge says so once rather than every frame.
- **R-DIAG-7 (SHOULD)** Verse `Print` and the engine's logging land in Godot's output panel with
  the script's identity attached. Status: **part**.

---

## 12. Engineering quality

- **R-QUAL-1 (MUST)** A real test suite replaces the hand-rolled `main`s. It covers three layers:
  the host ABI in isolation (what `host_smoke` does today), pure units with no Godot dependency
  (the lexer and class-declaration scanner already have this shape), and **integration tests that
  drive a headless Godot with Verse scripts attached and assert on behaviour** — the layer that
  does not exist and where every parity requirement in §5 will actually be verified.
  Status: **done in shape, growing in coverage** — the third layer exists:
  `tests/integration/` is a Godot project driven headless by `tools/run_tests.py`, attaching a
  `.verse` script to a node and asserting on what `node.call()` answers. What it covers is what §6
  currently carries; the marshalling matrix grows with §6 rather than with this requirement.
- **R-QUAL-2 (MUST)** Every requirement in this document that is marked done is covered by a test
  that would fail if it regressed.
- **R-QUAL-3 (MUST)** One documented command runs everything a contributor can run locally, and
  reports pass/fail without interpretation. Status: **done** — `python tools/run_tests.py`. A layer
  whose prerequisites are absent is reported as **skipped**, never as a pass, so a contributor
  without a UE checkout sees exactly which of the three layers ran.
- **R-QUAL-4 (SHOULD, blocked)** CI runs builds and tests on every commit. Blocked on **OQ-1**:
  CI needs a UE source checkout with the Verse toolchain, which cannot be provisioned on a hosted
  runner under current licensing. Until then, R-QUAL-3 is the substitute and the gap is
  acknowledged rather than papered over.
- **R-QUAL-5 (MUST)** `verse_host_abi.h` is semantically versioned with a written compatibility
  policy: what a bump means, what a mismatch does, and which side must be rebuilt.
  Status: **part** — `VH_ABI_VERSION` is now `MAJOR * 1000 + MINOR` at 2.0 with the policy written
  at the top of the header: a major changes layout or meaning and both sides must be rebuilt, a
  minor adds what an older consumer can ignore behind a `StructSize` check. What is still missing is
  the compatibility *window* — nothing yet tests a host against a consumer of a lower minor.
- **R-QUAL-6 (MUST)** Releases are tagged, carry a changelog, and state the exact Godot and UE
  versions they were built and tested against.
- **R-QUAL-7 (MUST)** The project tracks Godot `master` and UE `main`. Consequently: a version
  mismatch must be *detected and reported*, never left to manifest as a crash, and the support
  matrix in each release is a statement of what was tested, not a promise of a range.
  Status: **none** — `compatibility_minimum` is `4.5` and nothing checks the engine's actual build.
- **R-QUAL-8 (MUST)** User-facing documentation exists and is distinct from README. README is the
  design document and stays that; a user needs a manual — installation, writing a first script,
  the `@export` reference, the Godot API mapping, the concurrency model, and the known
  limitations. Status: **none**.
- **R-QUAL-9 (SHOULD)** Generated files stay generated. `GodotClasses.native.verse`,
  `verse_api_classes.h` and `verse_keywords.h` are never hand-edited, and regenerating them is
  part of R-QUAL-3. Status: **done** as a rule.

---

## 13. Performance

**Targets are deliberately deferred; numbers are not.** The posture is correctness first: no target
is committed in this draft, and no design decision in §§4–12 may be justified by an unmeasured
performance claim. What is measured is recorded in R-PERF-2 and carries no threshold.

- **R-PERF-1 (MUST)** Before 1.0, this section is replaced by measured numbers and stated targets
  for, at minimum: per-frame overhead of an empty `Process` against an empty GDScript `_process`;
  property read and write cost; a method call with marshalled arguments; project compile time at
  10, 100 and 1000 scripts; and editor analysis latency per keystroke. **The last of the five is
  measured**: R-PERF-2's table has it at 520 ms, with a call cost and an instantiation cost beside
  it. The empty-`Process` comparison against GDScript, the property costs and the compile-time
  curve at 10/100/1000 scripts are all still outstanding, and so is every target.
- **R-PERF-2 (MUST)** Benchmarks exist and run under R-QUAL-3 for visibility, before any target is
  set. What gets measured early is what can be reasoned about later. `tests/host_bench` is where
  they live — reported rather than asserted, because a threshold would fail on a slower machine.

  The numbers below are `tools/build_bench.py` on the machine this document is written on, n=10,
  against the full 1038-class mirror (4537 KB of Verse) with `tests/host_smoke`'s two fixtures as
  the project and `dodge-the-creeps`' five scripts as the generation. Medians; the mean is quoted
  only where the bench reports no median.

  | what | figure |
  | --- | --- |
  | `vh_init` | **77 ms** |
  | **first** `vh_compile_project` | **2.36 s** — the mirror is still source here, and this is where the location/accessor side table is recorded |
  | a **generation** after it | **694 ms** (min 621, max 715) |
  | a generation **after an analysis**, with nothing edited since | **68 ms** (min 66, max 76) |
  | `vh_check_project` — one whole-project analysis | **520 ms** (min 516, max 739 — the max is the one analysis that parses the mirror) |
  | the same through `_begin`/`_poll`, wall clock | **597 ms**, 291 polls |
  | a read taken **during** an analysis (`vh_class_members`, then `vh_class_export_list`) | **0.0 ms** each, wait counter 0 — it was 1735 ms |
  | completion, members: refused / behind the analysis / warm | **0.0 / 593 / 0.7 ms** |
  | completion, scope: refused / behind the analysis / warm | **0.0 / 580 / 5.9 ms** |
  | `vh_signature_at`: refused / warm | **0.0 / 0.0 ms** |
  | `vh_lookup_symbol`, warm | **0.1 ms** |
  | `vh_class_members`, `vh_class_export_list`, with an analysis landed | **0.0 ms** each |
  | `vh_class_override_candidates` | **0.0 ms**, 259 candidates for a `node2d` |
  | `vh_instantiate` | **5.2 µs** per node |
  | `vh_instance_call` | **0.24 µs** per call |
  | `vh_instance_call`, a `<decides>` method succeeding | **0.25 µs** per call |
  | `vh_instance_call`, a `<decides>` method declining | **0.26 µs** per call |
  | retained per instance | **5.0 KB** |
  | retained per generation | **1.0 MB** |

  **Declining costs nothing.** The two `<decides>` rows are the same fixture method called two
  ways -- `NotBelow(25, 17)` and `NotBelow(17, 25)` -- so the only difference between them is
  which `FOpResult` the VM answers, and the gap is inside the noise of a 0.24 µs call. That is
  what made it safe to spell all 161 Godot bool virtuals `<decides>:void`: `_HasPoint` is asked
  once per input event per `Control` under the cursor, and every "no" is now a Verse failure
  rather than a returned `false`. `HostScript`'s `FOpResult::Fail` arm sets a status and breaks
  without aborting the surrounding AutoRTFM transaction, which is why there is nothing to pay.

  **A build costs one analysis, not two.** The first `vh_compile_project` was 3.70 s and a
  generation 1.54 s when each ended with a whole analysis-only pass over the same sources, run only
  to rebuild a program equal to the one code generation had just discarded. The snapshot is taken
  from inside the build now — uLang's `IPostSemAnalysisInjection`, after the last semantic pass and
  before IR generation — and the pass is gone.

  **And the mirror's digest is parsed once per process, not once per build.** It is 2.1 MB of the
  2.2 MB the parse phase reads, and the same bytes at every build and every analysis for the life
  of the process. `SToolchainOverrides::Parser` substitutes the pass, so that parse is kept and
  each build is handed a clone of it — **36 ms** to clone. The parse phase goes from **202 ms to
  77 ms**, an analysis from 787 ms to 555 and a generation from 895 ms to 636, all against a mirror
  15 classes larger than the one the earlier figures were taken against.

  **And a build after an analysis costs neither of them.** The two phases a build spends its time
  in are the two an analysis has already run, so a build whose project nothing has edited since the
  last analysis generates code straight from the program that analysis left: **68 ms**. That is the
  editor's ordinary rhythm — stop typing, the analysis lands, press Play — and it is what the
  build-on-Play trigger actually costs most of the time.

  Together: a generation was **1.54 s**; it is **694 ms** when an author presses Play mid-edit and
  **68 ms** when they do not, and the first build of a session was **3.70 s** and is **2.36 s**.

  **The per-keystroke editor lag is the analysis figure, 520 ms**, and it is that rather than the
  1.4–1.8 s it was because every analysis after the project's first successful build reads
  `/Godot.org/Godot` as an External package from its digest rather than from 4.4 MB of source. A
  project that has never compiled keeps the mirror as source and pays the larger figure, which is
  correct: the side table that makes a digest lossless is recorded at the first build.

  **Nothing on the editor's thread waits for that 520 ms.** Every read keyed by a class name answers
  from the snapshot the last analysis left, at 0.0 ms, including one taken while an analysis is in
  flight; the three entry points that resolve a *position* refuse with `VH_ERR_STATE` in no time
  rather than blocking. `verse/analysis_wait_ms` is the custom monitor that says so — it is the
  stall the other three could not show, because a frame that spent 1.7 s inside a `join` reported a
  pump that did nothing in no time at all. It reads zero.

  The retained-per-generation figure is a **median** deliberately: the mean is commit-charge noise
  around a build, and the "10 MB mean" an earlier pass recorded reads −10 MB now while the median
  has never moved from ~1 MB.

  What is left in both figures is **semantic analysis**: ~445 ms of re-deriving the mirror's
  33,043 definitions from a digest that has not changed, because `CProgramBuildManager::Build` calls
  `ResetSemanticProgram()` before every compile *and* every analysis. Reusing an analysis dodges it
  for a build; nothing dodges it for the analysis itself. What would is off-thread building, or a
  compiler that can carry a semantic program across builds.

  **An exported game does not pay any of the figures above**, which is what Phase 7 and 7b are for.
  Measured on the same machine, `dodge-the-creeps` headless, from process start to the first Verse
  `_Ready` having run:

  | what | figure |
  | --- | --- |
  | exported, cooked Verse loaded from its container | **0.54 s** |
  | the same game in the editor, compiled at startup | **4.08 s** |

  That pair predates the removal of the build's trailing analysis; the editor side carries one whole
  analysis less than it did, and has not been re-measured.

  **7.6x**, and the difference is the whole-project compile the export no longer does: the cooker did
  it once, at export time, inside a 14.2 s headless export whose container step is 0.40 s. What the
  game loads instead is a 4.9 MB container and a 248 KB sidecar (R-DIST-11).
- **R-PERF-3 (SHOULD)** Nothing in the design makes a future optimisation structurally impossible —
  specifically, marshalling and dispatch must not bake in per-call allocation (R-TYPE-6).

---

## 14. Open questions and risks

Each blocks one or more requirements above. A question is closed by a written answer in this
document, not by an implementation that assumes one.

**OQ-2, OQ-5 and OQ-8 are closed**, by the Phase 0 spikes. Their answers are §14.1 below; the
measurements and the code they were read out of are in [`phase-0-spikes.md`](phase-0-spikes.md).
A closed question keeps its row so that the reason it is closed is not lost.

| id | question | blocks | next step |
| --- | --- | --- | --- |
| **OQ-1** | When, if ever, is the Verse compiler toolchain licensed such that binaries built from it may be redistributed? No ETA is known. | R-DIST-6, R-DIST-7, R-QUAL-4 | Track Epic's announcements. Design so the answer changes packaging only, never architecture. |
| **OQ-2** ✅ | Does an exported game ship the compiler and `.verse` sources, or precompiled Verse and a runtime-only host? | R-DIST-8, R-DIST-11, R-PLAT-2, R-PLAT-3 | **Closed: precompiled Verse and a runtime-only host.** See §14.1. |
| **OQ-3** | Is a monolithic UE Program target viable on Android and iOS — binary size, and whether VerseVM requires JIT that iOS forbids? | R-PLAT-2 | Attempt a UBT Program build for Android first; it is the permissive platform and answers the size question. Narrowed by OQ-2: the question is only about the **runtime** host, which carries no compiler. |
| **OQ-4** | Is Verse on wasm reachable at all? UBT has no wasm Program target; Godot's web export is constrained wasm. | R-PLAT-3 | Narrowed by OQ-2 — a web target would need only the runtime host, not Solaris — but still blocked on UBT having no wasm Program target at all. **Phase 7.5's question** (`roadmap.md`), by decision in Phase 7's planning: Phase 7 builds the runtime host, which is the only binary a web build would ever need, and answers nothing else here. |
| **OQ-5** ✅ | How does a project escape the single flat `/user@localhost` scope, so it can have modules, subdirectories and shared library code? | R-LANG-6 | **Closed: submodules within the one user package, built from the project's directory tree.** See §14.1. |
| **OQ-6** | What is the correct interaction between Verse's task model and Godot's threading — `WorkerThreadPool`, threaded loading, calls into Verse off the main thread? **Half of it is already answered by the engine, against us:** VerseVM's top-level entry asserts `IsInGameThread()` (`VVMEnterVMInline.h`) with the comment "Verse bytecode and AutoRTFM transactions must run on the game thread", so the question is not *whether* Verse can run on a worker thread — it cannot — but what a call from one should *do*. A mutex is not an answer: the assertion is thread identity, not mutual exclusion. | R-ASYNC-7, and now R-ASYNC-8 | Its own scoping document, which must choose between three tiers: **refuse** (R-ASYNC-8, which Phase 4 builds, because it converts corruption into a message); **marshal and block**, whose deadlock is concrete — the game thread is routinely inside Verse calling out into Godot, and anything on that path that waits on the worker hangs both; or **marshal and defer**, which cannot return a value and is Godot's own `call_deferred` bargain. Until it exists, §7 must not adopt a design that assumes single-threaded forever. **Phase 5 forecloses nothing and adds one fact worth carrying in**: resumption is event-driven rather than scheduled — a task resumes synchronously inside the call that signals it, measured — so there is no scheduler whose thread affinity would have to be redesigned, only the pump, which already runs where `_frame` does. |
| **OQ-7** | Build our own LSP over `verse_host_abi.h`, or get `uLangLSP` into a linkable target? | R-TOOL-10 | Low priority — Godot's editor is primary (§9). |
| **OQ-8** ✅ | Which hot-reload mechanism: fresh package name per generation, out-of-process compilation, or an engine change? | all of §10, and R-EXP-5 | **Closed: fresh package name per generation**, with `IncrementalizeProjectSource` before each build. See §14.1. |
| **OQ-9** | Can any DAP client speak `Verse::SocketDebugger`'s framing? | R-DIAG-6 | **Closed as not needed** (Phase 6). It was only worth answering if R-DIAG-4 turned out blocked, and it did not: a snippet-compiled procedure carries its file path into `VProcedure::FilePath` verbatim, which is the one fact the whole of R-DIAG-4 rested on. The port still opens on `verse/host/enable_debugger` and the framing question is still unanswered; nothing depends on the answer. |
| **OQ-10** ✅ | Can an editor-class UBT Program target be built — `bCompileAgainstEditor`, and therefore `bCompileAgainstEngine`? Cooking Verse needs `WITH_EDITOR=1` (§14.1), and nothing else this project builds does. | R-DIST-9, R-DIST-10, R-DIST-11 | **Closed: yes, as `verse_cook.exe`** — Phase 7's planning ran it (`phase-7-design.md` §2 S-1, thirteen builds). The "and therefore" was the wrong half: the flag alone is legal for a Program and drives only `WITH_EDITOR`, but Solaris's rules drag Engine in under `bBuildEditor` whatever the host lists, and with `WITH_ENGINE=0` UHT loses `UWorld` — so the cooker compiles against Engine on purpose, is an executable (a monolithic editor-class DLL exports 143,570 symbols against lld-link's 65,535), carries developer tools, and boots with the `EDITOR` token, `-nullrhi` and `-NoShaderCompile`. 768 MB, no PDB, boots in 2.4 s. The commandlet fallback is not needed. Stage 1 then settled the exit code by not needing one from teardown: the cook flushes and hard-exits with the status it chose, and `GEngineLoop.Exit()` is never called. `verse_cook.exe` cooks a project, writes the class sidecar and exits 0; `run_tests.py`'s abi layer drives it over `tests/host_smoke`'s fixtures and asserts what it wrote. 732 MB. |
| **OQ-11** ✅ | How do free functions and value-type methods cross, given that every mirrored call rides `VhCallValue(Handle, …)` and neither a `@GlobalScope` function nor a `vector2` has a handle? Named by Phase 2 §8 and never recorded here until Phase 4's spikes answered it. | R-SCN-3, and the 16 math types' methods | **Closed: Verse can carry the value types itself.** Type-based extension methods (`(V:vector2).Length<public>()<computes>:float`) and definable operators (`operator'+'(:vector2, :vector2)`) both compile against the mirror's own structs, so the math is ordinary Verse with no handle and no ABI — which is also what Godot's C# does. What genuinely has no handle is Godot's 114 statics and the ~28 utility functions with no `/Verse.org` counterpart, and those get one by-name dispatch callback apiece. See `docs/phase-4-design.md` §1.3 and §7. |
| **OQ-12** ✅ | Does a generation change the package *name* only, or the *verse path* too? S-2 varied the name; whether `/user@localhost` held across generations was not recorded. Module paths are user-visible text that R-TOOL-12 writes into the author's file, and `ScriptVersePath` is compiled into eight lookup sites in `HostScript.cpp`. | R-LANG-6, R-TOOL-12, and the shape of Phase 3 | **Closed: the name only.** The verse path is pinned at `/user@localhost` across generations and nothing in `HostScript.cpp` learns which generation it is asking about. See §14.1. |
| **OQ-13** | What bounds a script that raises every frame? A raise now stops script code for the rest of the frame and the next tick resumes it, so a `Process` that raises raises again next frame, forever — the error is reported each time, which is what Godot does for GDScript, and no progress is ever made. Options: report it once and stop calling that method, disable the instance, disable the script, or leave it and rely on the author reading the log. | R-DIAG-3 | Phase 6, with the rest of R-DIAG-3. Opened by Phase 3's fix: before it, the first raise silenced everything and the question could not arise, which is not the same as it having an answer. Whatever is chosen has to be per instance rather than per process, so it wants R-ASYNC-4 first. **Phase 5 changes its shape twice.** A raise stops only the raising call and the instance gets a fresh scope at its next call, so "stops script code for the rest of the frame" stops being true and the every-frame raise costs that instance's suspended work each time rather than the project's. And Phase 5 adds a **second** runaway of the same shape, deliberately: `spawn` is the taught way to start a task, so a `spawn` in a `_Process` makes sixty tasks a second on one instance and nothing bounds them. Whatever answers this has to answer both, and per-instance scopes are what make either countable. **Closed (Phase 6): nothing is bounded, and the defect it was pointing at was somewhere else.** Three reasons for the decision — Godot does not bound GDScript either; Phase 5's per-instance scopes already confine the cost to the raising node; and disabling an instance is a policy an author cannot see coming and cannot undo without a reload. What actually needed fixing was the bridge's own unthrottled stack printing, which ate the shared character budget and silenced every other script's output — R-DIAG-3 rule 4. The second runaway Phase 5 opened gets a number rather than a limit: `verse/instance_tasks`, a custom monitor carrying the largest live task count any one instance's scope holds, so a `spawn` in `_Process` is visible in the profiler where the author is already looking. |
| **OQ-14** ✅ (measured, open) | Does per-keystroke analysis stay usable once the mirror carries the 1413 virtuals, the 489 signal accessors and the per-class constant modules Phase 4 adds? It was 1190 ms before, from 158 ms curated. **Measured through Phase 4 stage 6: 1273 ms median** (min 1265, max 1364, n=10) with 1283 virtuals, 489 signal accessors, 352 constants and 114 statics emitted, a 4364 KB mirror and a 1395 ms generation. Against 1190 ms before the phase, the whole of Phase 4's mirror growth cost about **83 ms** — far less than the question feared, and no threshold is attached by decision. **It is 555 ms now**, and the answer to "does it stay usable" turned out to have two halves the question did not separate. The *latency* halved because every analysis after the first successful build reads the mirror as an External package **from its digest** rather than from 4.4 MB of source — a digest drops each definition's file and line and `_bIsAccessorOfSomeClassVar`, both of which a side table recorded at that first build restores, so hover, goto-definition and override completion are unaffected. The *stall* was never the analysis: ~22 entry points began with a `join`, so the editor's own thread paid 1.7 s for a read that costs 0.1 ms once one has landed, and no editor-thread call waits now. The parse of the mirror's own digest — 97% of the parse input — **is gone too**, and neither engine-side change it seemed to need was needed: uLang's own reuse mechanism is unreachable (`CSourceDataSnippet` implements neither validity virtual, and `bCloneValidSnippetVsts` is set on a build context `CProgramBuildManager::Build` constructs), but substituting the *pass* is a supported seam — `SToolchainOverrides::Parser` on a manager handed to `ISolarisIde::SetBuildManager`. The parse is kept and cloned — 36 ms, and the parse phase falls from 202 ms to 77 — and `BuildAll` still runs every build. What remains is **semantic analysis**, ~475 ms of re-deriving 33,043 mirror definitions because `Build` resets the semantic program before every compile and every analysis; that one has no seam. R-PERF-2 has the table. | R-TOOL-2, and the urgency of Phase 7's cooked route | Record it at Phase 4 stage 5 with `tools/build_bench.py`, **with no threshold attached** — feature parity first, performance goals later, by decision. It changes no design: the decision to mirror everything is made (`phase-2-design.md` §3), and the fix if the number turns out to matter is the cooked digest OQ-10 already owns. |
| **OQ-15** ✅ | What should the bridge say about Verse's effect semantics? A function with no effect specifier carries a default set wider than `<transacts>` — it includes `no_rollback` — so an explicit specifier *narrows*, and a **failure context** (an `if (X := F[])`, an option unwrap, a cast) refuses a `no_rollback` callee because failure has to unwind. One failable helper therefore pulls `<transacts>` onto everything it calls, which is what `dodge-the-creeps.md` wall 8 hit. (Wall 8 first recorded the cause as the host's AutoRTFM transaction; that was wrong, and Phase 4's probes corrected it.) | R-AUD-1, R-AUD-3, and the manual | **Closed by Phase 4.5: it says three things, and R-AUD-1 and R-AUD-3 carry them.** (1) Godot's **const and answering** methods are `<reads>`, so a read-only helper stops infecting its callers — 6728 in Godot plus 127 Godot forgot to mark, 3996 in the mirror. The test is const *and* answering: Godot's `const` means "does not mutate the C++ object", and the 38 const-and-void methods are `OS.set_environment` and 37 others that plainly do something. (2) A failure undoes every deferred Godot write at any depth, which is measured rather than assumed; what it does not undo is a method that mutates *and* answers, and those are enumerated in the generated `docs/nonatomic-methods.md` — **1073**, not the 1354 this row once estimated, which counted statics, methods the mirror does not emit, and 54 whose Godot source proves they do not mutate. (3) The trap was answered with an appended diagnostic and a template that warned about it, and **both were removed after the by-hand session**: the appended sentence never checked *which* effect had been refused, so a `suspends` refusal took the `transacts` branch and gave advice that was the opposite of correct (`by-hand-findings.md` B7). The compiler's own text stands, and what the trap still costs is recorded in `dodge-the-creeps.md` wall 8 rather than papered over. The property surface needed nothing: a `<reads>` getter is refused by the accessor protocol (S-1), and a property *read* from `<reads>` code is accepted anyway, because the read site is not checked against the getter's effect. See `phase-4.5-design.md` §11. |
| **OQ-16** | What anchors a Verse callback that is not a bound method? Godot answers this twice: a `self`-capturing lambda reports the captured object and dies with it, while a plain lambda is anchored to the script resource, overrides `is_valid` to ignore ObjectDB, and is Godot's own documented leak (the `GDScriptLambdaCallables` TODO, GH-102327). | R-SIG-3, R-INT-4, and library-level handlers | Phase 4a accepts only a bound method — the half of Godot's design that does not leak — and refuses an unbound function with a diagnostic. Answering means choosing an owner: a runtime-owned anchor with an explicit `Cancel`, or an explicit-owner spelling (`SubscribeAs(Owner, F)`) that keeps lifetime visible. **Phase 5 closes it for the case it creates and leaves the rest**: an awaiting continuation is owned by its task, which is owned by its instance's scope, so freeing the node cancels the task and drops the connection with no new spelling — one mechanism serving this and R-ASYNC-5 together. An unbound callback *outside* a task stays refused, exactly as Phase 4a decided, so the original question is narrowed rather than answered. |
| **OQ-17** | Does any of the C# interop work? R-SIG-6, R-INT-1, R-INT-2 and R-INT-5 name C# as a MUST, and **no test in this repository has ever run C#** — every fixture is GDScript, and exercising C# needs a .NET Godot build that `tools/run_tests.py` does not have. | R-SIG-6, R-INT-1, R-INT-2, R-INT-5 | Get a .NET Godot into the harness and run the existing interop cases from C# before 1.0. Until then those four statuses describe GDScript only, and say so. Phase 4 enlarges the claim rather than testing it, which is why this is recorded now. |
| **OQ-18** ✅ | Can a Verse package be loaded from an IoStore container by a host that is not a cooked game? `FLinkerLoad` has no `Verse::VCell` support at all, so a loose cooked `.uasset` is unreadable (`phase-7-design.md` §13.7); the zen loader's `FExportArchive` is the only thing in the engine that reads a cell (`AsyncLoading2.cpp:3184`). | R-DIST-9, R-DIST-10, R-DIST-11 | **Closed: yes, to both halves.** `verse_cook.exe` converts its loose cook with `CreateIoStoreContainerFiles` — which parses `FCommandLine::Get()` rather than the line it is handed, and needs a script-objects buffer, a commands list and a compact-binary oplog manifest that a cook of this shape does not otherwise produce — and `verse_host_runtime.dll` mounts the result the way `FPakPlatformFile` mounts a pak's. The mount needed one line nothing in this host was calling: `USE_IO_DISPATCHER` is false for a Program with no Engine, so `FIoDispatcher::InitializePostSettings()` never ran, `IsInitialized()` answered true anyway and every read was issued and never completed. **What the question did not ask, and what actually cost the phase, is whether a package loaded that way can be *called***: a cooked `VNativeProcedure`'s thunk is a C++ function pointer and does not serialise, and the engine rebinds the module-scoped ones only from the assembler, which a compiler-less host never runs. `phase-7b-design.md` §13.8 and §13.9 are the whole of it; the cooked payload is 5.0 MB and an exported game reaches its first Verse `_Ready` in 0.54 s against 4.08 s compiled at startup. |
| **OQ-19** | What does the host's UObject pool get raised to, and who decides? Every Verse class is a `UVerseClass` with a `UFunction` per method, and the mirror's own 1036 are already in a pool a Program pre-sizes to 131,072. Generated bindings put the roster on top of that, and it runs out at a few hundred binding classes of ordinary shape — at the diagnosed end *"ErrRuntime_MemoryLimitExceeded ... while attempting to construct a Verse object of type event!"*, at the fatal end UE's own message naming `MaxObjectsInProgram` and nothing about Verse or this bridge. Raising it works and costs one flag: `-ini:Engine:[/Script/Engine.GarbageCollectionSettings]:gc.MaxObjectsInProgram=...` on the `GEngineLoop.PreInit` line took 1000 binding classes from fatal to a clean build (`generated-bindings.md` §10.5). | R-INT-7, R-INT-8 | Pick the number with the bindings phase, and pick it knowingly: the pool is pre-sized, so it is memory spent whether or not a project has an addon. Until then the ceiling is a wall a large addon hits with no message an author can act on. |
| **RISK-1** | UE's licensing applies to games shipped with the host, including royalties. This is a permanent property of the current distribution model and may deter adoption regardless of anything built here. | adoption | Disclose prominently (R-DIST-3). No mitigation available. |
| **RISK-2** | Tracking Godot `master` and UE `main` simultaneously means two moving dependencies with no compatibility window. | R-QUAL-7 | Accepted deliberately while pre-1.0; revisit at the first release. |

### 14.1 Answers from the Phase 0 spikes

Evidence, measurements and code citations: [`phase-0-spikes.md`](phase-0-spikes.md).

**OQ-12 — a generation changes the package name only; the verse path is pinned.**

Answered by the first stage of Phase 3 rather than by a Phase 0 spike, and it is the thing the rest
of that phase rested on. `GodotScripts_N` publishes at `/user@localhost` for every N, and:

- publishing generation N+1 over the same verse path does **not** assert on a duplicate definition,
  provided generation N's package is removed from the *source project* first. It is only the source
  that has to go; the published package stays live in the VM, which is what lets an old instance
  keep running and is also the leak R-ITER-6 tracks;
- `LookupDefinition` finds the new generation's class at the unchanged path, so `vh_has_class`,
  `vh_instantiate` and the five other lookups that concatenate `ScriptVersePath` are untouched;
- an instance made against generation N goes on answering with generation N's code after N+1 is
  published — no adoption, which is R-ITER-4's meaning and costs no state-transfer path;
- `FSolarisModule::IncrementalizeProjectSource` before each `BuildAll` is what keeps the build from
  republishing the already-loaded native VNI packages, which is where S-2's first attempt died.

Confirmed in the same run: a file in a submodule reaches a definition declared in the **root**
module with no `using` written. Root is implicit by ordinary lexical scoping, so Phase 3 §3's
"root is implicit" costs nothing to build.

The negative branch — a stable public path decoupled from the package name, or a per-generation
alias module — is not needed and was not built. `tests/host_smoke` carries the whole answer as
running checks, with the rest of that suite deliberately running against the *second* generation so
that a generation which only half works cannot pass.

**One defect found while answering it, which is not Phase 3's and is recorded so it is not lost.**
After a Verse runtime error is raised out of a script, **no Verse code runs in the process again**.
Every later `vh_instance_call` returns `VH_OK` having done nothing: a method declared `:int` answers
as if it were `:void`, and a method with side effects has none.

Reproduced with `exports.TouchTarget` (which raises deliberately, for R-DIAG-2) followed by
`exports.AddInts` — which answers nothing rather than 42 — and by `exports.Bump`, whose `Scale`
does not move. It predates this work, and was invisible because no test called anything after a
raise.

**Cause:** the raise terminates the host's one `FContentScope`, and
`FRunningContext::EnterVM_Internal` returns *before invoking its functor* when the active scope is
terminated (`VVMEnterVMInline.h`: "The active content scope was terminated."). Nothing downstream
can tell that apart from a call that ran and returned nothing, which is why the host reports
success. The scope is created once in `GodotVerse::EnterContentScope` and lives for the process, so
once terminated it stays that way.

**Fixed**, against R-DIAG-3, which has the rules it now follows. Every entry into the VM goes
through one wrapper that revives the scope, so a seventh entry point cannot be added that forgets;
execution resumes at the next `vh_tick` rather than at the next call; and `VH_ERR_HALTED` (ABI 3.1)
is what every other call gets in between, because "did not run" reading as "ran and found nothing"
is what kept this invisible. Eight checks in `host_smoke` pin it, including the one nobody made when
it was first written up: that a *void* method has its effect again, not merely that a value comes
back.

Two things this did not fix and two it exposed. The cancellation is still project-wide
(**R-ASYNC-4**), and a `@tool` script's error still reaches the scene being edited. It opened
**OQ-13** — a script that raises every frame now raises every frame forever, where before the first
raise silenced everything, which looked like a bound and was a failure. And the first minor ABI bump
found `vh_init` comparing the whole version where the header's own policy says majors must match and
minors need not — corrected with it.

**OQ-2 — an exported game ships precompiled Verse and a runtime-only host.**

The engine already carries both halves. A target built without editor-only data gets
`WITH_VERSE_COMPILER=0` and a `Solaris` module with no compiler in it; such a target obtains its
Verse by `LoadPackage` on cooked `.uasset` files and `FCompiledPackageRegistry::AddCompiledUPackage`.
`SavePackage2.cpp` carries a VerseVM cell import/export table for the write side.

The producing side is a **cook, not a save**, and the cook is expensive.
`FPackageHarvester::TryHarvestCellExport` asserts `SaveContext.IsCooking()`, so cells are only
written when `FSavePackageArgs` carries an `ITargetPlatform`. Supplying one is cheap and was done —
`TargetPlatform` and `WindowsTargetPlatform` link into the monolithic host and the manager finds
Windows. `FSaveContext`'s constructor then refuses anyway:

```cpp
check(!IsCooking() || WITH_EDITOR);
checkf(!IsCooking() || PackageWriter, TEXT("Cook saves require an IPackageWriter"));
```

**Cooking requires an editor-class binary.** So `host/` becomes three targets over one set of
sources: today's **editor host** (compiler, runs in Godot), a **cooker** (editor-class, runs only
at export, never ships), and a **runtime host** (no compiler, loads `.uasset`, ships with the game).
Writing the cells outside SavePackage is not an escape: `FArchive::operator<<(Verse::VCell*&)` is a
no-op on the base class, so cell identity comes from `FLinkerSave`'s import/export tables.

Consequences: R-DIST-8 and R-DIST-11 resolve to "precompiled, no compiler shipped". R-PLAT-2 and
R-PLAT-3 now ask about the runtime host only, which is a smaller binary with a smaller dependency
set — OQ-3 and OQ-4 are narrowed but not closed. The artifact a player receives contains no Verse
compiler, which is a materially different object from the one §2 assumed; whether that changes
anything is for OQ-1 to say. And the cost of the export pipeline sits in building the cooker, not
in loading what it produces — **OQ-10** is what remains of it.

**OQ-10 — an editor-class Program target builds, links and boots** (answered during Phase 7's
planning rather than by a Phase 0 spike; recorded here because it is the other half of OQ-2's
answer). `host/VerseHostCooker.Target.cs` derives from the editor host's target and adds
`bCompileAgainstEditor`, `bCompileAgainstEngine`, `bBuildDeveloperTools`, an executable link,
`ALLOW_OTHER_PLATFORM_CONFIG=1`, a 16384-byte PDB page size and no debug info;
`host/VerseHost.Build.cs` lists the modules an editor `Launch` links, because
`RequiredProgramMainCPPInclude.h` compiles `LaunchEngineLoop.cpp` into the host's own module;
`host/Private/CookMain.cpp` boots with `EDITOR -nullrhi -NoShaderCompile`. The one finding that
was not in any precedent: a Program that is editor-class but *not* engine-class fails in UHT, not
at link — `Solaris.Build.cs` pulls `TargetPlatform` in under `bBuildEditor`, `TargetPlatform`
loads `TurnkeySupport` dynamically, and that reaches `UnrealEd` and `Engine`, whose headers UHT
then parses with `WITH_ENGINE=0` and cannot resolve `UWorld` for. Its entry point is
`AUTORTFM_DISABLE`, the way `AutoRTFMTests.cpp` writes its own `main`, because this target is
built by the AutoRTFM clang and instrumented code may not call `GEngineLoop.Exit()`. **The
teardown still segfaults**, past where a cook would have written, and stage 1 owns it.
`phase-7-design.md` §2 S-1 has all seventeen builds and what was already ruled out.

**OQ-8 — fresh package name per generation.**

Each code-generating build publishes under a name never used before, and
`FSolarisModule::IncrementalizeProjectSource` runs first so every package already compiled in this
process is marked external and skipped. Both are required: without the second, the build
republishes the *native* packages and aborts there, which is where the first attempt died.

Measured over 25 generations in one process: every generation compiled and ran the edited code,
161–220 ms each in steady state with no upward trend, and roughly 0.5 MB per generation retained
for a one-class project. Instances created by an earlier generation keep working against their own
generation's class, which is the behaviour R-ITER-4 wants.

This closes the obstacle §10 describes. It also changes what §10 costs: full hot reload is a
feature to build, not a constraint to escape. The one price not previously counted is that the
host must own its script package rather than borrow the IDE's — whose name is fixed — which costs
a small `ISourceSnippet` implementation to keep analysis and completion working.

*Out-of-process compilation* lost for hot reload but remains the shape of the export pipeline under
OQ-2. *An engine change* lost because none is needed.

**OQ-5 — submodules inside the one user package.** *Prototyped and observed, not only read.*

A `CSourcePackage` carries a module tree (`_RootModule`, `_Submodules`), the toolchain emits a
`Vst::Module` per submodule, and the desugarer turns each into a real Verse module with its own
scope. The flat scope is not a property of packages; it is that the host adds every snippet to the
root module and never creates a submodule.

So R-LANG-6 is met by mapping `res://` subdirectories onto submodules — `res://gameplay/player.verse`
becomes `/user@localhost/gameplay/player` — and the one-top-level-name-per-file rule narrows from
project-wide to directory-wide. Publishing additional *packages* is possible (the host already does
it for its attribute package) but is not what modules need.

The prototype compiled `root.verse`, `gameplay/player.verse` and `ui/player.verse` together —
two files with the same top-level name — and `root` read a member off each. A directory-derived
module needs no declaration of its own; only the definitions need `<public>`. One thing the run
added: a module path is relative to the project root, and the ABI carries only absolute paths, so
`res://` has to cross it (R-DIST-8's neighbour, and Phase 2's first task here).

Two follow-ons, both design rather than unknown: whether godot-verse writes the cross-module `using`
or the author does, and that moving a file between directories changes its Verse path and every
reference to it.

---

## 15. Changes to this document

A requirement is added, changed or retired by editing this file in a commit that says why. A
requirement that turns out to be infeasible is **not deleted** — it is marked blocked with a
pointer to the open question that closed it, because the reason a thing is not being built is the
most expensive knowledge in the repository to reacquire.
