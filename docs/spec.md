# godot-verse — Specification

**Status:** Draft 2 · 2026-09-11 · targets no release yet
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
  than forwards — `godot_signal.Subscribe` — is compensated with an
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

**Exporting a game.**

- **R-DIST-9 (MUST)** Exporting a Godot project that uses Verse produces a runnable game through
  Godot's ordinary export dialog, with no manual copying of DLLs or engine directories. The
  export plugin collects everything the game needs. Status: **none** — nothing is exported today,
  and the `.gdextension` declares only `editor` and `debug` Windows libraries.
- **R-DIST-10 (MUST)** An exported game does not require the user who *runs* it to have anything
  installed. Status: **none**.
- **R-DIST-11 (SHOULD)** An exported game ships compiled Verse rather than `.verse` source plus a
  compiler. This is the difference between a game that ships a language toolchain in its data
  directory and one that ships a program; it also removes compilation from startup time and is a
  precondition for §3's mobile and web targets, where shipping a compiler is not viable.
  Status: **none**, but no longer uncertain — **OQ-2** is closed in favour of it (§14.1), so this
  reads as MUST in everything but its numbering.

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
  but the first two stand. Blocked on **OQ-4**.
- **R-PLAT-4 (MUST)** A platform that is not supported fails at export time with a clear message,
  not at game startup on a user's device.
