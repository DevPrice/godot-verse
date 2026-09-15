# Phase 7 — Export, and the platforms it reaches

**Status:** Draft 1 · 2026-09-14 · **designed, not built.** Written *before* the work, the way
Phase 4.5's, 5's and 6's were, from an interview and three delegated investigations held against
the Godot and Unreal sources directly. **§13 is empty and is where the implementing agent writes
what turned out wrong**; until then §1 and §2 are the record. One spike (S-1, OQ-10) ran during the
planning and its answer is in §2.

**Prerequisite: Phase 6 is complete** — built 2026-09-14, ABI **8.1**. Phase 4b (custom
Resources, autoloads, `RefCounted` instantiation, `_Get`/`_Set`) is **not** built and is not in
this phase; it stays owed before 1.0 (`roadmap.md` "Phase 4b").

**Companion to:** `spec.md` §2 (R-DIST-8 … R-DIST-11), §3 (R-PLAT-1 … R-PLAT-5), §14's **OQ-2**
(closed, and the reason this phase has the shape it has), **OQ-10** (answered here, §2 S-1), OQ-3
and OQ-4 (deferred, §14); `roadmap.md` "Phase 7" and "Phase 7.5"; `phase-0-spikes.md` S-1 (the
cook needs an editor-class binary — the finding this whole phase rests on).

---

## 0. How to read this

**§1 is the decisions.** D1–D22 came out of one interview and three delegated reads of the engine
sources; every row says what it rests on. Do not relitigate them; if a spike contradicts one,
record it in §13 and raise it.

**§2 is the spikes.** S-1 ran during planning and is answered. S-2 … S-7 are the implementing
agent's first work, in that order, because each decides something a later stage builds on. None
of them is optional, and a stage that starts before its spike has an answer will be built twice.

**§3 is what the two engines actually offer**, as found rather than as remembered, with the file
and line each fact was read from. It is the factual base under every stage.

**§4–§10 are the stages**, in build order. Each names the files it touches and what "done" is.
**§11 is the ABI delta**, **§12 the tests**, **§14 what is deliberately not built**, and **§15
the exit**.

**§13 is not written.** When the phase is built, it is where the design is corrected.

---

## 1. Decisions

