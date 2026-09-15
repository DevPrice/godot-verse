# Phase 7b — Loading what the cooker wrote

**Status:** 2026-09-14 · **stages 1–3 built, both walls down.** §1 and §3 were written *before* the
work; **§13 is what building it corrected and is the section to read first.** The VCell wall this
phase exists to remove is gone — a cooked Verse package loads with its cells intact — and so is the
second wall behind it, which was not in any spike: a cooked `VNativeProcedure`'s C++ thunk is a
function pointer, so it does not serialise, and nothing rebinds the module-scoped ones after a
cooked load (§13.8). An exported game now calls the Godot mirror and the Verse standard library.

**Prerequisite: Phase 7a is complete** — built 2026-09-14, ABI **8.2**. `verse_cook.exe` cooks a
project to loose `.uasset` files, the export plugin ships them in a data directory beside the
executable, and the exported game **dies loading the first one**. That failure is the whole of this
phase's brief.

**Read `phase-7-design.md` §13.7 first** — the wall as the agent that hit it recorded it — and then
§3 below, which is the same ground re-verified line by line during this planning, with three facts
§13.7 did not have.

**Companion to:** `phase-7-design.md` (the design 7a was built from; §13 is where it was corrected,
and D1–D22 stand except for D6's contents); `spec.md` §2 (R-DIST-9 … R-DIST-11), §3 (R-PLAT-1,
R-PLAT-5), §14's **OQ-18** (opened by this phase); `roadmap.md` "Phase 7b".

---

## 0. How to read this

**§1 is the decisions.** D1–D12 came out of one interview. Every row says what it rests on. Do not
relitigate them; if a spike contradicts one, record it in §13 and raise it.

**§2 is the spikes, and they ran before any stage was written.** Both passed; §13.1 and §13.2 are
what they answered. Putting them first was right and did not go far enough: the wall that stopped the
phase for a session sat *behind* both of them, at the first mirrored call, and no spike this design
named would have found it. A spike that loads a cooked package proves the loading; only one that
*calls* something proves the calling.

**§3 is what the engine actually offers**, as read rather than as remembered, with the file and line
each fact came from. It is the factual base under every stage and it corrects §13.7 in two places.

**§4–§9 are the stages**, in build order. Each names the files it touches and what "done" is.
**§10 is the measurements owed**, **§11 the ABI delta** (expected: none), **§12 the tests**,
**§14 what is deliberately not built**, and **§15 the exit**.

**§13 is where the design is corrected, and is written.** Read it before §1 or §3: two of §1's
rows did not survive contact, §3 was short of one fact that cost two hours, and §13.8 is a wall no
part of this design anticipated.

---

## 1. Decisions

| # | decision | why, and what it rests on |
| --- | --- | --- |
| D1 | **The route is the IoStore container, and the two spikes run first.** The cooker keeps writing loose files and then converts them to a `.utoc`/`.ucas` pair with `CreateIoStoreContainerFiles`; the runtime host mounts that container and loads through the zen loader, which is the only loader that reads a `Verse::VCell`. **If S-8a or S-8b cannot be made to pass with the API surface the engine already exposes, stop and report** — the engine patch, `FZenStoreWriter` and a custom loader are each a separate decision Devin makes, not a fallback this phase takes on its own. | Interview. §3 confirms both halves have precedent: the conversion is the standard cook-then-container pipeline (`FPackageStoreOptimizer` carries cells across, §3.3), and the mount is the ordinary DLC/patch-pak path rather than anything editor-only (§3.4). What is genuinely unknown is what a container needs *beside* the packages, which is what S-8a and S-8b are for. |
| D2 | **The phase is three things: the wall, the export layer launching what it exported, and the R-DIST-10 by-hand check.** Everything else 7a owed is out. | Interview. Out, explicitly: Linux and the missing `dlopen` path; macOS; Shipping-versus-Development per template (D12 of 7a, still "not true yet"); the debugger and profiler in an exported game; the two uncookable VNI packages, unless S-8b shows the runtime needs them. |
| D3 | **The mirror's 67 MB ships as it is.** Of the 68 MB cook, 67 is `/Engine/Content/_Verse/VNI/VerseHost` — the mirror of all 1036 Godot classes — and every exported game pays it. It is recorded in §10 and in `spec.md`, and not trimmed. | Interview: worth trimming eventually, not a priority now. The runtime host is 72.7 MB (Shipping) beside it, so the mirror is not what makes this export large. Trimming means cooking only the definitions a project references, and a class reached through a cast, a virtual dispatch or the sidecar's Godot-class mapping is not in any reference set the compiler would hand over — the failure would land at the moment the class is first needed rather than at cook time. Left as a roadmap note. |
| D4 | **`dodge-the-creeps` stays a by-hand yardstick.** `run_tests.py`'s export layer exports *and now launches* `tests/integration` only. | 7a's rule, unchanged: a yardstick that gates the build stops measuring. dtc exported, run and recorded by hand is what R-DIST-10 rests on (§9). |
| D5 | **`test_main.gd` splits into a library two drivers share, and every case runs in both.** A case that cannot run in an export carries `editor_only` and is **printed as skipped**, not silently dropped; `run_tests.py` asserts the skip count as well as the pass count, so a case cannot quietly vanish from the exported run. | Interview. `dodge-the-creeps/checks.gd` is the shape and already works — 143 lines, `tree`, `begin()`, `step() -> bool`, one driver in-editor and one autoload in the export. What is new here is the tag, because dtc's checks all run in both and `tests/integration`'s do not (§7). |
| D6 | **A game that cannot load its Verse data refuses to start, with one sentence, and the sidecar carries a stamp.** `verse_classes.json` gains `abi`, `cooker_commit`, `engine_commit` and `generation`; the runtime host compares them at load and refuses a mismatch by name. Missing, unreadable and mismatched are three different sentences. | Interview. A game with no Verse in it is not a game worth starting silently. The stamp costs nothing to produce: `tools/build_host.py:116` already writes exactly these fields as a provenance record beside every host binary, and `HostSidecar.cpp:18` already versions the sidecar format (`SidecarVersion`) and refuses a foreign one. |
| D7 | **Costs are measured and recorded, never thresholded.** The container step's cost at export time, and the exported game's startup cost to the first Verse `_Ready`, go in §10 and in `spec.md` R-PERF. No caching of the mirror container, no progress reporting beyond the lines the plugin already relays, no budget the test layer enforces. | Interview, and the repo's convention (R-PERF-2): a recorded number on a named machine, because a threshold fails on a slower one. Caching the mirror container across exports was considered and declined — a stale mirror against a rebuilt host is a silent wrong answer, and the stamp in D6 would have to grow a second consumer. |
| D8 | **The exit flips R-DIST-9, R-DIST-10 and R-DIST-11 to `done`, with the Windows-only caveat written into each line** the way R-PLAT-1 already carries one. | Interview. R-PLAT-1 and R-PLAT-5 are untouched and stay `part`. |
| D9 | **Only the container ships.** The loose cook becomes an intermediate: the cooker writes it into a temporary directory, converts, and the data directory carries the `.utoc`/`.ucas` pair plus the sidecar. `--keep-loose` keeps the intermediate for debugging. | Follows from D1. The loose files are the *input* to the conversion, not an artifact anyone loads (§3.1), and shipping both doubles a 68 MB payload. |
| D10 | **No ABI change is expected.** `vh_init_desc.CookedDirUtf8` already names the directory; what changes is what the runtime host finds inside it. If a spike forces a field, it is a minor bump under the header's own policy and both sides rebuild. | ABI 8.2's `CookedDirUtf8` was added behind `StructSize` for exactly this (`verse_host_abi.h:548`). The sidecar's stamp is a *file format* change, versioned by `SidecarVersion`, not an ABI one. |
| D11 | **The debugger and the profiler in an exported game stay untested, and the design says so.** The debug-template runtime host still links `HostDebug`; nobody has attached Godot's remote debugger to an exported game and hit a Verse breakpoint. | Interview. Left open with its one known unknown recorded: Solaris permits debugging under `WITH_EDITOR \|\| GIsServer` only (`SolarisModule.cpp:505-511`), and an exported game is neither — so whether the runtime host needs an `AllowDebugging(true)` of its own is not known. |
| D12 | **The engine patch is not in this phase.** Giving `FLinkerLoad` a `Verse::VCell` override is a real route and the data it would need is already in the legacy format (§3.2) — but it makes a patched UE checkout a prerequisite of the bridge, and Phase 8 is trying to remove the checkout requirement altogether. | Interview: "we will probably escalate to an engine patch, but that's for a separate design/phase." |