- **R-PLAT-5 (MUST)** Nothing in the GDExtension assumes Windows. Status: **part** — the code is
  portable in shape, `verse_host.cpp` is a `GetProcAddress` loader, and the build has never been
  attempted elsewhere.

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
  `godot_signal` payload in both directions — out as one Godot argument per field, named by the
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
  the mirror uses `<decides>` where absence is real (`GetParent()` at the root, a missing
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
  `RefCounted`/`Object` rather than `Node`. Status: **none**.
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
  Status: **done** (Phase 4 stage 5). All **1413** of `extension_api.json`'s virtuals are generated
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

  The rest of that set — `_ToString`, `_Get`, `_Set`, `_GetPropertyList`, `_ValidateProperty` — is
  **R-NODE-10**, and sits in 4b where `_Get` and `_Set` can be designed beside the export machinery
  they overlap with.
- **R-NODE-9 (MUST)** A script method list (`_get_script_method_list`, `_get_method_info`,
  `_has_method`) reports what the script actually defines. Status: **done**. `vh_class_method_list`
  reads the class's own declarations out of the semantic program — names, parameters with their own
  names and types, result type, `<decides>`/`<suspends>`, and Godot's name for the virtual it
  overrides — and the script instance answers `has_method`, `get_method_list` and
  `get_method_argument_count` from it.
- **R-NODE-10 (SHOULD)** The script-level hooks Godot offers a script rather than registering in
  ClassDB are reachable: `_to_string`, `_get`, `_set`, `_get_property_list`, `_validate_property`.
  Status: **none**. Named by Phase 4 once `_notification` proved that none of this family is in
  `extension_api.json` and so none of it can be generated. `_to_string` is what makes `print(node)`
  in GDScript show something a Verse author chose; `_get`/`_set` overlap §5.4's export machinery,
  which is why the set is scheduled with it (roadmap, Phase 4b) rather than with R-NODE-7.

### 5.3 Signals

No script *declares* a signal today: `_has_script_signal` returns false and
`_get_script_signal_list` returns empty. Signals are how Godot programs are wired together, so this
is parity-critical. What already works is receiving one — a scene-file connection reaches a Verse
method with its arguments (R-SIG-4), which is what let the Dodge the Creeps port be wired at all.

- **R-SIG-1 (MUST)** A Verse script declares signals with argument types, and they appear in the
  editor's Node panel where a designer connects them. Status: **done** for the declaration and the
  list (Phase 4 stage 4); the Node panel itself is R-SIG-4's by-hand check.

  **There is no `@signal` attribute.** The member's *type* is the declaration and its *name* is the
  signal's name, so there is no second place to spell either and nothing to drift:

  ```
  player := class(area2d):
      Hit<public>:godot_signal(tuple()) = godot_signal(tuple()){}
      Struck<public>:godot_signal(tuple(int, string)) = godot_signal(tuple(int, string)){}
  ```

  A bridge attribute exists where the *text* is the only source — `@global_class` survives without
  compilation because Godot asks about files it has only scanned — and a signal list is not one of
  those cases: the host reads declared types out of the semantic program already, so
  `vh_class_signal_list` refreshes per keystroke the way the method and export lists do. A signal
  declared in a script that has never been built appears after the next Build, which is the same
  bargain an `@export` *default* already makes.

  The shape is Verse's own `listenable` **without implementing it**, and the difference is forced
  rather than preferred: `signalable.Signal` is `no_rollback`, so it would be uncallable from
  inside the transaction every Godot callback runs in, and `subscribable.Subscribe` fixes its
  callback at a no_rollback domain that could not touch Godot. `cancelable` is the one of the four
  whose domain fits, and `Subscribe` answers it. `awaitable` is Phase 5's.

  **The payload is one type, and tuples carry arity above one.** A multi-parameter Verse function
  satisfies a one-tuple-parameter callback, because a function's parameter *is* its tuple, so
  `OnStruck(Damage:int, By:string)` subscribes to a `godot_signal(tuple(int, string))` and Godot
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
  emit, and `_validate` turns the reason into a warning at the member's own line. The five reasons,
  all decidable from the declaration:

  | `vh_signal_reject` | the declaration |
  | --- | --- |
  | `IS_VAR` | a `var` member — a signal is an identity, and the binding is minted once |
  | `NOT_PUBLIC` | not `<public>`, so nothing outside the class can connect to it |
  | `NO_GODOT_OWNER` | a class that does not derive from `object`, so nothing ever hands it a handle |
  | `PAYLOAD_UNSUPPORTED` | an argument with no Godot type |
  | `PAYLOAD_NESTED_STRUCT` | a struct payload whose field is itself a struct |

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
  stage 4). `Hit.Signal(())`, `Struck.Signal((9, "spike"))`.

  **Emission is immediate, and that is a stated exception.** Every other void mutation in the mirror
  defers to `AutoRTFM::OnCommit`, which is what makes `<transacts>` literally true for 6813 methods.
  Emission does not: handlers run synchronously, as they do in GDScript, so "emit, then read what the
  handler changed" behaves the way a Godot author expects and a Verse emission is indistinguishable
  from a GDScript one. The cost is stated rather than hidden — if the emitting transaction later
  aborts, the handlers have already run. It joins the container write as the second member of the
  set **Phase 4.5** audits.

  Underneath, a `godot_signal(t)` member is bound at construction: the host walks the class's data
  members for the ones whose declared type reaches the native `vh_signal`, mints an id per member
  per instance, and writes it into the member's own object exactly where `Handle` is written. The
  payload's decomposition is read off the *instantiation* — the declared type comes back as the
  generic `godot_signal(t)`, and the type argument is on it as a substitution table.
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
  name answers "Nonexistent function", measured. The editor's own C++ is its only caller. That check
  and the Node panel connection are on the by-hand checklist, with the exact text to expect.