| # | decision | why, and what it rests on |
| --- | --- | --- |
| D1 | **Phase 7 is export and the desktop platforms. 4b stays deferred.** | The roadmap put 4b before 7 ("after parity"); the interview decided export is the next thing worth having and 4b shares nothing with a cooker. 4b is owed before 1.0 and `roadmap.md` says so. |
| D2 | **Three UBT targets over one `host/` source set**: the editor host (today's `verse_host.dll`), a **cooker** (`verse_cook.exe`, editor-class, runs only at export), a **runtime host** (`verse_host_runtime.dll`, `WITH_VERSE_COMPILER=0`, ships with the game). | `phase-0-spikes.md` S-1: a cooking save asserts `WITH_EDITOR` and needs an `IPackageWriter` (`SaveContext.cpp:63`); a target without editor-only data gets `WITH_VERSE_COMPILER=0` and loads Verse from cooked `.uasset` (`Solaris.Build.cs:47-82`). Nothing has changed since. |
| D3 | **The cooker is an executable the export plugin runs as a subprocess**, not a DLL the editor loads. | Two `FEngineLoop`s in one process has never been tried and has no reason to be; a subprocess's stdout is the export log. `UnrealAssetStringify.Target.cs` is the model: a console Program with `bCompileAgainstEditor`. |
| D4 | **The cooker compiles against Engine as well** (`bCompileAgainstEngine = true`). | S-1 (§2): with Engine merely dragged in and `WITH_ENGINE=0`, UHT cannot resolve `UWorld`/`APlayerController` for the three `Within=` headers; with `WITH_ENGINE=1` it parses. Weight is not an objection for a binary that never ships and never loads in the editor. `ChaosVisualDebugger.Target.cs:22-24` sets exactly these three flags. |
| D5 | **One ABI header, one loader, a required/optional split, and a host *kind* readable before init.** `vh_host_kind()` answers `VH_HOST_EDITOR`/`RUNTIME`/`COOKER`; the compiler-side entry points answer `VH_ERR_UNSUPPORTED` in a runtime host; the loader tolerates their absence; a wrong-kind host is refused with one sentence naming the kind and the fix. ABI **8.2**, minor. | Delegated proposal, accepted. ~1200 of the header's 1672 lines are shared vocabulary the runtime host needs *more* than the editor host (`vh_value`, the callback table, `vh_tick_stats`, the debug structs); a second header duplicates them and drift is a silent miscompile (header:42-43). All 37 ABI calls live in `src/verse_runtime.cpp` and none is under an `#ifdef`, so two loaders buy nothing. |
| D6 | **Cooked packages and the sidecar live in a data directory beside the executable**, `verse_<app>_<platform>_<arch>/` (`Contents/Resources/…` on macOS), emitted with `add_shared_object`. **The runtime host DLL is a `.gdextension` `[dependencies]` entry** per template, so the existing GDExtension export copies it. | Delegated proposal, accepted. This is .NET's layout exactly: `ExportPlugin.cs:250-262, 419-423` (`data_<csproj>_<platform>_<arch>`), `godotsharp_dirs.cpp:224-231` (resolved from the executable path, bundle fallback on macOS). `add_shared_object` copies a directory recursively, after the PCK, embedded or not (`editor_export_platform_pc.cpp:230-256`). A `.gdextension` dependency is per feature tag and becomes the same `SharedObject` (`gdextension_library_loader.cpp:42-66`). |
| D7 | **The data directory is also the host's engine directory.** `vh_init` receives `<data>/Engine`; UE accepts any directory with a `Binaries/` child as `GForeignEngineDir` (`GenericPlatformMisc.cpp:1408-1415`). What else `PreInit` needs under it is S-3's to find. | The editor host already boots against a foreign engine dir (`VerseHost.cpp:189-192`); a shipped game cannot point at a checkout, so it points at itself. |
| D8 | **`verse/host/dll_path` and `verse/host/engine_dir` are editor-only.** An exported build derives both from the executable path and never reads the settings. | They hold one machine's absolute paths (`dodge-the-creeps/project.godot` ships them today). `OS::has_feature("template")` is the test. |
| D9 | **The class shape ships as a sidecar the cooker writes; it carries no export defaults.** The sidecar is the analysis *snapshot*, serialised — the same struct every class-describing read already answers from — so the runtime host loads it and the seven snapshot-fed entry points keep their bodies. | Prior art, delegated read: at scene load Godot asks a script only `can_instantiate` and `instance_create`, then applies stored values by name through `set` (`object.cpp:1049-1076`, `packed_scene.cpp:477-508, 572`); the property list is asked lazily. Neither shipping language precomputes a shape into its export, and both answer "no default" outside `TOOLS_ENABLED` (`gdscript.cpp:395-409`, `csharp_script.cpp:2629-2644`) — defaults come from the initializer, which a cooked class still runs. The snapshot already exists (`TakeAnalysisSnapshot`, `HostScript.cpp`), so serialising it is the smallest sidecar there is. |
| D10 | **`.verse` sources are stripped from the export and a one-byte stub ships at each `res://` path.** | The C# pattern: `ExportPlugin.cs:120-153` writes `"\n"` and `Skip()`s the real file so `ext_resource path="res://player.cs"` still resolves. A `.verse` stub loads as a `VerseScript` whose class the runtime host already holds. |
| D11 | **The runtime host keeps Phase 6's debugger and profiler** in the debug template. | `HostDebug` rests on `Verse::FDebugger`, VM-side, and `Solaris` allows debugging under `WITH_EDITOR \|\| GIsServer` only (`SolarisModule.cpp:505-511`) — S-2 finds out whether the runtime host needs `AllowDebugging(true)` of its own. |
| D12 | **Runtime host configuration: Development for the debug template, Shipping for the release template.** Editor host and cooker stay Development. | Interview. Shipping has never been run; §12 makes the release export the first time it is. `bUseLoggingInShipping = true` is already set (`VerseHost.Target.cs:36`). |
| D13 | **Godot 4.7 official is the editor and the templates.** Stage 0 moves godot-cpp's API dump from 4.6 to 4.7 and regenerates the mirror; `compatibility_minimum` becomes `4.7`. | `tools/run_tests.py:33-36` already guesses `C:\Apps\Godot_v4.7-stable_win64.exe\…console.exe` and the 4.7.stable templates are installed for every desktop platform and web; `../godot` has no `bin/` and no templates. `godot-cpp/gdextension/extension_api.json` says `4.6.stable`. |
| D14 | **Exit for this machine: Windows exports and runs; Linux is built and attempted, unverified; macOS is written as blocked on hardware.** R-PLAT-1 stays MUST at status *part*. | Interview: Windows plus WSL2, no Linux cross-toolchain yet, no Mac. §9 and §10 say what each needs. |
| D15 | **An automated `export` layer in `run_tests.py` exports *both* `tests/integration` and `dodge-the-creeps` headless and runs the results.** The first is the coverage, the second is the exit. Skipped-and-said-so when the cooker, the runtime host or the templates are absent. | R-QUAL-3's rule. An exported template accepts no `-s` (`main.cpp:4116-4120`, `TOOLS_ENABLED`), so each project gets an autoload that runs the checks on a user argument. |
| D16 | **Mobile is deferred, with no design.** R-PLAT-2 keeps its status; OQ-3 stays open. **Web becomes Phase 7.5**, a roadmap entry and nothing more. | Interview. Neither gates 1.0 (`roadmap.md`). R-PLAT-4 is what this phase owes them: an export to `android`, `ios` or `web` fails at export with a sentence. |
| D17 | **R-DIST-3, R-DIST-4 and R-DIST-5 are not in this phase.** D5's kind check closes the "wrong host" half of R-DIST-5 as a side effect and the status line says so; the rest stays where the roadmap has it. | Interview. |
| D18 | **The spike that could reshape the phase ran during planning; the rest are stage 0 of the build.** | Interview. S-1 is a multi-hour UBT build and was the one whose "no" changed everything. |
| D19 | **The cooker keeps every module the editor host links, and adds the ones an editor `Launch` would.** The exclusion of `VerseSimulationMetadata` and `VerseSpatialMath` that S-1's builds 2 and 3 tried was a wrong turn — it did not fix UHT, and under D4 `UnrealEd` is linked anyway — and build 5 restored them. What the cooker *adds* is `Launch.Build.cs`'s engine and editor blocks, under `Target.bCompileAgainstEditor` in `VerseHost.Build.cs`, because the program-main include compiles `LaunchEngineLoop.cpp` into the host's own module. | S-1, §2. A script that imports `/Verse.org/Simulation` or `/Verse.org/SpatialMath` must cook the way it compiles. |
| D20 | **The cooked package name is deterministic: generation 1.** The cooker is a fresh process, `CompileProject` publishes `GodotScripts_1`, and the runtime host looks that name up. | `HostScript.cpp:94` (`ScriptPackageBaseName`), `:906-908` (the name is `<base>_<generation>`). The UPackage path is `/GodotScripts_1/_Verse` (`VVMNames.cpp:342, 385`). |
| D21 | **In-PCK-with-extraction is held in reserve; a custom `IPlatformFile` is rejected.** | Delegated proposal. The first is .NET's `embed_build_outputs` (`ExportPlugin.cs:245, 403-431`; `godotsharp_dirs.cpp:182-221`) and is the only path to a single-file export — copyable later. The second has no precedent in Godot and costs an ABI surface. |
| D22 | **No `RequiredHostKind` field in `vh_init_desc`.** The consumer reads `vh_host_kind()` before init and writes the better sentence itself. | Delegated proposal. Adding a field to earn a worse message is not worth the `StructSize` dance. |

---

## 2. Spikes

### S-1 · Can an editor-class UBT Program target be built? (OQ-10) — **run during planning**

**Method.** `host/VerseHostCooker.Target.cs` derives from `VerseHostTarget` and adds the editor
flag; `tools/build_host.py --target VerseHostCooker` stages and builds it (the `--target` switch is
new). Thirteen builds, each answering one thing:

| build | change | result |
| --- | --- | --- |
| 1 | `bCompileAgainstEditor = true`, nothing else | **UHT fails in 7 s**: `NavigationSystem.h(291)`, `LevelStreaming.h(137)`, `CheatManager.h(97)`, all *"Within class 'World'/'PlayerController' not found"* — the same three headers the earlier attempt recorded as "Engine module links". |
| 2 | drop `VerseSimulationMetadata` from the cooker | same three errors |
| 3 | drop `VerseSpatialMath` too (it depends on the first) | same three errors; the module set falls from 534 to 520 and `Engine`, `UnrealEd`, `NavigationSystem` are still in it, unreachable by any *link* edge from `VerseHost` |
| 4 | **`bCompileAgainstEngine = true`** as well | **UHT passes**; 992 compile actions follow, and **991 succeed**. The one failure is the host's own `VerseHost.cpp` at 937 s: `RequiredProgramMainCPPInclude.h` textually compiles `LaunchEngineLoop.cpp`, which under `WITH_EDITOR && WITH_ENGINE` includes `EditorCommandLineUtils.h` from `UnrealEd` (`LaunchEngineLoop.cpp:126-134`) — and an editor `Launch` gets that module from its own `bBuildEditor` block (`Launch.Build.cs:208-216`), which a Program's launch module has to repeat itself, as `ChaosVisualDebugger.Build.cs` does. |
| 5 | `VerseHost.Build.cs` lists Launch's engine and editor modules under `Target.bCompileAgainstEditor`; the two `/Verse.org` modules restored (D19) | Two new failures, both instructive. `LaunchEngineLoop.cpp:221` wants `AppMediaTimeSource.h` (`MediaUtils`) — one more of the modules `ChaosVisualDebugger.Build.cs` lists, so the fix is to copy that list whole rather than add a header at a time. And `Landscape.cpp:2462` and `EditorEngine.cpp:7090` call `IConsoleVariable::GetPlatformValueVariable` and `UDeviceProfileManager::GetPreviewDeviceProfileSelectorModule`, which exist only under **`ALLOW_OTHER_PLATFORM_CONFIG=1`** — the define build 1 had set to 0 on AutoRTFMTestsWithEditor's advice; that target links neither module, and UnrealAssetStringify's "required by IConsoleManager.h" was the right comment to follow. |
| 6 | ChaosVisualDebugger's module list under the flag; `ALLOW_OTHER_PLATFORM_CONFIG=1` | **Every module compiles.** The link fails on the spike's own shape: `lld-link: error: too many exported symbols (got 143570, max 65535)` — a monolithic editor-class *DLL* exports every module's `*_API` symbol. D3 said the cooker is an executable; this is the link saying so. |
| 7 | `bShouldCompileAsDLL = false`, `bHasExports = false`, a stub `INT32_MAIN_INT32_ARGC_TCHAR_ARGV()` in `host/Private/CookMain.cpp` under `#if WITH_EDITOR` | **The link succeeds up to the PDB**: `lld-link: error: Output data is larger than 4 GiB. File size 4,663,934,976 too large for current PDB page size 4096`. UBT raises the page size to 16384 on its own only for a monolithic *Editor* target (`UEBuildWindows.cs:1309-1311`); `UnrealConsole.Target.cs` sets `WindowsPlatform.PdbPageSize` by hand for exactly this. |
| 8, 8b | `WindowsPlatform.PdbPageSize = 16384` | **Killed for memory, twice**, during the link, on a 62 GB machine with 42 GB free and no stray linker alive — lld-link writing a 4.6 GB PDB for a monolithic editor-class exe is the cost. |
| 9 | `bDisableDebugInfo = true` | **Links.** `verse_cook.exe`, 710 MB, 518 s for the full recompile plus link. OQ-10's literal question is answered here: yes. |
| 10, 11 | a `main` that calls `PreInit` with the editor host's boot line | Runs 1.3 s and dies: *"Unreal Engine games require a project file as the first parameter."* Under `WITH_EDITOR && WITH_ENGINE`, `PreInit` demands a project unless it is running as the editor or as a commandlet (`LaunchEngineLoop.cpp:2715-2725`). |
| 12 | the literal token `EDITOR` appended to the command line, as `ChaosVisualDebugger.cpp:51` does | Past the project check; 5.2 s in, `FShaderCompilingManager`'s constructor `LoadModuleChecked`s `ShaderPreprocessor` unconditionally (`ShaderCompiler.cpp:789-790`), and `PreInit` constructs the manager whatever `-NoShaderCompile` says (`LaunchEngineLoop.cpp:3267-3282`). The module is a developer tool, and the editor host has `bBuildDeveloperTools = false`. |
| 13 | `bBuildDeveloperTools = true` | 561 s, 768 MB. Boots to the shader-worker launch (`ShaderCompileWorker.exe`, not built here); with **`-NoShaderCompile -nullrhi`** on the command line it reaches `engine booted (WITH_EDITOR=1, WITH_ENGINE=1)` in **2.4 s** wall. Both flags are baked into `CookMain.cpp`. |
| 14–17 | the entry point marked `AUTORTFM_DISABLE`, teardown moved to `GEngineLoop.Exit()` | **The boot is clean and the teardown is not** — see "what is still open" below. |

**Answer: yes.** An editor-class UBT Program target builds, links and boots, as `verse_cook.exe`
— provided it is an *executable*, compiles against Engine as well as the editor, carries
developer tools, lists the modules an editor `Launch` would, sets `ALLOW_OTHER_PLATFORM_CONFIG=1`,
and boots with the `EDITOR` token, `-nullrhi` and `-NoShaderCompile`. Seventeen builds, of which
four were full recompiles of ~1000 actions at 9–17 minutes each; the rest were seconds. The
binary is 768 MB without debug info, and a debug-info link needs more memory than a 62 GB machine
with 42 GB free would give lld-link, so the cooker ships without a PDB until someone needs one.
`host/VerseHostCooker.Target.cs`, `host/Private/CookMain.cpp` and the `bCompileAgainstEditor`
block of `host/VerseHost.Build.cs` are the artefacts, and `build_host.py --target VerseHostCooker`
reproduces the build. **The fallback (a commandlet in UnrealEditor-Cmd) is not needed and §5
stands.**

**What the spike leaves open, and stage 1 inherits: the process segfaults during teardown.**
It boots, runs the body, logs, and then dies at the very end of shutdown — after
`LogExit: Object subsystem successfully closed`, `Chaos Debug Draw Shutdown` and
`Destroying PakPlatformFile`, which is past everything a cook would have written. The exit code
is 139 on every run. Three things were tried and are recorded so they are not tried twice:
adding `FIoDispatcher::Shutdown()` (what `UnrealAssetStringify` does and the editor host does
not) changed nothing; replacing the editor host's hand-rolled
`AppPreExit`/`UnloadModulesAtShutdown`/`AppExit` with **`GEngineLoop.Exit()`** got measurably
*further* into teardown and is what the code now does, because it is the engine-class call and
`ChaosVisualDebugger`'s `-RUN=` path is this exact shape (PreInit, work, Exit, no Init, no Tick);
and marking the entry point `AUTORTFM_DISABLE` was required to call `Exit()` at all but did not
address the crash. **This was not chased further on purpose** — the body of `CookMain.cpp` is
still a stub, and how the cooker reports success is a stage-1 decision, not a spike's.

It bears on **D3 and §5**: the design has the export plugin read the cooker's exit code (0, or 2
for a build that did not compile), and an exit code cannot be trusted from a process that always
segfaults on the way out. Stage 1 settles this, and has three options — fix the teardown; flush
and hard-exit with `FPlatformMisc::RequestExitWithStatus(true, Code)` once the outputs are
written, which is what a tool whose work is done ordinarily does; or drop the exit code as the
signal and have the plugin check for the cooker's own sentinel line and the files on disk. The
last is the most robust and the least pleasant. **Do not take the "Answer: yes" above to mean the
cooker is a working program**: it is a target that builds and an engine that boots, which is
exactly what OQ-10 asked and no more.

