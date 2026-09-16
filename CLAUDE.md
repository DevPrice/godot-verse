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
register a Godot class name, and the staged plan that follows from it. Part of it is written and
unbuilt, and the section says which part.

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

Two documents are not phase records and are the ones to read before adding a feature:

- **`docs/dodge-the-creeps.md`** — the eight things a Godot author writes without thinking, each
  measured in a real game rather than estimated, with the requirement that gives it a spelling.
  Seven of the eight are down; the table says which, and §"After Phase 4"/§"After Phase 5" say what
  each diff came to. The one standing is the `<transacts>` trap (wall 8), narrowed twice and not
  removed.
- **`docs/by-hand-findings.md`** — what the by-hand editor sessions found, because everything from
  `EngineDebugger` and the editor UI inward has no automated test and never will. B1–B9 and B15–B18
  are defects, all fixed; B12 is a Verse fact; B13 a latency finding; B14 the sandboxed export run.
  Its "What is still open" section is where the two remaining by-hand checks live.

`docs/nonatomic-methods.md` is generated — R-AUD-3's list of the 1132 emitted methods whose
`<transacts>` promises a rollback the bridge cannot perform.

## Two binaries, one C header

    verse_host.dll    host/       UBT + AutoRTFM clang, monolithic UE Program target.
                                  Boots FEngineLoop, owns VerseVM, compiles and runs .verse.
    godot_verse.dll   src/        SCons + MSVC against godot-cpp.
                                  Loads the host, feeds it Godot callbacks, pumps it per frame.

`include/verse_host_abi.h` is the only thing that crosses. Plain C — the two sides cannot share a
C++ ABI. It is staged into the host's `Public/` by `build_host.py`, so both compile the same file.

**`VH_ABI_VERSION` is 8.4.** It is `MAJOR * 1000 + MINOR`, with the policy at the top of the header:
a major bump is a layout or meaning change and both sides must be rebuilt; a minor bump adds
something an older consumer can ignore behind a `StructSize` check. A change to the header means
bumping it and rebuilding **both** sides — the mismatch surfaces at `vh_init`, not at compile time.
**A callback added at a minor must be cleared past the consumer's own `StructSize`**: `vh_init`
copies the whole `vh_godot_api` out of the descriptor, so everything past what a consumer built at a
lower minor actually wrote is that consumer's stack, not a null pointer, and "check the pointer
before calling" would pass. `InitHost` zeroes the tail; nothing before 8.3 needed it.

`vh_host_kind()` is readable before `vh_init` and answers editor, runtime or cooker; the eleven
compiler-side entry points answer `VH_ERR_UNSUPPORTED` in a runtime host.

### `src/` — the GDExtension

