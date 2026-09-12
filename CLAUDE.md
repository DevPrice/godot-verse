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

**Phase 3 is designed and not started.** `docs/phase-3-design.md` is the whole of it: the decisions
and where they came from, the work order with per-stage code anchors, and — read this part first —
**§1, the spike that gates the phase.** Nothing in Phase 3 starts until OQ-12 is answered, because a
negative answer redesigns the module half rather than adding to it. Two things the roadmap said
about this phase are corrected there: modules are **not** one-per-subdirectory (a directory is a
module only if it carries a `<name>.vmodule` marker), and the leak gets a measured number rather
than a bound with a test.

**`docs/dodge-the-creeps.md` is the one to read before adding a Verse-facing feature.** The port
closed Phase 2 and it plays, so the document is not a progress report — it is the eight things a
Godot author writes without thinking that have no spelling yet, each with the requirement that will
give it one, measured in a real game rather than estimated. It also corrects two statuses that were
recorded as done: R-INT-2 (a script cannot make the `godot_array` `callv` needs) and the cost of
R-SCN-6's absence.

**README predates Phase 1 and is stale on marshalling.** It still describes three hand-written
value types, a `variant` tuple, `object` as the only `<native>` class, and packed arrays crossing as
copies — all four now wrong — and it says nothing about general dispatch, the method list, or
runtime errors with stacks. It is awaiting a rewrite rather than a patch. Two things it says that
*are* still true and read like they might not be: `Ready`, `Process` and `PhysicsProcess` remain the
only Godot **virtuals** the bridge carries (the full set is R-NODE-7, Phase 4), and `@GlobalScope` is
still out of reach (R-SCN-3, which Phase 2 moved to Phase 4 with OQ-11). Its editor-tooling, export and constraints sections are
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
| `verse_script_language.{h,cpp}` | the `ScriptLanguage`: `_validate`, the analysis cache, `_complete_code`/`_lookup_code`, `_frame` (which pumps `vh_tick` and reaps `vh_check_project_poll`) |
| `verse_resource_format.{h,cpp}` | load/save, without which a `.verse` cannot be attached to a node |
| `verse_lexer.{h,cpp}` | resumable per-line lexer; no godot-cpp dependency, so it is unit-testable standalone |
| `verse_class_decl.{h,cpp}` | scans a `.verse` file's top-level class and its `@global_class` attribute out of the text; defers comments and strings to the lexer, and shares its lack of godot-cpp |
| `verse_syntax_highlighter.*`, `verse_editor_plugin.*` | editor-only (`TOOLS_ENABLED`) |

`VerseScriptLanguage` overrides only the virtuals it actually answers — godot-cpp binds a virtual
with Godot only when the subclass declares it, so **omitting one is how you say "unsupported."**
Adding an override you do not implement changes behaviour.

### `host/` — the UE Program target

`Private/` is the ABI implementation (`VerseHost.cpp`, `HostRuntime`, `HostScript`, `HostEventLoop`,
`GodotBindings`, `GodotClasses`). `Verse/*.native.verse` is the `/Godot.org/Godot` package.

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
    python tools/build_bench.py           # host benchmark (timings, not pass/fail)

Run the tests:

    python tools/run_tests.py                    # all three layers; the one command (R-QUAL-3)
    python tools/run_tests.py --only units       # or one of units / abi / integration
    python tools/run_tests.py --build            # rebuild the test binaries first

It runs three layers and reports each: **units** (lexer, class-declaration scanner, generator —
no Godot, no UE), **abi** (`host_smoke`, the whole C ABI with no Godot), and **integration** — which
is two headless Godot projects, `tests/integration` for behaviour and `tests/coverage_diagnostic`
for the R-SCN-2 diagnostics. The second is its own project because its one script deliberately does
not compile, and one unresolvable name in the first would take every other case down with it. Its
assertions live in `run_tests.py` rather than in the project, because `ScriptLanguage` exposes
nothing a script can ask — so the only way to read what an author would see is to read what the
editor prints.

`tests/host_bench`, built by `tools/build_bench.py`, is not part of `run_tests.py`: it reports
timings rather than pass/fail, because R-PERF-2 asks for a recorded number and a threshold would
fail on a slower machine. It is what took the numbers in `phase-2-design.md` §3.1, and how to take
them again. A layer whose prerequisites
are absent is **skipped and said to be skipped**, never counted as a pass. `UE_ROOT` names the
Unreal checkout and `GODOT` the Godot binary; both are guessed when unset.

The binaries still run standalone, which is what to reach for when bisecting one failure:

    bin/host_smoke.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine .
    bin/verse_lexer_test.exe
    bin/verse_class_decl_test.exe
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
`godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd`, 29 checks, one
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
| `src/verse_keywords.h` | `tools/gen_verse_keywords.py` | the UE compiler's `ReservedSymbols.inl` |