**A measurement warning, because it cost a wrong claim in an earlier draft of this document.**
`( time ./verse_cook.exe | grep … ); echo ${PIPESTATUS[0]}` reads the *subshell's* status, not
the binary's, and reported a clean 0 for a process that was segfaulting every time. Check an exit
code with a bare `cmd > /dev/null 2>&1; echo $?`.

**What was learned, in order.**

- The flag is legitimate: `TargetRules.cs:1428-1429` says a Program may set it and "mainly drives
  the value of `WITH_EDITOR`", and UBT adds `WITH_EDITOR=1` for a Program exactly as for an Editor
  target (`UEBuildTarget.cs:7103-7107`). `UnrealAssetStringify.Target.cs` sets it with
  `bCompileAgainstEngine = false` and builds with **38 modules and no Engine**.
- What drags Engine into *this* host is `bBuildEditor`, which is `Type == Editor ||
  bCompileAgainstEditor` (`TargetRules.cs:1223`): `Solaris.Build.cs:84-91` and
  `SolarisLoadCompiler.Build.cs:35-42` add `TargetPlatform` under it, and
  `TargetPlatform.Build.cs:36` adds `TurnkeySupport` as a *dynamically loaded* module, which a
  monolithic build links, and which reaches `UnrealEd` and so `Engine`.
  `VerseSimulationMetadata.Build.cs:52-56` adds `SolarisEditor` under the same flag, "for accessing
  Editor Permissions state". A UBT JSON export (`Build.bat <target> Win64 Development
  -Mode=JsonExport -OutputFile=…`) is how to see the graph; count link edges only, not include-path
  edges, or the chain you find is not the chain that links.
- **The UHT failure is `WITH_ENGINE=0` with Engine's headers in the manifest.** `World.h` *is* in
  the manifest (1359 Engine headers, `verse_cook.uhtmanifest`), six other `Within=` users in the
  same module resolve, and only `UWorld` and `APlayerController` are missing; UHT evaluates
  `WITH_ENGINE` as a compiler directive (`UhtHeaderFileParser.cs:1070-1072`). Setting the flag is
  what fixed it. Why exactly those two classes drop out under `WITH_ENGINE=0` was not chased
  further: the fix is the one every engine Program with these flags already uses.
- **The cooker cannot be lean and need not be.** Once `bCompileAgainstEditor` is on, the editor's
  module set arrives through Solaris's own rules whatever the host lists, so there is nothing to
  gain by trimming, and D19 keeps the host's list whole.

- **The rest of the boot is the engine's own checklist for an editor-class Program**, and every
  item has a precedent: `ChaosVisualDebugger.cpp:51` for the `EDITOR` token,
  `UnrealAssetStringify.cpp:1109` for `-NoPreviewPlatforms`, `UnrealConsole.Target.cs` for the
  PDB page size. What has *no* precedent among the four is a Program that is both engine-class
  and lean, which is why builds 1–3 were spent looking for one.

### S-2 · The runtime host target builds, and what it weighs

**Question.** Does `host/` compile with `bBuildWithEditorOnlyData = false` — which is what gives
`Solaris` `WITH_VERSE_COMPILER=0` (`Solaris.Build.cs:47-82`) — and what does the DLL weigh against
the editor host's 121 MB?

**Method.** `host/VerseHostRuntime.Target.cs` deriving from `VerseHostTarget` with
`Name = "verse_host_runtime"`, `bBuildWithEditorOnlyData = false`, and `GlobalDefinitions.Add(
"VH_HOST_KIND=2")`. Expect: `VerseCompiler`, `uLangUE`'s IDE half, `SolarisTestUtils`,
`ScriptDisassembler` and `VerseSimulationMetadata` (which privately depends on `VerseCompiler`)
either drop out of the graph or refuse to build; `HostScript.cpp`'s compile, analysis, lookup,
completion and signature bodies need `#if WITH_VERSE_COMPILER`. Build in Development and in
Shipping (D12). Record both sizes and the module count.