---

## 2. Spikes

**Both run before any stage is written**, and each is a throwaway driver, not a stage's first
commit. The pattern is the repo's: a small `main` over the ABI or a small addition to `CookMain.cpp`
behind a flag, printing one line per fact. §13 records what they answered whether or not the answer
was the one this design assumed.

### S-8a · Can the cooker turn its loose cook into a container?

**The question.** After `CookProjectPackages` has written its `.uasset`/`.uexp` pairs, can
`CreateIoStoreContainerFiles` (`IoStoreUtilities.h:19`) be called in the same process to produce a
`.utoc`/`.ucas` pair that contains the Verse packages *with their cells intact*?

**How.** Add `IoStoreUtilities` to `VerseHost.Build.cs`'s editor-only block (it is a `Developer`
module and the cooker is already `bCompileAgainstEditor`); call the entry point with a command line
built the way `UnrealPak -CreateGlobalContainer` builds one. The argument shape is the thing to read
first — `IoStoreUtilities.cpp` parses it, and the response file listing the cooked packages is the
half that matters.

**What "pass" is, in order:**

1. A `.utoc`/`.ucas` pair exists and `DumpIoStoreContainerInfo` or `ListIoStoreContainer`
   (same header) lists `/GodotScripts_1/_Verse` in it.
2. The container's package entries carry cell exports. `FPackageStoreOptimizer::FCellExport`
   (`Internal/PackageStoreOptimizer.h:139`) and `GetCellExportHash` (`:278-280`) are what to look
   for; the optimizer mentions `Cell` 92 times and this is the step that carries them across.
3. It runs on `tests/host_smoke`'s fixtures and on `dodge-the-creeps`.

**The unknowns to report either way:** whether a **global container** is required alongside the
project's own (§3.5 argues it may not be, and that argument is exactly what S-8b tests); what the
step costs in seconds and in peak memory on a 68 MB cook; and whether the two VNI packages the cook
already skips (`/Solaris/_Verse/VNI/VerseNative`, `VersePredicts`, `phase-7-design.md` §13.5) make
the container step complain where the save did not.

**If it fails:** report with the command line, the parse error or the check that fired, and stop.

### S-8b · Can the runtime host mount one and load a Verse package out of it?

**The question.** In `verse_host_runtime.dll` — a Program target with `bBuildWithEditorOnlyData =
false`, so **not** `bCompileAgainstEditor` and therefore **not** `WITH_IOSTORE_IN_EDITOR` — can the
container S-8a produced be mounted such that `LoadPackage("/GodotAttributes/_Verse")` resolves
through the zen loader rather than through `FLinkerLoad`?

**How, in the order these have to happen.** `RegisterCookedMountPoints` runs *before*
`ISolarisModule::Get()` today, because `FSolarisModule::JitVniPackages` asks
`FPackageName::DoesPackageExist` during module startup (`phase-7-design.md` §13.6) — the container
mount inherits that ordering constraint and is the thing S-8b must get right first. Then, per §3.4:
mount the container through the pak platform file, or register an `IPackageStoreBackend` with
`FPackageStore::Mount` (`PackageStore.h:237`, public).

**What "pass" is, in order:**

1. `FIoDispatcher::DoesChunkExist` answers true for a chunk of the mounted container.
2. `FPackageStore` resolves the package id of `/GodotAttributes/_Verse`.
3. `LoadPackage` returns a package, and **no** `Missing VClass for VerseClass` fatal — which is the
   one line that says the cells arrived.
4. `AddCompiledUPackage` accepts it and a class from the project's own package instantiates.

