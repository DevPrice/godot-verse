# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Epic's Verse as a scripting language for Godot 4, as a GDExtension. This file is the map of the
source tree and the working rules; the reasoning lives in `docs/`.

## Read first

`README.md` is current: the design intent, a feature status table, the build prerequisites and the
licensing consequence. It is the right thing to read for *what this does*. Two documents outrank it
wherever they disagree with it:

- **`docs/spec.md`** is the requirements document — what the finished software must do, numbered so
  a commit can cite one, and carrying the **per-requirement status of what it does today**. Check a
  requirement's status before assuming a gap is unexamined. §14 holds the open questions, with the
  answer written into the row when one closes.
- **`docs/abi-v2-design.md`** is the wire: what godot-cpp does, why the containers are references,
  and the four spikes that settled the shape — the fixed-width `variant`, whether VNI marshals a
  native struct, whether a dropped Verse value releases anything, how often a nullability rule would
  be wrong. Read it before changing `variant`'s lanes or proposing a different encoding.

`docs/roadmap.md` sequences the requirements into phases and says which phase the work in front of
you belongs to. `docs/phase-0-spikes.md` is why three of the spec's answers read the way they do —
read it before re-deriving anything about hot reload, the export pipeline, or the flat scope.
`docs/property-export.md` and `docs/editor-tooling.md` hold the research behind `@export` and behind
the debugger/LSP story. **`property-export.md`'s last section is the one to read before touching a
class-typed export**: what GDScript's inner classes do, measured on all three planes they live on
(typed in script, anonymous to the engine, *lost on save*), why a second class in a file can never
register a Godot class name, and the staged plan that follows from it. **All of it is settled**:
A and B are built and green, and C is answered as C1 — a class to be authored as a `.tres` or
persisted in one lives in its own `.verse`, because what carries a value across a save is the
script and only the file-named class can be one. C2 (`res://x.verse::second`) was spiked and is
**dead**: `::` is how Godot spells "internal to a file", so such a path loads and can never be
referenced.

### The phase documents, and the one section of each to read

**Every phase design document ends with a section written *after* the work, which is where the
design turned out to be wrong.** Read that section before trusting the body — several of them
contradict their own §1. The bodies are still worth reading for *why* a shape was chosen; they are
not a description of what exists.

| document | the section that corrects it | what the phase left standing |
| --- | --- | --- |
| `phase-2-design.md` | §11 | the whole mirror: 1036 classes, 793 enums, `vh_object` as the native root |
| `phase-3-design.md` | §11 | generations; §1.1 has OQ-12's answer |
| `phase-4-design.md` | §13, and **`phase-4-gaps.md`** (21 entries, all closed) | virtuals, signals, `@GlobalScope`, the math types |
| `phase-4b-design.md` | §15, which corrects §3 outright (it describes work Phase 2 had already shipped) and half of §4.2 | R-NODE-3: `helper{}` is a live Godot object. §5–§9 are still unbuilt |
| `phase-4.5-design.md` | §11 | `<reads>` from Godot's `is_const`; what a failure undoes |
| `phase-5-design.md` | §14 (and §2, twelve compiler answers from `tests/verse_probe`) | task scopes, `Await`, `Sleep`, `spawn` |
| `phase-6-design.md` | §13 | the step debugger and the profiler |
| `phase-7-design.md` | §13 (§14 not built, §15 exit) | the three UBT targets, the cooker, the export plugin |
| `phase-7b-design.md` | §13 (§14, §15) | an exported game runs its Verse |
| `phase-7.5-design.md` | §14 | `vm/`, the clean-room interpreter; Verse on the web |

Three documents are not phase records and are the ones to read before adding a feature:

- **`docs/signal-declaration.md`** — why a script-declared signal is moving from a `signal(t)` member
  to `@export_signal` over an ordinary `event(t)`, what that cannot change (the emit verb stays the
  bridge's: `signalable.Signal` is `no_rollback` and Godot has to be the dispatcher), and the staged
  plan. §4 is the measured table, including the three refusals; §7 is why the 503 engine accessors
  **stay** on `signal(t)` rather than moving with it; §10 is the five `IsSignalClass` call sites and
  which three move. Read it before touching anything that tests for a signal type.

- **`docs/dodge-the-creeps.md`** — the eight things a Godot author writes without thinking, each
  measured in a real game rather than estimated, with the requirement that gives it a spelling.
  Seven of the eight are down; the table says which, and §"After Phase 4"/§"After Phase 5" say what
  each diff came to. The one standing is the `<transacts>` trap (wall 8), narrowed twice and not
  removed.
- **`docs/by-hand-findings.md`** — what the by-hand editor sessions found, because everything from
  `EngineDebugger` and the editor UI inward has no automated test and never will. B1–B9, B15–B18,
  B20, B22–B35, B37 and B39–B42 are defects, all fixed, and B36 is reported rather than closed; B12 is a Verse fact; B13 a latency finding; B14 the
  sandboxed export run. **B30 is the one to read before calling `ResourceLoader` from anything a
  resource load can reach**: Godot answers a cyclic load `ERR_BUSY` and a null `Ref` silently, so
  the only thing printed is the asking side's own sentence — which names the resource *asked for*
  and never the one it collided with, and reads as that resource being broken. A `.verse` load
  builds the project, a build generates the bindings, and generating them loads every `class_name`
  script, so a GDScript naming a Verse class reaches itself.
  **B38 is the one to read before touching where a script's documentation is registered, the
  name it is registered under, or what counts as the comment above a declaration**: Godot draws a
  script-class member's tooltip from the *registered* script doc and from nothing else — the
  lookup result's `description` is read for the two local results alone — and `EditorHelp` queues
  a doc registered before its own regeneration has finished and discards that queue when the
  regeneration starts, so B20's once-per-re-arm pass could land and be thrown away. A lookup that
  names a script class registers that script's doc first (`ensure_script_doc_published`), the
  name is module-qualified on both sides (`left/widget`, which `OwnerNameOf` now answers for a
  class scope), and the comment reader lexes because two of Verse's three comment forms span
  lines. Verse has no doc-comment syntax: the comment above a declaration is the documentation,
  which is Epic's own convention, and a script may write `@doc("...")` with
  `using { /Verse.org/Native }`.
  **B39 is the one to read before touching what a placeholder answers for an export it cannot
  currently evaluate**: a default lives nowhere but in generated code, so a failed or pending
  build cannot read one, and answering `_get_property_default_value` with a bare null let the
  scene saver decide the live value differed from the default and write `Speed = null` over it —
  for an *inherited* export, which B26 did not cover. The placeholder is frozen in its last good
  configuration now: `last_good_defaults` caches each default read successfully and is served
  when the build cannot, and the fallback is left *off* once a build has ever succeeded
  (`had_successful_exports`) rather than switched on by any diagnostic, so a known member never
  reads null. It also closes B26's step 3, where a break in a *different* file dropped the default.
  **B40 is the one to read before touching how a Godot-package function hovers**: an extension
  method on a Verse type Godot has no page for — `event(t).Emit`, `signal_ref.Subscribe`,
  `variant.AsInt` — hovered as a "Local Constant" whose type was the whole function type, because
  Godot's only prose-carrying results without a registered doc are the two locals. A page is
  registered on demand instead: `EditorHelp` is not exposed, so the one door is
  `ScriptEditor::update_docs_from_script`, fed by a hidden carrier script (`api_doc_carrier`, kept
  out of `live_scripts`) whose documentation is a page per receiver a hover has asked about
  (`publish_api_method`). The lookup answers `CLASS_METHOD` under the receiver's own name whether or
  not an editor is present, so it is testable headless; the drawing is by hand. The method's
  arguments and return come from `verse_signature`, and the declared type carries no parameter
  names, so the drawn signature is `Emit(: t) -> void` until the host carries a spelled signature.
  **B41 is the one to read before touching where a parameter's documentation comes from**: a
  parameter's comment — a `#` line above it or an inline `<# doc #>` before it — did not reach its
  hover, because the consumer re-reads the source by line and a parameter's line is the one its
  function opens on, so "the comment above it" is the function's. The host reads it instead
  (`DocOf` on the parameter's own VST node, filled by `LookupSymbol`), and the hover draws the
  host's `doc` for a parameter while leaving the line-based path for everything else. Both forms
  the parser keeps on the parameter node are covered, the first parameter included. Measured in
  `tests/integration` (`hover_probe.verse`'s `Marks`), not by hand, because the host is loaded in a
  headless run.
  **B37 is the one to read before declaring a Godot parameter anywhere**: Verse has no null and
  a class has no value for one, so an object argument is declared `?class` unless Godot's own
  dump marks it `"meta": "required"` (godotengine/godot#86079) — 112 of the mirror's 1020 are
  marked, and they are the ones written most, so `AddChild(Child)` is unchanged and the
  yardstick needed no edit. A plain value does **not** coerce to an option, so the rest cost
  their callers an `option{}`; options *are* covariant, so one `VhFromMaybeObject(?object)`
  packs every one. A generated binding has no metadata in either source, so all of its object
  arguments are optional. The same metadata reads the other way for a *result*:
  `RequiredResult<T>` takes `<decides>` off 40 of the mirror's 759 object returns, so the Tween
  chain and `SceneTree.GetRoot` are ordinary calls. **A virtual reads both halves**, because
  Godot's declaration is a promise the override keeps — which is what finally gave the 43
  object-returning virtuals a default body and took them off the skip list: 41 are `?class`
  defaulting to `false`, and the 2 Godot marks keep the class and default to `Err`. A signal's
  payload is still not spelled for null and can carry it. The **singletons** are not this rule
  and have no metadata to read: the dump's table is a name and a type, and which two can be
  absent comes from `"api_type": "editor"`.
  **B36 is the one to read before changing when the bindings are generated**: a GDScript that
  names a Verse class is held back during a `.verse` load to avoid B30's cycle, and its binding
  is then declared with no members — so a Verse file naming one of its *methods* fails to
  compile, the script never becomes valid, and the node the scene meant to give it to silently
  gets nothing. It is not a first-build state: such a script names the class on every load. A
  generation taken during a load no longer unsays what a complete one said, which covers a
  session that has already built and not a cold start, and the withheld build now says all of
  this rather than leaving GDScript's “on a base object of type 'Nil'” as the only sentence.
  **B35 is the one to read before touching what a generated binding can name**: a method the
  generator cannot type is left out of the binding, so a GDScript method taking its own
  `class_name` was simply absent — and one *answering* a class the mirror carries was worse,
  emitted with a declared result over a `variant` body, which refuses the whole bindings
  package and with it every Verse script in the project. A binding may name a binding: Verse
  resolves module-scope definitions in any order, so the roster is collected before anything
  is typed, and an object result is `<decides>` over `AsObject[]` and a downcast because a
  class has no value standing for “Godot answered nothing”.
  **B34 is the one to read before touching how an argument reaches the VM**: `WireToValue`
  decided which class a declaration named *before* looking at whether a value had arrived, so
  an optional parameter of any class but the mirror's refused Godot's own null — the one value
  every object slot can hold. Null is answered first now, and the class lookup behind it is
  per package (`FindMirroredClass`, `FindGodotClass`, `FindBindingClass`), which is the rule
  `WriteInstanceFieldInstance` already followed for a member.
  **B31 is the one to read before drawing a type's own name anywhere**: a type has no
  type to spell, so `vh_lookup_desc::TypeUtf8` is empty for one, and the kind cannot stand in
  for it either — a class, a struct and an interface all arrive as `VH_LOOKUP_CLASS`. The word
  comes from the declaration, which is what `verse_scan_type_keyword` reads.
  **B27 is the one to read before adding a completion option that inserts
  anything but a bare name**: Godot re-asks for completion after confirming one only when the
  inserted text's last character is in `code_completion_prefixes`, and that re-ask is the whole of
  how the argument hint appears — so every `<decides>` call, inserting a `[`, silently got none.
  Its second half is why `request_check` keeps a slot per kind: one slot let `_validate` displace
  the analysis a hint was waiting on, and only a completion analysis re-asks. **B28 is the one to
  read before walking a class ancestry from a mirrored name**: `verse_api::classes` carries the
  sixteen math types and `rid` beside the 1036 classes, ClassDB has heard of none of them, and
  `godot_classdb_class_for` is the test every such walk has to make first. **B26 is the one to read before touching `_reload`, the placeholder path or
  anything a script answers about a default**: a placeholder's `values` map is the only copy a
  non-tool script's exported values have in the editor, Godot refuses to store one for a name
  `_has_property_default_value` says no to, and B8's re-attach destroyed both — so a save during a
  failed compile emptied the node it was meant to carry across. **B22 is the one to read before adding anything to the native root**: a hook
  with no row in `LIFECYCLE_METHODS` is never offered as an override and never hovers, and four of
  the five sat that way for a phase because only `_Notification` had a row. **B23 is the one to read
  before adding a type to the mirror**: the editor learns what a type *is* from three generated
  tables, and a public type with a row in none of them is drawn as plain text, offered by no
  completion and hovered as a local constant — which `variant` was for a phase. **B24 is the one to
  read before adding anything to the generated mirror that a script cannot write**: a fifth of every
  completion popup was the mirror's own class var accessors, 7344 names no author can spell, because
  `DescribeCompletion` describes any function that fits the filter and the fact that separates them
  was already in hand for a narrower purpose. **B25 is the one to read before asking the compiler
  anything about where a definition was written**: Verse's own library documents itself with a
  `@doc` attribute rather than a comment, a parametric class is a `CFunction` so a walk that stops
  at functions loses every member of one, and an instantiated definition is not the one that was
  written — `PrototypeOf` is the answer to the last. **B21 is the one
  open defect**: asking Godot for the `IP` singleton — which the
  generated accessor does whether or not it then succeeds — segfaults the process *after* everything
  has shut down, so it reads as a whole suite failing with nothing in the log. **B20 is the one to
  read before touching `_get_documentation`**: Godot asks for a
  script's documentation once per session and off the game thread, where every ABI read is refused.
  Its "What is still open" section is where the remaining by-hand checks live.

**`docs/gdscript-conversion.md`** is "Convert to Verse" (R-TOOL-13): the requirements as the owner
settled them, every GDScript construct and what it becomes, and the five Verse spellings the
converter writes that no fixture here has compiled. Read it before changing what the converter
emits — and note that its editor half has never been run.

`docs/nonatomic-methods.md` is generated — R-AUD-3's list of the 1132 emitted methods whose
`<transacts>` promises a rollback the bridge cannot perform.

## Two binaries, one C header

    verse_host.dll    host/       UBT + AutoRTFM clang, monolithic UE Program target.
                                  Boots FEngineLoop, owns VerseVM, compiles and runs .verse.
    godot_verse.dll   src/        SCons + MSVC against godot-cpp.
                                  Loads the host, feeds it Godot callbacks, pumps it per frame.

`include/verse_host_abi.h` is the only thing that crosses. Plain C — the two sides cannot share a
C++ ABI. It is staged into the host's `Public/` by `build_host.py`, so both compile the same file.

**`VH_ABI_VERSION` is 12.1.** It is `MAJOR * 1000 + MINOR`, with the policy at the top of the header:
a major bump is a layout or meaning change and both sides must be rebuilt; a minor bump adds
something an older consumer can ignore behind a `StructSize` check. A change to the header means
bumping it and rebuilding **both** sides — the mismatch surfaces at `vh_init`, not at compile time.
**A callback added at a minor must be cleared past the consumer's own `StructSize`**: `vh_init`
copies the whole `vh_godot_api` out of the descriptor, so everything past what a consumer built at a
lower minor actually wrote is that consumer's stack, not a null pointer, and "check the pointer
before calling" would pass. `InitHost` zeroes the tail; nothing before 8.3 needed it.
**`vh_complete_item` is the struct a minor can never grow**: the items are handed back as an array,
so a field at the end changes the stride an older consumer indexes by, and the mismatch would read
as corruption rather than as a refusal. 9.0 added `IsNamed` there for that reason alone.
**10.0 changed no layout**: it is a major because `vh_compile_project` stopped leaving an
analysis-only program behind it, so the three position entry points answer `VH_ERR_STATE` after
a build until a consumer asks for an analysis. An older consumer would have read that as "no
such symbol" and drawn nothing, silently.
**12.1 is where a host fatal error is recorded**: `vh_init_desc` grew `FatalLogPathUtf8` and
`ShowFatalDialog` (`by-hand-findings.md` B43).
**12.0 is what a parameter says about the class it declares**: `vh_param_desc` grew
`ClassUtf8` and a `ClassKind` beside it, and `vh_method_desc` the same pair for its result, so
a consumer can tell Godot that `Fire(Target:timer)` takes a **Timer** rather than an Object.
A signal argument is a `vh_param_desc` and came along with it. It had to be a major because
**both of those structs are handed over as arrays**, which makes them the third and fourth
whose stride a minor can never change. A script class the consumer has not registered is
reported as its nearest mirrored ancestor rather than by a name nothing can resolve — the rule
`vh_export_desc::NativeClassUtf8` already followed, and GDScript's own.
**11.1 is generated bindings' half of the wire**: `vh_set_bindings`, the `vh_binding_class`
row and a `GetScriptClassOf` callback appended to `vh_godot_api`. A minor, because a consumer
that never calls it is unaffected — but `vh_binding_class` is the *second* struct a minor can
never grow, for `vh_complete_item`'s reason exactly: the rows are handed over as an array, so a
field appended at the end changes the stride the host indexes by and the mismatch reads as
corruption rather than as a refusal.
**11.0 grew `vh_lookup_desc`**, which carries a definition's documentation now (`DocUtf8`) — and it
had to be a major because that struct had no `StructSize`, so there was nothing a consumer could
check before reading a field appended after the version it was built for. The size field went in
with it, so the next addition there can be a minor.

`vh_host_kind()` is readable before `vh_init` and answers editor, runtime or cooker; the eleven
compiler-side entry points answer `VH_ERR_UNSUPPORTED` in a runtime host.

### `src/` — the GDExtension

| file | owns |
| --- | --- |
| `register_types.cpp` | registration order: language before resource loader |
| `verse_host.{h,cpp}` | `GetProcAddress` loader over the ABI; no Verse logic |
| `verse_host_paths.{h,cpp}` | where this machine's Unreal checkout, host DLL and cooker are: environment, then EditorSettings, then the legacy project settings (R-DIST-12) |
| `verse_runtime.{h,cpp}` | the `VerseRuntime` singleton — `vh_init_desc`, the Godot callback table, `verse/host/enable_debugger`, and finding `verse_data` in an export |
| `verse_value.{h,cpp}` | `Variant` ⇄ `vh_value`, arena-allocated. `variant_type_for` is where a declared type becomes a Godot one, and `VH_TYPE_VARIANT` → `Variant::NIL` is only half an answer: the descriptions pair it with `PROPERTY_USAGE_NIL_IS_VARIANT`, without which NIL means "must be null" and Godot refuses the call before the VM sees it |
| `verse_ref_table.{h,cpp}` | the id → `Variant` table the `Ref` lane names: Array, Dictionary, Callable, Signal and the packed arrays, which cross as references rather than copies |
| `verse_callable.{h,cpp}` | the mirror image: a Godot `Callable` that calls a Verse function. Only a function **bound to a script instance** is accepted, which is the half of Godot's own design that does not leak (GH-102327) |
| `verse_script.{h,cpp}` | a `.verse` file as a Godot `Resource`; valid only if it defines its own class |
| `verse_script_instance.{h,cpp}` | one script bound to one node; raw `GDExtensionScriptInstanceInfo3` vtable, not a `godot::Object` |
| `verse_script_language.{h,cpp}` | the `ScriptLanguage`: `_validate`, the analysis cache, `_complete_code`/`_lookup_code`, `_frame` (which pumps `vh_tick`, reaps `vh_check_project_poll` and attaches the debugger), and the `_debug_*`/`_profiling_*` surface |
| `verse_resource_format.{h,cpp}` | load/save, without which a `.verse` cannot be attached to a node |
| `verse_lexer.{h,cpp}` | resumable per-line lexer, and `verse_repair_completion_buffer` — which finishes off the caret's line so a half-written `if` does not cost the whole file its AST. No godot-cpp dependency, so both are unit-testable standalone |
| `verse_class_decl.{h,cpp}` | scans the top-level class **named after the file** and its `@global_class` attribute out of the text; defers comments and strings to the lexer, and shares its lack of godot-cpp |
| `verse_module_map.{h,cpp}` | which module each `.verse` is in, from the `.vmodule` markers; pure, and the third godot-cpp-free unit |
| `verse_bindings.{h,cpp}` | the naming, the classification and the emission for generated bindings (R-INT-7, R-INT-9) — `split_pascal` and the predicate rule are `gen_verse_api.py`'s exactly, and the enum naming is too. The fourth godot-cpp-free unit |
| `verse_doc_markup.{h,cpp}` | which comment documents a declaration (`verse_doc_comment_above`, over the lexer: `#` lines, a `<# #>` block dedented, a `<#>` body, attribute lines stepped over, a blank line or code ending the walk) and the prose as the doc BBCode Godot's renderer reads: GDScript's paragraph join, backticks to `[code]`, an indented or fenced block to `[codeblock lang=verse]`, every other `[` escaped. Applied wherever a description is handed to Godot — `_lookup_code` and `_get_documentation` — and never before, so the two readers keep one shape; the host's `DocOf` reads the parser's comment nodes by the same rules. The fifth godot-cpp-free unit, linked with the lexer |
| `verse_signature.{h,cpp}` | splits a Verse function signature (`vh_complete_item::Signature`, the host's `SpellSignature`) into arguments, effect specifiers and result type, all at the top level so a type's own brackets are not separators. `_get_documentation` builds a `MethodDoc` from it, because Godot draws `Name(arg: type) -> return` from the three apart — handing the whole function type over as `return_type` drew no arguments. The sixth godot-cpp-free unit |
| `verse_bindings_gen.{h,cpp}` | the half that needs Godot: which classes exist. **Only `API_EXTENSION`** of ClassDB, plus every script class with a `class_name` — the editor's ClassDB carries every editor-only class and binding those emitted 1677 lines of Verse for classes no game has |
| `verse_export_plugin.{h,cpp}` | editor-only: runs `verse_cook.exe` over the project, strips every `.verse` to a one-byte stub so `ext_resource path=` still resolves, and refuses a platform this bridge does not reach |
| `verse_export_paths.{h,cpp}` | the one rule for where a game's cooked Verse lives — `verse_data` beside the executable — shared by the export plugin that creates it and the runtime that finds it |
| `verse_module_menu.{h,cpp}` | editor-only: "Make Verse Module" in the FileSystem dock, because Godot's dock cannot create an empty file |
| `verse_gd_syntax.{h,cpp}` | GDScript's lexer and parser, for the converter: GDExtension cannot reach Godot's own. The seventh godot-cpp-free unit |
| `verse_gd_convert.{h,cpp}`, `verse_gd_resources.cpp` | "Convert to Verse" (R-TOOL-13): GDScript to Verse, a batch in rounds, the `.tscn`/`.tres` rewrite and the caller rewrite. Pure; every mirror spelling comes from `verse_gd_api.gen.h`. Built into the editor library only (SConstruct) |
| `verse_convert_menu.{h,cpp}` | editor-only: the menu item in the dock and the script list, reading the project, one undoable file-set action, and the callers dialog |
| `verse_syntax_highlighter.*`, `verse_editor_plugin.*` | editor-only (`TOOLS_ENABLED`) |

`VerseScriptLanguage` overrides only the virtuals it actually answers — godot-cpp binds a virtual
with Godot only when the subclass declares it, so **omitting one is how you say "unsupported."**
Adding an override you do not implement changes behaviour.

### `host/` — three UBT targets over one `Private/`

| target | produces | for |
| --- | --- | --- |
| `VerseHost.Target.cs` | `verse_host.dll` | the editor: compiles, analyses, runs |
| `VerseHostRuntime.Target.cs` | `verse_host_runtime.dll` | what an exported game ships; `WITH_VERSE_COMPILER=0` |
| `VerseHostCooker.Target.cs` | `verse_cook.exe` | an **executable** the export plugin runs as a subprocess |

`Private/` is the ABI implementation. `VerseHost.cpp` is the entry surface; `HostRuntime`,
`HostScript` and `HostEventLoop` are compile/analyse/run, class shape, and the task pump;
`HostDebug` is the `Verse::FDebugger` and the profiler's accumulators, and nothing else in the host
knows either exists; `HostFatal` records a fatal error before the process ends; `GodotBindings`
and `GodotClasses` are the native Verse surface. The cooked
path is `HostCook`/`HostCookWriter` (cooker only, behind `VH_HOST_KIND == VH_HOST_KIND_COOKER`),
`CookMain.cpp` (the cooker's `main`), `HostCooked` (mount points and load, in the runtime host) and
`HostSidecar` (the analysis snapshot serialised, which is what a host with no semantic program reads
instead of sources). `Verse/*.native.verse` is the `/Godot.org/Godot` package.

`GodotClasses.h` holds every C++ shadow a `<native>` Verse declaration needs, and there are three:
`vh_object` (a UObject, so a script's class has one to be instantiated and called through), `variant`
(a struct, the fixed-width lanes one Godot value crosses as) and `godot_ref` (a UObject whose
`BeginDestroy` is what releases a reference id when Verse drops the value holding it).

**Those three are also the only types a native may name.** VNI refuses anything else at build time
— *"V3564: `class engine used as a parameter/result in a native function must also be native"* — so
a native can never answer a *mirrored* class, only `vh_object` for a cast to narrow. That is why a
singleton accessor is a cast rather than a call, and why the 39 that cannot fail have to raise
through `Err` to be total rather than simply dropping `<decides>` (spec R-TYPE-4). It is the first
thing to check against any plan that would have the host answer a typed object.

**A `variant` crosses the script-call wire as `VH_TYPE_VARIANT` (ABI 8.6), which is a *declaration*
type and never a payload.** A `vh_value` still carries whatever the variant holds; the type only
tells the consumer "this argument or result accepts anything". Two traps sit under it. The
conversion belongs to `GodotBindings.cpp` and is exported from there (`VariantFromWire`,
`VariantToWire`) — never write a second one, because the lane rules for the 16 math types have to
agree exactly in both directions. And `variant` is a **struct**, so leaving it out of one
classification in `HostScript.cpp`'s `DescribeType` silently drops it into another: as a user struct
it asks Godot for 22 arguments, one per lane, and as neither that nor a variant it becomes a
*reference* and is refused as a handle to a class Godot has never heard of. Both say the same
useless sentence, *"Cannot convert argument 2 from int to Nil"*.

**`rid` is the second instance of that trap and it cost the same afternoon.** It is a struct of one
int, so it is the likeliest of all of them to pass for a project's own: as a user struct a method
answering one handed Godot a one-field tuple, and once it was taken out of `UserStructClass` without
being claimed in `DescribeType` it fell into the reference arm and produced *"Cannot convert argument
2 from RID to RID"*. **A struct the mirror declares must be claimed in `DescribeExportType`,
`DescribeType` and `UserStructClass` together, or two of the three will quietly disagree.** `rid`
also needs its own arms in `ValueToWire` and `WireToValue`, because it is the one mirrored struct
that crosses as a *scalar* — `VH_TYPE_INT` under `VH_VARIANT_RID` — rather than as components.

`vh_object`, not `object`: `object` is the generated mirror of Godot's own `Object` class and derives
from `vh_object`. Verse cannot reopen a class, so Object's methods could not be added to the
hand-written root. Nothing a script writes should name `vh_object`. A native Verse type without its
shadow is an "incomplete type" build failure naming the generated header.

`VerseHost.Build.cs` and the `.Target.cs` files carry load-bearing comments —
`SetupVerse(..., InternalUser)` and the `VerseSimulationMetadata` dependency each exist for a reason
spelled out inline.

**Read `phase-7-design.md` §2 S-1 before touching the cooker target.** Seventeen builds are recorded
there, and the facts that decided its shape are all counter-intuitive: an editor-class Program passes
UHT only when it *also* compiles against Engine (`Solaris.Build.cs` drags Engine in under
`bBuildEditor` whatever the host lists, and `WITH_ENGINE=0` with Engine's headers in the manifest
loses `UWorld`); it must be an **executable**, because a monolithic editor-class DLL exports 143,570
symbols against lld-link's 65,535; it needs developer tools, because `PreInit` constructs the shader
compiling manager unconditionally; and it boots only with the `EDITOR` token, `-nullrhi` and
`-NoShaderCompile`, from an entry point marked `AUTORTFM_DISABLE`. It is ~730 MB with no PDB and
boots in 2.4 s. The cook flushes and hard-exits with the status it chose; `GEngineLoop.Exit()` is
never called, because teardown segfaults past where a cook has already written.

**The runtime host cannot be made smaller by dropping modules.** Solaris lists `VerseCompiler` and
`VerseVMCodeGen` in its own public dependencies unconditionally, so a game ships a Verse compiler it
can never reach (`phase-7-design.md` §13.2). What helps is Shipping — 72.7 MB against Development's
112.4 — and an export still ships the Development host, because the `.gdextension` names one file.

### `vm/` — the second execution path (Phase 7.5)

A clean-room interpreter of Epic's VerseVM bytecode, which is how Verse runs on the web, where the UE
host cannot (it asserts 64-bit pointers; `verse-on-web.md`). `phase-7.5-design.md` is the design and
its §14 the record; `docs/web-vm/` holds the spec, the container format, the task list and the
clean-room log. **The wall is real: `vm/` was written without reading VerseVM's source, from the
reviewed files of `docs/web-vm/spec/` alone. Keep it that way — do not bring VerseVM knowledge into
`vm/`, and route a question the spec cannot answer to a spec change** (`phase-7.5-design.md` §3).

- The cooker writes `program.vbc` beside the sidecar (`host/Private/HostVbcWriter.*`, the encoder
  generated from `docs/web-vm/ops.json`). It is a snapshot **after** initialization, so a loader
  runs no Verse (`web-vm/spec/modules.md` §2).
- `vm/` is godot-cpp-free and implements the runtime `vh_*` subset itself, so it builds two ways:
  `bin/verse_vm.dll`, a drop-in for `verse_host_runtime.dll`, and statically into the GDExtension
  (`scons verse_vm=yes`; a Windows release build and every web build carry it). `src/` fills
  `VerseHostLibrary` from it with `load_static` and hands it a file reader over `FileAccess`.
- **`verse/runtime/backend`** (`host` or `vm`) picks it in an exported game; an editor session always
  uses the host. Its `.web` feature override defaults to `vm`, the way Godot defaults
  `rendering_method.web`, and a Web export or run that resolves it to `host` is refused. **Read it
  through an override** — `get_setting_with_override` in the game, `EditorExportPreset::
  get_project_setting` in the export plugin, which is what resolves a Web preset's features from a
  Windows editor — or `.web` is invisible. On the vm backend the export ships no UE binary, and on Web
  `verse_data` lives inside the `.pck`.
- Frames are on the heap, so a Verse call never recurses in C++, but **there is no scheduler**:
  whoever makes a task runnable runs it on its own stack, in `web-vm/spec/tasks.md` §4.3's order.
- The collector is precise, never runs inside an entry, and treats the loaded program as a
  permanent generation (`Heap::tenure`), so a pause costs what it frees.
- **`docs/vm-performance.md` is its speed**: the instruments (`tools/run_vm_bench.py`,
  `cooked_probe --bench --sample`, the `verse/verse_ms` and `verse/godot_ms` monitors), what they
  measured, and the ranked list of what to change. After §4's first three rows it runs Verse faster
  than the UE host on fourteen of fifteen workloads, and entering it costs a fifth as much. The
  dispatch loop's fast cases (`drive`) must bail before changing anything, so `execute` can run the
  whole op instead; cells come from `Heap`'s slabs, not the system heap.

## Commands

    python tools/build_host.py            # stages host/ into the UE tree, runs UBT
    python tools/build_host.py --target VerseHostRuntime  # the host an exported game ships
    python tools/build_host.py --target VerseHostCooker   # verse_cook.exe; not collected into bin/
    python tools/build_host.py --clean    # after a worktree build poisoned the shared staging
    scons target=editor                   # the GDExtension; target=template_release is what an export ships
    python tools/gen_verse_api.py         # regenerates the Verse mirror of Godot's API
    python tools/gen_verse_keywords.py    # src/verse_keywords.h, from the UE compiler's list
    python tools/audit_const_overrides.py # CONST_OVERRIDES, read out of a Godot source checkout
    python tools/build_smoke.py           # ABI test binary
    python tools/build_lexer_test.py      # lexer test binary
    python tools/build_class_decl_test.py # class-declaration scanner test binary
    python tools/build_module_map_test.py # module-map test binary
    python tools/build_doc_markup_test.py # doc-markup converter test binary
    python tools/build_signature_test.py  # signature parser test binary
    python tools/build_gd_convert_test.py # GDScript converter test binary (MSVC, or g++/clang elsewhere)
    python tools/build_bench.py           # host benchmark (timings, not pass/fail)
    python tools/build_verse_probe.py     # the Verse probe (asks the compiler a question)
    python tools/build_cooked_probe.py    # the cooked probe (asks a runtime host what an export sees)
    python tools/build_verse_vm.py        # bin/verse_vm.dll, the interpreter; --wasm compiles vm/ with em++
    python tools/build_vm_test.py         # vm/'s unit tests; --release builds the /O2 bench binary
    python tools/run_vm_conformance.py    # vm/ against recorded UE-host transcripts; --record, --gc-stress
    python tools/vbc_dump.py <program.vbc> # read a .vbc; --check, --proc, --class
    python tools/gen_vbc_ops.py           # validate docs/web-vm/ops.json; --digest, --emit-cpp
    python tools/emsdk_env.py -- scons platform=web arch=wasm32 threads=no target=template_release
    python tools/emsdk_env.py -- scons platform=web arch=wasm32 threads=yes target=template_release
    python tools/run_dtc_web.py           # dodge-the-creeps on the interpreter in headless Chrome; --threads
    python tools/run_dtc_frames.py        # its frame times, exported for Windows on each backend
    python tools/run_vm_bench.py          # vm/ against the UE host on one cook; --wasm, --wasm-profile

`tools/build_host.py` needs a UE source checkout with the Verse toolchain — `--engine`, or
`UE_ROOT`. Building the host and running the tests are fine to do unprompted, and so is **headless**
Godot. **Ask before launching the editor** (`godot --path demo` with no `--headless`), which opens a
window.

**godot-cpp's API dump is pinned in `SConstruct`** (`api_version` = 4.7), and `gdextension.py`
derives both the built library's name and the `.gdextension`'s `[libraries]` rows from the same
logic, so nothing is declared that is not on disk. Bumping Godot means bumping `api_version`, the
submodule if the dump is not in it yet, `compatibility_minimum` in `godot-verse.gdextension.in`, and
then regenerating the mirror.

`tools/run_verse_lsp.py` exists to say clearly that there is no Verse language server binary to
launch: uLangLSP is a message-type library and nothing in the UE checkout links it into an
executable. If a future engine drop provides one, that script finds and execs it.

### Tests

    python tools/run_tests.py                    # every layer; the one command (R-QUAL-3)
    python tools/run_tests.py --only units       # or units / abi / integration / export / web / web-threads
    python tools/run_tests.py --build            # rebuild the test binaries first

**units** — lexer, class-declaration scanner, module map, doc-markup converter, signature parser,
the GDScript converter, generator, and `vm/`'s own cases (`verse_vm_test`). No Godot, no UE. The
converter's goldens are whole files (`tests/verse_gd_convert/fixtures/*.expected`);
`bin/verse_gd_convert_test.exe --update` rewrites them, and the diff is the review.

**abi** — `host_smoke`, the whole C ABI with no Godot, plus a `verse_cook` case that cooks
`tests/host_smoke`'s fixtures and asserts the packages, the container, the sidecar and the
`program.vbc` (read by the clean-room `tools/vbc_dump.py`, which shares no code with the writer),
and a runtime-host case that runs `task(t)` methods and a raise through `cooked_probe`.

**The interpreter's differential harness is not a layer.** `tools/run_vm_conformance.py` runs
`tests/cooked_probe` over `tests/vm_conformance`'s fixtures against `bin/verse_vm.dll` and diffs the
transcripts recorded from the UE runtime host (`--record`); `--gc-stress` collects after every
entry. It needs no UE checkout unless recording.

**integration** — three headless Godot projects. `tests/integration` for behaviour;
`tests/coverage_diagnostic` for the R-SCN-2 diagnostics, which is its own project because its one
script deliberately does not compile and one unresolvable name in the first would take every other
case down with it; and `tests/binding_cycle` for B30, where a GDScript with a `class_name` also
names a Verse class and the binding generator is asked for the script Godot is already loading.
That one is its own project because the cycle fires during startup, before any case could run. All
three projects' assertions live in `run_tests.py` rather than in the project,
because `ScriptLanguage` exposes nothing a script can ask — the only way to read what an author
would see is to read what the editor prints. **`binding_cycle`'s assertions are refutations**
(`refute_all`), because what a cyclic load and a premature diagnostic produce is printed output and
nothing else — a working run says nothing at all, so `require_line` has to be paired with them or a
run that died early would pass every one.

**A `_validate` warning is not something the editor prints**, which is the trap in that sentence: it
is returned to the editor's own C++ for the gutter and the warnings panel, and reaches no log, so no
headless run can assert one. What a test *can* read is a compile error and a `push_warning`. So a
diagnostic that has to be both seen at a line and asserted needs **two reporters over one message**,
and there are two worked examples: `inert_global_class_message`, which is one sentence written
twice, and `log_script_warnings`, which is the general form — it re-runs
`refresh_script_warnings` once per build and pushes everything the gutter would have drawn, so the
export rejections (R-EXP-2), the signal rejections (R-SIG-1) and B19 Stage C's "cannot be saved" are
assertable in the integration layer. It refreshes the map before reading it, because a session that
has only built has never called `_validate` and the map is empty. **The gutter itself is still
by-hand** — the build copy proves the sentence and the line, not that the editor draws either.

**export-vm** and **web** are the same `tests/integration` on the interpreter, each from a
throwaway copy of the project (a committed `project.godot` is never touched; `override.cfg` is
ignored while exporting). export-vm's copy sets `verse/runtime/backend="vm"`, is exported for
Windows and asserted at the host backend's own 519/0/11. web's copy sets **nothing**, so it proves
the `.web` override's default; it is exported for Web, run in headless Chrome through
`tools/run_web.py` and asserted at 517/0/13 — R-ASYNC-8's two thread cases skip in a build without
threads — and then exported once more with `backend.web="host"` to assert the refusal.
**web-threads** is the web layer's own code with the threads library (`godot-verse.wasm`, built
`threads=yes`), `variant/thread_support=true` and the `web_dlink_release` template, served with
`run_web.py --coop-coep` — without the headers the page is not cross-origin isolated and cannot
start a worker. It is asserted at the export layer's full 519/0/11, because R-ASYNC-8's two cases
run there and the off-thread call has to be refused; the refusal export is the web layer's alone.
Each layer stages only its own library, so the copy's `.gdextension` has one web row. **A Web export's page
passes the engine no command line**, so `run_web.py --godot-arg` rewrites its `GODOT_CONFIG`; without
it the test driver's `--verse-check` gate never opens and the game sits idle, which reads as a hang.

**export** — exports `tests/integration` headless, asserts the *tree* it produced, then **launches
it** and asserts what its cases reported: 519 passed, 0 failed, 11 skipped, with the counts named in
`run_tests.py` so a case that stops running in an export reads as a failure rather than as a shorter
log. It is the only layer that exercises the cooked path end to end; everything else compiles at
startup. It needs more staged than the other layers do, because what it is exporting *is* them —
`godot-verse.dll` (`scons target=template_release`), `verse_host_runtime.dll`
(`build_host.py --target VerseHostRuntime`) and `tbbmalloc.dll` in `demo/addons` — and skips itself
with the reason when one is missing. One assertion reads the `.pck` directly (`read_pck`, the format is Godot's
`core/io/file_access_pack.cpp:288-370`), because the one thing that has to be asserted about a
shipped `.verse` is its *size* — a one-byte stub and the whole file both read as "Storing File" in
an export log. **A pack stores paths with `res://` trimmed off** (`editor_export_platform.cpp:449`);
`read_pck` puts it back.

A layer whose prerequisites are absent is **skipped and said to be skipped**, never counted as a
pass. `UE_ROOT` names the Unreal checkout and `GODOT` the Godot binary; both are guessed when unset.

`tests/integration` is a real Godot project, and three things in it are generated rather than
committed: `run_tests.py` copies the built GDExtension into its `addons/`, generates its
`.gdextension` from what is actually on disk — so the integration layer runs against an editor build
alone and the export layer skips itself when the release library is absent — and writes
`.godot/extension_list.cfg`, because outside the editor Godot loads extensions from that list rather
than by scanning. It writes no `verse/host/*` setting: `run_tests.py` exports `UE_ROOT` into every
Godot it launches and the extension reads that first (R-DIST-12), so nothing machine-specific lands
in a committed file. Adding a `.verse` fixture means adding it under `scripts/`; the host compiles
every `.verse` under `res://` together. Its `main.tscn` and its `VerseExportCheck` autoload are the
*exported* run's alone — `--script res://test_main.gd` replaces the main loop, so the editor-side run
loads neither, and an export with no main scene refuses to start.

**No test framework anywhere.** Each test is a `main` (or a plain script) that prints one line per
case and exits non-zero on failure; keep new tests that shape. The integration layer is the same
shape in GDScript, and split the way `dodge-the-creeps` is: **`tests/integration/test_cases.gd` is
the library** — one line per case, a `tree`, `begin()` and `step()` — and the two drivers are
`test_main.gd` (a `SceneTree`, in the editor) and `export_check.gd` (an autoload, in an export). One
set of lines, so the two runs cannot disagree about what passing means. A case that cannot run in an
export sets `editor` false and is **printed as a skip and counted**, never dropped.

**An autoload is the only way to drive an exported game**: `--script` is inside `TOOLS_ENABLED`, so
an export template has none. Both autoloads must `set_process(false)` first — declaring `_process`
is what enables it, so without that they run against a null library in every ordinary play of the
game.

The binaries still run standalone, which is what to reach for when bisecting one failure:

    bin/host_smoke.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine .
    bin/verse_lexer_test.exe
    bin/verse_class_decl_test.exe
    bin/verse_module_map_test.exe
    bin/verse_doc_markup_test.exe
    bin/verse_signature_test.exe
    bin/verse_gd_convert_test.exe
    python tests/verse_api_gen/test_gen_verse_api.py

### Instruments, which are not tests

**`tests/verse_probe`** (`tools/build_verse_probe.py`) asserts nothing. It compiles whatever
`.verse` files it is handed as one project, prints every diagnostic, and calls a class's
zero-argument methods — which makes a language question ("does a two-parameter function satisfy a
tuple-parameter callback?") something you *run* rather than something you read out of
`SemanticAnalyzer.cpp`. Six of Phase 4's design decisions and all twelve of Phase 5 §2's answers came
out of it; the eight fixtures beside `example.verse` are kept so every claim can be re-run rather
than recalled. `async_reject.verse` compiles **nothing** on purpose — it is the file of refusals, and
the *text* of each refusal is its result. Take the path as absolute; the probe resolves nothing
relative to `bin/`:

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        tests/verse_probe/example.verse --class example

Run it **from `bin/`**, where `build_host.py` leaves `tbbmalloc.dll`: without it `LoadLibrary`
answers a bare 126 and names nothing.

**`tests/cooked_probe`** (`tools/build_cooked_probe.py`) is the same kind of thing for the *cooked*
path: it mounts a cooked data directory in a runtime host and calls a class's zero-argument methods,
so what an exported game sees is ten seconds away rather than a two-minute export and a game that
dies with no output. It is what Phase 7b's second wall was found with, and what any later one should
be. Its `CallMethod` is a stub that prints and answers void, so it proves a call *reaches* Godot and
nothing about what Godot does with it.

**`tests/host_bench`** (`tools/build_bench.py`) reports timings rather than pass/fail, which is why
it is not in `run_tests.py`: R-PERF-2 asks for a recorded number and a threshold would fail on a
slower machine. Its arguments are the host DLL, the engine, **the repo root** and an iteration count
— the third is not a project path, it is where the bench finds `tests/host_smoke`'s fixtures and
`dodge-the-creeps/scripts`:

    bin/host_bench.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine . 10

**`VH_TRACE_ANALYSIS=1`** works against anything that loads the host: a per-analysis trace to
**stderr** — each package's role (Source or External), the digest bytes read, parse and semantic
milliseconds, and what the snapshot cost. stderr rather than the diagnostic callback because a
background analysis runs off the game thread and every ABI callback is the game thread's alone.

**`tools/probe_hover.py`** and **`tools/probe_complete.py`** are the editor's two answers, measured.
`_lookup_code` and `_complete_code` are both virtuals, which ClassDB stores as metadata rather than
as a callable MethodBind, so no script can reach either — `probe_hover` and `probe_complete` on
`VerseScriptLanguage` are the seams, and both tools report findings rather than passing or failing.
Neither sees what the editor *draws*: the tooltip's rendering and whether the completion popup opens
at all are Godot's own C++, and that half is `docs/by-hand-findings.md`. A completion position costs
an analysis where a hover costs none, so `probe_complete` takes a `--limit` and picks its carets.

**`dodge-the-creeps/`** is the yardstick: the whole game in Verse, with no GDScript in it but the
check drivers. `godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd`,
30 checks, one line each. `--fixed-fps` is not optional; headless, a `Timer` counts real seconds
while the loop runs flat out. It is deliberately **not** in `run_tests.py` — a yardstick that gates
the build stops measuring. `checks.gd` is the library its two drivers share, `headless_check.gd` in
the editor and `export_check.gd` as an autoload in an export.

## Generated files — never hand-edit

| generated | by | from |
| --- | --- | --- |
| `host/Verse/GodotClasses.native.verse` | `tools/gen_verse_api.py` | `godot-cpp/gdextension/extension_api.json` |
| `src/verse_api_classes.h` | `tools/gen_verse_api.py` | same |
| `host/Private/GodotMathLayout.gen.h` | `tools/gen_verse_api.py` | same — the math types' field trees, so the host builds one the way the Verse struct declares it |
| `src/verse_api_skipped.h` | `tools/gen_verse_api.py` | same — every Godot member the mirror does not carry under its own name, and why, which is what `_validate` turns into a sentence (R-SCN-2) |
| `host/Private/GodotClassNames.gen.h` | `tools/gen_verse_api.py` | same — every Godot class and the mirrored Verse class an object of it crosses as, which is what R-SCN-6's cast is built on. Every class, not only the emitted ones: a `--classes-file` build still has to make a handle cross as *something*, so each row names its nearest emitted ancestor |
| `docs/nonatomic-methods.md` | `tools/gen_verse_api.py` | same — R-AUD-3's list. Written by the pass that writes the mirror, so it cannot drift |
| `src/verse_gd_api.gen.h` | `tools/gen_verse_api.py` | same — how every Godot name is spelled in the mirror and in which shape (value, failable, test), for the GDScript converter. Recorded where each member is emitted |
| `src/verse_keywords.h` | `tools/gen_verse_keywords.py` | the UE compiler's `ReservedSymbols.inl` |
| `host/Private/HostVbcOps.gen.h` | `tools/gen_vbc_writer.py` | `docs/web-vm/ops.json` — the cooker's per-op `.vbc` encoder, each op's size, may-park table and the schema digest the file is stamped with. `static_assert`s every opcode number against the engine's, so an engine bump that moved the op set fails to compile rather than writing a wrong file. `--check` reports a stale header |
| `vm/vbc_ops.gen.h` | `tools/gen_vbc_ops.py --emit-cpp` | same — the interpreter's half of the same op schema: an enum class of opcodes, and per-op constexpr tables (name, emitted, may-park, yields, operand roles/kinds) the decoder reads instead of hand-maintaining a mirror of `ops.json`. Carries the same schema digest `HostVbcOps.gen.h` does, so a `.vbc` stamped by one engine commit and read on another is refused rather than misread |
| `bin/host_build_id.gen.h` | `tools/build_host.py` | the staged host sources themselves — a digest of `host/` plus the ABI header, and the engine commit beside it — staged into the host's `Private/` and baked into every host binary, so a cooked sidecar and the host reading it can be told apart. A digest rather than `HEAD` so a doc commit does not invalidate three binaries. Not committed |

**What the mirror is**, since no single file shows it: all 1036 Godot classes as a Verse class
hierarchy, Godot's own `Object` among them; its 793 enums as real Verse enums; all 1413 of
`extension_api.json`'s virtuals, spelled Godot's way, every one of them emitted; properties as writable members rather than get/set pairs; **Godot's `bool` as two
different things** — 568 predicates and 161 bool virtuals as `<decides>:void`, the way Verse's own
comparisons and `GodotMath`'s `HasPoint` are spelled, and `logic` kept for the 306 methods that
answer a value rather than a test (an accessor with a `set_` twin, an outcome like `MoveAndSlide`);
503 engine-signal
accessors; `@GlobalScope`'s constants and statics reachable through per-class `...Statics` modules;
the 16 math types with methods and definable operators in ordinary Verse; **Godot's RID as a `rid`
struct** rather than a bare `int` — it is not a math type, because it crosses as a scalar rather
than as a component array, and its lane lives in `I0` beside every other integer rather than in
`Ref`, which is what "an id with identity" means and a RID is not; and `typedarray::Node` as a
`typed_array(node)` whose elements are objects a script calls methods on.

**Every Godot class is mirrored by default.** `tools/verse_api_classes.txt` is a smaller curated
list kept for anyone who wants a smaller build, selected with `--classes-file`; there is no `--all`,
because all *is* the default. The reasoning is `phase-2-design.md` §3: adding a class means
rebuilding `verse_host.dll`, which means a UE source checkout, so a subset is a wall rather than a
setting. It costs per-keystroke analysis latency, which is measured and recorded there.

`src/verse_api_skipped.h` also carries what the **math** file does not define, and that row source is
unusual: `gen_verse_api.py` *reads* `host/Verse/GodotMath.native.verse` to find out what is written
and records every other `builtin_classes` method and operator as a skip. So adding a method there
deletes its own skip row on the next generation, and the record cannot drift from the code.

`gen_verse_api.py`'s type table is the other half, and it no longer skips anything for a type it
cannot carry — `unsupported_type` is zero. Three small tables decide the awkward names, and each
says why in place: `VERSE_AMBIGUOUS_MEMBER_NAMES` (five names, compiler-confirmed, not guessed),
`PROPERTY_RENAMES` (`Min`/`Max` → `Minimum`/`Maximum`, the only invented names in the mirror) and
`FREE_FUNCTION_REPLACEMENTS` (`Object.to_string` is Verse's own `ToString`, which is also what
string interpolation desugars to). `VERSE_STDLIB_NAMES` keeps the mirror clear of the names Verse's
own extension methods claim.

`host/Verse/Godot.native.verse`, `GodotApi.native.verse` and `GodotMath.native.verse` **are**
hand-written: the first is the whole native primitive surface, the second the ordinary-Verse packing
layer above it, and the third the math types' methods and operators — ordinary Verse with no handle
and no ABI, because that is OQ-11's answer. `GodotMath` carries `.native.verse` despite declaring
nothing native: VNI refuses a plain `.verse` in a VNI-capable package. Mirroring another Godot
*class* still costs no C++ and no new native function. What did cost native functions was the
reference types: the primitive surface went from 8 to 24, because a container has to be asked for
its elements rather than decomposed, and `<reads>` needed its own dispatchers.

The container wrappers (`godot_array`, `dictionary`) are **generated**, not hand-written, even
though they are not mirrored Godot classes. A script cannot spell a `variant` — the packers are
module-scoped by R-TYPE-7 — so every way into and out of a container has to be a typed accessor, and
ten element types against four key types is not a list to maintain by hand.

`CONST_OVERRIDES` in `gen_verse_api.py` is where Godot's `is_const` flag is missing rather than too
broad, and nothing in it was judged: `tools/audit_const_overrides.py` reads the rows out of a Godot
*source* checkout and takes a method only when its body is exactly `return <member>;`. Run it by
hand against a newer Godot to revise the list; it needs `../godot`, which nothing else here does, so
it is not in `run_tests.py`.

## Constraints that break things silently

### Where the host must live, and what a build is

- **The host must load from `Engine/Binaries/Win64`.** VNI records each Verse package's source
  directory relative to the loaded module and the compiler reads those `.verse` files at runtime.
  A copy elsewhere compiles against an empty package set and every identifier is unknown.
  `bin/verse_host.dll` exists for the smoke test only. Godot finds the engine tree through
  `verse_host_paths` — `UE_ROOT`, then Editor Settings `verse/host/engine_dir`, then the legacy
  project setting of that name, which is read with a warning and never written (R-DIST-12). The
  host DLL and `verse_cook.exe` are derived from it unless `VERSE_HOST_DLL`/`VERSE_COOKER` or the
  matching Editor Settings entries name one directly.
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
- **`vh_init` gets one attempt per process.** It boots `FEngineLoop` and the host module never
  unloads, so a second call runs `PreInit` again and asserts. `VerseRuntime` remembers a refusal and
  answers it without re-entering — without which a stamp mismatch, which is meant to be a sentence,
  took the game down on the second script. `vh_shutdown` tears the engine down; the DLL stays
  resident.
- **The host's build stamp is a digest of the staged host sources**, so a change under `host/` or
  to `include/verse_host_abi.h` invalidates all three host binaries and a commit that touched
  neither — a doc commit — invalidates nothing. It used to key on `HEAD`, which both over- and
  under-reported: every doc commit relinked three targets and refused every cook taken before it,
  while an uncommitted edit to `host/` left a stale cook loadable. Rebuild all three after touching
  the host. `build_host.py` and `scons` stage different halves of
  `addons/godot-verse` — the host and the library — so both have to run before an export is
  trustworthy. `build_host.py` refreshes every copy of the addon, not just `demo/`'s.
- **Loading the host moves the process working directory, and the bridge moves it back.** The
  monolithic host points it at `<engine>/Engine/Binaries/Win64` twice — once from a static
  initializer when the DLL loads, and again inside `vh_init`, where UE's `PreInit` does it
  deliberately. Godot resolves relative paths against the working directory, so from the first Verse
  build onwards every one of them landed inside the Unreal checkout. `load_host_internal` restores it
  around both (`FScopedWorkingDirectory`), and that is load-bearing rather than tidy: Godot stores an
  export path **relative to the project** (`editor_export_preset.cpp:380-383`) and `prepare_template`
  resolves it with `DirAccess::exists`, so exporting anywhere outside the project died with *"The
  given export path doesn't exist"* — after the Verse cook had printed its own lines, which made it
  read like a Verse failure. Restoring is safe because UE derives its paths from
  `FPlatformProcess::BaseDir()` and everything this bridge hands the host is absolute. Anything else
  that loads the host in-process has to do the same.
- **The attribute package must be added before the first `AddDataSource`.** `@global_class` is
  declared in a source package the host adds at runtime, not in `host/Verse` — VNI compiles that
  at build time and rejects `class(attribute)`. `FSolarisIde::EnsureDataSourcePackageExists`
  snapshots the project's other packages as the script package's dependencies exactly once, so a
  package added after the first script is never depended on and the attribute stops resolving.
  Authorship comes from a `IPreSemAnalysisInjection`, which must stay registered for the life of
  the process: `CProgramBuildManager::Build` resets the semantic program on every compile *and*
  every analysis, so a one-shot grant is gone by the first build.
- **A host build passing is not enough to know a `.verse` file compiles.** VNI compiles `host/Verse`
  at build time against one package set, and the *runtime* compiler re-reads those same files
  against another — a bare `Pi` passes the first and is an unknown identifier in the second. Run
  `tests/verse_probe` after touching anything in `host/Verse`.

### The editor's thread, and what may wait

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
- **A build describes itself, and leaves no AST.** The snapshot is taken from inside the build, at
  `FGodotSnapshotInjection` — uLang's `IPostSemAnalysisInjection`, which runs after the last
  semantic pass and before IR generation — so every class-describing read answers about the
  generation the moment `vh_compile_project` returns. What a build does *not* leave is a program a
  *position* can be resolved against: IR generation hangs an IR package off every module. A build
  used to end with a whole analysis-only pass to put one back, which was ~770 ms of the ~1.6 s
  between Play and the game; the consumer queues one from `_frame` instead
  (`VerseScriptLanguage::build_project`), and `probe_hover`, which has no frames, flushes it itself.
  **A hook of uLang's own is the only place a code-generating build is still describable** — add
  anything that needs the build's AST there, not after `BuildAll`.
- **Ask a definition's *prototype* where it was written, what it says and what it is called.**
  Instantiating a parametric class mints a fresh `CDefinition` per member — `typed_array(node)` has
  its own `ToArray` — and none of them was written anywhere: the file, the line and the prose all
  belong to the generic declaration. uLang states the rule where it `ensure`s against
  `GetAttributes` on one, *"which inherits its attributes from its prototype definition"*
  (`Definition.h:222`), and that ensure is the only thing that reports it. `PrototypeOf` is the
  helper; an ordinary definition is its own prototype, so it is identity everywhere else. The same
  sentence has a second half: **a parametric class is a `CFunction`**, so any walk that stops at a
  function walks past every member of `signal(t)`, `typed_array(t)`, `typed_dictionary(k,v)` and
  `event(t)` (`by-hand-findings.md` B25).
- **Verse's own library documents itself with `@doc`, not with a comment.** 132 attributes across
  `/Verse.org/Verse`, and `GetAttributeTextValue` against `CSemanticProgram::_doc_attribute` is the
  only way to read one — nothing above the declaration in the source carries the prose. A digest
  *does* rewrite `@doc` into `#` comments (`DigestGenerator.cpp:1962`), but that is not the text the
  host holds: those definitions come from the real engine files, loci and attributes intact.
- **Nothing may read declared types off the live semantic program.** IR generation *rewrites* the
  program the build was holding: a method answering a struct gets a coerced override generated
  beside it, decorating to the same name with one synthetic `Argument` parameter. `InstanceCall`
  walked the class live, found a one-parameter signature for `_GetMinimumSize()`, refused the call
  as `VH_ERR_NOT_FOUND`, and `Control.get_minimum_size()` answered Godot's own default with nothing
  said anywhere. Declared types come from the snapshot (`RecordedTypes`), which is the same table a
  runtime host reads out of the cook.
- **A consumer that begins an analysis must poll it to completion.** Nothing else reaps one: until
  `vh_check_project_poll` says finished, the next `vh_check_project_begin` is refused and `vh_tick`
  stays a no-op. The bench relied on a later wait to do the reaping and refused forever once the
  waits were gone.
- **The mirror is read from its digest after the first successful build**, which is half the
  per-keystroke cost, and a digest drops exactly two things: every definition's **file and line**
  (a digest is one synthetic snippet at a path no file is ever written to) and
  `CFunction::_bIsAccessorOfSomeClassVar` (DigestGenerator re-emits a class var without the
  `<getter>`/`<setter>` attributes the analyzer reads it off). A side table recorded during the
  first build's own semantic analysis — the last program that reads the mirror's own files, reached
  through `FGodotSnapshotInjection` — restores both,
  keyed by qualified name plus the function type's code, because `GodotMath.native.verse` declares
  eight two-parameter `operator'+'` and a verse path alone is ambiguous. **Anything new that reads a
  mirror definition's location or accessor flag must go through that table**, `GetScopeName()`
  included: from a digest a top-level definition's Owner and its path are *both* the digest path,
  so an `Owner == DeclaredIn` test keeps passing while both are wrong.
- **A build after an analysis runs neither of the phases a build spends its time in.** The program
  a clean analysis leaves *is* the next generation, so `vh_compile_project` generates code straight
  from it — 68 ms against 694. Three things make that legal and each is load-bearing. A build
  prepares the **next** generation's package as its last act, so every analysis between two builds
  already runs under the name the next publish will use (`PrepareGenerationPackage`). The reuse is
  refused unless every file on disk says exactly what the analysis read, because an analysis reads
  the editor's *buffer* and Godot only saves before running while `run/auto_save/save_before_running`
  is on (`HeldProgramIsThisBuild`). And `FSolarisIde::BuildAll` cannot be used for it — it goes
  through `CProgramBuildManager::Build`, which calls `ResetSemanticProgram()` first — so
  `GenerateFromHeldProgram` drives IR generation, assembly and the link itself and reproduces the
  two things BuildAll does around them that matter: `SetBlockExecution`, and the two FN version
  gates read from the CVars BuildAll reads them from. Its tail is *not* reproduced, and does not
  need to be: everything in it is fed by an injection that runs during semantic analysis, which
  this path does not run.
- **Only the generation's own package may be forced back to Source after a build.** The attribute
  package used to be too, and that alone made the reuse above impossible: the assembler publishes
  every Source package the program carries, and publishing one twice asserts inside
  `AsyncLoading2.cpp` (`LoaderImport`) rather than reporting anything. It is safe to let it go
  External because a build generates a digest for every Source package it compiles, this one
  included — 2175 bytes, which `VH_TRACE_ANALYSIS` prints beside the package — so `@export` keeps
  resolving out of the digest.
- **The mirror's digest is parsed once per process.** It is 2.1 MB of the 2.2 MB the parse phase
  reads and the same bytes every time, so `FGodotCachingParser` keeps the tree the parser produced
  and hands each build a clone of it: 36 ms to clone, and the parse phase falls from 202 ms to 77. uLang's own mechanism for this
  (`SBuildContext::bCloneValidSnippetVsts`, with `ISourceSnippet::IsSnippetValid` as its test) is
  unreachable twice over — the flag is set on a context `CProgramBuildManager::Build` constructs,
  and a digest's snippet is a `CSourceDataSnippet`, which does not override the test — so the seam
  used instead is `SToolchainOverrides::Parser` on a build manager handed to
  `ISolarisIde::SetBuildManager`, which is why `EnsureIde` constructs one. **`SetBuildManager` must
  come before `SetSourceProject`**, which wires the project into whichever manager the IDE holds.
  The cache fills on a text's *second* sighting: the one large snippet parsed exactly once is the
  mirror's own source at the first build, and caching that cost a clone nothing ever read.
- **The "user package" test is `InternalUser`, and the mirror passes it.** `SetupVerse(...,
  InternalUser)` in `VerseHost.Build.cs` sets it on `/Godot.org/Godot` and the attribute package
  sets it too, so "walk every InternalUser package" walks all 4.3 MB of the mirror's AST before
  reaching the two snippets that could hold a cursor — 97 ms per completion. Walk the package at
  `ScriptVersePath` and nothing else.
- The numbers this bought are in `docs/spec.md` R-PERF-2 with the machine they were taken on: a
  whole-project analysis is **520 ms** where it was 1273, a generation **694 ms** where it was 2.2
  (**68 ms** when an analysis has landed since the last edit),
  and reads that cost 1.7 s during an analysis cost 0.0 ms. The four commits `dcd517e`, `40d72f4`,
  `8bbba32` and `1469dc1` are the record — that work has no design document, by decision.
- **Adding `@tool` to an existing script needs the scene reloaded.** Editing a live `@tool` script
  takes effect on save; giving one `@tool` for the first time does not, because the node is holding
  a *placeholder* and the swap to a real instance does not happen. Known, small, and not fixed —
  `by-hand-findings.md` B8 has what is ruled out. Nothing automated can see it: a placeholder only
  exists under `is_editor_hint()`.

### Transactions, effects and raising

- **Every Godot callback goes through `AutoRTFM::Open`,** and writes defer to `AutoRTFM::OnCommit`.
  The GDExtension was never instrumented by the AutoRTFM compiler, so calling into it from closed
  Verse code is a fatal "could not find function" at runtime, not a link error.
- **Calling *into* the VM must be open too.** `vh_instance_call` invokes through
  `VFunction::Invoke` inside an `AutoRTFM::Open` nested in its transaction, which is what
  `TVerseFunction::operator()` does for the same reason: a Verse runtime error raised from closed
  code trips `AutoRTFM::UnreachableIfClosed` in `FContext::RaiseVerseRuntimeError` and takes the
  process down instead of unwinding.
- **`Verse::Stm::OnRollback` does nothing here.** It is the Solaris *interpreter's* STM and
  `VerseStm.h` says "Noop if StmActive() returns false". The mechanism that works from this bridge is
  `AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>` — `SameAsClosed` is load-bearing,
  because every Godot callback reaches C++ inside `AutoRTFM::Open` and a plain `OnAbort` from open
  code is ignored.
- **What a failure undoes is measured, not assumed**, and the rule is in `spec.md` next to R-AUD-1.
  A failure at any depth drops the deferred writes; a read does not see a write the same computation
  just made; a raise halts only the call that raised.
- **A raise terminates the *active* content scope, and `EnterVM` then declines to run anything in
  it** — silently, which is why this was invisible for a phase. That scope is the raising
  **instance's** (R-ASYNC-4), so the blast radius is that node's suspended work: another instance's
  next call runs, and so does the raising instance's, because a terminated scope is *replaced* at the
  next call rather than un-terminated at the next tick. Every entry into the VM goes through
  `EnterVerse` or `EnterVerseOn`, which is where the scope handling lives — never call
  `Context.EnterVM` directly. `VH_ERR_HALTED` is what an execution entry point answers when the body
  genuinely did not run, which is now rare.
- **An explicit effect specifier narrows, and narrowing is contagious downward.** A function with no
  specifier carries the *default* set, which is wider than `<transacts>` — it contains
  `no_rollback`. So a `<transacts>` function may not call a specifier-less one, and anything that
  forces `<transacts>` on a method (a `Subscribe` handler; a failure context) forces it on
  everything that method calls, a file at a time. The compiler reports it at the **call** site, not
  at the declaration that needs changing — `ErrSemantic_EffectNotAllowed`, uLang glitch **3512**.
  This is `dodge-the-creeps.md` wall 8, narrowed twice and not removed. **Reading Godot no longer
  starts the cascade** — a const-and-answering method is `<reads>` — but a helper that writes still
  needs the word, and effects are *contravariant*, so a `<reads>` callee satisfies a `<transacts>`
  caller and a `<transacts>` function type alike.
- **The bridge annotates a diagnostic only where the bridge is what the author is confused by.**
  `explain_effect_errors` is gone: it keyed on the code and the callee's package and never on which
  effect had been refused, so a `suspends` refusal took the `transacts` branch and gave the opposite
  of correct advice (`by-hand-findings.md` B7). The compiler's own text stands.
- **A method's effect is Godot's `is_const`.** 3996 mirror methods carry `<reads>`, and the test is
  `const` **and answering a value** — Godot's `const` means "does not mutate the C++ object", so the
  38 const-and-void methods are `OS.set_environment`, `CanvasItem.draw_string` and 36 more that
  plainly do something. A `<reads>` body dispatches through `VhCallValueConst`, not `VhCallValue`;
  the two have to move together.
- **The dynamic route has a `<reads>` twin too, and its honesty is the caller's.** `object.Call`
  and `Callv` are generated from Godot's own `call` and `callv`, which are not const, so both are
  `<transacts>` — which meant R-INT-2's escape hatch could not be used from a `<reads>` function
  at all, and reading one value off a GDScript node pulled the specifier onto every caller above
  it. `CallConst` and `CallvConst` in `GodotApi.native.verse` are the twins: same C++ call, six
  arities plus the `godot_array` spelling, hand-written because Verse cannot reopen the generated
  `object`. **Nothing checks that the method named is const** — for a mirrored method the
  generator reads Godot's `is_const` and for a generated binding it reads `METHOD_FLAG_CONST`
  (spec R-INT-9), but a hand-written call has only the author's word, and naming a mutating method
  through one means a failure that should have undone the write does not. They are also what makes
  a bindings package possible at all: **Verse's internal access is scoped by verse path, not by
  package**, so anything outside `/Godot.org/Godot` reaches neither `VhCallValueConst` nor the
  packers nor `vh_object.Handle` (`docs/generated-bindings.md` §10.6).
- **An archetype instantiation carries the constructing class's own effect.** `variant{Tag := ...}`
  inside a `<reads>` or `<computes>` function is *"This archetype instantiation constructs a class
  that has the 'transacts' effect"* unless the class says `<computes>`. `variant` and `godot_ref` do,
  and so do `callable` and `signal_ref`; the four containers say **`<reads>`** instead, which is as
  wide as the default that mints them and the block that adopts them and no wider, so a `<reads>`
  converter builds one and a `<computes>` body does not. A **mirrored Godot class cannot**, because
  it descends from the native
  `vh_object`, so anything a narrowed body needs must be reached by a *cast* over what the host
  built rather than by construction — which is what the singleton accessors do
  (`GetInput()` casts what `VhSingletonObject("Input")` answered), and what R-SCN-6 says they
  should always have done.

### Signals, tasks and awaiting

- **A task scope per script instance** (R-ASYNC-4), made at `vh_instantiate` and terminated at
  `vh_release_instance`. It costs ~2.6 KB per scripted node and ~0.06 µs per call, both measured
  with `tools/build_bench.py`. `vh_tick` is not where anything recovers.
- **`Await` is ordinary Verse over `/Verse.org/Verse`'s `event(t)`.** `signal(t)` holds one and
  `Await<public>()<suspends>:t` forwards to it, which covers a script's own signals and all 503
  mirrored engine-signal accessors alike. The host half is small because **`verse::event` is a
  UObject with a public C++ `Signal`**: the host reads the event off the signal object and signals it
  directly, and Epic's code then does FIFO resumption, per-task scopes and dropping a cancelled
  awaiter.
- **Awaiting is the one thing that cannot be narrowed, and `spawn` is how a script starts it.**
  `event.Await` carries `no_rollback`, so a `<suspends><transacts>` body that awaits is glitch 3512
  and **a `<reads>` body may not `spawn` at all**. The caller must be specifier-less too: a mirrored
  virtual override is, and so is a `signal.Subscribe` handler. `_Ready<override>()<suspends>` is
  *not* a spelling — glitch 3532 plus 3523, because the specifier makes it a different function.
  Both are said in the `.verse` template rather than diagnosed.
- **`defer` in a suspending body runs on cancellation as well as on return** (measured,
  `tests/verse_probe/sleep_probe.verse`). The whole connection lifetime of `Await` rests on it: it
  connects with `CONNECT_ONE_SHOT`, holds the signal object, and disconnects in a `defer` — which is
  the only reason a `race` whose loser never resumed leaves nothing behind.
- **`Sleep(Seconds)` is a native `<suspends>` on `FPlatformTime::Seconds()`**, resumed from the pump,
  because it has to work where there is no scene tree. It ignores `Engine.time_scale` and keeps
  counting in a paused game; game timing awaits a Timer instead. `vh_tick`'s budget governs the queue
  and nothing else — a task resuming inside an emission is unbudgeted, exactly as GDScript's resume
  is — and `vh_tick` fills a `vh_tick_stats` that becomes three Godot custom monitors.
  `verse/instance_tasks` is the monitor that stands in for a cap on the `spawn` runaway (OQ-13 closed
  as "nothing is bounded", and Godot does not bound GDScript either).
- **A signal's struct payload works in both directions** — out as one Godot argument per field, named
  by the field, which is what gives the connect dialog real names; back in through `InstanceCall`'s
  rule that N arguments satisfy one struct parameter with N fields. That path needs no Godot
  counterpart for a struct, because a signal delivers the fields separately; a struct as a *method
  parameter* still has none, and that half is R-LANG-2's, which the spec answers with a Dictionary.
  `signal()` is an alias for `signal(tuple())`, spelled the way `/Verse.org/Concurrency` spells
  `listenable()`.
- **A foreign signal is named with `MakeSignal(Owner, Name)`** — Godot's own `Signal(object, "name")`,
  the analogue of `MakeCallable`. It answers a `signal_ref`, which nothing could produce before, so
  `signal_ref.Await()` and `.Subscribe()` would have been unreachable. Its payload is a
  **`godot_array`** of the emission's arguments, and its `Subscribe` is the rollback-safe way to
  receive a foreign signal, which `Object.Connect` is not.
- **A signal declaration is validated in `GetClassSignals`**, so Godot is never told about a signal
  nothing can emit: `vh_signal_desc` carries a `Reject` the way `vh_export_desc` does, and
  `_validate` says why at the member's line.
- **`@export_signal` is what registers a member, on both spellings** — a `signal(t)` *or* an
  ordinary `event(t)`. What differs is silence: an event without it is not a signal and is absent
  from the list, while a `signal(t)` without it is listed and refused with
  `VH_SIGNAL_NEEDS_ATTRIBUTE`, because that type has no purpose but Godot. The attribute could not
  be spelled `@signal` — a bare marker is a class, the attribute package shares
  `/Godot.org/Godot`'s verse path, and a third definition of `signal` is glitch 3532.
- **The attribute is the whole gate: a member's access level is not tested, and
  `VH_SIGNAL_NOT_PUBLIC` is retired** (the enumerator keeps its value until the next major bump, or
  the three codes after it renumber). Connecting is not done from Verse — the Node panel and
  GDScript both connect by name — and nothing else in the bridge tested access either, so a
  non-public `@export` member has always reached the inspector. A `<private>` signal is therefore
  private from *Verse* callers and from nothing else: anything holding the node can connect to it
  and emit it. Binding stays unambiguous because the compiler refuses a member that shadows an
  inaccessible one of the same name (glitch 3593, `tests/verse_probe/signal_shadow_probe.verse`).
- **Both member types are supported and neither is deprecated**, because they are not
  interchangeable. `event(t)` satisfies `awaitable(t)`/`signalable(t)` and is where Epic is heading;
  `signal(t)` satisfies **`listenable(t)`**, keeps its connection scoped to the wait, and has no
  bypass hazard. `docs/signal-declaration.md` §11 is the table of which to reach for. `signal(t)` is
  also what the 503 engine accessors answer, which is not going to change.
- **The Verse book's `subscribable_event` does not exist in this drop** — the book says the feature
  is unreleased. What exists is `subscribable_event_intrnl`, `<epic_internal>` and slated for
  deletion by its own comment; it *is* reachable from a script package and *does* satisfy
  `listenable`, and its `Signal` is still `no_rollback`, so it changes nothing about the emit verb.
  `docs/signal-declaration.md` §12 is why it is not built on. Re-check on every engine drop.
- **The emit verb is the bridge's, whichever type declares the member.** `signal(t).Signal` and
  `event(t).Emit` both go out to Godot and come back through the member's connection, which is what
  makes a Verse handler and a GDScript handler see one ordering. An event's own `Signal` resumes
  Verse awaiters *without telling Godot* — legal, undiagnosable, and right on a non-`@export_signal`
  event; C# carries the same hazard in its `backing_` field.
- **An `@export_signal` event member holds one Godot connection for the instance's life**, because a
  bare `event(t)`'s `Await` is Verse's own native and offers no hook to connect from. It is made at
  the **first entry into the instance** (`EnsureEventConnections`, from `InstanceCall`) and cannot be
  made earlier: `vh_instantiate` runs before the consumer installs the script instance, and
  `Object::has_signal` answers off the installed instance, so Godot refuses with *"Attempt to connect
  nonexistent signal"* — as it does at the end of the consumer's `create()` too, since the object
  does not hold the instance until `_instance_create` has returned. The failure is silent in the
  worst shape (registers, emits, never delivers back, every await hangs), so `ConnectDelivery`'s
  refusal is reported. Every `signal(t)` — declared or engine accessor — keeps connect-while-awaiting, and
  `tests/integration` asserts the connection count returns to zero on both sides of a `race`.
  `GEventBindingIds` is what an event has instead of `vh_signal`'s `Id` field, and `ReleaseInstance`
  drops the row, the callback, the reference and the strong pointer together — the strong pointer is
  a GC root per scripted node if it outlives the instance.
- Verse's own `signalable` cannot be implemented here — its `Signal` is `no_rollback` and every
  Godot callback runs in a transaction. **`listenable` can and is**: it is `awaitable` +
  `subscribable` and does *not* extend `signalable`, so `signal(t)` implements it and a declared
  signal or an engine accessor can be handed to any Verse code taking one.

### Objects that are not nodes

- **Every `vh_object` runs a block clause that asks the host for a Godot object** (R-NODE-3), and
  the host is much the commoner constructor: a script instance for a node Godot already made, a
  mirror wrapper for a handle crossing in, the transient instance the export defaults are read off,
  the bare `vh_object` the fallback answers. Every host-side `NewObject` of one is wrapped in an
  `FAdoptPeerScope`, which carries the **class** as well as the handle so a *member* of the class
  being built still mints its own. **Add a fifth construction path and it leaks a Godot object per
  construction**, silently, while a working scene looks entirely normal.
- **A reading device suppresses minting outright**, which is `FSuppressMintScope` and one caller:
  `NewDefaultsObject`. Its members' initializers run in full, so a class with `var Held:node2d =
  node2d{}` minted a real node per exporting class *per analysis* — a leak on the per-keystroke
  path, since a Verse-minted node is deliberately never freed.
- **`node2d{}` is a live Node2D**, not the handle of 0 it was before 4b. The "reach through a handle
  of 0" trick three fixtures used to spell a deliberate raise is gone; the replacement is
  `viewport{}` — an archetype of a class Godot will not instantiate, which raises naming the class.
- **`godot_array{}` and `dictionary{}` are live, empty Godot containers**, and the mechanism is not
  `node2d{}`'s. A minting `block:` clause is unwritable for a container: a `var` member needs
  `allocates` and an assignment is `transacts` outright, so the class would be `transacts` and would
  drag the mirror's 379 container-answering `<reads>` methods across with it. What mints instead is
  a **data-member default** — `Ref<override>:int = VhRefNewDefault(TagArray)` — which is legal
  because glitch 3582 bans only a *divergent* call in a default and `VhRefNewDefault` is
  `<converges>`, a specifier only a native may carry. **A field the archetype supplies wins over an
  overridden default**, which is the whole reason `VhToArray` pays nothing. The `block:` that
  remains does one thing, `VhAdoptRef(Self)`, because a container the archetype minted reached no
  converter and so had no `UObject` shadow to release its table entry; that call is `<reads>` and
  writes nothing, which is what keeps the class `<reads>`. All of it is measured in
  `tests/verse_probe/ref_block_probe.verse`. `MakeArray()` and `MakeDictionary()` still exist and do
  nothing; a `typed_array(t)` has no bare archetype, because `Unpack` and `Pack` are required
  members, so its maker supplies those two and no `Ref` and the same default mints it.
- **`BeginDestroy` releases only what the host recorded as minted.** Every object crossing *from*
  Godot is a `vh_object` too, and an unconditional release would free a node the scene owns. The
  row that records it carries two pointers to one object and they answer different questions: a
  weak one for "is it still alive" (which is also what makes a minted object cross back out and in
  as the *same* Verse object), and a raw one for "is this the object that made this row" — because
  **by the time `BeginDestroy` runs every weak pointer to the object already reads as null**, so a
  release keyed on the weak one matches nothing and silently never fires.
- **Only `vh_collect_garbage` can make a release observable**, and it needs both collectors in the
  right order. Waiting on the VM's collector from the game thread deadlocks (which is
  `abi-v2-design.md` §1a's warning, wider than it was written); a plain UE `CollectGarbage` frees no
  Verse object at all, because every cell the VM holds is a root to it. Request a fresh VM cycle
  without waiting, let the collector raise its start signal, then collect — which is the coupled
  pass `TickGC` takes opportunistically. Release is still "within a cycle or two", never "the next
  one": the VM's registers still name what the last frame held.
- **An object-returning method in the mirror is `<decides>` unless Godot says the result cannot
  be null, and the singleton accessors are not.** `GetParent[]`, `GetTree[]`, `GetViewport[]` are
  failable because Godot really answers null there — R-TYPE-4's rule that nullability belongs to
  the *type*, so an object return is the only failable one. **40 of the 759 are not**, because
  `RequiredResult<T>` marks them (godotengine/godot#86079): the whole Tween builder chain,
  `SceneTree.GetRoot`, `CreateTimer`, `CreateTween`, `GetMultiplayer`. Those are spelled
  `GetRoot()` and raise through `Err` if the *cast* refuses — Godot answering a class outside
  the mirror — which is the same answer the 39 total singleton accessors give. Of the 41 singletons only `EditorInterface` and `GDScriptLanguageProtocol` can be
  absent in a game, and only those two accessors are failable. The other 39 are spelled
  `GetEngine()` and raise through `Err` if the cast ever refuses — a raise rather than a plain
  total accessor because V3564 forbids the spelling that would need neither. **A raise is not a
  failure**: it costs the raising instance its content scope, so weigh that before spelling
  anything else this way.
- **`Object::get_class()` can answer a class `extension_api.json` has never heard of**, and
  `GodotClassNames.gen.h` is keyed by exact name off that dump. `GDCLASS` registers a class in
  ClassDB the first time one is constructed, so a driver class is in ClassDB and *not* in the dump —
  `IP` answers `IPWindows`, `NavigationServer2D` answers `GodotNavigationServer2D` — and the lookup
  misses, which costs the handle its class and every cast over it. `GDSOFTCLASS` is the one that
  does not, because it leaves `get_class()` to the nearest `GDCLASS` ancestor, which is why
  `DisplayServerWindows` crosses as `display_server` and needs nothing. Godot moves classes between
  the two macros between releases, so a caller that knows the class before it has a handle should
  say so (`MirroredClassFor`, and what the singleton accessors pass).

### The debugger and the profiler

- **The host owns *which frame*, the consumer owns *which line*.** `EngineDebugger`'s breakpoint list
  and step state are never duplicated in the host; what only the host can see is frame ancestry, and
  it is needed because the bridge sees no Verse call, only a bytecode op, so Godot's depth counter
  would never move and step-over would behave as step-in. `DebugShouldBreak` carries a
  `vh_debug_frame_relation` — SAME, DEEPER, OTHER — and the consumer reads `get_depth()` as *which
  kind* of step is pending (-1 in, 0 over, 1 out) rather than as a count it maintains.
- **A step may not land where it started.** `Total := Helper()` reports its line **twice**, once
  before the call and once when the result lands, so a step-over with no memory of where it began
  stops on the line it began on. GDScript never meets this: its line opcode is per source line, a
  Verse location is per op. The consumer remembers the (source, line) it stopped at.
- **A line that emits no op carries no location.** Every statement line reports one and so does a
  function's declaration line, but a trailing bare expression that only reads a register does not —
  `Inner` as the last line of a body — so a breakpoint there never fires.
- **Re-entering a stopped VM is safe** (measured): an ordinary call or property read from inside
  Godot's debug loop runs and the outer frame resumes, so the remote inspector stays live while
  paused. But **two entry points are still refused**: `vh_compile_project` and the two
  `vh_check_project` entry points answer `VH_ERR_STOPPED`, because publishing a generation underneath
  a frame of the retiring one is incoherent and an analysis resets the semantic program that frame is
  about to resume into. `vh_tick` is refused *silently* — resuming a slept task inside a VM stopped
  mid-op is not something any of this is designed for, and `flush_output` can reach `_frame` from
  that loop.
- **The two debuggers are mutually exclusive.** `SetDebugger` is one global pointer, so
  `verse/host/enable_debugger` (Epic's socket debugger) and Godot's own cannot both be attached;
  `vh_debug_set_enabled` refuses rather than overwriting and the consumer warns once.
- The debugger attaches whenever Godot's is active, and that is affordable: +1.6% of frame time on
  the yardstick, though a single `vh_instance_call` goes from 0.27 µs to 2.79 µs. Attaching also
  suspends VerseVM's computation watchdog, which is what makes sitting on a breakpoint legal.
- **A debug value crosses typed only when it can name itself.** `VNamedType::GetBaseName()` keys the
  generated layout table, so `GodotVerse::ReadMathStruct` builds a `vector2` with no declaration to
  consult. Without it every math struct reached the inspector as the *text* of one.
- **The profiler is boundary instrumentation plus `profile{}` blocks, and not a sampler** — a sampler
  cannot produce a call count. **A Verse function called from another Verse function has no row of
  its own** unless the author writes `profile("tag"){…}`, which the compiler accepts in a
  `/user@localhost` package and the VM reports through `FVerseProfilingDelegates`.
- **`ScriptLanguageExtensionProfilingInfo` is a stride trap.** Godot's real `ProfilingInfo` has had
  five fields since 4.3; its `GDREGISTER_NATIVE_STRUCT` string still lists four, so godot-cpp's
  struct is 32 bytes for an array whose elements are 40 and `p_info_array[i]` corrupts for any
  `i > 0`. The stride comes from `Engine::get_version_info()`. Check the registration string against
  the header before trusting any other generated native struct.
- **`TArray::AddDefaulted_GetRef()` does not zero a POD** — it default-*initializes*, so a descriptor
  built that way carries whatever the previous answer left in the lane this one does not fill. Assign
  from a zeroed local.
- The bridge's own stack printing is **rate limited per raise site**, with the "n dropped" summary
  flushed from `vh_tick`: unthrottled, it spent the shared character budget and silenced every other
  script.

### The cooked path and exporting

- **A cooked `VNativeProcedure` comes back with a null C++ thunk, and calling it is a jump to address
  0** — no crash report, no diagnostic, because `RIP` is zero and there is no unwind info. The engine
  rebinds *class*-scoped natives at load and *module*-scoped ones only from the assembler, so a host
  that loads VNI packages instead of compiling them has to do that walk itself —
  `GodotVerse::RebindVniModuleNatives`, over the same public `TryBindVniModule` the assembler uses.
  `VhCallValue`, `Print` and `Sqrt` are all module-level.
- **Every VM intrinsic was a null *cell* for the same shape of reason.** `$BuiltIn` gets an
  associated UPackage only under `IsRunningCookCommandlet()`, and without one the harvester writes a
  null package for every import of `Abs`, `Floor` or `BitOr`. The cooker sets
  `PRIVATE_GIsRunningCookCommandlet` before `PreInit`, which it is entitled to: it is a cook.
- **FName does not preserve case outside an editor build.** `WITH_CASE_PRESERVING_NAME` is 0 in the
  runtime host, so `UClass::GetName()` answers the casing of whichever name was interned first — a
  script class named `concurrency` came back as `Concurrency`, colliding with
  `/Verse.org/Concurrency`, and every member of it read as absent with no diagnostic anywhere.
  **Never build a shape key or any other identity from `UClass::GetName()`**; `QualifiedClassName`
  reads it off the Verse type (`VNamedType::AppendMangledName`), where the name is a UTF-8 array.
  For the same family of reasons a `UClass`'s qualified name comes out of its *mangled* name —
  `PackageRelativeVersePath` is dead under VerseVM — and asking the *semantic* program for one must
  not use `EPathMode::PackageRelative`, which is fatal for a class with no package.
- **A runtime host has no semantic program and can never build one**, so everything the analysis
  alone could describe is recorded and carried in the sidecar (version **8**): the declared types of
  every member, method and signal, **whether a member is `var`** (without which every write an
  exported game made to its own state was silently dropped), the payload of all 503 mirrored
  engine-signal accessors (without which `Timer.Timeout().Await()` connects and never resumes), the
  **decorated name of a class's `ToString` extension method** (R-NODE-10: it is a module-level
  definition, so there is nothing on the class to find it from), the cooked package list — a
  container holds package *ids*, which are hashes, and mount points are still registered by name —
  and the **class-to-binding table** (R-INT-11), which only the editor can enumerate, since it comes
  from ClassDB and Godot's global class list. Without it a cooked game compiles its bindings package
  and can reach nothing in it: a handle crosses as its nearest *mirrored* ancestor and every cast
  declines, which is a wrong answer rather than an error.
- **Every package the program has must reach the container.** A VNI package the runtime host cannot
  find is only a warning from `JitVniPackages`, and then every import into it in every other package
  silently resolves to null — which is how `/Solaris/_Verse/VNI/VerseNative` went missing for a
  session. `SavePackage2.cpp:2076` asserts on a `UVerseClass` with no `Verse::VClass`, and a
  `UVerseClass` standing for a Verse *module* has none, so the cooker suppresses that one export for
  the length of one save rather than dropping the package.
- **`CreateIoStoreContainerFiles` parses `FCommandLine::Get()`, not the command line it is handed**,
  and needs three files a cook of this shape does not produce — a script-objects buffer, a commands
  list with a response file, and a compact-binary oplog manifest of which exactly one field is read
  (the package name, because a legacy `.uasset` carries none).
- **The I/O dispatcher is constructed but never brought up in this host.** `USE_IO_DISPATCHER` is
  false for a Program with no Engine, so `FIoDispatcher::InitializePostSettings()` — which
  initialises the backends and starts the thread — is called by nothing. `IsInitialized()` answers
  true anyway, and every read is issued and never completes: no error, no timeout, a `LoadPackage`
  queued forever.
- **A snippet-compiled procedure carries its file path verbatim**, the absolute path
  `vh_compile_project` was handed, mixed separators and all, because a data package has no `_DirPath`
  for `VVMLocationUtil.h`'s `GetPath` to relativize against.
- **When the host fails, an exported game says so and closes**: `OS::alert` with the host's own
  sentence, then `SceneTree::quit(1)`. Every script in such a game is dead, so a window that opens
  and does not respond is a worse answer than no window. Editor builds only log it — an editor with a
  broken host is still an editor.
- **A failed check inside the host is not a crash to anyone but Unreal.** It reaches
  `FWindowsErrorOutputDevice`, which reports to console output only — there is no log file and no
  crash reporter — and terminates with code 3; Godot's crash handler never sees it, though it does
  see a native crash, because `NOINITCRASHREPORTER` leaves Godot's exception filter in place.
  `HostFatal.cpp` writes the report to `user://logs/verse_crash.log` from `OnHandleSystemError`,
  and the next process reports it: the editor when Play ends, anything else when it loads the
  host. `VERSE_HOST_TEST_FATAL=check|access_violation` fails on purpose at the first `vh_tick`,
  and the consumer hides it from an editor's own host so a Play session can be tested
  (`by-hand-findings.md` B43).
- **Godot never creates the destination directory of an export**; it must already exist, or
  `prepare_template` answers *"The given export path doesn't exist"*
  (`editor_export_platform_pc.cpp:156`). That message also means the working-directory trap above.
- **Never put a comment in a `project.godot` or in a `dodge-the-creeps/export_presets.cfg`.** The
  editor parses and rewrites both and does not preserve comments — anything explanatory there is
  deleted the next time the project is opened, which for the yardstick is often. Those two files are
  **editor-owned**: read the diff before committing them, and blank `export_path` in the preset,
  which the editor fills in with whatever directory you last exported to.

### Modules and names

- **In `host/Verse`, a module-level function may not differ from a *type* in that package only by
  case.** `Variant` beside the `variant` type, or `Node` beside the mirrored `node` class, makes the
  whole package fail to load — and it fails **silently, in the runtime compiler only**: VNI accepts
  it, the host builds and links, and then every script is refused with `status 4` and zero
  diagnostics. Two *functions* differing only by case are fine (`VHFROMINT` beside `VhFromInt`
  compiles), and so is all of it in an ordinary script package (`widget` struct beside a `Widget`
  function). This is why the general variant builder is `MakeVariant` and not `Variant`, and it is
  a live constraint on any new mirror function: check the name against the 1036 mirrored class
  names and the four native types first. Measured in `tests/verse_probe/variant_any_probe.verse`'s
  header; three builds to find, because only the runtime compiler objects.

  **The mechanism, read out of the engine.** Verse's own naming is case-*sensitive* —
  `CSymbolTable::FindOrAddInternal` compares bytes (`uLangCore/Private/uLang/Common/Text/Symbol.cpp`
  :88-122) — and so is the native-thunk lookup, `VNativeProcedure::SetThunk` over a `VNameValueMap`
  that defaults to `ESearchCase::CaseSensitive` (`VVMNativeProcedure.cpp:53-72`,
  `VVMNameValueMap.h:74-90`). That is why function-versus-function is fine. What is *not*
  case-sensitive is **`FName`, unconditionally** — "case-insensitive, but case-preserving"
  (`NameTypes.h:629`), every `operator==` comparing only `ComparisonIndex` — and a Verse **type** in
  a VNI package is promoted to a real UObject keyed by one: `NewObject<UVerseStruct>(UEPackage,
  FName(UEName), ...)` (`VVMClass.cpp:1016-1047`). So the type's identity folds case where the
  function's does not, and they meet.
  **Epic knows this hazard and used to diagnose it.** The legacy BPVM assembler's
  `FUObjectGenerator::FindOrCreateUObject` (`VerseUObjectGenerator.inl:44-116`) does a
  case-insensitive package-scoped `FindObject` and reports *"Found existing type '%s' that is
  already being created this compile. Please rename the %s to be case insensitive unique."* through
  `AppendGlitch`. The VerseVM path has **no** equivalent check, and the adjacent bind failure is
  reported by `UE_LOGF(..., Error, ...)` from a `void` `TryBindVniType`/`TryBindVniModule` whose
  callers wrap it in `ensure()` and drop the result (`VerseVMEngineEnvironment.cpp:111-125`) — so
  nothing reaches `uLang::Diagnostics`, which is what the ABI's diagnostic callback listens to.
  **That is the whole reason it is silent**, and it makes this a diagnostic regression carried over
  from the BPVM→VerseVM migration rather than a rule anyone chose.
  Verified against the engine sources except one link: what the *function's* colliding registration
  is in the current pipeline was not found, only that the collision happens. There is no escape
  hatch — `cpp_name` overrides a **type**'s C++ identity (`DefinitionInfo.cpp:195-205`) and has no
  function equivalent — so renaming is the fix, not a workaround.
- **Every top-level name must be unique within its module.** A directory is a module only if it
  carries a `<name>.vmodule` marker, and the **marker names the module**, not the directory —
  `res://my-stuff/gameplay.vmodule` is module `gameplay`. Unmarked directories are organisational:
  their files join the nearest marked ancestor, so a project with no markers has every file in the
  root module and nothing on disk changes meaning. Root is implicit: a module reads the root module's
  definitions with nothing written. A file may declare any number of top-level names, and only the
  class **named after the file** can go on a node — a bridge rule now rather than a Verse one. A file
  with no class at all is a library file (R-LANG-6). Every `ClassNameUtf8` in the ABI is
  module-qualified: `player` at the root, `gameplay/player` in a module.
  `src/verse_module_map.{h,cpp}` is the whole rule, and it is a unit test away from Godot.
  **Anything that asks the host for a class must pass the module-qualified name** — `_validate` and
  the two warning passes each asked by bare stem, and a script under a marker got no method outline
  and no warnings (`by-hand-findings.md` B4).
- **A virtual is spelled the way Godot spells it — `_Ready`, not `Ready`.** `phase-4-design.md` §7.1
  counts the eight *signal* collisions that decided it. `_notification` is in no part of
  `extension_api.json`, and neither are `_get`, `_set`, `_get_property_list` or
  `_validate_property` — Godot offers that family to *scripts* rather than registering it in
  ClassDB — so all five are hand-written on the native root with **empty bodies and no specifier**,
  and an `<override>` is the spelling. Two things follow that are easy to get wrong. **They cost a
  script that overrides none of them nothing**, because `vh_class_method_list` reports a class's
  *own* declarations and an inherited empty body is not one, so `resolve` finds nothing and the VM
  is never entered — do not "optimise" that by adding an inherited-methods pass. And **they carry
  `no_rollback`**, so a script's own `<transacts>` code may not call its own `_Get`; Godot is the
  caller. R-NODE-10 is done; `_ToString` is not among them and never will be (see "Verse itself").
- **A virtual that answers Godot a `bool` is `<decides>:void`, and the specifier is alone.** All 161
  of them, `_Set` included: Godot asks "did you handle it" or "is it so", which is a test and not a
  value. `_HasPoint<override>(Point:vector2)<decides>:void = Solid?` is the spelling.
  `<decides>` **does not narrow** — its effect descriptor rescinds only `decides` and excludes
  nothing, so the declaration keeps the wide default set and an override still calls specifier-less
  helpers. `<decides><transacts>` would rescind `no_rollback` and start wall 8's cascade at every
  one of them. The price is that a bool virtual is **uncallable from Verse at all**, because a
  failure context refuses `no_rollback`: Godot is the only caller, through `vh_instance_call`.
  Measured both ways in `tests/verse_probe/decides_virtual_{probe,reject}.verse`, whose reject file
  also records the one migration mistake the compiler does **not** catch — an override that fixes
  the return type and drops the specifier is accepted, because effects are contravariant, and
  answers *handled, every time*.
- **The status is the whole answer for one of these, in both directions.** A `<decides>:void` call
  writes no value whether it succeeds or declines, and Godot reads a script virtual's result
  through `Variant::booleanize()` — `!is_zero()` — so an empty Variant is `false` either way.
  Declining is right by accident and succeeding is silently wrong, so `call_func` writes `true` and
  `false` itself. Anything else reading a `<decides>` result has to do the same.
- **A class member may not shadow an inherited mirrored one, and Godot's signals are members too.**
  `Hidden:signal(int)` on a `node2d` is *"Instance data member `Hidden` is already defined in
  `canvas_item`, did you mean to add the `<override>` specifier?"* — because `canvas_item` mirrors
  Godot's `hidden` signal as a `Hidden` accessor. Every one of the 503 signal accessors and 3312
  properties is a name a script cannot reuse, and the compiler reports it at the *declaration* with
  no hint that the collision is with generated code. Rename the member; there is nothing to override.
- **A module-level name and a local of that name are ambiguous, not shadowing** — and an *extension
  method* is a module-level name. `(V:vector2).Angle()` declares `operator'.Angle'` and Verse
  resolves a bare `Angle` against it, so a parameter or local named `Angle`, `Length`, `Dot`,
  `Cross`, `Normalized`, `Rotated`, `DistanceTo` or `LengthSquared` is *ambiguous* in any file that
  imports the Godot package, which is every script. **`ToString` belongs on that list and is the
  one a class is most likely to want as a member of its own** — see "Verse itself" below for what
  to write instead. **`event` belongs on it too** and is the
  one met first: the mirror spells `_Input`'s parameter `Event`, and an override that writes `event`
  collides with `/Verse.org/Verse`'s own `event` — glitch 3532, reported at the parameter with no
  hint that a capital letter is the fix (`by-hand-findings.md` B2). `gen_verse_api.py`'s
  `VERSE_STDLIB_NAMES` keeps the generated mirror clear of them; a script has to avoid them by hand,
  the way it already avoids `Abs` and `Clamp`.

## Verse itself: what the compiler does that surprises

- **A class cannot name a method `ToString`, and neither can a module.** `/Verse.org/Verse`'s own
  `ToString` is reachable as an extension method, so a *member* of that name is glitch 3532 at its
  declaration; and a module-level *overload* for a script's own class is 3532 against **the mirror's
  own** `ToString(:object)`, because every script class derives from `object` and Verse does not
  prefer the more specific overload. The spelling that works is the **extension method** —
  `(X:my_class).ToString<public>()<transacts>:string` — which is what a Verse author reaches for
  anyway, and what R-NODE-10 uses instead of the `_ToString` virtual `phase-4b-design.md` §7 first
  proposed. Measured in `tests/verse_probe/tostring_probe.verse`, which carries all five rounds.
- **`X.ToString()` and `"{X}"` are not the same lookup.** The first reaches an extension method; the
  second desugars to the free `ToString(X)` and reaches `ToString(:object)` — Godot's own
  `to_string()`. So a Verse override is invisible to interpolation *directly* and reached by it
  anyway, out through Godot and back. An extension method is
  `operator'.ToString'(:my_class, :tuple())` at **module** scope, receiver first and the call's own
  arguments as a tuple second, which is why no class method list carries one and why `InstanceCall`
  cannot reach it unaided.
- **A function's parameters are its tuple, and a tuple of one repeated type *is* an array of it.**
  This is one rule wearing three faces, and each was found separately: `f()` is ambiguous with
  `f(:[]variant)` because the empty tuple is the empty array; `f(:variant,:variant)` is ambiguous
  with it because a two-tuple of variants is one too; and `f(:variant)` is *not*, because one
  parameter is not a tuple. Any fixed parameter in front makes all of them distinguishable again.
  It decides how many loose arities a generated vararg may have — four with a prefix, one without —
  and it will decide the same for anything else overloading a value form against an array form.
- **An overload set may hold at most one parameter from the *emptiable* family: `logic`, any option
  and any array** — and `string` is `[]char`, so it is in that family too. int+float, int+string,
  int+logic and logic+vector2 all compile; logic+string, logic+`[]int`, string+`[]int`,
  `[]int`+`[]float`, logic+`?int` and `?int`+`[]int` are all refused, at the *definitions*, and
  refused just as firmly inside a class or as extension methods on the receiver. The likely
  mechanism is that `false` is both a `logic` and the empty option, and an option is a 0-or-1
  array — one value inhabiting all three families is a call site that resolves none of them, the
  same shape of argument as `array{}` having no element type. Measured in
  `tests/verse_probe/variant_api_probe.verse`; **this is why there is no overloaded `AsVariant`**
  and why each Variant lane keeps its own `Variant<GodotType>`. What it does *not* forbid is a
  single builder that never overloads: **`MakeVariant[Value]` takes `any`**, so there is nothing to
  resolve, and the host reads the lane off what the value says about itself. It is `<decides>`
  because a tuple, a map, a class of the author's own and an empty array say nothing; it reaches
  int, float, logic, string, a Godot object and the 16 math structs. The five lanes that *share* a
  Verse type — StringName and NodePath with `string`, two integer packings, one float packing —
  can never be what it picks, and are what the named builders are still for. **`rid` used to be a
  sixth and is not**: it is its own struct now, names its own class, and `MakeVariant[SomeRid]`
  reaches it through its own arm in the host — a scalar under `VH_VARIANT_RID` rather than a math
  type's component array. It also retires
  `phase-4b-design.md` §15's claim that module-level overloading was to blame —
  `GodotMath.native.verse` overloads `Abs` across nine receiver types.
- **A reader is spelled on the receiver: `V.AsInt[]`, not `AsInt[V]`.** It is an extension method,
  because `variant` is hand-written and the readers are generated and Verse cannot reopen a class,
  and `<decides>` survives the desugaring. Each lane also keeps a **non-public** `VhUnpack<GodotType>`
  doing the tag check, because `typed_array` needs the reader as a function *value* in its `Unpack`
  member and an extension method's shape is receiver-plus-tuple. `VariantKind(V)` deliberately did
  not move: as `V.Kind()` it would put `Kind` in module scope, where a local of that name becomes
  ambiguous rather than shadowing.
- **The mirror's `Tag...` constants are not a script's to write.** `TagInt` and its 38 siblings
  carry no access specifier, so they are the mirror's own; a script naming one is glitch 3593,
  whose message is about control scopes. What a Godot property dictionary's `"type"` key wants is
  the generated `ToInt(:variant_type)`, which is `<public>` and is the closer analogue of the
  `TYPE_INT` a GDScript author writes.
- **An attribute may take only one argument, and both reasons are the toolchain's unfinished
  work rather than a rule.** An attribute site *references* its constructor before calling it, and
  Verse refuses to reference an overloaded function at all -- *"Referencing an overloaded function
  without immediately calling it is **not yet implemented**"*, naming every candidate. And
  `GetAttributeTextValue`, the only accessor there is, refuses any attribute whose argument is a
  `MakeTuple` -- every attribute of more than one argument -- under a comment reading `@HACK:
  SOL-972, We need full proper support for compile-time evaluation of attribute types`. So a
  multi-argument attribute takes **one string and splits it**: `@rpc("any_peer call_local")`,
  `@export_flags("Fire,Water,Earth")`. **Re-check this on every engine drop** -- when either fix
  lands, the several-argument spelling is the one to move to, and moving is additive because the
  one-string form keeps working beside it.
  **And `tests/verse_probe` cannot see any of this** -- a refused attribute comes back as
  status 4 with zero diagnostics, and a *user* package may not declare `class(attribute)` at
  all, so the attribute package cannot be checked in isolation either. The integration layer
  is where Godot's own diagnostic path prints it.
- **Verse has no doc-comment syntax, and `@doc` is a `using` away.** The parser knows `#`,
  `<# ... #>` and `<#>` and no documentation variant; what documents a declaration is the comment
  above it, which is what Epic's own generators read and what a library `@doc("...")` is rewritten
  into in a digest. A script may write `@doc("...")` itself with `using { /Verse.org/Native }`
  (`tests/verse_probe/doc_attribute_probe.verse`); without the `using` it is glitch 3506. The
  bridge reads the comment first and the attribute as the host's fallback, so either documents a
  member.
- **`operator'()'` is a reserved intrinsic.** Verse rewrites `Data[Key]` on a non-function callee
  into a call to it, but refuses to let anything *define* one — as a class member or as a free
  function — so the bracket syntax cannot be given a meaning. Container lookup is
  `Data.GetInt[Key]`.
- **A `var` with `<getter>`/`<setter>` may only be written two ways, and only one of them keeps the
  class constructible.** The compiler asks for *"either uninitialized or initialized with
  `= external{}`"*. `external{}` **is available outside a digest for this one case** — glitch 3558
  is raised only when the package's role is not External *and*
  `bAllowExternalMacroCallInNonExternalRole` is unset, and that flag is set for a member with
  accessors, under the comment *"optional accessors must be initialized with `= external{}`
  regardless of package role"* (`SemanticAnalyzer.cpp:16009` and `:20121`). The other spelling
  costs the class its archetype: an uninitialized member must be supplied by every archetype, so
  `some_class{}` becomes *"Object archetype must initialize data member `X`"*. **The trap is that a
  container-typed var is refused first** — `string` is `[]char`, so Verse asks for indexed accessor
  overloads (`XGetter(:accessor, :int):char`) — **and its failure then produces 3558 as a cascade**,
  which reads as `external{}` being banned outright. That is what generated bindings got wrong for a
  round (`generated-bindings.md` §10.9); a container property is the accessor pair, everything else
  is a member.
- **A `var` property cannot hold a nested struct or a container.** Verse asks a struct-typed `var`
  for a field-named accessor overload per nesting level, and `transform3d`'s two members have
  different types, so no one getter signature satisfies it. `gen_verse_api.py` leaves those as
  ordinary getter and setter methods; only the flat math types keep `set Node.Position = ...`.
- **A bare `logic` in an `if` clause list is evaluated and thrown away.** `if (X)` alone is refused
  — *"Expected an expression that can fail in the 'if' condition clause"* — but as soon as *some*
  clause can fail, a `logic`-valued one beside it is accepted and **not tested**, so the body runs
  either way with no diagnostic. Measured in `tests/verse_probe` after it produced a passing test
  that was counting twice (`by-hand-findings.md` B12). **The mirror no longer hands you one**: a
  Godot predicate is `<decides>`, so `Button.IsPressed[]` is the only spelling and the brackets
  cannot be forgotten. The trap is still live for a `logic` from anywhere else — a `var` of your
  own, a virtual's return, an accessor with a `set_` twin — and `X?` is what tests one.
- **Verse silently drops a continuation line that begins with an operator.** An expression written
  as `0.5 * ((A * 2.0)` then `+ B * W` on the next line compiles, runs, and answers *the first line
  only* — no diagnostic, no warning. Two of `GodotMath.native.verse`'s formulas answered 0.0 that
  way. Keep arithmetic on one line or bind a term at a time.
- **Verse's float `=` is reflexive for NaN**, unlike IEEE and unlike C: both `X = X` and `X <> X`
  answer "equal", so the usual NaN test never fires. What does distinguish NaN is that it is
  *unordered* against every other value — `NaN <= 1.0` and `NaN >= 1.0` both fail. Against itself
  `<=` and `>=` succeed, because each holds when `=` does (`docs/web-vm/facts.md`).
- **Float division is total** and answers `Inf`/`-Inf`/`NaN` exactly as C does; **integer division
  is `Quotient`, which is Euclidean** — `Mod` is always in `[0, |B|)` — where C and Godot truncate
  toward zero. So `Quotient[-3, 2]` is -2 where Godot's `-3 / 2` is -1, and `Quotient[7, -2]` is -3,
  not the -4 flooring would give (`docs/web-vm/facts.md`). `GodotMath`'s `TruncatedQuotient` is the
  bridge.
- **There is no `ToFloat`.** `X * 1.0` is the int-to-float conversion, and it works on a value and
  not only on a literal. `Floor`, `Ceil` and `Round` are `<decides>` and answer an `int`, so a
  float-valued floor is `if (V := Floor[X]) then V * 1.0 else X` — which is also the shape that
  makes `floor(inf)` agree with Godot.
- **Verse has no anonymous functions**, it forbids non-public struct fields (R-TYPE-7, which is why
  `variant`'s lanes are public), it cannot reopen a class, and its overloading is far narrower than
  `phase-2-design.md` §1 claimed. Verse **rejects mixed tabs and spaces**: Godot's script editor
  writes tabs and `.vscode/settings.json` matches, so keep `.verse` files tab-indented.
- When a language question comes up, **ask the compiler** — `tests/verse_probe` answers one in a
  minute, where reading `SemanticAnalyzer.cpp` does not.

## What cannot be tested from here

- **`_make_function`** is a `ScriptLanguageExtension` virtual with no ClassDB entry, and
  `Script.get_language()` is not in the public API, so GDScript can reach neither — the editor's own
  C++ is its only caller.
- **`_get_class_icon_path`** is the same shape: `Script::get_class_icon_path` is a pure virtual with
  no ClassDB entry and one caller, `EditorData::get_script_icon_path`. R-EXP-8's `@icon` is
  therefore asserted in the **units** layer, against `verse_scan_class_decl`, which is where the
  reading actually happens — and whether Godot *draws* it is a by-hand check.
- **`_CanDropData`** is the one Godot virtual that has never been exercised. (`_HasPoint` was in the
  same category and is now a case in `tests/integration`, because the engine asks it unprompted as
  soon as `Input.parse_input_event` supplies a click.)
- **Everything from `EngineDebugger` and the editor UI inward.** Everything from the ABI inward has
  `host_smoke` cases; the consumer half's only test is the by-hand session, whose steps are kept in
  `by-hand-findings.md` rather than retired. `tests/host_smoke/debug_probe.verse`'s line numbers are
  part of it — a member declared above line 22 moves an armed breakpoint.

Out by decision, so that a gap does not read as an oversight: Linux, `dlopen`, macOS, the
Shipping-per-template split (an export ships the Development host), and the debugger in an exported
game. `docs/roadmap.md`'s closing section records the rest of the deliberate omissions, including
that a release process needs a decision before Phase 8.

## Conventions

- C++20 on both sides (the `SConstruct` swaps godot-cpp's `c++17` out).
- godot-cpp style in `src/`: `p_` parameters, `r_` out-params, `_`-prefixed Godot virtuals, tabs.
- UE style in `host/`: `FName`, `bFlag`, Epic copyright header, tabs.
- Comments in this repo explain *why* and are dense where the reasoning is non-obvious — match the
  neighbouring file rather than the average.
