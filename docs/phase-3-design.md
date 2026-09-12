# Phase 3 — what a project is: the source set, live

**Status:** Draft 2 · 2026-09-12 · **built**, bar the two by-hand checks §9 asks for. §11 is the
record of where this document turned out to be wrong — read it before trusting a number here. Draft 2 moved the build trigger off save and
onto Play, and added §2.5 and §2.6. **§1's spike is answered — affirmatively — and nothing in this
document is redesigned by it;** §1 records what the run found, and `spec.md` §14.1 is the record.
**Companion to:** [`roadmap.md`](roadmap.md) §"Phase 3", [`spec.md`](spec.md) §10 and §14.1,
[`phase-0-spikes.md`](phase-0-spikes.md) S-2 and S-3.

The phase has two halves that are one piece of machinery: **hot reload** (the host owning a script
package it can republish) and **modules** (a source set whose files have module identity). They
share the package, the snippet, and the file enumeration, which is why the roadmap moved modules
out of Phase 2 to join them.

---

## 0. Where the decisions came from

Settled by interview before design, so the reasoning is not reconstructed later:

| decision | answer | consequence |
| --- | --- | --- |
| reload trigger | **on Play, plus an explicit Build action** | Godot's own hook does it; far fewer generations than per-save, which largely defuses the leak |
| reload scope | **editor session only** | a run is a fresh process; live-reloading a running game is deferred with a requirement of its own |
| instance adoption | **new instances only** | the spike's free behaviour; no state-transfer path in the ABI |
| module mapping | **marked directories are modules** | unmarked directories are organisational and join the nearest module ancestor |
| module marker | **`<name>.vmodule`**, empty, name from the file stem | a Godot asset directory name never has to be a Verse identifier |
| making one | **"Make Verse Module" in the FileSystem dock's directory context menu** | the editor plugin stops being a stub; Godot's dock cannot create an empty file otherwise |
| ambiguous base stem | **report, fall back to `Node`, let the build correct it** | the pre-build text scan is honest about the one thing it cannot know |
| rebuild entry point | **`vh_compile_project`, re-callable** | there is no real difference between the first build and the fifth |
| imports | **root implicit, cross-module `using` auto-maintained** | R-TOOL-12; rewriting on *move* is deferred |
| `@global_class` | **project-wide unique, module-free in ClassDB** | a duplicate is a diagnostic, never an auto-qualified name |
| top-level names | **any number per file; only the stem-named class attaches** | the project-wide uniqueness rule retires as a *Verse* rule and survives as a bridge rule |
| ABI | **break freely — v3.0** | `vh_compile_project` changes shape rather than growing a sibling |
| `@tool` | **editor execution of Ready/Process** | not the editor-only virtual surface, which is Phase 4's general mechanism |
| the leak | **acceptable while experimental** | measured and recorded, not fixed; tracked as a requirement so it is not forgotten |
| reload testing | **manual, plus `host_smoke`** | no scratch-project machinery in `run_tests.py` |

Three of these were settled by reading upstream rather than by preference, and it is worth knowing
which: the **module mapping** and **marker** are Epic's own `.vmodule` mechanism rather than an
invention (§2.1); the **trigger** is what Godot already does for C#, the one language it ships whose
compilation unit is the whole project like ours (§4.3); and the **ambiguous base stem** is not a
choice at all but a constraint found in our own code, which modules break (§2.5).

---

## 1. Stage 0: the one thing that must be answered before any of this is designed

**Does a generation change the package *name* only, or the *verse path* too?** — `spec.md` §14,
**OQ-12**.

S-2 gave each generation its own package name (`GodotScripts_1`, `GodotScripts_2`, …). What it does
not record is whether `/user@localhost` stayed fixed across those generations, and the whole phase
rests on the answer:

- Module paths are **user-visible text**. R-TOOL-12 writes `using { /user@localhost/gameplay }` into
  the author's file. If the verse path carries a generation number, the editor's own import is
  invalidated by the author's next save — the roadmap's "a name the user has written must survive a
  reload" stops being a caution and becomes a mechanism to build.
- `ScriptVersePath` is compiled into eight lookup sites in `HostScript.cpp` — `FindGodotClass`,
  `ShapeKeyFor`, `DescribeMemberType`, `GetClassMethods`, `GetClassExports`, `ClassMembers`,
  `Instantiate`, and `vh_run_main`'s `FVerseFunction`. A per-generation path means every one of them
  learns which generation it is asking about, and instances from older generations are looked up
  under their own path. A fixed path means none of them changes at all.

There is precedent for the optimistic answer: the attribute package already shares `/Godot.org/Godot`
with the native package, so two packages at one verse path is a thing the toolchain does. Whether it
survives the *same definitions* being republished at that path is the actual question.

**Method.** Extend the S-2 prototype in `host_smoke`: publish generation N as package
`GodotScripts_N` with the verse path pinned at `/user@localhost`, and check that (a) publishing does
not assert on a duplicate definition, (b) `LookupDefinition` finds the *new* generation's class, and
(c) an instance made in generation N-1 still resolves against its own.