**The unknowns to report either way:** whether the mount needs the **global** container's script
objects chunk (§3.5); whether `FFilePackageStore` being private to the `PakFile` module forces the
mount to go through `FPakPlatformFile` rather than through `FPackageStore::Mount` directly; whether
the mirror's cells have to be registered as a *script cell package* before the project's package
loads, and if so by what (`FAsyncLoadingThread2::NotifyScriptVersePackage`, §3.5); and what the
mount plus load costs against the current loose-file path, for §10.

**If it fails:** report with the mount call, the first log line that differs from the editor host's
own load, and stop.

### S-9 · What does a container change about the export tree?

Small, and runs only once S-8a passes. The data directory's shape is D6 of `phase-7-design.md` and
the `export` layer asserts it file by file (`run_tests.py:513-526`). S-9 is one question: does the
`Engine/Binaries/` marker and the `<data>/Engine` engine directory (D7 of 7a) still do their job
when nothing is loaded from `<data>/Engine/Content` any more? `GForeignEngineDir` needs only a
directory with a `Binaries/` child (`GenericPlatformMisc.cpp:1408-1415`), so the expected answer is
yes and the assertion list shrinks by four rows — but the `export` layer fails loudly if it is
wrong, and knowing before rewriting those rows is cheaper than after.

---

## 3. What the engine actually offers

Every line below was read out of `../UnrealEngine` during this planning. §13.7 of
`phase-7-design.md` had the first two; the last three are new and are why D1 reads the way it does.

### 3.1 Why a loose cooked Verse package cannot be loaded

A Verse package's contents are `Verse::VCell`s — the VM's own heap objects, not UObjects — and
serialising one goes through `FArchive::operator<<(Verse::VCell*&)`. There are exactly three
implementations in the engine:

| | where | what it does |
| --- | --- | --- |
| save | `FLinkerSave::operator<<` — `CoreUObject/Private/UObject/LinkerSave.cpp:398-411` | writes an `FPackageIndex`, **four bytes unconditionally**, even for a null cell |
| load, IoStore | `FExportArchive::operator<<` — `CoreUObject/Private/Serialization/AsyncLoading2.cpp:3184-3200` | reads the index and resolves it against `FZenPackageHeader`'s `CellExportsView` |
| load, legacy | **none** — the base class, `Core/Public/Serialization/Archive.h:1283-1286` | body is `return *this;` |

`FLinkerLoad` has no override, so every cell reference in a loose `.uasset` consumes zero bytes and
everything after it mis-parses. It is a byte-stream desync, not a missing feature that degrades.

### 3.2 The legacy format does carry the cells

`FPackageFileSummary` has `CellImportCount`/`CellImportOffset` and
`CellExportCount`/`CellExportOffset` (`CoreUObject/Public/UObject/PackageFileSummary.h:153-168`),
and `FLinker` declares `TArray<FCellImport> CellImportMap` and `TArray<FCellExport> CellExportMap`
(`CoreUObject/Public/UObject/Linker.h:71-74`). The cooker's loose output is therefore a complete
description of the package — it is only the legacy *loader* that cannot act on it. That is what
makes the loose cook the right input to a conversion (D1) and what would make an engine patch
possible (D12, not this phase).

### 3.3 The conversion is the standard pipeline

`IoStoreUtilities` exposes one entry point for this: `int32 CreateIoStoreContainerFiles(const
TCHAR* CmdLine)` (`Developer/IoStoreUtilities/Public/IoStoreUtilities.h:19`), the function
`UnrealPak -CreateGlobalContainer` calls. It is the only caller of `FPackageStoreOptimizer`, whose
`CreatePackageFromCookedHeader` (`Internal/PackageStoreOptimizer.h:249`) reads a **legacy cooked
header** and builds a zen package from it — cells included: `FCellExport` at `:139`, `CellExports`
at `:206`, `GetCellExportHash` at `:278-280`.

The same header is where `FZenStoreWriter` lives (`Public/ZenStoreWriter.h`), which is route 2 of
§13.7 and not taken (D1).

### 3.4 Mounting a container is not an editor-only path

This is the half §13.7 left as "not traced", and it is more ordinary than it feared:

- `FPackageStore::Mount(TSharedRef<IPackageStoreBackend>, int32 Priority)` is public
  `COREUOBJECT_API` (`CoreUObject/Public/Serialization/PackageStore.h:237`).
- `FFilePackageStore`, the backend that reads a container header's package entries, lives in the
  **`PakFile`** module (`Runtime/PakFile/Private/FilePackageStore.{h,cpp}`) — a runtime module, not
  a developer one. The mount that brings it up is `IPlatformFilePak.cpp:5827`,
  `IoDispatcher.Mount(IoDispatcherFileBackend.ToSharedRef())`.

So the precedent for what the runtime host has to do is **DLC and patch pak mounting**, which is a
shipped-game path. `WITH_IOSTORE_IN_EDITOR` (`UEBuildTarget.cs:7103-7106`) gates the *editor's*
ability to bring the package-store backend up without a global container; it is a gate on the
cooker, and §13.7's worry that the runtime host had "a different gate to satisfy" reads, from here,
like a gate it does not need at all. **S-8b is what settles that.**

### 3.5 Script imports come from memory, not from a chunk

The failure mode to expect from a container mounted without a global container is an unresolved
script import — `/Script/CoreUObject.VerseClass` and friends. It may not arise:
`FAsyncLoadingThread2::NotifyRegistrationEvent` calls
`FGlobalImportStore::AddScriptObject(...)` unconditionally for every registered UObject
(`AsyncLoading2.cpp:12110-12118` and `:2727-2754`), and the runtime host is a monolithic program
with every `/Script/` class linked in. `FindAllScriptObjects` — the sweep that reads like the
editor's special case — is `#if WITH_EDITOR` in `FGlobalImportStore::RegistrationComplete` and
verify-only under `DO_CHECK` (`:7192-7204`); the cooked script-objects chunk
(`EIoChunkType::ScriptObjects`) is read only by `FCookedScriptObjectsDebug::LoadDebugData`
(`:7037-7073`), which is debug naming and nothing more.