- **R-SIG-5 (MUST)** A script `await`s a signal from a concurrent context: the Verse spelling of
  GDScript's `await button.pressed`. Status: **done** (Phase 5). `godot_signal(t)` holds a
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

  **How an emission reaches the event** is the host's half and is smaller than the design budgeted
  for: `verse::event` is a UObject with a public C++ `Signal`, so the host reads the event off the
  signal object and signals it directly, and Epic's own code does the FIFO resumption and the
  per-task content scope. The payload is rebuilt from the emission's Godot arguments against the
  same shape the signal descriptor was generated from, so a tuple payload comes back a tuple and a
  struct payload comes back a struct.

  **One line of R-SIG-1's surface changed with it**, and it is a correction rather than an addition:
  `godot_signal.Subscribe`'s callback is now specifier-less, matching Verse's own
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
  groups, subgroups and categories. Status: **part** and well advanced — `@export_group`,
  refusal of types the inspector cannot draw, and range constraints derived from the Verse type's
  own `where` clause so the slider and the type cannot disagree (README).

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
  editor — including a changed *default value*, which today requires code generation and therefore
  a restart. Depends on §10.
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
  nothing about the editor acting on it — that check needs a window and is on the by-hand checklist.
  A tool script runs the **last built** generation, per §10's trigger — the same bargain a C# `[Tool]`
  script makes today, and the workflow most likely to send an author looking for the Build action.
  **Stated risk:** Verse now runs against the scene the author is editing, and R-DIAG-3 does not
  land until Phase 6 — the defect recorded there, where a raised runtime error empties every later
  method result, is one a tool script can now reach without running the game.
- **R-EXP-6 (MUST)** A Verse class can be a custom `Resource`, saved to and loaded from `.tres`,
  with its exported properties serialised. Status: **none**.
- **R-EXP-7 (MUST)** A Verse script can be registered as an autoload singleton. Status: **none**.
- **R-EXP-8 (SHOULD)** A script declares an editor icon. Status: **part**
  (`_get_class_icon_path` exists).
- **R-EXP-9 (SHOULD)** `_get_rpc_config` reports RPC annotations so a Verse script participates in
  Godot's high-level multiplayer. Status: **none** (returns an empty `Variant`).

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
  converter cannot be an inline lambda: it names the element type's own `As<GodotType>` reader where
  one exists, and a generated per-class function where the element is a class — because
  `VhFromObject` takes the base `object` and a Verse function type is not satisfied by one that
  merely accepts a supertype.

  **One half is still missing, and the port found it:** a script can read and write the containers
  Godot hands it and cannot **make** one. `godot_array{}` compiles and holds reference 0, which
  crosses as `Nil`; `VhRefNew` is module-scoped and there is no public constructor. So a container
  is something a script passes on rather than something it can originate, which is what stops
  R-INT-2 short — see `docs/dodge-the-creeps.md` wall 7.
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
  The normal route is unaffected — `As<GodotType>[V]`, `VariantKind(V)`, `VariantFrom<GodotType>` —
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
  **done** (Phase 5) — `godot_signal(t).Await()<suspends>:t`, which covers a script's own declared
  signals and all 489 mirrored engine-signal accessors alike because every one of them answers a
  `godot_signal(t)`. `GetTree[].ProcessFrame()`, `GetTree[].PhysicsFrame()` and
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
  way `godot_signal.Subscribe` does, which `Object.Connect` cannot (see `nonatomic-methods.md`).
- **R-INT-2 (MUST)** A Verse script calls methods on, and reads properties of, an object whose
  script is GDScript or C#, dynamically. Status: **done** (Phase 4 stage 2 closed the argument
  array). The dispatch itself arrived as a side effect rather than as work of its own: `Object.callv(StringName, Array) -> Variant` is an ordinary concrete method, and Godot's
  `Object` became mirrorable the moment Variant and Array both crossed (Phase 2 §4.4). A script
  writes `AsInt[Target.Callv("_double", Args)]` and the GDScript method runs. `get`, `set`,
  `has_method` and `get_class` came with it.

  **The argument array** was what was missing, which the Dodge the Creeps port found and
  `docs/dodge-the-creeps.md` records as its wall 7: a script could not make a `godot_array` of its
  own, so dispatch by name worked and *originating* such a call did not. `MakeArray()` and
  `MakeDictionary()` now mint one through `VhRefNew`, `Make<Element>Array()` and
  `Make<Key><Value>Dict()` do the same for the typed forms — which no script could ever have
  spelled, since their converters are module-scoped — and `Add<Element>` appends. This is
  R-TYPE-2's other half and landed with it.

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