**Also confirm, in the same run** (cheap, and §3 depends on it): a definition in the root module is
visible from a submodule with nothing written, by ordinary lexical scoping. The S-3 prototype proved
the *outward* direction — root reading into `gameplay` and `ui` — and the direction this phase needs
is the other one.

**If the answer is negative**, the fallback is a stable *public* path decoupled from the package
name, or a per-generation alias module. Either is a design change large enough that it must land
before Stage 1, not after. **Nothing else in this phase starts until this is written down.**

### 1.1 The answer: the name only

**The verse path is pinned and the package name carries the generation.** Not one of the eight
lookup sites in `HostScript.cpp` learns which generation it is asking about; `ScriptVersePath` is
still a `constexpr`. What the run found, all of it now running as checks in `tests/host_smoke`:

- publishing generation N+1 at the same verse path does not assert on a duplicate definition
  **provided generation N's package leaves the *source project* first**. That is the one thing the
  question did not anticipate and it is a two-line function (`RemoveScriptPackage`): only the
  *source* goes, while the published package stays live in the VM, which is exactly what lets an old
  instance keep running and is also R-ITER-6's leak;
- `LookupDefinition` finds the new generation's class at the unchanged path;
- an instance made against generation N keeps answering with generation N's code — R-ITER-4, free;
- `IncrementalizeProjectSource` before each `BuildAll` is what stops the build republishing the
  already-loaded native VNI packages, which is where S-2's first attempt died.

Confirmed in the same run, as §1 asked: a file in a submodule reaches a definition in the **root**
module with nothing imported. §3's "root is implicit" therefore costs nothing to build.

The spike's checks are deliberately placed **before** the rest of `host_smoke` rather than after it,
so every remaining check in that file runs against the *second* generation. A generation that only
half worked would otherwise pass here and fail in an editor.

**One defect found on the way, which is not this phase's.** After a Verse runtime error is raised,
every later `vh_instance_call` returns `VH_OK` with no result value — a method declared `:int`
answers as if it were `:void`. It predates this work and is recorded against **R-DIAG-3** in
`spec.md`, with the reproduction in §14.1.

---

## 2. Modules

### 2.1 The model, and why it is Epic's

`CSourceFilePackage::ResolveModuleForRelativeVersePath` (`SourceFileProject.cpp:352`) walks a
snippet's relative directory and calls `FindOrAddSubmodule` per component: **every directory is a
module, from the tree, with no declaration.** A component failing `IsValidModuleName`
(line 765 — ASCII `[A-Za-z_][A-Za-z0-9_]*`) raises `ErrSystem_InvalidModuleName`.

`TryRenameModule` (line 512, live behind `VERSE_ALLOW_VMODULE_FILES 1`) is the other model, and it
is the one taken: a directory carrying a `.vmodule` file is a module **named by that file**, and
once any ancestor is named, unnamed intermediate directories are deleted and their snippets are
moved up into the nearest named ancestor.

Taken as the primary rule rather than a legacy path, for three reasons:

1. **`res://` is an asset tree.** Its directory names were chosen for sprites and scenes, not for
   Verse identifiers. Under Epic's default, `res://2d/` and `res://my-stuff/` are hard errors in a
   project that has done nothing wrong.
2. **It is backwards compatible, exactly.** No project on disk has a `.vmodule`, so every existing
   project keeps every file in the root module and nothing changes meaning on upgrade. Epic's
   default would move all five of `dodge-the-creeps/scripts/*.verse` out of root on day one.
3. **The marker names the module, not the directory.** `res://my-stuff/gameplay.vmodule` is module
   `gameplay`. The identifier rule applies only to names the author deliberately chose, which is
   where a diagnostic about it is actionable.

The cost is one concept to explain, and that "which module is this file in" is a walk up the tree
rather than a glance at the path.

### 2.2 The rule, as testable code

A new `src/verse_module_map.{h,cpp}`, with **no godot-cpp dependency**, joining `verse_lexer` and
`verse_class_decl` as a standalone unit-testable component and its own `tools/build_*_test.py`:

    module_map verse_build_module_map(const std::vector<std::string>& source_paths,
                                      const std::vector<std::string>& marker_paths);

**Two markers with the same name merge into one module** rather than colliding. This is a judgment
call, not an interview answer, and it is made this way because it is what the toolchain already
does: `FindOrAddSubmodule` is find-*or*-add, so `res://a/gameplay.vmodule` and
`res://b/gameplay.vmodule` naming the same module is a module spread across two directories, not an
error. Per-module name uniqueness then applies across the merged set, and the collision diagnostic
in §2.4 names all the contributing directories rather than assuming one. Revisit if it turns out to
surprise people more than it helps them.

Pure: `res://`-relative paths in, module path per source file out. Every rule in §2.1 — nearest
marked ancestor, marker-names-the-module, invalid marker name, two markers merging — is a case in
a test that needs neither Godot nor UE. Godot's side contributes only the two enumerations, which
`find_verse_sources` already does for one of them.