The cell half has its own registration: `FAsyncLoadingThread2::NotifyScriptVersePackage(Verse::
VPackage*)` → `AddScriptCellPackage` (`:12119-12125`). This section left as S-8b's third unknown
whether Solaris calls it for the mirror in a compiler-less host; **it is answered, and the answer is
that nothing here was ever at risk.** `$BuiltIn` registers itself unconditionally from
`VIntrinsics::Initialize` (`VVMIntrinsics.cpp:52`, whose comment says why in one line: *"This
VPackage enables cooked data to import intrinsics by Verse path"*), so the intrinsics — `Abs`,
`Floor`, `Ceil` and the rest — were never affected. The second wall was of a different family
entirely: not a missing registration but a **missing function pointer**, §13.8.

---

## 4. Stage 1 — the container, in the cooker

**Files.** `host/Private/HostCook.{h,cpp}` (the conversion, after `CookProjectPackages`),
`host/Private/CookMain.cpp` (the temp directory and `--keep-loose`), `host/VerseHost.Build.cs`
(`IoStoreUtilities` in the editor-only block), `tools/build_host.py` (nothing, unless the module
pulls a new runtime dependency into the collected set).

**Shape.** `CookProjectPackages` keeps writing loose files, into a temporary directory rather than
into the output (D9). A new `BuildCookedContainer(LooseDir, OutDir, OutError)` runs after it and
produces `<OutDir>/verse_scripts.utoc` and `.ucas` — one container for everything the cook wrote,
mirror included, unless S-8a says otherwise.

**The three things to get right.** The teardown rule from 7a still holds: the cook flushes and
hard-exits with `FPlatformMisc::RequestExitWithStatus(true, Code)` and never calls
`GEngineLoop.Exit()` (`phase-7-design.md` §13.5), so the conversion has to be complete before the
exit and its own failure has to become an exit code rather than an exception. The cooker's output
reaches the export plugin only through stdout with `bAllowEngineStdioOutput = false` — and
`FPlatformMisc::LocalPrint` is `OutputDebugString` on Windows, which reaches a debugger and nothing
else. And the package list is read off the VM rather than written down
(`Verse::GlobalProgram->NumPackages()`), so whatever the container step is handed must be derived
the same way.

**Done when:** `verse_cook.exe` produces a container over `tests/host_smoke`'s fixtures and over
`dodge-the-creeps`, exits 0, and `ListIoStoreContainer` lists the Verse packages in it.

---

## 5. Stage 2 — mounting it, in the runtime host

**Files.** `host/Private/HostCooked.{h,cpp}` (the whole of it: `RegisterCookedMountPoints` and
`LoadCookedProject` both change), `host/VerseHostRuntime.Target.cs` (whatever module S-8b needs),
`host/Private/VerseHost.cpp` (the `vh_init` path, unchanged in shape).

**What replaces what.** `RegisterCookedMountPoints` today enumerates directories under `<Cooked>/`
and calls `FPackageName::RegisterMountPoint` per mount point (`HostCooked.cpp:37-66`). With a
container the mount points still have to exist — Solaris asks `FPackageName::DoesPackageExist`
during module startup — but the bytes come from the container, so the function becomes *mount the
container, then register the mount points it declares*, and it keeps its position **before**
`ISolarisModule::Get()`.

`LoadCookedProject` keeps its shape exactly: `LoadPackage` per Verse package, `FullyLoad`,
`AddCompiledUPackage`, `AdoptCookedGeneration(ScriptPackageName, 1)`, then the sidecar
(`HostCooked.cpp:69-119`). What changes is that the loads now resolve through the zen loader.

**Done when:** an exported `tests/integration` boots, instantiates a Verse-scripted node and calls a
method on it; and `host_smoke` passes against `verse_host_runtime.dll` with a cooked fixture, in
Development and in Shipping.

---

## 6. Stage 3 — the stamp and the three refusals

**Files.** `host/Private/HostSidecar.{h,cpp}` (write and read four more fields),
`host/Private/HostCooked.cpp` (the comparison), `src/verse_runtime.cpp` (the sentence).

`verse_classes.json` grows `abi`, `cooker_commit`, `engine_commit` and `generation` beside its
existing `version` (`HostSidecar.cpp:442`, `:519-524`). The values are the ones
`tools/build_host.py:116-137` already computes for its provenance record — `abi_version`,
`godot_verse_commit`, `engine_commit` — so the cooker reads them from the provenance file beside
its own executable rather than inventing a second source of truth.

Three failures, three sentences, all `VH_ERR_INIT` with a diagnostic the consumer prints verbatim:

- **missing** — *"Verse data not found at `<path>`. The export is incomplete; export the project
  again."*
- **unreadable or foreign format** — the sidecar's existing `SidecarVersion` refusal, which already
  names both versions.
- **mismatched** — *"This game's Verse data was cooked by a different build of godot-verse (cooked
  8.2/abc1234, host 8.3/def5678). Export the project again."*

**Done when:** each of the three is reachable from a test — deleting the sidecar, truncating it, and
hand-editing the stamp — and the game refuses to start rather than starting without Verse.

---

## 7. Stage 4 — the test split

**Files.** `tests/integration/test_cases.gd` (new, the library), `tests/integration/test_main.gd`
(reduced to a driver), `tests/integration/export_check.gd` (new, the autoload),
`tests/integration/project.godot` (the autoload), `tests/integration/export_presets.cfg` (exists).

**The shape is `dodge-the-creeps/checks.gd`'s**, which already does this and is the file to copy
from: `extends RefCounted`, a `tree: SceneTree` the driver sets, `begin()` for everything that runs
at once and `step() -> bool` for the frame-stepped cases, `check(name, ok, detail)` printing one
line, a `failures` count. The two drivers are `test_main.gd` (`extends SceneTree`, as today) and
`export_check.gd` (an autoload, because `--script` is inside `TOOLS_ENABLED` and an export template
has no such thing — `dodge-the-creeps/export_check.gd` is 41 lines and is the model).

**`test_main.gd` is 1391 lines**, nearly all of it in `_init()` (155–1101) and `_process()`
(1102–1391). The split is mechanical; the tagging is not.

**The `editor_only` rule.** A case is `editor_only` if it needs one of the eleven compiler-side
entry points the runtime host answers `VH_ERR_UNSUPPORTED` for, or if it writes to `res://` — which
is a read-only pack in an export. Known in advance:

- `_check_reload_replaces_the_instance` (`:65-105`) — rewrites a fixture, calls `build_project`,
  `reload()`s. Both halves are impossible in an export.