---

## 9. Editor and tooling

**Godot's built-in script editor is the primary authoring surface** and gets full support. An
external editor is secondary.

- **R-TOOL-1 (MUST)** Syntax highlighting, including members, enums, strings, comments and
  interpolation. Status: **done**.
- **R-TOOL-2 (MUST)** Inline diagnostics as you type, from real semantic analysis rather than a
  local parse. Status: **done** — analysis re-runs per keystroke, off the main thread, and is not
  subject to the single-generation rule.
- **R-TOOL-3 (MUST)** Code completion: members, locals, types in scope, imported package contents,
  and keywords. Status: **part**.
- **R-TOOL-4 (MUST)** Hover and ctrl-click: type, signature, doc comment; go to definition, for
  both user code and the mirrored Godot API. Status: **part** — a known defect is that ctrl-hover
  inside a string interpolation underlines the whole string.
- **R-TOOL-5 (MUST)** Signature help while typing a call. Status: **part**.
- **R-TOOL-6 (SHOULD)** Find references and rename across the project. Status: **none**.
- **R-TOOL-7 (SHOULD)** The script editor's outline/member list is populated. Status: **none**.
- **R-TOOL-8 (SHOULD)** Doc comments on a script's classes and members reach Godot's own
  documentation panel, so a Verse class is documented the way an engine class is.
  Status: **part** (`_get_documentation`, `_supports_documentation` exist).
- **R-TOOL-9 (MUST)** Auto-indent, comment toggling and the new-script templates produce Verse that
  compiles and is tab-indented. Status: **part**.
- **R-TOOL-10 (SHOULD)** A language server exists and speaks LSP over the same host analysis, so
  VS Code and other editors get R-TOOL-2 through R-TOOL-5. Today this is not possible: `uLangLSP`
  in the UE checkout is a message-type library, not a Program target, and nothing links it into a
  binary. Writing our own server over `verse_host_abi.h` is the alternative. Related: **OQ-7**.
- **R-TOOL-11 (MAY)** A formatter.
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
  compiler's message. Status: **done**.
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
  **part**, and narrowed twice.
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

  What is **missing**, and why this is still *part*: an error in a `@tool` script runs against the
  scene the author is editing; and nothing bounds a script that raises every frame (**OQ-13**),
  which Phase 3's fix opened and Phase 5 reshaped — the every-frame raise now costs that instance's
  suspended work each time rather than the project's. §14.1 has the measurement.
- **R-DIAG-4 (MUST)** Godot's own debugger works on Verse: breakpoints in the script editor, step
  in/over/out, the call stack, local and member inspection, and expression evaluation at a
  breakpoint.
  *Current state is worse than absent:* `verse_script_language.h` declares every
  `_debug_*` virtual and `verse_script_language.cpp` returns zero, empty string or empty
  dictionary from each. godot-cpp binds a virtual with Godot when the subclass declares it, so
  Godot believes the language supports debugging and is told there are no stack frames. Per
  CLAUDE.md's own rule — "omitting one is how you say unsupported" — these declarations are
  currently a lie to the engine and must either be implemented or removed. Status: **none**.
- **R-DIAG-5 (MUST)** The same applies to profiling: `_profiling_start`, `_profiling_stop`,
  `_profiling_get_accumulated_data` and `_profiling_get_frame_data` are declared and empty. Verse
  functions must appear in Godot's profiler with their own timings, and per-frame Verse tick cost
  must be visible. Status: **none**.
- **R-DIAG-6 (SHOULD)** The Verse debugger the host already links (`Verse::SocketDebugger`, port
  1963) is usable from an external editor. Known: the port is real and the flag that opens it
  works. Unknown: whether any DAP client can speak its 4-byte-length-prefixed JSON framing, which
  is not the `Content-Length` transport standard DAP uses. Related: **OQ-9**.
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

