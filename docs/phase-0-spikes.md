# Phase 0 spikes — findings

Three questions in [`spec.md` §14](spec.md) could each have changed the shape of the host, and
[`roadmap.md`](roadmap.md) put them before any feature work for that reason. This is what ran, what
it showed, and what the answers cost.

Everything below was measured against UE `ue6-main` at `203d76492e` (2026-09-08) with **no engine
changes**. The spikes' own code is not in the tree: it was written, run, and reverted. What
survives is this document, the answers now in the spec, and the provenance record described under
[0.1](#01--the-engine-dependency).

| spike | question | answer |
| --- | --- | --- |
| **S-1** | Can VerseVM run precompiled Verse? (OQ-2) | **Yes**, but producing the artifact is a *cook*, which needs an editor-class binary. The host becomes three targets, and one of them does not exist yet (OQ-10). |
| **S-2** | Which hot-reload mechanism? (OQ-8) | **Fresh package name per generation**, and it is cheaper than the roadmap assumed. |
| **S-3** | How does a project escape one flat scope? (OQ-5) | **Submodules inside the one user package**, built from the project's directory tree. Prototyped and run. |

---

## S-1 · Can VerseVM run precompiled Verse? (OQ-2)

### The engine already has both halves

A shipping game client **does not contain the Verse compiler.** `Solaris.Build.cs` and
`CoreUObject.Build.cs` both define `WITH_VERSE_COMPILER=1` only when
`Target.bBuildWithEditorOnlyData || Target.Type == TargetType.Server`; everything else gets `=0`,
and with it a `Solaris` module built without `uLangIdeSupport`, `uLangDigests` or `VerseCompiler`.

Where such a target gets its Verse from is written down in `SolarisCompiledPackageRegistry.h`:

> A compiled Verse package can come from the compiler, when uncooked, or from the loader, when
> cooked. Cooked packages can be loaded when mounting a plugin in editor builds, or by VHR in
> server builds.

The loader path is ordinary UObject loading. `FSolarisModule::AddMountedPlugin`
(`SolarisModule.cpp`) calls `LoadPackage` on each cooked package name and hands the result to
`FSolarisRuntime::AddCompiledUPackage`, which walks the package's objects and registers the
generated types. On the write side, `SavePackage2.cpp` carries a full VerseVM cell import and
export table next to the object one — `SaveContext.GetCellImports()`, `Linker->CellExportMap`,
`Verse::FStructuredArchiveVisitor` — and `VPackage`, `VProcedure` and every other cell type
implement `SerializeLayout`/`SerializeImpl` for it.

So the answer to the literal question is yes: serialised Verse loads and runs with no Solaris
compiler and no `.verse` files on disk. This is how every shipped UEFN client works, and it is
also why the host's current constraint — it must load from `Engine/Binaries/Win64` because VNI
records each package's source directory relative to the module (`VerseHost.package.gen.cpp` ends
in `TEXT("../../Source/Programs/VerseHost/Verse")`) — is a *compiler* constraint. `ConvertVniPackage`,
which turns that path into something the toolchain reads, is only ever called from code inside
`#if WITH_VERSE_COMPILER`.

### The wall is on the producing side

Getting the compiled package *out* of the host is where it stops. A throwaway export was added that
looks the package up and saves it:

```cpp
UPackage* Outer = Verse::GlobalProgram->LookupPackage(ScriptPackageName())->GetOrCreateUPackage(Context);
UPackage::Save(Outer, nullptr, *Filename, FSavePackageArgs{...});
```

It aborts the process:

```
Assertion failed: SaveContext.IsCooking()
  [File:.\Runtime/CoreUObject/Private/UObject/SavePackage/PackageHarvester.cpp] [Line: 809]
  FPackageHarvester::TryHarvestCellExport()
  InnerSaveInternal() / UPackage::Save()
```

**VerseVM cells are only written on a cooking save.** An uncooked save refuses to harvest them at
all, which is consistent with the registry's comment: uncooked Verse *is* the source, and the
compiled form only exists as a cook output.

A cooking save needs `FSavePackageArgs::ArchiveCookData`, which needs an `ITargetPlatform&`. That
part turned out to be cheap, and was carried all the way:

- `bBuildDeveloperTools = true` plus `TargetPlatform` in `VerseHost.Build.cs` compiles **and links**
  into the monolithic Program target.
- The target platform manager then reported `No target platforms found!`, because
  `InitializeSinglePlatform` loads `<Platform>TargetPlatform` by module name and a monolithic build
  only knows the modules it linked. Adding `WindowsTargetPlatform` (which pulls its Settings and
  Controls siblings) fixed it — those modules guard their Engine dependencies behind
  `bCompileAgainstEngine`, so they are light in a target that does not compile against Engine.
