# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Read first

`README.md` is the design document, not a quickstart. It carries the two-DLL rationale, the
`@editable` property-export story, the `_validate` threading story, and "Five constraints worth
knowing" — read the relevant section before changing anything in that area. `docs/property-export.md`
and `docs/editor-tooling.md` hold the full research and citations behind those sections.

This file is the map and the working rules; the reasoning lives there.

## Two binaries, one C header

    verse_host.dll    host/       UBT + AutoRTFM clang, monolithic UE Program target.
                                  Boots FEngineLoop, owns VerseVM, compiles and runs .verse.
    godot_verse.dll   src/        SCons + MSVC against godot-cpp.
                                  Loads the host, feeds it Godot callbacks, pumps it per frame.

`include/verse_host_abi.h` is the only thing that crosses. Plain C — the two sides cannot share a
C++ ABI. It is staged into the host's `Public/` by `build_host.py`, so both compile the same file.

A change to that header means bumping `VH_ABI_VERSION` and rebuilding **both** sides: the mismatch
surfaces at `vh_init`, not at compile time.

### `src/` — the GDExtension

| file | owns |
| --- | --- |
| `register_types.cpp` | registration order: language before resource loader |
| `verse_host.{h,cpp}` | `GetProcAddress` loader over the ABI; no Verse logic |
| `verse_runtime.{h,cpp}` | the `VerseRuntime` singleton — `vh_init_desc`, the Godot callback table, `verse/host/*` project settings |
| `verse_value.{h,cpp}` | `Variant` ⇄ `vh_value`, arena-allocated |
| `verse_script.{h,cpp}` | a `.verse` file as a Godot `Resource`; valid only if it defines its own class |
| `verse_script_instance.{h,cpp}` | one script bound to one node; raw `GDExtensionScriptInstanceInfo3` vtable, not a `godot::Object` |
| `verse_script_language.{h,cpp}` | the `ScriptLanguage`: `_validate`, the analysis cache, `_frame` (which pumps `vh_tick` and reaps `vh_check_project_poll`) |
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
`VerseHost.Build.cs` and `.Target.cs` carry load-bearing comments — `SetupVerse(..., InternalUser)`
and the `VerseSimulationMetadata` dependency each exist for a reason spelled out inline.

## Commands

    python tools/build_host.py            # stages host/ into the UE tree, runs UBT
    scons target=editor                   # the GDExtension (also: target=template_debug)
    python tools/gen_verse_api.py         # regenerates the Verse mirror of Godot's API
    python tools/build_smoke.py           # ABI test binary
    python tools/build_lexer_test.py      # lexer test binary
    python tools/build_class_decl_test.py # class-declaration scanner test binary

Run the tests:

    bin/host_smoke.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine .
    bin/verse_lexer_test.exe
    bin/verse_class_decl_test.exe
    python tests/verse_api_gen/test_gen_verse_api.py

No test framework anywhere. Each test is a `main` (or a plain script) that prints one line per case
and exits non-zero on failure; keep new tests that shape.

`tools/build_host.py` needs a UE source checkout with the Verse toolchain — `--engine`, or `UE_ROOT`.
Building the host and running the tests are fine to do unprompted. **Ask before launching Godot**
(`godot --path demo`) — it opens a window on the user's machine.

## Generated files — never hand-edit

| generated | by | from |
| --- | --- | --- |
| `host/Verse/GodotClasses.native.verse` | `tools/gen_verse_api.py` | `godot-cpp/gdextension/extension_api.json` |
| `src/verse_api_classes.h` | `tools/gen_verse_api.py` | same |
| `src/verse_keywords.h` | `tools/gen_verse_keywords.py` | the UE compiler's `ReservedSymbols.inl` |

`tools/verse_api_classes.txt` is the hand-maintained list of which Godot classes get mirrored;
edit that and regenerate, or pass `--all`. `gen_verse_api.py`'s type table is the other half —
a Godot type absent from it is a method the generator skips.

`host/Verse/Godot.native.verse` and `GodotApi.native.verse` **are** hand-written: the first is the
whole native primitive surface, the second the ordinary-Verse packing layer above it. Mirroring
another Godot class should cost no C++ and no new native function.

## Constraints that break things silently

- **The host must load from `Engine/Binaries/Win64`.** VNI records each Verse package's source
  directory relative to the loaded module and the compiler reads those `.verse` files at runtime.
  A copy elsewhere compiles against an empty package set and every identifier is unknown.
  `bin/verse_host.dll` exists for the smoke test only; Godot points at the engine tree through the
  `verse/host/dll_path` project setting.
- **One code generation per process.** `vh_compile_project` publishes a package and may run once;
  the first script that needs compiling builds every `.verse` under `res://`. Scripts added while
  the editor runs are not picked up until restart. Analysis (`vh_check_project*`) has no such limit.
- **The host module never unloads.** `vh_shutdown` tears the engine down; the DLL stays resident.
- **Every Godot callback goes through `AutoRTFM::Open`,** and writes defer to `AutoRTFM::OnCommit`.
  The GDExtension was never instrumented by the AutoRTFM compiler, so calling into it from closed
  Verse code is a fatal "could not find function" at runtime, not a link error.
- **One top-level name per file.** The whole project shares one flat `/user@localhost` scope and
  Verse forbids shadowing. A script's class is named after its own file for exactly this reason.
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