- the second-build/generation case (`:490-495`) — `VerseRuntime.build_project()`.
- anything else the exported run turns up, which is how the rest get tagged: run it, read the
  failures, and tag only what the rule above covers. **A case that fails in an export for any other
  reason is a finding, not a tag.**

A tagged case prints `skip <name>  -- editor only` and counts into a third tally.

**Done when:** `test_main.gd` and the autoload print the same lines for every untagged case, and the
in-editor run's pass count is unchanged from today's 317.

---

## 8. Stage 5 — the export layer's second half

**Files.** `tools/run_tests.py` (`run_export`, `:528-634`).

The layer already exports `tests/integration` headless and asserts the tree, the `.pck` and the
sidecar. What it adds: run the result.

    <tmp>/integration.exe --headless --fixed-fps 60 -- --verse-check

`--fixed-fps` is not optional — headless, a `Timer` counts real seconds while the loop runs flat out
(the same reason `dodge-the-creeps`'s driver needs it). The layer requires the last case's line, a
zero exit, **and the expected skip count** (D5), so a case that stops running in an export is a
failure rather than a silently shorter log.

The tree assertions change as S-9 answers: `EXPORT_DATA_FILES` (`:515-524`) lists four cooked
`.uasset` paths that become the container pair, and `EXPORT_DATA_DIR` keeps its name.

`dodge-the-creeps` is **not** added to this layer (D4).

**Done when:** `python tools/run_tests.py` reports four layers, and the fourth exports, launches and
passes.

---

## 9. Stage 6 — R-DIST-10, by hand

The one check that cannot be automated from inside the build machine, recorded in
`docs/by-hand-findings.md` beside Phase 6's editor session:

1. Export `dodge-the-creeps` release from the 4.7 editor to a directory **outside the repo**
   (`C:\Temp\dtc-export\`).
2. Run it from a shell with `UE_ROOT` unset and no PATH entry pointing at the UE checkout, Godot or
   the repo's `bin/`.
3. Play it. The 30 checks in `checks.gd` are the yardstick and `export_check.gd` runs them, so
   `dtc.exe --headless --fixed-fps 60 -- --verse-check` is the fast version and playing it is the
   real one.
4. Record **what was scrubbed**, not just that it passed. The honest limit goes in the finding: a
   sandboxed run on the build machine cannot prove the absence of a machine-wide dependency such as
   a VC redistributable, and the finding says so.

**Done when:** the finding is written, with the scrub list and the limit.

---

## 10. Measurements owed

No thresholds (D7). Each of these is a number on a named machine, in §13 and in `spec.md` R-PERF:

| what | where it goes | what it is measured against |
| --- | --- | --- |
| the container step, in seconds and peak memory | §13, beside 7a's 4.3 s compile and 1.3 s cook | a `dodge-the-creeps` export |
| total export wall time | §13 | the same export, end to end |
| the shipped payload | §13 and `spec.md` R-DIST-11 | container + sidecar + runtime host + tbbmalloc, against 7a's 68 MB loose cook |
| exported startup: process start → `vh_init` returns → cooked project loaded → first Verse `_Ready` | `spec.md` R-PERF | the exported dtc, `--headless --fixed-fps 60`, against the editor's own compile-at-play |

---

## 11. ABI — the delta

**Expected: none** (D10). `vh_init_desc.CookedDirUtf8` (`verse_host_abi.h:548`) already names the
directory and the runtime host already decides what to do with it; a container inside it is not
something the consumer has to know about. `vh_host_kind()` and `VH_ERR_UNSUPPORTED` are 8.2's and
stand.

If a spike forces a field, it is a **minor** bump under the policy at the top of the header, both
DLLs rebuild, and `run_tests.py --build` follows.

---

## 12. Tests

- **abi** — the existing `verse_cook` case (`run_tests.py:177-239`) asserts what the cooker wrote.
  Its file list becomes the container pair; add an assertion that the container is non-empty and
  that the sidecar carries the stamp.
- **abi** — `host_smoke` against `verse_host_runtime.dll` with a cooked fixture, in Development and
  in Shipping, which is 7a's stage-2 "done when" and has never run.
- **export** — the tree assertions as they are, plus the launch and the skip count (§8).
- **by hand** — §9, and the debugger left untested and said so (D11).

---

## 13. What building this corrected

**Status: the wall this phase was written to get past is down, and a second one of the same family
is standing behind it.** Both spikes passed, stages 1–3 are built, `run_tests.py` reports four green
layers, and an exported `dodge-the-creeps` boots, loads its cooked Verse, attaches its scripts and
runs their own code. What it cannot do is call *Godot* — the first mirrored call takes the process
down. §13.8 is that wall; everything before it is what is settled.

### 13.1 S-8a — yes, and not through the entry point D1 named

`CreateIoStoreContainerFiles` builds the container and carries the cells, as §3.3 said it would. What
D1 did not know is how much has to be *manufactured* to call it, and that it does not read the
command line it is handed:

- **It parses `FCommandLine::Get()`, not its own argument** (`IoStoreUtilities.cpp:10338-10360` reads
  exactly one switch from `CmdLine` and every other one from the process's line). So the cooker
  appends its switches to its own command line, calls, and puts the original back. One call, at the
  end, with nothing after it but the exit.
- **Three files have to exist that a cook of this shape does not produce.** A `-ScriptObjects` file,
  which `FPackageStoreOptimizer::Initialize()` + `CreateScriptObjectsBuffer()` produce from the
  process's own registered `/Script/` objects. A `-Commands` list and the `-ResponseFile` it names,
  which are UnrealPak's plain-text formats. And a `-PackageStoreManifest`, which is a **compact
  binary oplog** — but only one field of it is read on this path: a legacy cooked `.uasset` carries
  no package name, so `FindOrAddLegacyPackage` asks the package store for one by filename
  (`IoStoreUtilities.cpp:1742`) and silently drops any file it cannot name. Everything else an oplog
  entry can hold — imports, shader maps, chunk hashes — is rebuilt from the cooked header. So the
  manifest this cooker writes is a package name and a chunk id per package, and nothing else.
- **`FPackageStoreOptimizer::Initialize(ScriptObjectsBuffer)` leaves `ScriptCellsMap` empty**, because
  `CreateScriptObjectsBuffer` does not serialise it — the cell half is built only by the no-argument
  `Initialize()`, out of `$BuiltIn`. It costs one "referencing missing script import" warning per
  built-in cell and nothing else: `ProcessImports` writes the `FPackageObjectIndex` from the verse
  path either way (`PackageStoreOptimizer.cpp:470-481`).
- **`IoStoreUtilities` is a `Developer` module and `FPackageStoreOptimizer` is in its `Internal`
  folder**, so the cooker takes one more `PrivateIncludePaths` line, exactly as it already did for
  CoreUObject's.

**Cost:** the container step is **0.5–0.7 s** over a 68 MB loose cook, every time it has been run.

**And the payload is 7.3 MB, not 68.** Uncompressed the container is *larger* than the loose cook it
came from — 76 MB — because the zen header replaces the legacy one and nothing is packed. With
`-compressionformats=Oodle` on the command line and `-compress` per response-file entry it is
**7.3 MB**, a tenth of 7a's, and the container step does not get measurably slower. D3's "the
mirror's 67 MB ships as it is" is still true of what is *cooked* and no longer true of what ships.

**The global container is not needed and is not shipped** (§3.5, measured: the game loads with
`global.utoc`/`.ucas` deleted). `CreateIoStoreContainerFiles` will not run without producing one, so
it is written beside the loose cook and thrown away with it. D9's directory therefore holds two files
and not four.

### 13.2 S-8b — yes, and the mount needed one line nothing in this host was calling

The mount is the iostore half of `FPakPlatformFile::Mount` with the pak half taken out: make the
file backend, `FIoDispatcher::Mount` it, make an `FFilePackageStoreBackend`, `FPackageStore::Mount`
it, then `IFileIoDispatcherBackend::Mount` each `.utoc`. Twenty lines, and both backends live in
PakFile's `Private`/`Internal` folders, which a monolithic link makes reachable and two
`PrivateIncludePaths` lines make includable. §3.4 was right that this is not an editor-only path.

**What §3.4 did not have is that the I/O dispatcher is never brought up in this host.**
`FIoDispatcher::Initialize()` only *constructs* it; `InitializePostSettings()` is what initialises
the backends and starts the dispatcher thread, and LaunchEngineLoop calls that only under
`USE_IO_DISPATCHER`, which is `WITH_ENGINE || WITH_IOSTORE_IN_EDITOR || !(IS_PROGRAM || WITH_EDITOR)`
(`LaunchEngineLoop.cpp:98-99`) — all three false for a Program with no Engine. The only thing that
had ever touched the dispatcher was the async loader's own `Initialize()`
(`AsyncPackageLoader.cpp:195-200`), which allocates it and stops.

So `FIoDispatcher::IsInitialized()` answered **true** and the dispatcher was not running. `Mount`
took the backend, skipped `Backend->Initialize` and skipped `StartThread`
(`IoDispatcher.cpp:643-659`), and every read was issued and never completed: **no error, no timeout,
a `LoadPackage` that stays queued forever.** Two hours went into that, most of it into the async
loading thread, which was not it. `FIoDispatcher::InitializePostSettings()` before the mount is the
whole fix, and it is idempotent.

**Pass criteria, in the design's order:** `DoesChunkExist` true; `FPackageStore` resolves
`/GodotAttributes/_Verse`; `LoadPackage` returns a package with **no `Missing VClass for VerseClass`
fatal** — which is the line that says the cells arrived, and the sentence this phase existed to
delete; `AddCompiledUPackage` accepts it and a class from the project's own package instantiates.

**The backends must be released before `AppExit`.** They are allocated through GMalloc and a
file-static `TSharedPtr` holding one into static destruction is a segfault after everything has
worked — which is what the first clean run did. `ReleaseCookedContainers()` is called from
`vh_shutdown`, and the comment already in `vh_shutdown` said why before it existed.

### 13.3 S-9 — yes, and the assertion list is four rows shorter

`GForeignEngineDir` wants a directory with a `Binaries/` child and nothing else
(`GenericPlatformMisc.cpp:1408-1415`), so `<data>/Engine/Binaries` still does its job with
`<data>/Engine/Content` gone. `EXPORT_DATA_FILES` is two container files, the marker and the sidecar.

### 13.4 A container carries package *ids*, so the names come from the sidecar

Nothing in the design anticipated this and it is load-bearing. `FPackageName::DoesPackageExistEx`
answers None for a path outside a registered mount point before it ever asks the I/O dispatcher
(`PackageName.cpp:2474-2477`), so mount points still have to be registered by name — and with the
loose tree gone there are no directories left to read the names off. An `FIoContainerHeader` carries
`FPackageId`s, which are hashes.

So the sidecar carries the list: `verse_classes.json` gains `packages`, and
`RegisterCookedMountPoints` reads it before it has loaded anything. That is also where D6's stamp
went, and the sidecar version is **2**.

### 13.5 D6's stamp is baked into the binaries, not read from a file beside them

The design had the cooker read its provenance out of `verse_host.build.txt`. A game directory is
exactly where such a file goes missing, and a stamp that can be absent is not a stamp — so
`tools/build_host.py` generates `host_build_id.gen.h` (the godot-verse commit and the engine commit)
into `bin/` and stages it into the host's `Private/` the way it already stages the ABI header. It is
rewritten only when it changes, so it costs no relink.

All three refusals are reachable and were run by hand:

- missing — *"Verse data not found at `<path>`. The export is incomplete; export the project again."*
- truncated — *"… is not valid JSON"*; foreign — *"… was written by sidecar version 99; this host
  reads version 2. Re-export the project."*
- mismatched — *"This game's Verse data was cooked by a different build of godot-verse (cooked
  8002/deadbee, host 8002/2b171df). Export the project again."*

### 13.6 Three defects that only an exported game could show

None of these is about the container. All three had been shipping since 7a, behind a wall that
stopped anything from reaching them — which is the argument for D2's "the export layer launching what
it exported", made by the thing itself.

- **The cook directory is reused and was never cleared.** The export plugin cooks into one directory
  under the user's cache, per project, and ships it whole. A 7a loose `Cooked/GodotAttributes/_Verse.uasset`
  sitting beside this phase's container is not a leftover — it is the file the runtime host finds
  *first*, and the game dies on the VCell wall this phase exists to get past. The cooker now deletes
  what it owns (`Cooked`, `Engine`, `_loose`, the sidecar) before it writes, bounded rather than a
  wipe of a path handed in from outside.
- **`sources.txt` shipped.** The plugin wrote the cook manifest *into* the directory it ships, so
  every exported game carried a list of absolute paths on the author's machine. It is written beside
  that directory now (R-DIST-11).
- **`host_has_compiler()` tested for a symbol that is always there.** It asked whether
  `vh_compile_project` resolves; the ABI header declares all eleven compiler entry points
  unconditionally and `VerseHost.cpp` defines them for every host kind, answering
  `VH_ERR_UNSUPPORTED` when called. So in an exported game `build_project` took the *compiling*
  branch, compiled the twenty one-byte `.verse` stubs an export ships, published nothing, and every
  script came up with no class. `vh_host_kind()` — which ABI 8.2 added for exactly this — is the
  test now.
- **`analysis_is_current` said false forever.** With `project_built` true and nothing analysed, every
  script queued a check that no compiler could run and waited; `valid` stayed false and not one
  scene came up with its script attached, with no error anywhere, because waiting is not failing.
- **`dodge-the-creeps/export_check.gd` had never been run.** An autoload's `_ready` runs while the
  root is still adding the main scene, so its `remove_child` was "Parent node is busy" and its
  `add_child` was "Parent node is busy setting up children". Deferred now.

### 13.7 A runtime host has no semantic program, and declared types come from the sidecar

**This is the largest thing the design did not know, and it is why an exported game can run Verse at
all.** Reading a field, calling a method and emitting a signal each need the *declared* type: the
bytecode has erased it by the time a `VValue` exists, so the semantic program was the only view that
had one (the comment at `InstanceCall` said so already). A runtime host has no semantic program and
can never build one — `MakeDevEnvironment` is one of the four `ISolarisModule` members
`WITH_VERSE_COMPILER=0` takes away.

Unfixed, `InstanceCall` dereferenced an invalid `TSPtr` on its first call; guarded, every call in an
exported game answered `VH_ERR_NOT_FOUND`, `DescribeMemberType` described nothing and `BindSignals`
returned before binding one.

So the analysis records them and the sidecar carries them across. `FAnalysisSnapshot::FClass` gains
an opaque `FDeclaredTypes` — member name → declared type, decorated method name → parameters and
result, signal name → payload shape — filled once per class per analysis by `CollectDeclaredTypes`
and read back by `RecordedTypes` wherever `GIde` is invalid. Two things made it possible:

- **`FMemberType` needed a uLang pointer only for two strings.** `ReferenceClass` was read for
  `AsNameCString()` and `QualifiedNameOf()` and for a null test; it carries both names beside the
  pointer now, and the pointer stays for the one analysis-only caller that needs the class itself
  (`GetClassSignals`' payload walk).
- **`FStructLayout` is a generated table every host links**, so a mirrored struct is found again by
  name rather than carried.

Measured: `tests/host_smoke`'s `exports` class, cooked and mounted in a runtime host, answers all
eighteen of its methods — ints, floats, strings, arrays, enums and a `@export` read — exactly as the
editor host does.

### 13.8 The wall behind the wall: a cooked native's C++ thunk is not serialisable

**Found and removed after the rest of this section was written**, by a session that did nothing but
chase it. What it had looked like: a script's own Verse ran and the first call it made into the
mirror was a raw access violation inside `Verse::VFunction::Invoke`, with no UE crash report, no
diagnostic, and the Godot callback never reached. Every `_Ready` in `dodge-the-creeps` died on its
first `GetNode`; `tests/host_smoke`'s classes, which call no mirrored function, ran to completion.

**`Verse::VNativeProcedure` holds its C++ entry point as a raw function pointer**, `FThunkFn Thunk`
(`VVMNativeProcedure.h:38`). `SerializeImpl` writes the parameter count, the name and the path
registry payload and **not** the thunk; `SerializeLayout` constructs the loaded cell with
`/*InThunk*/ nullptr` (`VVMNativeProcedure.cpp:37-50`). The interpreter then calls it with no check
at all — `(*NativeProcedure->Thunk)(Context, Self, Args)`, `VVMInterpreter.cpp:2666`. That is a jump
to address 0, which is why there was no crash report and no handler: `RIP` is zero, so there is no
unwind info for anything to find.

Thunks go back on through `Verse::VNativeProcedure::SetThunk`, and the engine has two callers.
**Class- and struct-scoped natives are rebound at load**, by `UVerseClass::BindVerseCallableFunctions`
(`VVMVerseClass.cpp:2237-2243`). **Module-scoped ones are rebound only at build time**, by
`FVerseNativeModule::TryBindVniModule`, whose one caller is the assembler —
`Engine->TryBindVniModule(CurrentPackage, AssetPath)`, `VVMAssembler.cpp:322`. The engine says so
itself, two lines above the function:

```cpp
// Called at build time (via FVerseVmAssembler) for modules.
// TODO: Call at load time when we start using VerseVM cooked framework packages.
bool FVerseNativeModule::TryBindVniModule(...)
```
`VerseNativeModule.cpp:340-341`

A compiler-less host **loads** its VNI packages instead of compiling them, so that call never
happens and every module-level `<native>` in every VNI package comes up with a null thunk.
`VhCallValue` (the whole of `/Godot.org/Godot`), `Print` and `Sqrt` are all module-level — which is
exactly "a script's own Verse runs, and the first mirrored call dies". Bisected with a four-method
fixture cooked as one class: a local `set`, and constructing a `vector2` and reading its `.X`, both
ran; `vector2.Length()` — ordinary Verse, no Godot in it — `Print(...)` and `QueueFree()` all
faulted. Cross-package *data* was always fine; cross-package *native call* was always fatal. It was
never the Godot boundary and never marshalling.

**`GodotVerse::RebindVniModuleNatives`** (`HostCooked.cpp`, called from `LoadCookedProject` after
`AdoptCookedGeneration`) does the assembler's walk with no assembler: for every loaded VNI
`VPackage` it derives the module asset names and calls the same public `TryBindVniModule`. Three
things that cost that session time and are the reason it is written the way it is:

- **The module list is derivable from the loaded package alone.** A definition's key is its
  decorated path — `(/Verse.org/Verse/(/Verse.org/Verse:)Print:)Native` — so the module is the
  leading scope with everything from its first `(` cut off. Taken relative to
  `VPackage::GetRootPath()`, with `/` spelled `_` and the empty one spelled `_Root`, that is exactly
  the `MangledVerseName` the VNI generator writes into each registration
  (`DefinitionInfo.cpp:214-222`). Only `:)Native` definitions are considered, so the candidates are
  the modules that actually have thunks.
- **`_Root` is not enough.** `/Verse.org` is a package whose root module holds nothing — `Print` is
  in the *submodule* `Verse`. The mirror binds at `/Engine/_Verse/VNI/VerseHost._Root`; the standard
  library at `/Verse/_Verse/VNI/Verse.Verse`, `.Verse_Easing` and `.Random`.
- **Do not dedupe by `UPackage`.** `GlobalProgram` holds several `VPackage` objects that share one,
  and `SetThunk` looks the procedure up in the specific `VPackage` it is handed. Binding only the
  first reproduces the crash exactly.
- A parametric class writes its scope undecorated, so the cut can yield a *class* rather than a
  module, and asking `TryBindVniModule` for a type's key trips its own ensure. The UObject beside the
  name separates them: a module's is a `UVerseModuleClass`, a class's a `UVerseClass`.

**A second, real defect was found on the way, and it is what the null-import evidence above was.**
`WouldAssertOnSave` was skipping whole packages, and one of them was
`/Solaris/_Verse/VNI/VerseNative` — where `/Verse.org/Concurrency`'s `task` and `awaitable` and the
`/Verse.org/Native` attributes live. The cook shipped **7** packages where the program had **9**
savable ones, and `LogStreaming: ImportPackages: SkipPackage: … Skipping non mounted imported
package None (0x3ABF75E5CFC7C097)` is `FPackageId::FromName("/Solaris/_Verse/VNI/VerseNative")`. All
41 `FExportArchive: … Import index N is null` lines resolved into that one absent package. It was
skipped because exactly one object in it, `VerseNative.Persona`, is a `UVerseClass` standing for a
Verse **module** — and a module has no `Verse::VClass`, which `SavePackage2.cpp:2076` asserts on
unconditionally even though every use of `Class` below it is already null-guarded.
`FScopedModuleExportSuppression` takes that one export out of the export set for the length of one
save rather than dropping the package: `RF_Transient` alone does not do it, because
`FSaveContext::GetSaveableStatusNoOuter` reads the flag only for a non-native object
(`SaveContext.cpp:233-243`) and a VNI-generated `UVerseClass` carries
`EInternalObjectFlags::Native`, which is what `UObject::IsNative()` answers from. Both flags come
off and both go back on. Null imports: **41 → 3**, the three being references to the suppressed
`Persona`, which nothing on this bridge can name.

**Neither half needs an engine change**, and both are in `host/`. If Epic ever acts on the TODO at
`VerseNativeModule.cpp:340`, `RebindVniModuleNatives` becomes redundant and harmless — `SetThunk` is
idempotent.

Two of the three leads this section originally named were wrong, and are recorded so nobody runs
them again. **Public export hashes were never the problem**: the hashes were correct and the imports
were null because the *package* was absent. **The `-ScriptObjects` buffer was never the problem**
either — script imports resolve from `FGlobalImportStore::AddScriptObject` registrations made in
memory, 1668 of them, with no unresolved script import in the log. The third lead is **answered**:
see §3.5.

Measured after the fix, through `tests/cooked_probe` against a cooked `dodge-the-creeps`:
`gameplay/mob`'s `_Ready` runs its `GetNode` and answers 0, and `OnScreenExited` runs its
`QueueFree` and answers 0.

### 13.9 What is not built

Stages 4, 5 and 6 — the `test_main.gd` split, the export layer launching what it exports, and
R-DIST-10 by hand — were blocked on §13.8 and are unblocked rather than done. Nothing has yet
*launched* an exported game: the export layer still asserts only the tree it produced, and
`tests/cooked_probe` answers every Godot call with a stub. `spec.md`'s R-DIST-9/10/11 stay where
they are until one runs.

---

## 14. Deliberately not built

- **The engine patch** (D12) — its own phase if the container route fails.
- **`FZenStoreWriter`** (route 2 of `phase-7-design.md` §13.7) — an export that starts a Zen server
  is a dependency this bridge should not take on without a decision.
- **Trimming the mirror** (D3) — recorded, not done.
- **Caching the mirror container across exports** (D7).
- **Linux, the missing `dlopen` path, and macOS** (D2) — `src/verse_host.cpp` is `LoadLibraryExW`
  with no `dlopen` branch, which is portable work that does not need a cross-toolchain, and it is
  still out of this phase.
- **Shipping-versus-Development per template** — 7a's D12, still "not true yet".
- **The debugger and profiler in an exported game** (D11).
- **In-PCK cooked data with extraction at first launch** — held in reserve since 7a.
- **Phase 4b.**

---

## 15. Exit

- **`run_tests.py` reports four layers, all green**, and the fourth **launches** what it exported:
  `tests/integration` exported headless from the 4.7 editor, run, every untagged case passing and
  the tagged ones printed as skipped with the count asserted.
- **`dodge-the-creeps` exported, run sandboxed, 30 checks green**, recorded in
  `docs/by-hand-findings.md` with what was scrubbed and what that cannot prove (§9).
- **`spec.md`**: R-DIST-9 → **done**, R-DIST-10 → **done**, R-DIST-11 → **done**, each with the
  Windows-only caveat in the line (D8). R-PLAT-1 and R-PLAT-5 unchanged at **part**. **OQ-18
  closed**, either way.
- **§13 is written**, with §10's measurements in it.

**What 7b will not claim.** Linux and macOS are untouched. The cooker is still 732 MB and still
needs a UE source checkout — Phase 8's wall, not this one's. The exported game's debugger is
untested. And a sandboxed run is not a clean machine, which the finding says in its own words.
