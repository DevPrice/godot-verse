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
- **R-AUD-3 (SHOULD)** Verse's less familiar features — `<decides>`, structured concurrency,
  parametric types — are available but never on the critical path of a first script. A user must
  be able to write a working script knowing only "class, member, method, `set`".

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
  (R-EXP-1). A **struct** member at the boundary is still outstanding: the sixteen mirrored math
  types cross, but a struct a project declares for itself has no Godot counterpart to become.
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
  *Current state:* **part.** The third clause is **done**: a `.verse` with no class of its own is a
  library file rather than a broken script — it compiles, every other file in the project resolves
  its module-level definitions with nothing written to import them, and it reports no instance base
  type, so Godot refuses to attach it and says why. That much needs no modules: in one flat scope
  same-module files already see each other, and it is most of what makes that scope livable while
  the rest waits. The first two clauses are **none**, and the current state is a hard rule in the
  opposite direction — the whole project shares one flat `/user@localhost` scope and Verse forbids
  shadowing, so a script's class is named after its file to keep collisions from happening (README,
  "Five constraints"). That remains the single largest structural gap between the prototype and a
  language a project can be written in. **OQ-5** is closed (§14.1): the escape is submodules built
  from the project's directory tree, inside the one user package, and it is Phase 3's.
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
  Status: **none** (`_has_static_method` exists; `_get_constants` returns nothing).
- **R-NODE-5 (SHOULD)** Abstract Verse classes report as abstract so Godot refuses to instantiate
  them. Status: **none** (`_is_abstract` returns false unconditionally).

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
  Status: **none** beyond the three.
- **R-NODE-8 (MUST)** `_notification` reaches a script, with the notification constant, so
  `NOTIFICATION_WM_CLOSE_REQUEST` and friends are handleable. Status: **none**
  (`notification_func` is wired to the vtable but does not reach Verse).
- **R-NODE-9 (MUST)** A script method list (`_get_script_method_list`, `_get_method_info`,
  `_has_method`) reports what the script actually defines. Status: **done**. `vh_class_method_list`
  reads the class's own declarations out of the semantic program — names, parameters with their own
  names and types, result type, `<decides>`/`<suspends>`, and Godot's name for the virtual it
  overrides — and the script instance answers `has_method`, `get_method_list` and
  `get_method_argument_count` from it.

### 5.3 Signals

Nothing exists today: `_has_script_signal` returns false and `_get_script_signal_list` returns
empty. Signals are how Godot programs are wired together, so this is parity-critical.

- **R-SIG-1 (MUST)** A Verse script declares signals with argument types, and they appear in the
  editor's Node panel where a designer connects them.
- **R-SIG-2 (MUST)** A script emits a declared signal with arguments.
- **R-SIG-3 (MUST)** A script connects to any signal on any object, with a Verse function or
  closure as the target, and disconnects.
- **R-SIG-4 (MUST)** A connection made in the editor to a Verse script's method works — including
  the editor's "connect and create the function for me" flow, which is what `_make_function` is
  for.
- **R-SIG-5 (MUST)** A script `await`s a signal from a concurrent context: the Verse spelling of
  GDScript's `await button.pressed`. Depends on §7.
- **R-SIG-6 (MUST)** Signals declared in Verse are connectable and emittable from GDScript and C#
  with no knowledge that Verse is involved (§8).

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
  scene validation, `_get_configuration_warnings`. Status: **none** (`_is_tool` returns false
  unconditionally). *This interacts badly with the single-code-generation constraint* — a tool
  script runs in the same process that must later run the edited version of itself — and is
  therefore gated on §10.
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
- **R-SCN-3 (MUST)** Godot's `@GlobalScope` utility functions and constants are reachable under
  names that do not collide with `/Verse.org/Simulation`. Today none are: every mirrored call
  rides `VhCallValue(Handle, …)`, a free function has no handle, and 78 math plus 8 random names
  would collide (README). Status: **none**.