**Godot resolves the module; the host is told.** The host never learns what `res://` means, which
keeps §14.1's "a module path is relative to the project root and the ABI carries only absolute
paths" from becoming a second path vocabulary inside `HostScript.cpp`.

### 2.3 Class identity

Every `ClassNameUtf8` parameter in the ABI becomes a **qualified class name**: `player` at root,
`gameplay/player` in a module. The host splits on the last `/` to build
`(/user@localhost/gameplay/player:)Field` where it builds `(/user@localhost/player:)Field` today.
One consistent change across `vh_has_class`, `vh_instantiate`, `vh_class_method_list`,
`vh_class_export_list`, `vh_class_default_field` and `vh_class_members` — no new handle type, and
`VerseScript::verse_class_name()` gains the module prefix from the map.

### 2.4 What retires, and the diagnostics that replace it

**"Every top-level name in the project must be unique" is gone**, in both halves:

- A file may declare any number of top-level names. `verse_scan_class_decl` currently finds *the
  first* top-level class and calls it the file's class; it must instead find **the one named after
  the file** and tolerate the others. Phase 2's `derived_entity.verse` already declares an
  interface, two structs, an enum and a parametric class beside its class, so the scanner is
  already being asked more than it promises.
- Uniqueness narrows from project-wide to per-module.

Three diagnostics carry the change, and they are part of the feature rather than fallout:

| situation | what it must say |
| --- | --- |
| two files in one module declare the same top-level name | name both files, and say that **marking a directory** puts them in different modules — not Verse's generic redefinition error |
| a `.vmodule` file's stem is not a Verse identifier | name the file, the rule, and a legal example |
| two `@global_class` classes register the same Godot name | name both files. Godot's ClassDB is one flat namespace and the module is deliberately not in it, so uniqueness is the author's to resolve |
| a base class name matches files in more than one module | name the candidates, and say that the registry answer is provisional until the next build — §2.5 |

The first is the one that matters: it is the only thing that tells an author the marker exists, and
its fix is the context-menu action in §2.6.

### 2.5 The pre-build text scan, and the one thing it cannot know

Modules break an assumption buried in `VerseScriptLanguage::base_types_for`, and it is worth being
explicit because the failure is silent.

Godot's `EditorFileSystem` asks `_get_global_class_name(path)` for every `.verse` during its startup
scan — **on a scan thread, before anything has been built**. Both halves put the host out of reach
(`verse_script_language.cpp:1451` says so), so the answer is read from text: `verse_scan_class_decl`
finds `player := class(node2d):`, and `base_types_for` walks the base chain upward to find the Godot
type at the top, because "which nodes may this script be attached to" is what Godot is asking.

Walking that chain means resolving a base that is *another script*. For `enemy := class(player)`,
`script_path_for_class` finds `player.verse` by **matching the file stem against every `.verse` under
`res://`** — which is unambiguous today only because every top-level name in the project is unique.
After modules, `gameplay/player.verse` and `ui/player.verse` both exist and the stem alone does not
choose between them. The compiler has no such problem; it resolves through modules and `using`. Only
this scan does, and it cannot be fixed by asking the host.

**Decision: report the ambiguity and fall back to `Node`**, then let the build correct it — once the
project is built, `VerseScript` reads the real base from the semantic program. The cost is a window
between editor startup and the first build in which the class picker offers the wrong base type for
those scripts, and the window is visible rather than silent. The alternative considered was teaching
the scanner to read `using` lines, which would be right in nearly every real case and would
introduce a second, text-only resolution path guaranteed to disagree with the compiler somewhere.

### 2.6 Making a module

Godot's FileSystem dock cannot create an empty file, so without an affordance the phase's headline
feature would require leaving the editor. `VerseEditorPlugin` adds **"Make Verse Module"** to a
directory's context menu in the dock: it writes `<dirname>.vmodule`, pre-filled and renameable, and
the duplicate-name diagnostic in §2.4 names the same action as its fix.

This is the second of the plugin's new jobs — `_build` in §4.3 is the first — and together they turn
`verse_editor_plugin.{h,cpp}` from a 19-line stub into a real component of this phase.

---

## 3. Imports (R-TOOL-12)

**Root is implicit.** Definitions at `res://` root are visible from every module with nothing
written — which keeps library files working exactly as Phase 2 built them, `vectors.verse`
included. Pending Stage 0's confirmation this is Verse's own lexical scoping and costs nothing.

**Cross-module imports are auto-maintained.** Completion offers symbols from modules not in scope,
and the `using` materialises when analysis reports the unknown name. Insert-only; nothing is ever
removed, because removing a line the author may have written is a different and worse promise.

Two things make this the riskiest item in the phase, and it is staged last for both:

- **There is no edit-on-accept hook.** Godot's completion API does not tell us an item was accepted,
  which is why the roadmap specifies the goimports shape — react to the *diagnostic*, not to the
  keystroke. Insertion therefore happens on validate, which means writing into a buffer the author
  is typing in.