| file | owns |
| --- | --- |
| `register_types.cpp` | registration order: language before resource loader |
| `verse_host.{h,cpp}` | `GetProcAddress` loader over the ABI; no Verse logic |
| `verse_host_paths.{h,cpp}` | where this machine's Unreal checkout, host DLL and cooker are: environment, then EditorSettings, then the legacy project settings (R-DIST-12) |
| `verse_runtime.{h,cpp}` | the `VerseRuntime` singleton — `vh_init_desc`, the Godot callback table, `verse/host/enable_debugger`, and finding `verse_data` in an export |
| `verse_value.{h,cpp}` | `Variant` ⇄ `vh_value`, arena-allocated |
| `verse_ref_table.{h,cpp}` | the id → `Variant` table the `Ref` lane names: Array, Dictionary, Callable, Signal and the packed arrays, which cross as references rather than copies |
| `verse_callable.{h,cpp}` | the mirror image: a Godot `Callable` that calls a Verse function. Only a function **bound to a script instance** is accepted, which is the half of Godot's own design that does not leak (GH-102327) |
| `verse_script.{h,cpp}` | a `.verse` file as a Godot `Resource`; valid only if it defines its own class |
| `verse_script_instance.{h,cpp}` | one script bound to one node; raw `GDExtensionScriptInstanceInfo3` vtable, not a `godot::Object` |
| `verse_script_language.{h,cpp}` | the `ScriptLanguage`: `_validate`, the analysis cache, `_complete_code`/`_lookup_code`, `_frame` (which pumps `vh_tick`, reaps `vh_check_project_poll` and attaches the debugger), and the `_debug_*`/`_profiling_*` surface |
| `verse_resource_format.{h,cpp}` | load/save, without which a `.verse` cannot be attached to a node |
| `verse_lexer.{h,cpp}` | resumable per-line lexer; no godot-cpp dependency, so it is unit-testable standalone |
| `verse_class_decl.{h,cpp}` | scans the top-level class **named after the file** and its `@global_class` attribute out of the text; defers comments and strings to the lexer, and shares its lack of godot-cpp |
| `verse_module_map.{h,cpp}` | which module each `.verse` is in, from the `.vmodule` markers; pure, and the third godot-cpp-free unit |
| `verse_export_plugin.{h,cpp}` | editor-only: runs `verse_cook.exe` over the project, strips every `.verse` to a one-byte stub so `ext_resource path=` still resolves, and refuses a platform this bridge does not reach |
| `verse_export_paths.{h,cpp}` | the one rule for where a game's cooked Verse lives — `verse_data` beside the executable — shared by the export plugin that creates it and the runtime that finds it |
| `verse_module_menu.{h,cpp}` | editor-only: "Make Verse Module" in the FileSystem dock, because Godot's dock cannot create an empty file |
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
knows either exists; `GodotBindings` and `GodotClasses` are the native Verse surface. The cooked
path is `HostCook`/`HostCookWriter` (cooker only, behind `VH_HOST_KIND == VH_HOST_KIND_COOKER`),
`CookMain.cpp` (the cooker's `main`), `HostCooked` (mount points and load, in the runtime host) and
`HostSidecar` (the analysis snapshot serialised, which is what a host with no semantic program reads
instead of sources). `Verse/*.native.verse` is the `/Godot.org/Godot` package.

`GodotClasses.h` holds every C++ shadow a `<native>` Verse declaration needs, and there are three:
`vh_object` (a UObject, so a script's class has one to be instantiated and called through), `variant`
(a struct, the fixed-width lanes one Godot value crosses as) and `godot_ref` (a UObject whose
`BeginDestroy` is what releases a reference id when Verse drops the value holding it).

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
    python tools/build_bench.py           # host benchmark (timings, not pass/fail)
    python tools/build_verse_probe.py     # the Verse probe (asks the compiler a question)
    python tools/build_cooked_probe.py    # the cooked probe (asks a runtime host what an export sees)

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

    python tools/run_tests.py                    # all four layers; the one command (R-QUAL-3)
    python tools/run_tests.py --only units       # or units / abi / integration / export
    python tools/run_tests.py --build            # rebuild the test binaries first

**units** — lexer, class-declaration scanner, module map, generator. No Godot, no UE.

**abi** — `host_smoke`, the whole C ABI with no Godot, plus a `verse_cook` case that cooks
`tests/host_smoke`'s fixtures and asserts the packages, the container and the sidecar.

**integration** — two headless Godot projects. `tests/integration` for behaviour;
`tests/coverage_diagnostic` for the R-SCN-2 diagnostics, which is its own project because its one
script deliberately does not compile and one unresolvable name in the first would take every other
case down with it. Both projects' assertions live in `run_tests.py` rather than in the project,
because `ScriptLanguage` exposes nothing a script can ask — the only way to read what an author
would see is to read what the editor prints.

**A `_validate` warning is not something the editor prints**, which is the trap in that sentence: it
is returned to the editor's own C++ for the gutter and the warnings panel, and reaches no log, so
`coverage_diagnostic` cannot assert one. The diagnostics it *does* assert are compile errors and
`report_name_collisions`' `push_warning`/`push_error`. A diagnostic that has to be both seen at a
line and asserted needs two reporters over one message function — `inert_global_class_message` is
the worked example.

**export** — exports `tests/integration` headless, asserts the *tree* it produced, then **launches
it** and asserts what its cases reported: 346 passed, 0 failed, 10 skipped, with the counts named in
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
| `src/verse_keywords.h` | `tools/gen_verse_keywords.py` | the UE compiler's `ReservedSymbols.inl` |
| `bin/host_build_id.gen.h` | `tools/build_host.py` | the staged host sources themselves — a digest of `host/` plus the ABI header, and the engine commit beside it — staged into the host's `Private/` and baked into every host binary, so a cooked sidecar and the host reading it can be told apart. A digest rather than `HEAD` so a doc commit does not invalidate three binaries. Not committed |

**What the mirror is**, since no single file shows it: all 1036 Godot classes as a Verse class
hierarchy, Godot's own `Object` among them; its 793 enums as real Verse enums; all 1413 of
`extension_api.json`'s virtuals, spelled Godot's way (74 skipped with a recorded reason); properties as writable members rather than get/set pairs; 503 engine-signal
accessors; `@GlobalScope`'s constants and statics reachable through per-class `...Statics` modules;
the 16 math types with methods and definable operators in ordinary Verse; and `typedarray::Node` as a
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
- The numbers this bought are in `docs/spec.md` R-PERF-2 with the machine they were taken on: a
  whole-project analysis is **721 ms** where it was 1273, a generation **1.54 s** where it was 2.2,
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
- **An archetype instantiation carries the constructing class's own effect.** `variant{Tag := ...}`
  inside a `<reads>` or `<computes>` function is *"This archetype instantiation constructs a class
  that has the 'transacts' effect"* unless the class says `<computes>`. `variant`, `godot_ref` and
  every container wrapper do; a **mirrored Godot class cannot**, because it descends from the native
  `vh_object`, so anything a narrowed body needs must be reached by a *cast* over what the host
  built rather than by construction — which is what the singleton accessors do
  (`GetInput()` is `input[VhObjectOf(VhSingleton["Input"])]`), and what R-SCN-6 says they should
  always have done.

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
- Verse's own `signalable`/`subscribable` cannot be implemented here — their domains are
  `no_rollback` and every Godot callback runs in a transaction — so `signal` has their *vocabulary*
  and not their interfaces.

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
  alone could describe is recorded and carried in the sidecar (version **4**): the declared types of
  every member, method and signal, **whether a member is `var`** (without which every write an
  exported game made to its own state was silently dropped), the payload of all 503 mirrored
  engine-signal accessors (without which `Timer.Timeout().Await()` connects and never resumes), and
  the cooked package list — a container holds package *ids*, which are hashes, and mount points are
  still registered by name.
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
- **Godot never creates the destination directory of an export**; it must already exist, or
  `prepare_template` answers *"The given export path doesn't exist"*
  (`editor_export_platform_pc.cpp:156`). That message also means the working-directory trap above.
- **Never put a comment in a `project.godot` or in a `dodge-the-creeps/export_presets.cfg`.** The
  editor parses and rewrites both and does not preserve comments — anything explanatory there is
  deleted the next time the project is opened, which for the yardstick is often. Those two files are
  **editor-owned**: read the diff before committing them, and blank `export_path` in the preset,
  which the editor fills in with whatever directory you last exported to.

### Modules and names

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
  `extension_api.json`, so `_Notification` is hand-written on the native root and the rest of that
  family is R-NODE-10, unbuilt.
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
  imports the Godot package, which is every script. **`event` belongs on that list too** and is the
  one met first: the mirror spells `_Input`'s parameter `Event`, and an override that writes `event`
  collides with `/Verse.org/Verse`'s own `event` — glitch 3532, reported at the parameter with no
  hint that a capital letter is the fix (`by-hand-findings.md` B2). `gen_verse_api.py`'s
  `VERSE_STDLIB_NAMES` keeps the generated mirror clear of them; a script has to avoid them by hand,
  the way it already avoids `Abs` and `Clamp`.

## Verse itself: what the compiler does that surprises

- **`operator'()'` is a reserved intrinsic.** Verse rewrites `Data[Key]` on a non-function callee
  into a call to it, but refuses to let anything *define* one — as a class member or as a free
  function — so the bracket syntax cannot be given a meaning. Container lookup is
  `Data.GetInt[Key]`.
- **A `var` property cannot hold a nested struct or a container.** Verse asks a struct-typed `var`
  for a field-named accessor overload per nesting level, and `transform3d`'s two members have
  different types, so no one getter signature satisfies it. `gen_verse_api.py` leaves those as
  ordinary getter and setter methods; only the flat math types keep `set Node.Position = ...`.
- **A bare `logic` in an `if` clause list is evaluated and thrown away.** `if (X)` alone is refused
  — *"Expected an expression that can fail in the 'if' condition clause"* — but as soon as *some*
  clause can fail, a `logic`-valued one beside it is accepted and **not tested**, so the body runs
  either way with no diagnostic. `if (Button := input_event_mouse_button[Event], Button.IsPressed())`
  runs on the release as well as the press; `Button.IsPressed()?` is the spelling that tests it.
  Measured in `tests/verse_probe` after it produced a passing test that was counting twice.
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