- **R-SCN-4 (MUST)** Referencing a freed object is an error with a diagnosable message and does not
  corrupt the scene. Status: **done** — it raises a Verse runtime error, unwinds to the root
  failure context, and discards queued writes. *But* see R-ASYNC-4: today it also kills every
  suspended task in the project.

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
- **R-TYPE-3 (MUST)** `Callable` is a Verse value a script can hold, invoke, and hand back to
  Godot — this is what makes R-SIG-3 and any callback-taking engine API work. Status: **done.** A `callable` is
  held, passed back, and invoked with arguments. The other direction — a Verse function *as* a
  Callable — is R-SIG-3's and arrives in Phase 4.

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
- **R-TYPE-7 (MUST)** The plumbing stays hidden. The `Vh…` primitives, `variant`'s *lanes* and
  `object`'s `Handle` carry no access specifier and stay out of completion lists; a user cannot
  accidentally hold a raw handle that outlives what it names. Status: **done**.

  Amended in Phase 2: the `variant` **type** is public, because a script has to be able to name it in
  a signature for Godot's 231 Variant-typed methods to be callable at all — and because
  `typed_array(t)`'s element converter mentions it, and Verse refuses a required member less
  accessible than its class. Nothing else moved. Every lane still carries no specifier, so
  `V.I0` does not compile and neither does `variant{Tag := 2}`; what a script can do with one is ask
  `As<GodotType>[V]`, branch on `VariantKind(V)`, and build one with `VariantFrom<GodotType>`. A
  hand-made `variant` is all defaults, and a Tag of 0 is Nil, so the worst it can say is "nothing".

---

## 7. Concurrency

Verse's structured concurrency is the headline reason to prefer it to GDScript, and it is the part
least served today: one `verse::FContentScope` for the whole project, no way to await a Godot
event, and `vh_tick` pumped once per frame with a budget.

- **R-ASYNC-1 (MUST)** `spawn`, `race`, `sync`, `branch`, `rush`, `loop` and `<suspends>` functions
  work in script code, with tasks resuming across frames.
- **R-ASYNC-2 (MUST)** A script awaits a Godot signal, a timer, or a frame from a concurrent
  context. Without this, Verse's concurrency cannot observe the engine and is decorative.
- **R-ASYNC-3 (MUST)** Verse tasks run on Godot's main thread and are pumped deterministically
  relative to `_process` and `_physics_process`; the ordering is documented, not emergent.
  Status: **part** — `_frame` pumps `vh_tick` with a budget; the ordering guarantee is not stated.
- **R-ASYNC-4 (MUST)** Task scopes are per-script-instance, not per-project. A runtime error in
  one script's task must not terminate tasks belonging to another script. Today it does, and the
  README names this as a known limitation. Status: **none**.
- **R-ASYNC-5 (MUST)** A node's tasks are cancelled when the node leaves the tree or is freed, and
  a scene change cancels the tasks of everything it unloads. Structured concurrency whose
  structure does not match the scene tree's lifetime is a leak with extra steps.
- **R-ASYNC-6 (MUST)** The per-frame Verse time budget is configurable and observable, and
  overrunning it is reported rather than silently dropping frames.
- **R-ASYNC-7 (deferred)** Interaction with Godot's own threading — `WorkerThreadPool`, threaded
  resource loading, calling into Verse from a non-main thread, and running Verse tasks off the
  main thread — is **out of scope for this document and requires its own scoping**. It is not
  dismissed: the design of R-ASYNC-3 and R-ASYNC-4 must not foreclose it, and §14 **OQ-6** holds
  the questions that scoping has to answer.

---

## 8. Interop with GDScript and C#

The requirement is full, two-way, first-class: to the rest of the engine a Verse class is
indistinguishable from a GDScript one.

- **R-INT-1 (MUST)** GDScript and C# can instantiate a Verse class by its global name, call its
  methods with arguments and return values, read and write its properties, and connect its
  signals — with no Verse-specific API and no knowledge that Verse is involved. Depends on
  R-NODE-6 and §5.3.