**Every Godot class is mirrored by default.** `tools/verse_api_classes.txt` is a smaller curated
list kept for anyone who wants a smaller build, selected with `--classes-file`; there is no `--all`,
because all *is* the default. The reasoning is `docs/phase-2-design.md` §3: adding a class means
rebuilding `verse_host.dll`, which means a UE source checkout, so a subset is a wall rather than a
setting. It costs per-keystroke analysis latency, which is measured and recorded there.

`gen_verse_api.py`'s type table is the other half, and it no longer skips anything for a type it
cannot carry — `unsupported_type` is zero. Three small tables decide the awkward names, and each
says why in place: `VERSE_AMBIGUOUS_MEMBER_NAMES` (five names, compiler-confirmed, not guessed),
`PROPERTY_RENAMES` (`Min`/`Max` → `Minimum`/`Maximum`, the only invented names in the mirror) and
`FREE_FUNCTION_REPLACEMENTS` (`Object.to_string` is Verse's own `ToString`, which is also what
string interpolation desugars to).

`host/Verse/Godot.native.verse` and `GodotApi.native.verse` **are** hand-written: the first is the
whole native primitive surface, the second the ordinary-Verse packing layer above it. Mirroring
another Godot *class* still costs no C++ and no new native function — that rule held through ABI v2.
What did cost native functions was the reference types: the primitive surface went from 8 to 22,
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
  against generation N keeps generation N's class for life.
- **The host module never unloads.** `vh_shutdown` tears the engine down; the DLL stays resident.
- **Every Godot callback goes through `AutoRTFM::Open`,** and writes defer to `AutoRTFM::OnCommit`.
  The GDExtension was never instrumented by the AutoRTFM compiler, so calling into it from closed
  Verse code is a fatal "could not find function" at runtime, not a link error.
- **Calling *into* the VM must be open too.** `vh_instance_call` invokes through
  `VFunction::Invoke` inside an `AutoRTFM::Open` nested in its transaction, which is what
  `TVerseFunction::operator()` does for the same reason: a Verse runtime error raised from closed
  code trips `AutoRTFM::UnreachableIfClosed` in `FContext::RaiseVerseRuntimeError` and takes the
  process down instead of unwinding.
- **`operator'()'` is a reserved intrinsic.** Verse rewrites `Data[Key]` on a non-function callee
  into a call to it, but refuses to let anything *define* one — as a class member or as a free
  function — so the bracket syntax cannot be given a meaning. Container lookup is
  `Data.GetInt[Key]`.
- **A `var` property cannot hold a nested struct or a container.** Verse asks a struct-typed `var`
  for a field-named accessor overload per nesting level, and `transform3d`'s two members have
  different types, so no one getter signature satisfies it. `gen_verse_api.py` leaves those as
  ordinary getter and setter methods; only the flat math types keep `set Node.Position = ...`.
- **Every top-level name in the project must be unique.** The whole project shares one flat
  `/user@localhost` scope and Verse forbids shadowing. A script's class is named after its own file
  for exactly this reason. What a file may *also* declare beside that class is its own business —
  Phase 2's `derived_entity.verse` carries an interface, two structs, an enum and a parametric class
  alongside it — and a file with no class at all is a library file, usable from every sibling with
  nothing written to import it (R-LANG-6). *Phase 3 removes this too*: uniqueness narrows to
  per-module, a file may declare any number of top-level names, and only the class named after the
  file stays attachable to a node — which keeps `VerseScript::verse_class_name` reading the stem
  even after Verse stops requiring it. `docs/phase-3-design.md` §2.4. Still true today.
- **The attribute package must be added before the first `AddDataSource`.** `@global_class` is
  declared in a source package the host adds at runtime, not in `host/Verse` — VNI compiles that
  at build time and rejects `class(attribute)`. `FSolarisIde::EnsureDataSourcePackageExists`
  snapshots the project's other packages as the script package's dependencies exactly once, so a
  package added after the first script is never depended on and the attribute stops resolving.
  Authorship comes from a `IPreSemAnalysisInjection`, which must stay registered for the life of
  the process: `CProgramBuildManager::Build` resets the semantic program on every compile *and*
  every analysis, so a one-shot grant is gone by the first build.
- **Verse rejects mixed tabs and spaces.** Godot's script editor writes tabs; `.vscode/settings.json`
  matches that. Keep `.verse` files tab-indented.

## Conventions

- C++20 on both sides (the `SConstruct` swaps godot-cpp's `c++17` out).
- godot-cpp style in `src/`: `p_` parameters, `r_` out-params, `_`-prefixed Godot virtuals, tabs.
- UE style in `host/`: `FName`, `bFlag`, Epic copyright header, tabs.
- Comments in this repo explain *why* and are dense where the reasoning is non-obvious — match the
  neighbouring file rather than the average.