**Decides.** Which entry points the runtime host implements (§11's optional list is a prediction;
this is the measurement), and whether `AllowDebugging` needs a call of its own (D11).

### S-3 · What the runtime host needs on disk

**Question.** With `EngineDirUtf8 = <empty dir>/Engine` holding only `Binaries/`, what does
`GEngineLoop.PreInit` open, and where does it write?

**Method.** A throwaway driver over the ABI (the shape `tests/host_smoke` has) against
`verse_host_runtime.dll`, run under Process Monitor or with `-LogCmds="LogConfig Verbose
LogInit Verbose"`, adding files under `<data>/Engine/` until `vh_init` answers `VH_OK`. Then the
same with `-saveddir=<data>/Saved` (or whatever redirects `ProjectSavedDir`, which for a Program
lands at `<BaseDir>/../../../Engine/Programs/<name>/Saved/` — `GenericPlatformMisc.cpp:1615-1627`,
*above* the game's directory for a DLL beside the exe).

**Decides.** The `Engine/` subtree the export plugin emits into the data directory (§7), and
whether `vh_init_desc` needs a writable-directory field or the host derives one from the data
directory.

### S-4 · One package cooks

**Question.** What does `UPackage::Save` demand of a cook from a Program that has `WITH_EDITOR`
but no `UnrealEd`?

**Method.** In the cooker, after `CompileProject` (reused verbatim), for the script package:
`Verse::GlobalProgram->LookupPackage(GScriptPackageName)->GetOrCreateUPackage(Context)`, then
`UPackage::Save` with `FSavePackageArgs{ .ArchiveCookData = &CookData, .TopLevelFlags = RF_Public |
RF_Standalone, .SavePackageContext = &Context }` where `CookData` wraps the Windows
`ITargetPlatform` (`GetTargetPlatformManager()->FindTargetPlatform(TEXT("Windows"))`; the S-1 spike
of Phase 0 already linked `TargetPlatform` and `WindowsTargetPlatform` into a monolithic host) and
`Context` is `FSavePackageContext(TargetPlatform, &Writer)` (`SavePackage.h:191-219`). `Writer` is
the spike's own: `IPackageWriter` is 36 pure virtuals (`Core/Public/Serialization/PackageWriter.h`),
but `TPackageWriterToSharedBuffer<FBaseCookedPackageWriter>` (`CoreUObject/Internal/Serialization/
PackageWriterToSharedBuffer.h:92-160`, `BasePackageWriter.h:21`) implements the recording half and
leaves one `CommitPackageInternal` to write the record's package data to
`<out>/<PackagePath>.uasset`. `UnrealEd`'s `LooseCookedPackageWriter` is the worked example of the
same thing and is not linkable here.

**Decides.** Whether a cooked package is one file or two (`.uasset` + `.uexp`), what
`ICookedPackageWriter::GetCookCapabilities` has to answer, and whether the harvester needs anything
beyond `IsCooking()` (`PackageHarvester.cpp:809`). Also the cooked VNI package for the mirror: the
same call on `Verse::GlobalProgram->LookupPackage("VerseHost")`'s UPackage, whose path is
`/Engine/_Verse/VNI/VerseHost` (`VVMNames.cpp:358-361`; mount point `Engine`, C++ module
`VerseHost`, from `VerseHost.package.gen.cpp:33-34`), and the attribute package
(`AttributePackageName`, `HostScript.cpp`).

### S-5 · The runtime host loads what S-4 wrote

**Question.** Can a cooked `/GodotScripts_1/_Verse` be loaded, registered and instantiated from a
data directory, with the mirror arriving the same way?

**Method.** Place S-4's outputs as §7 lays them out. `FPackageName::RegisterMountPoint(
TEXT("/GodotScripts_1/"), <data>/Cooked/GodotScripts_1/)`; the mirror needs no registration
because `/Engine/` is `FPaths::EngineContentDir()`, which under D7 is `<data>/Engine/Content/`, and
`FSolarisModule::JitVniPackages` loads a VNI package from `GetUPackagePathForVni` whenever
`FPackageName::DoesPackageExist` says it is there (`SolarisModule.cpp:3372-3404`, the
`!WITH_VERSE_COMPILER` delegate registered at `:528-530`). Then `LoadPackage(nullptr,
TEXT("/GodotScripts_1/_Verse"), LOAD_None)` and `ISolarisModule::Get().GetRuntime()->
AddCompiledUPackage(Package)` (`ISolarisRuntime.h:83`, public through `ISolarisModule.h:149`) —
the exact pair `AddMountedPlugin` uses for a plugin's cooked packages (`SolarisModule.cpp:2040-2055`).
Then `TryLink` if `IsFullyLinked()` says no, `LookupPackage(GScriptPackageName)`, `vh_instantiate`
a class, call `_Ready`.

**Decides.** Whether the attribute package must be cooked too or its attributes survive in the
script package's cells; whether generation naming stays as D20; and what the *link* step costs at
startup, which becomes a number in R-PERF-2.

### S-6 · Is a cooked package platform-specific?

**Question.** Does a package cooked against the Windows `ITargetPlatform` load on Linux?

**Method.** Once §9 has a Linux runtime host: load S-4's Windows output under WSL2. If it loads,
one data directory serves every desktop export and the cooker needs no `LinuxTargetPlatform`. If
not, the cooker links `LinuxTargetPlatform` and takes `--platform`, and the export plugin cooks
per preset. Blocked until §9; record either answer in §13.

### S-7 · What a headless Godot export of this project produces

**Question.** With an `export_presets.cfg` and the 4.7 Windows template, does
`godot --headless --path tests/integration --export-release "Windows Desktop" <out>/game.exe`
place a directory added with `add_shared_object` and a `[dependencies]` DLL beside the exe, and
does `game.exe --headless -- --verse-check` reach an autoload?

**Method.** Exactly that, before the export plugin has a cooker to run: a stub plugin that adds an
empty directory and a stub `.gdextension` dependency row. Read what lands on disk against
`editor_export_platform_pc.cpp:230-256`.

**Decides.** The shape of §8's export layer and whether `add_message(EXPORT_MESSAGE_ERROR, …)`
aborts an export or only reports it (`editor_export_platform.h:262`, bound at
`editor_export_platform.cpp:2739`); if it only reports, §7's plugin also withholds the data
directory so a failed cook produces a game that refuses to load with the same sentence.

---

## 3. What the two engines actually offer

### 3.1 Unreal's side

- **A compiler-less Solaris loads Verse from cooked UPackages and nothing else.** VNI packages:
  `JitVniPackages` (`SolarisModule.cpp:3372-3404`) tries `LoadPackageAsync` on
  `GetUPackagePathForVni(MountPoint, CppModule)` and registers the result with
  `AddCompiledUPackage`; only under `WITH_VERSE_COMPILER` is there an `#else` that compiles.
  Content packages: `AddMountedPlugin` does `LoadPackage` + `AddCompiledUPackage` per cooked
  package name (`:2040-2055`). Both halves are public: `ISolarisRuntime::AddCompiledUPackage`
  (`VerseNative/Public/ISolarisRuntime.h:83`) via `ISolarisModule::GetRuntime()`.