- **R-INT-2 (MUST)** A Verse script calls methods on, and reads properties of, an object whose
  script is GDScript or C#, dynamically. Status: **done**, and as a side effect rather than as work
  of its own: `Object.callv(StringName, Array) -> Variant` is an ordinary concrete method, and Godot's
  `Object` became mirrorable the moment Variant and Array both crossed (Phase 2 §4.4). A script
  writes `AsInt[Target.Callv("_double", Args)]` and the GDScript method runs. `get`, `set`,
  `has_method` and `get_class` came with it.

  Proving it turned up a rule that had leaked out of the place it belonged: an object-typed
  *exported member* must be optional, because the inspector can leave a slot empty, and that rule was
  being applied to method *arguments* too — where it is not merely unnecessary but wrong. A parameter
  declared `node2d` was handed a `?node2d`, so the first `.GetName()` on it died inside the
  interpreter instead of failing to compile. A bare parameter now receives the object itself, and a
  null handle for one is refused as a bad argument rather than becoming an empty option.
- **R-INT-3 (MUST)** Signals cross in both directions (R-SIG-6).
- **R-INT-4 (MUST)** A `Callable` produced by any language is invocable from any other, including
  a Verse function handed to a GDScript API (R-TYPE-3).
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

---

## 10. The iteration loop

**Full hot reload is required.** Edit a script, and the new code runs — no editor restart, ever.

This is the hardest requirement in the document and it is deliberate: the current behaviour is
that any change to a script's *code* (as opposed to its shape, which analysis refreshes live)
requires restarting the editor, and that a script added while the editor runs is not seen at all.
An authoring loop with a restart in it is not a tool people use.

- **R-ITER-1 (MUST)** Editing a script and running the project executes the edited code, in the
  same editor session, indefinitely many times.
- **R-ITER-2 (MUST)** A `.verse` file added, renamed or deleted while the editor runs is picked up
  without a restart.
- **R-ITER-3 (MUST)** A changed default value on an `@export` member refreshes in the inspector
  (R-EXP-4). Today it cannot, because a declared default is evaluated by generated code.
- **R-ITER-4 (SHOULD)** Reloading preserves the state of a running game where Godot's own
  `reload(keep_state)` contract allows it.
- **R-ITER-5 (MUST)** A compile error during reload leaves the previously-working code running and
  reports the error; it does not leave the project in a half-loaded state. Status: **part** — a
  failed build reports once per session.

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
- **R-DIAG-3 (MUST)** A script error never takes down the editor or the game process.
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
  set. What gets measured early is what can be reasoned about later.
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
| **OQ-6** | What is the correct interaction between Verse's task model and Godot's threading — `WorkerThreadPool`, threaded loading, calls into Verse off the main thread? | R-ASYNC-7 | Its own scoping document. Until it exists, §7 must not adopt a design that assumes single-threaded forever. |
| **OQ-7** | Build our own LSP over `verse_host_abi.h`, or get `uLangLSP` into a linkable target? | R-TOOL-10 | Low priority — Godot's editor is primary (§9). |
| **OQ-8** ✅ | Which hot-reload mechanism: fresh package name per generation, out-of-process compilation, or an engine change? | all of §10, and R-EXP-5 | **Closed: fresh package name per generation**, with `IncrementalizeProjectSource` before each build. See §14.1. |
| **OQ-9** | Can any DAP client speak `Verse::SocketDebugger`'s framing? | R-DIAG-6 | Only worth answering if R-DIAG-4 (Godot's own debugger) turns out to be blocked. |
| **OQ-10** | Can an editor-class UBT Program target be built — `bCompileAgainstEditor`, and therefore `bCompileAgainstEngine`? Cooking Verse needs `WITH_EDITOR=1` (§14.1), and nothing else this project builds does. | R-DIST-9, R-DIST-10, R-DIST-11 | Opened by the S-1 answer. Attempt it at the start of Phase 7. The one prior attempt failed on Engine module links, but it was made for a *lean* host, where the weight was the objection; a cooker that runs only at export has no such constraint. Fallback: cook through a real UE editor or commandlet process. |
| **RISK-1** | UE's licensing applies to games shipped with the host, including royalties. This is a permanent property of the current distribution model and may deter adoption regardless of anything built here. | adoption | Disclose prominently (R-DIST-3). No mitigation available. |
| **RISK-2** | Tracking Godot `master` and UE `main` simultaneously means two moving dependencies with no compatibility window. | R-QUAL-7 | Accepted deliberately while pre-1.0; revisit at the first release. |

### 14.1 Answers from the Phase 0 spikes

Evidence, measurements and code citations: [`phase-0-spikes.md`](phase-0-spikes.md).

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