**Deliberately deferred.** The posture is correctness first: no targets are committed in this
draft, and no design decision in §§4–12 may be justified by an unmeasured performance claim.

- **R-PERF-1 (MUST)** Before 1.0, this section is replaced by measured numbers and stated targets
  for, at minimum: per-frame overhead of an empty `Process` against an empty GDScript `_process`;
  property read and write cost; a method call with marshalled arguments; project compile time at
  10, 100 and 1000 scripts; and editor analysis latency per keystroke.
- **R-PERF-2 (MUST)** Benchmarks exist and run under R-QUAL-3 for visibility, before any target is
  set. What gets measured early is what can be reasoned about later. `tests/host_bench` is where
  they live — reported rather than asserted, because a threshold would fail on a slower machine.
  The numbers on the machine Phase 3 was written on, against `dodge-the-creeps` and the full
  1023-class mirror: `vh_init` **72 ms**; the **first** `vh_compile_project` **3.1 s**; a
  **generation after it 1.27 s**, because `IncrementalizeProjectSource` marks the native packages
  external and only the script package is rebuilt; `vh_check_project` **1.20 s**, which is Phase 2's
  enum cost unchanged. The generation figure is the one the build-on-Play trigger rests on: it is a
  second and a quarter an author pays on Play, and would have been a second and a quarter on every
  Ctrl+S. If it has to come down, off-thread building becomes a requirement rather than a guess.
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
| **OQ-4** | Is Verse on wasm reachable at all? UBT has no wasm Program target; Godot's web export is constrained wasm. | R-PLAT-3 | Narrowed by OQ-2 — a web target would need only the runtime host, not Solaris — but still blocked on UBT having no wasm Program target at all. |
| **OQ-5** ✅ | How does a project escape the single flat `/user@localhost` scope, so it can have modules, subdirectories and shared library code? | R-LANG-6 | **Closed: submodules within the one user package, built from the project's directory tree.** See §14.1. |
| **OQ-6** | What is the correct interaction between Verse's task model and Godot's threading — `WorkerThreadPool`, threaded loading, calls into Verse off the main thread? **Half of it is already answered by the engine, against us:** VerseVM's top-level entry asserts `IsInGameThread()` (`VVMEnterVMInline.h`) with the comment "Verse bytecode and AutoRTFM transactions must run on the game thread", so the question is not *whether* Verse can run on a worker thread — it cannot — but what a call from one should *do*. A mutex is not an answer: the assertion is thread identity, not mutual exclusion. | R-ASYNC-7, and now R-ASYNC-8 | Its own scoping document, which must choose between three tiers: **refuse** (R-ASYNC-8, which Phase 4 builds, because it converts corruption into a message); **marshal and block**, whose deadlock is concrete — the game thread is routinely inside Verse calling out into Godot, and anything on that path that waits on the worker hangs both; or **marshal and defer**, which cannot return a value and is Godot's own `call_deferred` bargain. Until it exists, §7 must not adopt a design that assumes single-threaded forever. **Phase 5 forecloses nothing and adds one fact worth carrying in**: resumption is event-driven rather than scheduled — a task resumes synchronously inside the call that signals it, measured — so there is no scheduler whose thread affinity would have to be redesigned, only the pump, which already runs where `_frame` does. |
| **OQ-7** | Build our own LSP over `verse_host_abi.h`, or get `uLangLSP` into a linkable target? | R-TOOL-10 | Low priority — Godot's editor is primary (§9). |
| **OQ-8** ✅ | Which hot-reload mechanism: fresh package name per generation, out-of-process compilation, or an engine change? | all of §10, and R-EXP-5 | **Closed: fresh package name per generation**, with `IncrementalizeProjectSource` before each build. See §14.1. |
| **OQ-9** | Can any DAP client speak `Verse::SocketDebugger`'s framing? | R-DIAG-6 | Only worth answering if R-DIAG-4 (Godot's own debugger) turns out to be blocked. |
| **OQ-10** | Can an editor-class UBT Program target be built — `bCompileAgainstEditor`, and therefore `bCompileAgainstEngine`? Cooking Verse needs `WITH_EDITOR=1` (§14.1), and nothing else this project builds does. | R-DIST-9, R-DIST-10, R-DIST-11 | Opened by the S-1 answer. Attempt it at the start of Phase 7. The one prior attempt failed on Engine module links, but it was made for a *lean* host, where the weight was the objection; a cooker that runs only at export has no such constraint. Fallback: cook through a real UE editor or commandlet process. |
| **OQ-11** ✅ | How do free functions and value-type methods cross, given that every mirrored call rides `VhCallValue(Handle, …)` and neither a `@GlobalScope` function nor a `vector2` has a handle? Named by Phase 2 §8 and never recorded here until Phase 4's spikes answered it. | R-SCN-3, and the 16 math types' methods | **Closed: Verse can carry the value types itself.** Type-based extension methods (`(V:vector2).Length<public>()<computes>:float`) and definable operators (`operator'+'(:vector2, :vector2)`) both compile against the mirror's own structs, so the math is ordinary Verse with no handle and no ABI — which is also what Godot's C# does. What genuinely has no handle is Godot's 114 statics and the ~28 utility functions with no `/Verse.org` counterpart, and those get one by-name dispatch callback apiece. See `docs/phase-4-design.md` §1.3 and §7. |
| **OQ-12** ✅ | Does a generation change the package *name* only, or the *verse path* too? S-2 varied the name; whether `/user@localhost` held across generations was not recorded. Module paths are user-visible text that R-TOOL-12 writes into the author's file, and `ScriptVersePath` is compiled into eight lookup sites in `HostScript.cpp`. | R-LANG-6, R-TOOL-12, and the shape of Phase 3 | **Closed: the name only.** The verse path is pinned at `/user@localhost` across generations and nothing in `HostScript.cpp` learns which generation it is asking about. See §14.1. |
| **OQ-13** | What bounds a script that raises every frame? A raise now stops script code for the rest of the frame and the next tick resumes it, so a `Process` that raises raises again next frame, forever — the error is reported each time, which is what Godot does for GDScript, and no progress is ever made. Options: report it once and stop calling that method, disable the instance, disable the script, or leave it and rely on the author reading the log. | R-DIAG-3 | Phase 6, with the rest of R-DIAG-3. Opened by Phase 3's fix: before it, the first raise silenced everything and the question could not arise, which is not the same as it having an answer. Whatever is chosen has to be per instance rather than per process, so it wants R-ASYNC-4 first. **Phase 5 changes its shape twice.** A raise stops only the raising call and the instance gets a fresh scope at its next call, so "stops script code for the rest of the frame" stops being true and the every-frame raise costs that instance's suspended work each time rather than the project's. And Phase 5 adds a **second** runaway of the same shape, deliberately: `spawn` is the taught way to start a task, so a `spawn` in a `_Process` makes sixty tasks a second on one instance and nothing bounds them. Whatever answers this has to answer both, and per-instance scopes are what make either countable. |
| **OQ-14** ✅ (measured, open) | Does per-keystroke analysis stay usable once the mirror carries the 1413 virtuals, the 489 signal accessors and the per-class constant modules Phase 4 adds? It was 1190 ms before, from 158 ms curated. **Measured through Phase 4 stage 6: 1273 ms median** (min 1265, max 1364, n=10) with 1283 virtuals, 489 signal accessors, 352 constants and 114 statics emitted, a 4364 KB mirror and a 1395 ms generation. Against 1190 ms before the phase, the whole of Phase 4's mirror growth cost about **83 ms** — far less than the question feared, and no threshold is attached by decision. | R-TOOL-2, and the urgency of Phase 7's cooked route | Record it at Phase 4 stage 5 with `tools/build_bench.py`, **with no threshold attached** — feature parity first, performance goals later, by decision. It changes no design: the decision to mirror everything is made (`phase-2-design.md` §3), and the fix if the number turns out to matter is the cooked digest OQ-10 already owns. |
| **OQ-15** ✅ | What should the bridge say about Verse's effect semantics? A function with no effect specifier carries a default set wider than `<transacts>` — it includes `no_rollback` — so an explicit specifier *narrows*, and a **failure context** (an `if (X := F[])`, an option unwrap, a cast) refuses a `no_rollback` callee because failure has to unwind. One failable helper therefore pulls `<transacts>` onto everything it calls, which is what `dodge-the-creeps.md` wall 8 hit. (Wall 8 first recorded the cause as the host's AutoRTFM transaction; that was wrong, and Phase 4's probes corrected it.) | R-AUD-1, R-AUD-3, and the manual | **Closed by Phase 4.5: it says three things, and R-AUD-1 and R-AUD-3 carry them.** (1) Godot's **const and answering** methods are `<reads>`, so a read-only helper stops infecting its callers — 6728 in Godot plus 127 Godot forgot to mark, 3996 in the mirror. The test is const *and* answering: Godot's `const` means "does not mutate the C++ object", and the 38 const-and-void methods are `OS.set_environment` and 37 others that plainly do something. (2) A failure undoes every deferred Godot write at any depth, which is measured rather than assumed; what it does not undo is a method that mutates *and* answers, and those are enumerated in the generated `docs/nonatomic-methods.md` — **1073**, not the 1354 this row once estimated, which counted statics, methods the mirror does not emit, and 54 whose Godot source proves they do not mutate. (3) The trap was answered with an appended diagnostic and a template that warned about it, and **both were removed after the by-hand session**: the appended sentence never checked *which* effect had been refused, so a `suspends` refusal took the `transacts` branch and gave advice that was the opposite of correct (`by-hand-findings.md` B7). The compiler's own text stands, and what the trap still costs is recorded in `dodge-the-creeps.md` wall 8 rather than papered over. The property surface needed nothing: a `<reads>` getter is refused by the accessor protocol (S-1), and a property *read* from `<reads>` code is accepted anyway, because the read site is not checked against the getter's effect. See `phase-4.5-design.md` §11. |
| **OQ-16** | What anchors a Verse callback that is not a bound method? Godot answers this twice: a `self`-capturing lambda reports the captured object and dies with it, while a plain lambda is anchored to the script resource, overrides `is_valid` to ignore ObjectDB, and is Godot's own documented leak (the `GDScriptLambdaCallables` TODO, GH-102327). | R-SIG-3, R-INT-4, and library-level handlers | Phase 4a accepts only a bound method — the half of Godot's design that does not leak — and refuses an unbound function with a diagnostic. Answering means choosing an owner: a runtime-owned anchor with an explicit `Cancel`, or an explicit-owner spelling (`SubscribeAs(Owner, F)`) that keeps lifetime visible. **Phase 5 closes it for the case it creates and leaves the rest**: an awaiting continuation is owned by its task, which is owned by its instance's scope, so freeing the node cancels the task and drops the connection with no new spelling — one mechanism serving this and R-ASYNC-5 together. An unbound callback *outside* a task stays refused, exactly as Phase 4a decided, so the original question is narrowed rather than answered. |
| **OQ-17** | Does any of the C# interop work? R-SIG-6, R-INT-1, R-INT-2 and R-INT-5 name C# as a MUST, and **no test in this repository has ever run C#** — every fixture is GDScript, and exercising C# needs a .NET Godot build that `tools/run_tests.py` does not have. | R-SIG-6, R-INT-1, R-INT-2, R-INT-5 | Get a .NET Godot into the harness and run the existing interop cases from C# before 1.0. Until then those four statuses describe GDScript only, and say so. Phase 4 enlarges the claim rather than testing it, which is why this is recorded now. |
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