- **Writing to the buffer may only be reachable from the editor plugin**, not from
  `ScriptLanguage`. `verse_editor_plugin.cpp` exists and is `TOOLS_ENABLED`, so there is a place to
  put it; whether a GDExtension can reach the active `ScriptTextEditor`'s `CodeEdit` needs checking
  before the stage starts.

**Fallback, if buffer editing is unreachable:** the diagnostic names the exact `using` line to
paste. That is a worse feature and a complete one, and it is what ships if the mechanism is not
there. Deciding this late is deliberate — it is the one item whose absence does not block anything
else in the phase.

**Deferred:** rewriting imports when a file moves between directories. A move breaks its references
and reports them. Recorded in the spec as owed, not as done.

---

## 4. Hot reload

### 4.1 The host owns its package

`ScriptPackageName` is `SolIdeDataSources` — `ISolIdeDataSource::DefaultDataSourceName`, and fixed.
A fresh name per generation means the host stops calling `ISolarisIde::AddDataSource` and builds the
package itself with `FindOrAddSourcePackage` + `AddSourceSnippet`, which is what `AddAttributePackage`
already does.

Two things come with the IDE's data source and not with a bare package, and S-2 paid for both:

- **A snippet whose text can be replaced.** Without it, three completion tests and one analysis test
  in `host_smoke` fail. A ~30-line `ISourceSnippet` with a text setter **that also drops the cached
  VST** — the toolchain clones a valid cached VST rather than reparsing, so a stale one analyses the
  text you just replaced.
- **The dependency list.** `EnsureDataSourcePackageExists` set it for the IDE's package; nothing
  sets it for ours.

This stage changes no behaviour. Its exit is that `host_smoke` is **247/247**, unchanged.

### 4.2 Generations

`GProjectBuilt` (and `VerseScriptLanguage::project_built`) stop being one-shot latches.
Per generation: `FSolarisModule::IncrementalizeProjectSource` first — without it the build
republishes the *native* VNI packages and aborts there, which is where S-2's first attempt died —
then a package name never used before, then `BuildAll`.

`vh_compile_project`'s once-per-process contract and the warning text in `ensure_project_built`
both go. So does the `push_warning` about restarting the editor.

### 4.3 Godot drives it — on Play, not on save

**Saving does not build.** A build happens when the project is run, and on an explicit Build action.
This is Godot's own answer for a language whose compilation unit is the whole project: C# is built
by `BuildManager.EditorBuildCallback` → `BuildProjectBlocking("Debug")`, invoked on Play, never on
save. GDScript reloads synchronously on save (`ScriptEditor` calls `scr->reload(true)` directly,
`script_editor_plugin.cpp:1526`) and gets away with it only because its unit is one file at ~1 ms.
Ours is ~200 ms for a one-class project and unmeasured for a real one.

The hook already exists and needs no new Godot API. `EditorRunBar::_run_scene` calls
`EditorNode::call_build()`, which calls `build()` on **every editor plugin** and aborts the run if
any returns false (`editor_data.cpp:444`). `EditorPlugin` exposes it as the `_build` virtual
(`editor_plugin.h:143`), so `VerseEditorPlugin` overrides it, builds a generation, and returns false
with the diagnostics on failure — which is both R-ITER-5 and "don't launch a game you know won't
run", in the engine's own idiom.

**What a save does instead** is what it already does: refresh analysis, so diagnostics, completion,
lookup and the export *shape* stay live per keystroke. `_reload_scripts` and `_reload_all_scripts`
(empty stubs today) refresh the script's own state from the current generation; they do not build.
Every build re-enumerates `res://`, which is what makes **R-ITER-2** — files added, renamed and
deleted live — fall out rather than need its own mechanism.

- **R-ITER-3 / R-EXP-4.** `vh_class_default_field` reads from the current generation, so a changed
  `@export` default refreshes once the script re-reads its export list and notifies Godot — **after
  a build**, not after a save. Worth stating plainly because the two halves now move at different
  speeds: a *new* exported member appears in the inspector as soon as analysis sees it, while its
  *default value* is whatever the last build generated. That is honest and it is the price of this
  trigger; the alternative was paying 200 ms on every Ctrl+S.
- **R-ITER-4.** Free. An instance keeps its own generation's class; nothing is invalidated under the
  engine, and adopting a new class is a deliberate act nobody has asked for yet.
- **R-ITER-5.** A failed build publishes nothing, so the previous generation keeps running. One
  decision that follows: **the inspector keeps showing the analysed text's shape, not the last good
  generation's.** Shape has come from analysis since Phase 1 and refreshes live; keeping that means
  the inspector can briefly show a property no running code has, which is the lesser of the two
  surprises and the one that is already true today.

### 4.4 The leak, recorded rather than fixed

~0.5 MB per generation for a one-class project — the previous generation's `VPackage`, its
`UPackage` and their pinned exports.