- The host then got as far as `SpikeSavePackage: cooking /GodotScripts/_Verse ... for Windows`.

And there it stopped, on the two lines that open `FSaveContext::FSaveContext`
(`SaveContext.cpp:63`):

```cpp
// if we are cooking we should be doing it in the editor and with a PackageWriter
check(!IsCooking() || WITH_EDITOR);
checkf(!IsCooking() || PackageWriter, TEXT("Cook saves require an IPackageWriter"));
```

The assertion text came back as `!IsCooking() || 0` — `WITH_EDITOR` expanded to zero. **A cook
requires an editor-class binary.** The host has `bBuildWithEditorOnlyData = true`, which is what
gives it the compiler, but `WITH_EDITOR` needs `bCompileAgainstEditor`, which needs
`bCompileAgainstEngine` — the same wall the `codegen-once-per-process` investigation recorded
hitting on NavigationSystem, LevelStreaming and CheatManager. A cook also needs an `IPackageWriter`,
supplied through `FSavePackageContext`, which is cooker infrastructure rather than a parameter.

**Writing the cells ourselves is not the shortcut it looks like.** `VPackage::SerializeImpl` and
`FStructuredArchiveVisitor` are public and take any `FStructuredArchive`, so pointing them at a file
seems like twenty lines — but `FArchive::operator<<(Verse::VCell*&)` on the base class is an empty
no-op (`Archive.h:1283`). Cell identity is supplied by `FLinkerSave`'s cell import and export tables,
which is precisely what `SavePackage2` builds. Serialising outside SavePackage means reimplementing
that, and a package whose definitions are cells would otherwise write out with every cross-reference
silently dropped.

### What it means

`host/` becomes three binaries rather than the two the spike expected, because the cook cannot live
in the lean host:

- the **editor host** — what exists today: `bBuildWithEditorOnlyData = true`, compiler present,
  `WITH_EDITOR = 0`. Compiles and runs `.verse` in the Godot editor. Unchanged.
- a **cooker** — an editor-class target (`bCompileAgainstEditor`, and therefore
  `bCompileAgainstEngine`), which runs only at export and produces the `.uasset`. It is heavy, and
  that is tolerable for something that never ships and never loads in the editor. Building it is
  the open item: the one previous attempt at `bCompileAgainstEditor` for a Program target failed on
  Engine module links, and it was abandoned then because the goal was a *lean* host. A cooker has
  no such constraint, so the attempt is worth making again on its own terms.
- a **runtime host** — `bBuildWithEditorOnlyData = false`, `WITH_VERSE_COMPILER = 0` — which loads
  the cooked package and runs it. This is what an exported game ships.

Four consequences worth stating plainly:

1. **The export story is decided in principle.** An exported game ships precompiled Verse and a
   runtime-only host; it ships neither `.verse` sources nor the compiler. R-DIST-8 and R-DIST-11
   resolve that way.
2. **But the cost landed on the producing side, not the consuming side**, which is the opposite of
   where the spike brief expected to find it. Loading cooked Verse is ordinary engine machinery.
   Producing it needs a cooker that does not exist yet. Phase 7 should budget for building one, and
   the first thing that phase does is find out whether an editor-class Program target is reachable
   at all — if it is not, the fallback is cooking through a real UE editor or commandlet process,
   which is out-of-process compilation returning as the export architecture exactly as 0.3
   predicted it might.
3. **Mobile and web get simpler questions.** OQ-3 and OQ-4 no longer have to ask whether Solaris
   can be built for those platforms — only whether the *runtime* half can. That is a much smaller
   binary and a much smaller dependency set. It does not make web reachable; it removes one of the
   three reasons it was not.
4. **It changes the licensing picture** (OQ-1, R-DIST-6). The binary a player receives would
   contain no Verse compiler at all. Whether that helps is a question for the answer to OQ-1, not
   for this document, but it is a materially different artifact from the one the spec assumed.

---

## S-2 · Which hot-reload mechanism? (OQ-8)

### The recorded cause was true and not the binding constraint

The repo's standing finding — `NotifyCompiledVersePackage` hands the loader's package ref a
`UPackage` only `#if !WITH_EDITOR`, and republishing that package then asserts — is correct. It is
also only reachable by publishing *the same package* twice, and nothing obliges us to do that.

The first attempt gave each generation its own package name (`GodotScripts_1`,
`GodotScripts_2`, …) and its own attribute package alongside. Generation 1 compiled and ran.
Generation 2 aborted — but not on our package:

```
Assertion failed: !ObjectItem->HasAnyFlags(EInternalObjectFlags::LoaderImport)
  Object='VerseFunction /Solaris/_Verse/VNI/VerseNative.Persona:FromJsonFromArray'
  FLoadedPackageRef::FPublicExportMap::PinForGC()
  FGlobalImportStore::AddPackageRef()
  FAsyncLoadingThread2::NotifyCompiledVersePackage()
```