- **UPackage paths.** A Verse package named `P` maps to `/P/_Verse` (`VVMNames.cpp:342, 385`); a VNI
  package to `/<MountPoint>/_Verse/VNI/<CppModule>` (`:358-361`). The host's script package is
  `GodotScripts_<generation>` (`HostScript.cpp:94, 906-908`); the mirror's VNI descriptor is
  `{Engine, VerseHost}` at verse path `/Godot.org/Godot` with five dependencies — `VerseNative`,
  `VersePredicts`, `Verse`, `VerseSimulationMetadata`, `VerseSpatialMath` — and a source directory
  `../../Source/Programs/VerseHost/Verse` that only the compiler reads
  (`VerseHost.package.gen.cpp:32-34`).
- **A cook is a save with `ArchiveCookData` and a `FSavePackageContext`** carrying an
  `IPackageWriter` (`SavePackage.h:64-94, 191-219`). The interface lives in **Core**
  (`Serialization/PackageWriter.h`, 36 pure virtuals); the recording base that makes a minimal
  writer small is in CoreUObject's *Internal* headers (`PackageWriterToSharedBuffer.h:84-160`);
  every complete implementation is in `UnrealEd` (`LooseCookedPackageWriter.h`,
  `DefaultCookedFilePackageWriter.h`).
- **`bCompileAgainstEditor` on a Program is documented and has four users**
  (`AutoRTFMTestsWithEditor`, `ChaosVisualDebugger`, `UnrealAssetStringify`, `UnrealConsole`).
  `WITH_EDITOR && ALLOW_OTHER_PLATFORM_CONFIG` spawns a per-platform config load in PreInit
  (`AutoRTFMTestsWithEditor.Target.cs` comment); the cooker sets it to 0 and
  `UE_CONFIG_ALLOW_ASYNC_LOADING=0` with it.
- **The engine directory is overridable and self-describing.** `GForeignEngineDir` is honoured
  when `<dir>/Binaries` exists (`GenericPlatformMisc.cpp:1398-1432`); a Program's `ProjectDir` is
  `<BaseDir>/../../../Engine/Programs/<TargetName>/` and `Saved/Config` hangs off it
  (`:1615-1627, 1780-1782`). Config init hard-fails only for a game-agnostic exe with a project
  name and no `DefaultEngine.ini` (`ConfigCacheIni.cpp:7205-7230`).
- **`ALLOW_LOG_FILE=0`** is already set on every target (`VerseHost.Target.cs:33`).

### 3.2 Godot's side

- **What a scene load asks of a script**: `Object::set_script` → `can_instantiate` →
  `instance_create` (`object.cpp:1049-1076`); stored values applied by name through `set`
  (`packed_scene.cpp:572` → `object.cpp:198-205`); connections built without asking the script
  (`packed_scene.cpp:735-760`). The property, signal and method lists are asked by the remote
  debugger (`scene_debugger_object.cpp:104`) and `Object::get_property_list`/`get_signal_list`
  (`object.cpp:560, 604, 1509`) — lazily, never at instantiation. `_placeholder_instance_create`
  is never reached outside the editor. `_validate`, `_complete_code`, `_lookup_code`,
  `_auto_indent_code`, `_find_function` and the documentation virtuals are inside `TOOLS_ENABLED`
  in `script_language_extension.h:84-108, 395-578`.
- **GDScript ships tokens and re-runs the whole front end at load**
  (`register_types.cpp:84-122`; `gdscript.cpp:818-860`); it answers no default outside the editor
  (`:395-409`). **C# ships an assembly with source-generated `GetGodotPropertyList` /
  `GetGodotSignalList` / `GetGodotMethodList`** (`ScriptPropertiesGenerator.cs:244-258`,
  `ScriptSignalsGenerator.cs:217-233`, `ScriptMethodsGenerator.cs:172-188`), found by reflection
  (`ScriptManagerBridge.cs:951-974`), with the default-value table under `#if TOOLS`
  (`ScriptPropertyDefValGenerator.cs:361-404`); it strips `.cs` to a `"\n"` stub
  (`ExportPlugin.cs:120-153`) and maps path → type through `[ScriptPath]`
  (`ScriptPathAttributeGenerator.cs:103-106`, `csharp_script_resource_format.cpp:44-95`). The
  **global class table** `global_script_class_cache.cfg` is a forced export
  (`editor_export_platform.cpp:1208`) read by `ScriptServer::init_languages`
  (`script_language.cpp:293-301`).
- **Export primitives.** `EditorExportPlugin`: `_export_begin(features, is_debug, path, flags)`,
  `_export_file(path, type, features)`, `_export_end`, `add_file(path, bytes, remap)` (into the
  PCK, `editor_export_platform.cpp:1379-1385`), `add_shared_object(path, tags, target)` (beside
  the exe under `target`, directories recursively, `editor_export_platform_pc.cpp:230-256`),
  `skip()`, `get_export_platform()` and `EditorExportPlatform::add_message(type, category,
  message)` — all script-bound (`editor_export_plugin.cpp:363-390`,
  `editor_export_platform.cpp:2739`). macOS: empty target → `Contents/Frameworks`, signed;
  custom target → under the app, non-code left unsigned; ad-hoc signing with any shared object
  forces the "Disable Library Validation" entitlement (`platform/macos/export/export_plugin.cpp:
  1470-1495, 2107-2115, 2315-2323`).