**The build-on-Play trigger largely defuses this.** A generation costs a run or a deliberate Build,
not a Ctrl+S, so a session spends generations in the tens rather than the hundreds. Had the trigger
stayed on-save it would have been the difference between a 200 MB and a 400 MB editor session on a
long day; it is now a rounding error next to the 110 MB the compiler and native packages cost once.

Still accepted rather than fixed while the project is experimental, and still tracked: it gets a
**measured figure against a real project** (dodge-the-creeps, not the one-class prototype) and a
**requirement** (R-ITER-6) so the deferral is visible. No reaping mechanism is built.

---

## 5. `@tool` (R-EXP-5)

Cheaper than the roadmap implies, because the routing already exists. `VerseScript::_can_instantiate`
(`verse_script.cpp:365`) already returns a real instance in the editor for a tool script and lets
Godot fall back to a placeholder otherwise. The work is making `_is_tool()` truthful:

- a `tool` attribute added to `AttributePackageSource` in `HostScript.cpp`, beside `global_class`
  and `export` — `tool` is **not** a reserved Verse symbol (checked against `verse_keywords.h`,
  generated from the compiler's own `ReservedSymbols.inl`), so it is spellable as written;
- `verse_scan_class_decl` reads it from the text the way it reads `@global_class`, because
  `_get_global_class_name` is asked from `EditorFileSystem`'s scan thread and cannot call the host;
- `VerseScript::_is_tool()` and `_get_global_class_name`'s hardcoded `result["is_tool"] = false`
  answer from it.

Ready and Process then run in the editor through the path that already carries them. **Not** in
scope: the editor-only virtuals — gizmos, `_get_configuration_warnings`, property-change hooks —
which are Phase 4's general virtual mechanism and would otherwise be built twice.

A tool script runs **the last built generation**, which follows from §4.3's trigger: editing one and
saving refreshes its analysis but not its behaviour, and the Build action is what makes the edit
live. That is the same bargain a C# `[Tool]` script makes in Godot today, so it is a familiar shape
rather than a surprise — but it is the workflow most likely to make an author reach for Build, and
therefore the strongest argument for the action being discoverable rather than buried.

**Stated risk:** Verse now runs against a scene the author is editing, and **R-DIAG-3** (a script
error never takes down the editor) does not land until Phase 6. Tool execution is opt-in per script,
which bounds it but does not remove it.

---

## 6. ABI v3.0

A major bump: layout and meaning both change, and both sides rebuild. Per the header's policy the
mismatch surfaces at `vh_init`.

| change | why |
| --- | --- |
| `vh_compile_project` takes **(absolute path, module path)** pairs | modules |
| the same `vh_compile_project`, **re-callable**, returning the new generation | generations. No sibling entry point: there is no real difference between the first build and the fifth, so the ABI should not invent one. Its once-per-process paragraph becomes a description of generations rather than a prohibition |
| every `ClassNameUtf8` becomes a **qualified** class name | §2.3 |
| `vh_resolve_unknown_name` — which modules define this name | R-TOOL-12; returns zero, one, or many candidates |

Whether §1 forces a sixth row — a generation-aware verse path — is Stage 0's to say.

---

## 7. Spec edits

**Already made, with this document** — the decisions above are recorded whether or not the code
follows, so the spec never describes a plan only this file knows about:

- **R-TOOL-12 (new, MUST)** — the editor maintains `using` statements. Status **none**, with both
  unknowns and the fallback named.
- **R-ITER-6 (new, SHOULD)** — retained memory across a long editor session is bounded. Status
  **none**, deliberately, so the deferral is tracked rather than rediscovered.
- **R-ITER-7 (new, SHOULD)** — a running game picks up an edit without being restarted. Status
  **none**, with the reason it is not Phase 3's: the game is a separate process with its own host,
  so reaching it is a second delivery mechanism rather than an extension of the first.
- **OQ-12 (new)** — §1's question, in the §14 table with its method and its negative branch.
- **R-LANG-6** and **R-EXP-5** carry the module model and the `@tool` split; **R-ITER-5** carries
  the inspector decision in §4.3; **R-ITER-3** carries the build-not-save refresh; **§10** carries
  the build-on-Play trigger, the hook it uses, and the no-adoption rule.

**Owed as the work lands**, per §15 — a requirement's status changes in the commit that changes it:

- **R-LANG-6** clauses 1 and 2 → **done** (Stage 4). The "one flat scope" and "one code generation"
  rules in README's "Five constraints" and in `CLAUDE.md` are then rewritten rather than annotated;
  both currently say *Phase 3 removes this* and remain true until it does.
- **R-ITER-1, R-ITER-2, R-ITER-3, R-ITER-5** → **done** (Stages 2–3). **R-ITER-4** → **done**, with
  its meaning stated: an instance keeps its generation.
- **R-TOOL-12** → whatever §3 actually ships, including the paste-this-line fallback if that is it.
- **R-EXP-5** → **part** (Stage 6), with the editor-only virtual surface named as what is missing.
- **R-ITER-6** gains the per-generation figure measured against a real project, not the one-class
  prototype.
- **OQ-12** → closed in §14.1 beside the Phase 0 spikes, whichever way it falls. A negative answer
  also edits §10's mechanism paragraph, because the settled mechanism would then have a second half
  nobody has priced.
- **R-SCN-2**'s "say why" bar extends to the four module diagnostics in §2.4.

---

## 8. Work order

| stage | what | done when |
| --- | --- | --- |
| **0** | the verse-path spike (§1), plus submodule→root visibility | a written answer; the phase is redesigned if it is negative |
| **1** | the host owns its script package: `ISourceSnippet` with settable text + VST drop, dependency list | `host_smoke` 247/247, no behaviour change |
| **2** | generations: `IncrementalizeProjectSource`, fresh name, latches removed | `host_smoke` compiles N generations in one process and each runs the edited code |
| **3** | Godot drives it: `VerseEditorPlugin::_build` on Play, a Build action, `_reload_scripts` refreshing without building; R-ITER-1/2/3/5; the restart warning removed | add, rename, delete and edit a script with the editor open, press Play, get the edited code — by hand |
| **4** | modules: `verse_module_map` + `.vmodule` + the dock context menu + qualified class names + the four diagnostics + multi-declaration scanning | two same-named classes in two modules coexist and both attach |
| **5** | R-TOOL-12, or its fallback | an unknown cross-module name acquires its `using` without the author typing one |
| **6** | `@tool` | a marked script's Process runs in the editor |
| **7** | yardstick: dodge-the-creeps regression, a `.vmodule` in `scripts/`, and the **windowed run it is owed** | 29 headless checks pass with the game in a module |

Stage 1 before Stage 2 because it is the regression risk and carries no new feature. Stage 3 before
Stage 4 because modules change what a class is *called*, and doing that before reload works means
debugging both at once. Stage 5 last because it is the only item whose fallback is acceptable.

**The editor plugin is on the critical path from Stage 3 onward**, which it was not in the first
draft of this order: `_build` (§4.3), the "Make Verse Module" action (§2.6) and R-TOOL-12's
insertion (§3) all live in it. Whatever Stage 3 learns about what a GDExtension `EditorPlugin` can
actually drive is therefore worth writing down when it learns it — Stage 5's main unknown is the
same unknown.

### 8.1 Where the code is

Line numbers are from the commit this document was written against and will drift; the names will
not. `CLAUDE.md` has the build and test commands, and every stage below ends with
`python tools/run_tests.py`.

**Stage 0 — the spike.** `tests/host_smoke/` and `tools/build_smoke.py`. The prototype S-2 ran was
reverted, so this is written fresh against the shape S-2 recorded: `IncrementalizeProjectSource`
before each `BuildAll`, a package name never used before, and this time the verse path held at
`/user@localhost`. Nothing in `src/` or `host/Private/` changes; the answer goes in `spec.md` §14.1
and, if negative, this document is redesigned before Stage 1.

**Stage 1 — the host owns its package.** All in `host/Private/HostScript.cpp`:
`ScriptPackageName` (:76) and `ScriptVersePath` (:77); `EnsureIde` (:317), which is where
`MakeDevEnvironment` and `AddDataSource` are today; `AddAttributePackage` (:178), which is the
worked example of `FindOrAddSourcePackage` + `AddSourceSnippet` and the thing to copy;
`CompileProject` (:392) and `RunCheck` (:444), whose `GDataSources` loop becomes a lookup into our
own snippets. The new `ISourceSnippet` subclass wants `CSourceDataSnippet`
(`VerseCompiler/Public/uLang/SourceProject/SourceDataProject.h`) as its model — note it stores a
`TOptional<Vst::TNodeRef<Vst::Snippet>>` that the setter must clear.

**Stage 2 — generations.** `GProjectBuilt` (`HostScript.cpp:144`, set at :421, read at :394 and
:446) and `ResetScriptState` (:384). `VerseScriptLanguage::project_built` and
`project_build_status` (`src/verse_script_language.h:236`), `ensure_project_built`
(`src/verse_script_language.cpp:1618`) and the `push_warning` at its end. The once-per-process
paragraph in `include/verse_host_abi.h:404`, and the matching one in `RunCheck`'s comment.

**Stage 3 — Godot drives it.** The build trigger is `EditorPlugin`'s `_build` virtual, overridden in
`src/verse_editor_plugin.{h,cpp}`. Godot's side, for reference while wiring it:
`EditorRunBar::_run_scene` → `EditorNode::call_build()` → `EditorData::call_build()` (`godot/editor/
editor_data.cpp:444`), which calls `build()` on every plugin **in registration order** and stops at
the first false; the virtual is declared at `godot/editor/plugins/editor_plugin.h:143`. C#'s use of
the same hook is `GodotSharpEditor.cs:430` → `BuildManager.EditorBuildCallback` →
`BuildProjectBlocking`, which is the shape to copy including the failure path.

On our side: `_reload_scripts`, `_reload_all_scripts` and `_reload_tool_script`
(`src/verse_script_language.cpp:559`–578, all stubs) refresh script state **without** building;
`VerseScript::_reload` (:441) and `VerseScript::compile` (:115); `find_verse_sources` (:1581) re-runs
per build, which is R-ITER-2. For R-ITER-3, `class_default_field` reaches `vh_class_default_field`
and the inspector needs `notify_property_list_changed` after the export list is re-read
(`verse_script.cpp:787`) — on build, not on save.

**Stage 4 — modules.** New `src/verse_module_map.{h,cpp}` + `tests/verse_module_map_test/` +
`tools/build_module_map_test.py`, following `verse_class_decl` exactly — no godot-cpp, one `main`
per case, non-zero on failure — and added as a fourth unit binary in `tools/run_tests.py`.
`verse_scan_class_decl` (`src/verse_class_decl.cpp`) stops taking the *first* top-level class and
takes the one matching the file stem. `VerseScript::verse_class_name` (`verse_script.cpp:203`)
gains the module prefix. §2.5's ambiguity lands in `script_path_for_class`
(`verse_script_language.cpp:1480`) and its caller `base_types_for` (:1492), which is also the walk
that must stop guessing when the stem matches more than one file. The dock context menu is
`verse_editor_plugin.cpp` again. On the host, the qualified-name split lands in `FindGodotClass`
(`HostScript.cpp:624`) and in the five path builders that concatenate `ScriptVersePath`:
`DescribeMemberType` (:1250), `ShapeKeyFor` (:1557), `GetClassMethods` (:2513), `GetClassExports`
(:2607), `ClassMembers` (:3512), plus `Instantiate` (:3598) and the `FVerseFunction` at :3708.

**Stage 5 — R-TOOL-12.** `src/verse_editor_plugin.{h,cpp}` (19 and 20 lines today, `TOOLS_ENABLED`),
and a new ABI entry point beside `vh_complete_symbol` (`verse_host_abi.h:969`). The completion side
is `_complete_code` in `verse_script_language.cpp`.

**Stage 6 — `@tool`.** `AttributePackageSource` (`HostScript.cpp:101`, the string literal declaring
`global_class` and `export`); `verse_scan_class_decl`'s attribute reading; `VerseScript::_is_tool`
(`verse_script.cpp:376`) and `result["is_tool"] = false` (`verse_script_language.cpp:1472`).
`_can_instantiate` (`verse_script.cpp:365`) already does the rest and should not need touching.

**Stage 7 — the yardstick.** `dodge-the-creeps/`, an empty `scripts/<name>.vmodule`, and
`headless_check.gd`'s 29 checks unchanged. Not in `run_tests.py`, deliberately.

---

## 9. Exit criteria

- **No workflow requires restarting the editor.** Concretely, all with one editor session open: add
  a `.verse` and attach it; rename it; delete it; change a method body and press Play to get the
  edited code; change an `@export` default and see the inspector update after a build; break the
  build and watch Play refuse to launch while the previously-working code keeps running.
- **A failed build aborts the run** rather than launching a stale game, and says why — which is
  `_build` returning false, the same thing C# does.
- **Two classes with the same name in two modules** coexist, both attach, and the collision
  diagnostic for two in *one* module names the marker.
- **`dodge-the-creeps` still plays headless** with its five files in a `scripts` module, and has
  been run **once with a window** — owed since Phase 2 and not yet paid.
- **A `@tool` script executes in the editor.**
- **`demo/` runs.**
- **The leak has a measured number** against a real project, in the spec, with R-ITER-6 open.

---

## 10. Risks

| risk | mitigation |
| --- | --- |
| Stage 0 says the verse path must change per generation | the whole of §3 is redesigned before anything is built; this is why it is Stage 0 |
| a GDExtension `EditorPlugin` cannot drive what §2.6 and §3 need — the dock's context menu, the script editor's buffer | Stage 3 exercises the plugin first with `_build`, which is the cheapest of the three and the one with no alternative; what it learns prices the other two. §3 already has a fallback, §2.6 falls back to a documented hand-made file |
| the Build action is missed, and an author edits a `@tool` script or an `@export` default expecting it to be live | it is the reason the action must be discoverable rather than buried, and the reason the diagnostic and the inspector should be able to say "built N edits ago". Not fully solved in this phase |
| the build blocks the main thread | accepted, because it now happens on Play and on an explicit action — where Godot already blocks for C#. Stage 2 measures the real number against a real project; if it is bad, off-thread building becomes a requirement rather than a guess |
| a `@tool` script's error takes down the editor | stated, not mitigated; R-DIAG-3 is Phase 6 |
| memory in a long session | accepted; R-ITER-6 tracks it |

---

## 11. What it built, and where the design was wrong

Written after the code, like §11 of `phase-2-design.md` and for the same reason: the plan above is
worth keeping, and so is the record of where following it taught something the plan did not know.

**§1's spike came back positive and nothing was redesigned.** §1.1 has the answer. The one thing
the question did not anticipate is that the retiring generation must leave the *source project*
before the next build — `RemoveScriptPackage`, two lines — or every class is declared twice at one
path.

**Three numbers replaced three estimates.**

| the design said | it is |
| --- | --- |
| a build is "~200 ms for a one-class project" | **1.27 s** for the five-file game, against a **3.1 s** first build. Later generations are cheaper because `IncrementalizeProjectSource` marks the native packages external and only the script package is rebuilt |
| "~0.5 MB per generation" | **~1.3 MB**, median of ten generations of the same game |
| `host_smoke` is "247/247" | 274 before this phase, 292 after |

The build figure is the one worth sitting with. A second and a quarter is a real pause, and it is
the whole argument for the trigger: on Play it is a pause nobody notices next to loading a scene,
and on Ctrl+S it would have been unusable. If it has to come down, off-thread building is a
requirement rather than a guess (spec R-PERF-2).

**§3's riskiest unknown was not a risk.** A GDExtension *can* write into the active buffer:
`ScriptEditor::get_current_editor()->get_base_editor()` is the `CodeEdit`. The fallback shipped
anyway, because it is better than the mechanism in the case the mechanism cannot reach — a file the
author is not looking at. What is **not** built is the completion half: symbols from modules not yet
in scope are not offered. Typing a name you already know is covered; discovering one is not.

**The qualified-name problem had two halves and only one was in the design.** §2.3 has the ABI half
— split the name, build the path — and it was a few lines. The half nobody had thought about is the
*reverse*: a `UObject` knows only its class's leaf name, and six lookups start from one. The answer
is that a `UClass`'s own name is the module path mangled with `-` by `VNamedType::AppendMangledName`,
so unmangling is a substitution. `UVerseClass::PackageRelativeVersePath` would have been the direct
answer and is dead under VerseVM — the line that sets it is commented out in `VVMClass.cpp`.

**And a third half, which only the yardstick found.** Asking the *semantic* program for a class's
qualified name is a third thing again, and `EPathMode::PackageRelative` is a trap: for a class with
no package — every intrinsic — it is a fatal error rather than an empty answer. Taking the whole
verse path and removing the package prefix is the same answer and survives every class. The bug it
caused was invisible to every test in the repo and immediate in `dodge-the-creeps`, because nothing
in `tests/integration` exports a member typed as another script's class and `main.verse` does. That
is the yardstick doing exactly what §8 said it was for.

**One defect found and not fixed**, recorded against R-DIAG-3: after a Verse runtime error is
raised, every later `vh_instance_call` returns `VH_OK` with no result value. It predates this phase
and `@tool` makes it worse, because a tool script can now reach it without the game running.

### 11.1 What is owed: two by-hand checks

Everything else in §9 is covered by `tools/run_tests.py` and the yardstick's 29 headless checks.
These two are not, because nothing headless can make them — one needs a window and the other needs
the editor, which has no scriptable Play button. Written out rather than summarised so that whoever
does them is not re-deriving what to look at.

**1. The windowed run of the yardstick.** Owed since Phase 2, which closed on 29 headless checks and
never saw the game drawn.

    godot --path dodge-the-creeps

Watch for: the player moves and its animation flips with direction; mobs spawn along the path and
leave the screen; touching one ends the round and the HUD says so; the Start button restarts it. All
of that is asserted headless — what a window adds is whether it *looks* right, which is the part a
Timer and an assertion cannot see.

**2. An editor session.** `godot --path demo --editor`, or `dodge-the-creeps`, and in one session,
without restarting:

| do this | expect |
| --- | --- |
| press Play | the game runs the current source. `VerseEditorPlugin::_build` published a generation first |
| edit a method body, save, press Play again | the **edited** code runs. A save alone does not build, so the running code between the save and the Play is the old one — that is the design, not a bug |
| break a script, press Play | the run is **refused**, the compiler's diagnostics are in the log, and the previously-working code is still what a re-Play would run once the break is fixed |
| add a `.verse` file, attach it to a node, press Play | it runs, with no restart (R-ITER-2) |
| rename and delete a `.verse` while the editor is open | no restart needed either |
| change an `@export` default, save | the inspector does **not** change. Then `Project > Tools > Build Verse` — it does (R-ITER-3). A *newly declared* member appears on save, its default on build |
| right-click a directory in the FileSystem dock | "Make Verse Module" is in the menu, and writes `<dirname>.vmodule` |
| put two files declaring one class in one module | the collision diagnostic names both files and names the menu action |
| open a script and type a name that only another module declares | the diagnostic names the exact `using` line — and, while that file is the one on screen, the line appears at the top of the buffer by itself (R-TOOL-12) |
| put `@tool` on a script with a `Process`, build, and watch the scene | it runs in the editor (R-EXP-5). This is also the one workflow where R-DIAG-3's recorded defect bites without the game running |