The collision was in `VerseNative` — a **native VNI package**. Nothing had told the build that the
native packages were already compiled, so it recompiled and republished all of them.

`FSolarisModule::IncrementalizeProjectSource` is what tells it. It walks the source project and
marks every package already compiled in this process as `EPackageRole::External`, so the build
skips it and reads its digest instead; VNI packages get an extra rule of their own
(`bNeedRebuild = IsCompiled(PackageName) == None`). The host never called it. One call before each
`BuildAll`, and the second generating build compiles only the packages that are actually new.

### Measured

With both pieces in place — `IncrementalizeProjectSource` before each build, and a fresh name for
the host's own two packages — a one-class script was compiled 25 times in one process, each
generation with a different declared default:

| | |
| --- | --- |
| generations | 25, all succeeded |
| first build | 402 ms |
| steady state | 161–220 ms, no upward trend |
| working set after `vh_init` | 89 MB |
| after generation 1 | 199 MB |
| after generation 25 | 203 MB |

The leak the roadmap warned about is real and is about **0.5 MB per generation** for a one-class
project — the previous generation's `VPackage`, its `UPackage` and their pinned exports. The
+110 MB at generation 1 is the compiler and the native packages, paid once.

Two behaviours worth recording because they are design inputs, not just measurements:

- **Each generation picks up the edited source.** Reading the class default back through
  `vh_class_default_field` returned the new value every time, which is R-ITER-3 — the requirement
  the spec singled out as impossible today.
- **Instances made by an earlier generation keep working, against their own generation's class.**
  An instance created in generation 1 still read `Speed = 10` after generations 2 and 3 had
  published `20` and `30`. That is the right default for Godot, which holds live nodes across a
  reload: nothing is invalidated underneath the engine, and swapping a node onto the new class
  becomes a deliberate act (R-ITER-4) rather than something the reload does to you.

### The cost nobody had priced

The script package used to be the IDE's own, whose name — `ISolIdeDataSource::DefaultDataSourceName`,
i.e. `SolIdeDataSources` — is fixed. A fresh name per generation means the host stops calling
`ISolarisIde::AddDataSource` and owns the package itself, via
`CProgramBuildManager::FindOrAddSourcePackage` and `AddSourceSnippet` — which is what the host
already does for its attribute package, so the path was known to work.

What comes with the IDE's data source and not with a bare package is the ability to replace a
snippet's text for analysis. Without it, three completion tests and one analysis test in the smoke
suite fail. Restoring it took a ~30-line `ISourceSnippet` implementation with a text setter that
also drops the cached VST — the toolchain clones a valid cached VST rather than reparsing, so
leaving one behind analyses the text you just replaced. With that in place the smoke suite is
**247/247**, the same as before the spike.

The host must also set the new package's dependency list itself: `EnsureDataSourcePackageExists`
did that for the IDE's package (every other package in the project), and nothing does it for ours.

### Answer

**Fresh package name per generation wins**, and the other two candidates lose for specific reasons:

- *Out-of-process compilation* is no longer needed for hot reload. It remains the shape of the
  **export** pipeline (S-1), which is a different problem with a different deadline. Building it
  for reload as well would be paying an architectural price for something a package name solves.