- **`.gdextension` `[dependencies]`** is per feature tag, `{path: target}`, and the GDExtension
  export plugin turns each matching row into a `SharedObject`
  (`gdextension_library_loader.cpp:42-66`, `gdextension_export_plugin.h`). At runtime Windows adds
  the library's own directory to the DLL search path (`os_windows.cpp:515-520`); Unix falls back
  to the executable's directory and `../lib` (`os_unix.cpp:1008-1022`).
- **`-s` / `--script` is `TOOLS_ENABLED`** (`main.cpp:4116-4120`). An exported game runs its main
  scene and autoloads; `OS.get_cmdline_user_args()` is what comes after `--`.
- **Today the consumer compiles the whole project at startup in *every* build**:
  `build_project` → `compile_project` is not under `TOOLS_ENABLED`
  (`verse_script_language.cpp:3046-3088`), reached from `VerseScript::reload` (`verse_script.cpp:
  145`). The `.gdextension` names only `editor` and `debug` Windows libraries and has no
  `template_release` row at all.

---

## 4. Stage 0 — Godot 4.7

**Why first.** Every later stage exports with the 4.7 editor against the 4.7 templates, and the
mirror must describe the API the exported game runs against.

1. Bump `godot-cpp` to the commit whose `gdextension/extension_api.json` header says
   `4.7.stable` (the `godot-4.7-stable` tag if the submodule's remote carries one; otherwise dump
   the API from the installed binary — `Godot_v4.7-stable_win64_console.exe --headless
   --dump-extension-api --dump-gdextension-interface` — into the submodule's `gdextension/`).
2. `python tools/gen_verse_api.py`; read the diff of `host/Verse/GodotClasses.native.verse`,
   `src/verse_api_classes.h`, `src/verse_api_skipped.h`, `host/Private/GodotClassNames.gen.h`,
   `docs/nonatomic-methods.md`. New classes and members are expected; a new `unsupported_type` is
   not, and `tests/verse_api_gen/test_gen_verse_api.py` must pass.
3. `compatibility_minimum = "4.7"` in `SConstruct`'s `write_gdextension`.
4. `python tools/build_host.py`, `scons target=editor`, `python tools/run_tests.py --build`. All
   three layers green before anything else in this phase starts. `tests/integration/project.godot`
   `config/features` moves to `4.7`; `dodge-the-creeps` already says so.
5. Update the counts CLAUDE.md cites (classes, enums, virtuals, signal accessors, skip rows) to
   what the generator prints.

**Done when:** `run_tests.py` is green on the 4.7 editor with the 4.7 mirror.

---

## 5. Stage 1 — the cooker, `verse_cook.exe`

**Files.** `host/VerseHostCooker.Target.cs` (exists from S-1, already an executable; add
`GlobalDefinitions.Add("VH_HOST_KIND=3")`), `host/Private/CookMain.cpp` (exists from S-1 as a
stub that boots and exits; its `#if WITH_EDITOR` becomes `#if VH_HOST_KIND == 3` and it gains the
body below), `host/Private/HostCook.{h,cpp}` (new; the save and the sidecar), `host/VerseHost.Build.cs` (already carries Launch's
editor modules under `Target.bCompileAgainstEditor` from S-1; adds `TargetPlatform` and
`WindowsTargetPlatform` there for S-4), `tools/build_host.py` (`--target VerseHostCooker` leaves `verse_cook.exe` in
`Engine/Binaries/Win64` beside `verse_host.dll`, and writes provenance beside it).

**Entry.** `INT32_MAIN_INT32_ARGC_TCHAR_ARGV()` with `IMPLEMENT_APPLICATION(VerseHost, "verse_cook")`
the way `UnrealAssetStringify` does; `bAllowEngineStdioOutput = false` so stdout carries only what
the cooker prints. `PreInit` with the editor host's command line (`VerseHost.cpp:199`).

**Command line.** `verse_cook.exe <manifest> <out_dir> [--platform Windows]`. The manifest is a
UTF-8 text file the export plugin writes, one source per line: `<absolute path>\t<module path>\t
<res:// path>` — the same three things `compile_project` hands the host today
(`verse_script_language.cpp:3070-3082`), plus the `res://` path for the sidecar. Absolute paths
because the ABI carries only absolute paths (`phase-0-spikes.md` §S-3).

**Steps.**

1. `GodotVerse::CompileProject(Sources, Generation)` — the existing function, unchanged; the
   attribute package and the authorship injection come with it (`HostScript.cpp:790-806`). A
   diagnostic is printed to stdout as `<res path>:<line>:<col>: <severity>: <message>` and a
   failed build exits **2** having written nothing.
2. Cook, per S-4, into `<out_dir>/`:
   - `Cooked/GodotScripts_1/_Verse.uasset` (+ whatever S-4 says accompanies it),
   - `Engine/Content/_Verse/VNI/VerseHost.uasset` — the mirror,
   - the attribute package if S-5 says it is needed, at its own `/<name>/_Verse` path under
     `Cooked/`.
3. Write `verse_classes.json`, the serialised snapshot (D9): for every class the snapshot holds —
   module-qualified name, `res://` path, the Godot class it crosses as, abstractness, `@tool`,
   `@global_class` name, exports (`name, type, hint, hint_string, usage`, and the `Reject` reason
   where there is one), signals (`name`, arguments), methods (`name`, arguments, return, static),
   and which virtuals are overridden. The reader is `HostScript.cpp`'s snapshot loader (§6), so
   the writer and the reader are two functions over one struct, in one file.
4. Write `Engine/Binaries/` (empty) and whatever S-3 found `PreInit` needs under `Engine/`.
5. Print one summary line and exit **0**.

**Step 5 is the one with a problem waiting for it.** The spike's binary segfaults during teardown
on every run, so *no* exit code it produces means anything yet (§2 S-1, "what the spike leaves
open"). Settle this first, before the cook itself: the plugin's whole failure path in §7 reads
that code. Whatever is chosen — fixing the teardown, a deliberate hard exit once the outputs are
flushed, or a sentinel line plus a check for the files — say so here and in D3.

**Done when:** `verse_cook.exe` cooks `tests/host_smoke`'s fixtures and `dodge-the-creeps` from
the command line, reports success in a way the export plugin can actually read, and S-5's driver
instantiates a class out of the result.

---

## 6. Stage 2 — the runtime host, `verse_host_runtime.dll`

**Files.** `host/VerseHostRuntime.Target.cs` (from S-2), `host/Private/VerseHost.cpp`
(`vh_host_kind`, the `VH_ERR_UNSUPPORTED` prologue clause, `CookedDirUtf8`), `host/Private/
HostScript.cpp` (`#if WITH_VERSE_COMPILER` around the compiler-side bodies; the snapshot loader),
`host/Private/HostCooked.{h,cpp}` (new; mount points, `LoadPackage`, `AddCompiledUPackage`,
link), `tools/build_host.py` (`--target VerseHostRuntime [--config Shipping]` collects
`verse_host_runtime.dll` and `tbbmalloc.dll` into `addons/godot-verse/bin/windows-x86_64/`,
which is git-ignored — R-DIST-2).

**Init.** `vh_init_desc` gains `const char* CookedDirUtf8` behind `StructSize`. When set, `vh_init`
— after `PreInit` and `ISolarisModule::Get()` — runs `LoadCookedProject`: register the
`/GodotScripts_1/` mount point over `<Cooked>/GodotScripts_1/`, load and register the package,
load the snapshot from `<Cooked>/../verse_classes.json`, publish it as *the* snapshot, set
`GScriptPackageName` and `GProjectBuilt`. The mirror arrives on its own through `JitVniPackages`
because `/Engine/Content` is under the data directory (D7, S-5). A missing or unreadable cooked
directory is `VH_ERR_INIT` with a diagnostic naming the path.

**Every entry point keeps its prologue shape** (`VerseHost.cpp:326-340`): `WrongThread`,
`IsDebugStopped`, and now `Unsupported`, which is `#if !WITH_VERSE_COMPILER` returning
`VH_ERR_UNSUPPORTED` for: `vh_compile_project`, `vh_check_project`, `vh_check_project_begin`,
`vh_check_project_poll`, `vh_check_project_busy`, `vh_resolve_unknown_name`, `vh_lookup_symbol`,
`vh_complete_symbol`, `vh_signature_at`, `vh_class_members`, `vh_class_override_candidates`. The
snapshot-fed seven (`vh_has_class`, `vh_class_is_abstract`, `vh_class_method_list`,
`vh_class_signal_list`, `vh_class_static_list`, `vh_class_export_list`, `vh_class_default_field`)
answer from the loaded snapshot; `vh_class_default_field` answers not-found (D9). Everything VM-side
— init, shutdown, tick, instantiate, release, call, fields, callbacks, `vh_run_main`, the six
`vh_debug_*`/`vh_profiling_*` — is unchanged.

**Done when:** `host_smoke` (§12) passes against `verse_host_runtime.dll` with a cooked fixture,
in Development and in Shipping.

---

## 7. Stage 3 — the GDExtension

**Files.** `src/verse_host.{h,cpp}`, `src/verse_runtime.{h,cpp}`, `src/verse_script_language.cpp`,
`src/verse_script.cpp`, `src/verse_resource_format.cpp`, `src/verse_export_plugin.{h,cpp}` (new,
`TOOLS_ENABLED`), `src/register_types.cpp`, `SConstruct` (`write_gdextension`).

**Loader.** `resolve()` splits into `resolve_required` and `resolve_optional`; the eleven
compiler-side entries are optional and stay `nullptr` when absent. Version check by the header's
own policy (major equal, minor ≤) instead of today's exact equality (`verse_host.cpp:129`). Then
`vh_host_kind()`; absent means editor (every pre-8.2 host). The consumer knows which kind it
wants: editor when `Engine::is_editor_hint()`, runtime otherwise. A mismatch is refused before
`vh_init` with one sentence: *"verse/host/dll_path names a runtime host; the Godot editor needs
the editor host. Build it with `python tools/build_host.py`."* — and the reverse in an exported
game. One null guard per optional pointer in `verse_runtime.cpp`, answering `ERR_UNAVAILABLE`,
the degradation `verse_runtime.h:70-72` already promises.

**Paths in an exported build** (`OS::has_feature("template")`): the data directory is
`OS::get_executable_path().get_base_dir().path_join("verse_" + app + "_" + platform + "_" + arch)`
with the macOS fallback `OS::get_bundle_resource_dir()`, exactly `godotsharp_dirs.cpp:224-231`;
`EngineDirUtf8 = <data>/Engine`, `CookedDirUtf8 = <data>/Cooked`; the DLL is found by name through
the extension's own directory (`os_windows.cpp:515-520`), so no path setting is read. `app` is the
project name sanitised the way .NET's is (`ExportPlugin.cs:286`).

**Build path.** `ensure_project_built` in a template build against a runtime host does not call
`compile_project`; `vh_init` already loaded the cooked project and `project_built` is true from
`load_host`. `refresh_from_analysis`'s `diagnostics_for` is empty and `has_class` answers from the
sidecar. `VerseResourceFormatLoader::_load` keeps reading the file — the stub is one byte — and
`compile()` → `reload()` runs the same path. `verse_module_map` needs the `.vmodule` markers, which
the export plugin keeps (below).

**`.gdextension`.** `write_gdextension` emits `windows.x86_64.template_debug` and
`template_release` rows (`godot-verse.template_debug.dll`, `godot-verse.template_release.dll` —
whatever `library_filename` names them), the `linux.x86_64.*` and `macos.*` rows §9 and §10 will
fill, and a `[dependencies]` section: per template tag, `verse_host_runtime.dll` and
`tbbmalloc.dll` with an empty target. `compatibility_minimum = "4.7"` (stage 0).

**`VerseExportPlugin`** (an `EditorExportPlugin`, registered by `VerseEditorPlugin::_enter_tree`):

- `_export_begin(features, is_debug, path, flags)`: if `features` has `android`, `ios` or `web`,
  `get_export_platform()->add_message(EXPORT_MESSAGE_ERROR, "Verse", "Verse does not export to
  <platform> yet; see docs/phase-7-design.md §14")` and return (R-PLAT-4). Otherwise: run the
  editor host's own build first (`VerseScriptLanguage::build_project`) so a project that does not
  compile fails with diagnostics the author has already seen; write the manifest to
  `OS::get_cache_dir()/verse_cook/`; run `verse_cook.exe` (path: `verse/host/cooker_path`, a new
  editor-only setting defaulting to `verse_cook.exe` beside `verse/host/dll_path`) with
  `OS::execute`, relaying its stdout lines as `add_message` warnings and errors; on exit 0,
  `add_shared_object(<tmp>/verse_<app>_<platform>_<arch>, [], "")` (or `"Contents/Resources"`
  under `macos`). On failure, per S-7's answer, either the export aborts or the data directory is
  withheld and the game refuses to load with the same sentence.
- `_export_file(path, type, features)`: for `*.verse`, `add_file(path, "\n", false)` then `skip()`
  (D10); for `*.vmodule`, `add_file(path, <bytes>, false)` so the module map survives an export
  filter that only takes resources.
- `_export_end`: remove the temporary directory.

**Done when:** an export of `tests/integration` from the 4.7 editor, headless, produces a tree
that runs with `--headless -- --verse-check` and passes every case §8 moves into the autoload.

---

## 8. Stage 4 — the export layer in `run_tests.py`

**Both projects get** an `export_presets.cfg` (one preset, "Windows Desktop", `export_filter=
"all_resources"`, `include_filter="*.vmodule"`) and an autoload `res://export_check.gd` that, when
`"--verse-check"` is in `OS.get_cmdline_user_args()`, runs the checks and `quit(code)`s. The checks
themselves move out of the two `SceneTree` scripts into a library each (`tests/integration/
test_cases.gd`, `dodge-the-creeps/checks.gd`) with one `run(tree: SceneTree) -> int` entry, which
`test_main.gd`/`headless_check.gd` (in-editor, headless, as today) and the autoload (exported) both
call. One line per case, the same lines.

**`run_export(results, godot, engine)`** in `run_tests.py`:

1. Prerequisites, each a skip-and-say-so: `verse_cook.exe` and `verse_host_runtime.dll` in
   `Engine/Binaries/Win64`; the runtime host and `tbbmalloc.dll` staged into the addon's `bin/`;
   `godot-verse.template_release.dll` built; the template at
   `%APPDATA%/Godot/export_templates/<version>/windows_release_x86_64.exe` where `<version>` comes
   from `godot --version`.
2. For each project: `stage_extension`, `point_at_engine` (the cooker path too), then
   `godot --headless --path <project> --export-release "Windows Desktop" <tmp>/<name>.exe`, then
   `<tmp>/<name>.exe --headless --fixed-fps 60 -- --verse-check` with `--quit-after` as the
   integration layer uses it, requiring the last case's line.
3. `dodge-the-creeps` is exported and run here **by decision**, unlike the integration layer,
   because the export is the phase's exit and a yardstick of *export* is what this layer is.

**Done when:** `python tools/run_tests.py` reports four layers and the fourth passes.

---

## 9. Stage 5 — Linux, built and attempted

Not part of the exit (D14). What it takes, in order:

1. UBT's Linux cross-toolchain: `LINUX_MULTIARCH_ROOT` pointing at Epic's clang toolchain of the
   version `LinuxPlatformSDK.GetMainVersion()` names (UBT prints the expected version when the
   variable is unset; `LinuxPlatformSDK.cs:185-193`).
2. `build_host.py --platform Linux --target VerseHostRuntime` (a `--platform` switch; `Build.bat
   verse_host_runtime Linux Development`) → `libverse_host_runtime.so`. The cooker stays a
   Windows binary.
3. The GDExtension for Linux built **inside WSL2** with `scons platform=linux
   target=template_release` — godot-cpp's own Linux build, against the same submodule.
4. `linux.x86_64.template_release` rows and dependencies in the `.gdextension`; a "Linux" preset.
5. S-6. Then the export layer gains a Linux branch that exports from Windows with the 4.7 Linux
   template and runs the result under `wsl.exe` with `--headless`, skipped-and-said-so when any
   prerequisite is missing.

Everything that does not work is written into §13 with the message it produced.

---

## 10. macOS — blocked, and what it needs

A Mac with Xcode and a UE source checkout built there (UE's macOS toolchain does not cross-compile
from Windows). Layout is decided (D6): `Contents/Resources/verse_<app>_macos_<arch>/` for the data
directory, `Contents/Frameworks/libverse_host_runtime.dylib` signed as code, and ad-hoc signing
requires the "Disable Library Validation" entitlement (`platform/macos/export/export_plugin.cpp:
2107-2115`). `rcodesign` refuses apps with dylibs, so the plan is Apple's `codesign`. R-PLAT-1's
status stays *part* with this paragraph as the reason.

---

## 11. ABI 8.2 — the delta

All additions; nothing moves. A consumer built for 8.1 keeps working against an 8.2 editor host.

- `vh_status`: `VH_ERR_UNSUPPORTED` appended after `VH_ERR_STOPPED` — *"this build of the host has
  no compiler; nothing ran."*
- `typedef enum vh_host_kind { VH_HOST_EDITOR = 1, VH_HOST_RUNTIME = 2, VH_HOST_COOKER = 3 }` and
  `VH_API int32_t vh_host_kind(void)`, answerable before `vh_init` like `vh_abi_version`
  (`VerseHost.cpp:92-97` documents the two existing exceptions; this is the third).
- `vh_init_desc.CookedDirUtf8` appended, read only when `StructSize` covers it; `NULL` means "no
  cooked project", which a runtime host answers with `VH_ERR_INIT`.
- The loader's required/optional table (§7). `host_smoke` gains the same split.

`VH_ABI_VERSION_MINOR` → 2. Both DLLs rebuild anyway (three now), and `run_tests.py --build`
follows.

---

## 12. Tests

- **abi.** `host_smoke` reads `vh_host_kind()` after `vh_abi_version()` and branches: against an
  editor host, every case as today; against a runtime host, the compile/analysis/completion cases
  are skipped *and said to be skipped*, one new case asserts each of the eleven refused entries
  answers `VH_ERR_UNSUPPORTED` without crashing, and the fixture is a cooked directory rather than
  `hello.verse`. `run_tests.py`'s abi layer produces that directory with `verse_cook.exe` from the
  same fixtures and runs `host_smoke` twice, once per host, skipping the second when the runtime
  host is not built.
- **units.** Nothing new; the sidecar's reader and writer are host code and `host_smoke` covers
  them through the seven snapshot reads.
- **export.** §8. Both projects, Windows.
- **by hand.** One editor session, recorded in `by-hand-findings.md`: export `dodge-the-creeps`
  through the dialog (not the CLI), run the result windowed, play a round; export it to Android and
  read the sentence R-PLAT-4 promises; set a breakpoint in the exported debug build from the
  editor's remote debugger (D11).

---

## 13. What building this corrected

*Empty until built. Write here: S-1's link result, S-2 … S-7's answers, and every row of §1 that
turned out wrong, with the file and line that proved it.*

---

## 14. Deliberately not built

- **In-PCK cooked data with extraction at first launch** (single-file exports). Held in reserve as
  a later `verse/embed_build_outputs` option; .NET's manifest-and-cache design is copyable
  (`ExportPlugin.cs:403-431`, `godotsharp_dirs.cpp:182-221`).
- **Mobile** (R-PLAT-2, OQ-3) — deferred with no design. **Web** (R-PLAT-3, OQ-4) — Phase 7.5.
  Both fail at export with a sentence.
- **R-DIST-3, R-DIST-4** — Phase 8, unchanged. **R-DIST-5** — closed only for "wrong kind of host".
- **Phase 4b.**
- **A Shipping editor host**, and Shipping for the cooker.
- **Cooked hot reload** (`bShouldGatherCookedVersePackagesForHotReload`) — an exported game does not
  reload.
- **A cooker that cooks without first compiling in the editor** — the export plugin builds twice
  on purpose, once for diagnostics with `res://` paths and once to cook.

---

## 15. Exit

- `python tools/run_tests.py` reports **four** layers on this machine, and the fourth exports
  `tests/integration` and `dodge-the-creeps` from the Godot 4.7 editor, headless, and runs both
  under `verse_host_runtime.dll` with every case green, in the release template.
- The exported `dodge-the-creeps` plays windowed, by hand, from a directory copied to a machine
  with no Unreal checkout and no Godot installed (R-DIST-10), and its data directory contains no
  `.verse` file and no compiler (R-DIST-11).
- An Android export fails at export time with one sentence (R-PLAT-4).
- Linux artefacts build; whether the game runs under WSL2 is recorded either way.
- macOS is recorded as blocked with §10 as the reason.
- `spec.md`: R-DIST-9, R-DIST-10, R-DIST-11 → **done**; R-PLAT-1 → **part** (Windows); R-PLAT-4 →
  **done**; R-PLAT-5 → **part** (Linux built); OQ-10 closed; R-PERF-2 gains the cooked-load time.
- §13 of this document is written.