- *An engine change* is unnecessary. Nothing here needs one, which is also why
  [0.1](#01--the-engine-dependency) resolved the way it did.

Full hot reload (§10, R-ITER-1 through R-ITER-5) is therefore buildable at roughly the cost of the
prototype: ~200 ms per reload and a bounded leak. That is a different phase-ordering problem than
the roadmap assumed — see below.

---

## S-3 · How does a project escape one flat scope? (OQ-5)

The question asked whether the host can publish more than one user package. It can — the attribute
package proves it — but that is not what modules are made of.

**A package carries a module tree.** `CSourcePackage::_RootModule` is a `CSourceModule` with
`_SourceSnippets` and `_Submodules`, and `CToolchain::FillInVst` walks it, emitting a
`Verse::Vst::Module` node per submodule. `Desugarer::DesugarModule` turns each of those into a
`CExprModuleDefinition` — a real Verse module with its own scope. Two files in different submodules
may both define `player`.

The host gets one flat scope because `CSourceProject::AddSnippet` puts every snippet in
`_RootModule` and nothing ever adds a submodule. Epic's own code for building that tree from a
relative path is `ResolveModuleForRelativeVersePath` in `SolarisModule.cpp`, used when a project
takes on additional packages:

```cpp
uLang::FilePathUtils::ForeachPartOfPath(RelativeVersePath, [&](const uLang::CUTF8StringView& Part) {
    if (Part == ".." || Part == ".") { return; }
    if (uLang::CSourceFileProject::IsValidModuleName(Part)) {
        Module = FindOrAddSubmodule(Module, Part);   // CSourceModule::New(Part); _Submodules.Add
    } else {
        /* ErrSystem_InvalidModuleName */
    }
});
```

So the recipe is: for each `.verse` under `res://`, resolve its path relative to the project root
into a chain of submodules and add the snippet to the leaf. `res://gameplay/player.verse` becomes
`/user@localhost/gameplay/player`, and the one-top-level-name-per-file rule narrows from
project-wide to directory-wide. A directory name Verse cannot spell is a diagnostic pointing at the
directory, which is the right error.

### Run, not just read

The prototype replaced `AddDataSource` with a host-owned package and placed each snippet in the
submodule its directory named, against three files:

```
root.verse             root := class(object)     — reads a member off each player below
gameplay/player.verse  player<public> := class   — Label = "gameplay player"
ui/player.verse        player<public> := class   — Label = "ui player"
```

Two files with the same top-level name, which is exactly what the flat scope forbids today. The
package came out as:

```
  (/user@localhost:)root
  (/user@localhost/ui:)player
  (/user@localhost/gameplay:)player
```

`root` instantiated, `Ready` ran, and it printed `gameplay player` then `ui player` — both classes
alive at once and both reachable by qualified path. The smoke suite stayed at **247/247**, since a
project whose files all sit in one directory resolves to a root with no submodules, which is what it
does today.

Three details the run settled that reading did not:

- **A directory-derived module needs no declaration of its own.** No `.vmodule` file, no `module`
  macro; the submodule simply exists and its `<public>` members are reachable. Only the definitions
  need `<public>`.
- **The project root has to come from somewhere.** A module path is a path *relative to* something,
  and the ABI carries only absolute file paths. The prototype inferred the root as the longest
  common directory, which works and is wrong in one case: a project whose scripts all live in one
  subdirectory would silently root there. Phase 2 should pass `res://` across the ABI instead.
- **Ambiguity surfaces at the use site.** `root.verse` has `using` for both submodules and names
  both `player` classes by qualified path; that compiles clean. This is the behaviour README already
  describes for two `using`s that define the same name.

Two things this still does not settle, both Phase 2's business:

- **Whether godot-verse writes the `using` or the author does.** A member needs `<public>` and the
  importing file needs the import. A design decision, not an unknown.
- **The rename/move story.** Moving a `.verse` file between directories changes its Verse path, and
  every reference to it. Godot's resource UIDs do not help; this is the problem C# namespaces have,
  with the same answer — the author fixes it.

---

## 0.1 · The engine dependency

The roadmap put the choice as "a documented patch set, or a UE fork as a submodule", and leaned
toward the fork on reproducibility grounds — *a spike result is worthless if nobody can reproduce
which engine state produced it.*

**Decision: a documented patch set, and no patches yet.**

The evidence is the spikes themselves. All three ran against a stock `ue6-main` checkout with zero
engine changes, and S-2 — the one that looked most likely to need an engine change, and whose
recorded cause was an engine `#if` — turned out not to need one. A fork imposes a second
multi-gigabyte remote and a permanent merge obligation on a repository whose whole appeal is being
cheap to check out, in exchange for solving a problem we do not currently have.

The reproducibility half of the argument stands on its own and was worth building regardless, so it
was: `tools/build_host.py` now writes `verse_host.build.txt` beside the DLL it produces, recording
the engine's commit and branch, the count and paths of any local engine changes, the ABI version
and godot-verse's own commit. `bin/host_smoke.exe` prints it as its first output, so a test log
carries the engine revision it was produced against.

```
[smoke] provenance: abi_version=27
[smoke] provenance: engine_commit=203d76492ebc201b95a505d16cbc045d5a38d37d
[smoke] provenance: engine_branch=ue6-main
[smoke] provenance: engine_local_changes=0
```

**Revisit the fork when** either a shipped feature requires a patched engine, or the patch set
reaches three patches. Until then, `engine_local_changes=0` in the provenance record is the claim
being made, and it is checked on every build.

---

## What remains open

Everything asserted above was run. What is left is one question this phase deliberately does not
answer, and one it hands to a later phase:

1. **Is an editor-class Program target reachable?** S-1 establishes that the cook needs one; it does
   not establish that one can be built. The single prior attempt at `bCompileAgainstEditor` failed
   on Engine module links, but it was made for a lean runtime host, where the weight was the
   objection. For a cooker that runs only at export, weight is not an objection. **Phase 7 opens
   with this**, and its fallback — cooking through a UE editor or commandlet process — is known.
2. **Loading a cooked package in a runtime host** was not exercised, because nothing can produce one
   yet. The engine's own cooked-Verse path is the evidence that it works, and it is the half of the
   split that is ordinary machinery rather than new code.
