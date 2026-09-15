# godot-verse

Epic's [Verse](https://dev.epicgames.com/documentation/en-us/uefn/verse-language-reference) as a
scripting language for Godot 4, as a GDExtension.

The goal is a first-class language, not a demo: anything a GDScript author can express about a
Godot project, a Verse author can express — and Verse's own features work, concurrency and failure
contexts and parametric types included. Godot's model stays Godot's: a node is a node, a signal is
a signal, a Resource is a Resource. The intended user is a Godot developer who wants a better
language than GDScript and does not already know Verse.

## Status: experimental. Do not build a game on this.

This is a research build. Phases 0–6 are complete and Phase 7a (export) is built; Phase 7b —
actually loading an exported game — is blocked on an engine limitation. See
[the roadmap](docs/roadmap.md).

- **Windows only.** No macOS, no Linux.
- **There is no addon to install.** Verse's compiler ships only inside Unreal, so building
  requires *your own Unreal Engine source checkout* plus Visual Studio, and the resulting host DLL
  cannot be redistributed. A download-and-unzip install waits on Epic licensing the toolchain
  separately.
- **A build happens on Play, not on save.** Pressing Play compiles the whole project and runs
  the edited code; saving refreshes diagnostics and completion but not what runs. There is a
  `Project > Tools > Build Verse` for when there is no run to hang it on.

What works today: a `.verse` file is a script you attach to a node; every one of Godot's virtuals
runs, spelled the way Godot spells it (`_Ready`, `_Process`, ...); properties export to the
inspector; every Godot `Variant` type crosses in both directions, containers by reference; a
script declares signals as typed members and can `Await` any Godot signal; GDScript can call any
method a script defines and a runtime error names a file, a line and a Verse call stack. The
editor gets syntax highlighting, live diagnostics, completion, symbol lookup and a step debugger.

All 1036 Godot classes are mirrored with their enums, and a project is a real source set — hot
reload on Play, modules marked with a `.vmodule` file, `@tool` scripts, and imports the editor
writes for you. Concurrency works: `spawn`, `Await`, `Sleep` and `race`, each with a task scope
tied to the script instance.

Not yet: an exported project builds the right tree, but the exported game cannot load it —
Phase 7b's blocker — and there is no macOS or Linux support. [`docs/spec.md`](docs/spec.md)
carries the per-requirement status, and the rest of this file predates most of it — where the two
disagree, believe the spec.

## What a script looks like

```verse
using { /Godot.org/Godot }

# A script is a class named after its own file — this is mover.verse. The node it is attached to
# is Self, so there is no GetNode lookup.
@global_class
mover := class(node2d):

    @export
    Greeting<public>:string = "Verse is running inside Godot, as a class."

    # The range lives on the type, so the compiler enforces it and the inspector builds a slider
    # from the same bounds.
    @export
    @export_group("Movement")
    var Speed<public>:type{_X:float where 0.0 <= _X, _X <= 500.0} = 60.0

    _Ready<override>():void =
        Print(Greeting)

    _Process<override>(Delta:float):void =
        set Position = vector2{X := Position.X + Delta * Speed, Y := Position.Y}
```

Attach it the way you would a GDScript: select a node, **Attach Script**, choose Verse as the
language. The template it writes already names the class after the file, which is the only shape a
script has — a `.verse` file defining no class named after itself is reported as one that failed to
compile. `@global_class` additionally registers the
PascalCase name with Godot, so `Mover` shows up in **Create New Node** and can be named as a type
from GDScript. Godot's API is mirrored as a Verse class hierarchy under `/Godot.org/Godot`, with
properties as writable members (`set Position = …`) rather than get/set pairs.

A top-level name has to be unique within its module, and a directory becomes a module by
carrying a `<name>.vmodule` file — marked rather than implied, because `res://` is an asset tree
whose directory names were chosen for sprites. A project with no markers has every file in one
root module, which is what a small project wants. Naming the class after the file is still what
makes it attachable to a node.

## Building

Prerequisites:

- An **Unreal Engine source checkout** with the Verse toolchain, which requires a GitHub account
  linked to an Epic account. Developed against Epic's main branch (reports version 6.0).
- **Visual Studio 2022** with the C++ workload.
- **Godot 4.7** or newer.
- **Python 3** with **SCons**.

Point `UE_ROOT` at the engine checkout, then:

    python tools/build_host.py     # stages host/ into the engine tree and runs UBT
    scons target=editor            # builds the GDExtension into demo/addons/godot-verse/bin

The host must be loaded from the engine tree's `Engine/Binaries/Win64` — the Verse compiler reads
each package's sources at runtime, relative to the loaded module, so a copy elsewhere compiles
against nothing. Godot is pointed at it by two project settings, which you set per project under
**Project → Project Settings → Verse → Host** (they name one machine's checkout, so nothing
portable can be committed):

    verse/host/dll_path      …/Engine/Binaries/Win64/verse_host.dll
    verse/host/engine_dir    …/UnrealEngine

Then run the demo — `godot --path demo` — or copy `demo/addons/godot-verse/` into your own project
and set those two settings there.

Tests:

    python tools/run_tests.py      # units, the C ABI, two headless Godot projects, and an export check

## Licensing

This repository's own source is [MIT](LICENSE). That covers what is written here and nothing else.

The Verse host is built from Unreal Engine source and links it, so **a game shipped with it is
subject to Epic's Unreal Engine EULA, royalties included** — a property of the engine's licensing,
not of this project, and unaffected by the MIT grant above. Binaries built from UE source may not
be redistributed to anyone without their own license, which is why nothing here ships prebuilt.

## Design notes

- [`docs/spec.md`](docs/spec.md) — what the finished thing must do, numbered, with per-requirement
  status. §14 holds the open questions.
- [`docs/roadmap.md`](docs/roadmap.md) — those requirements sequenced into phases.
- [`docs/abi-v2-design.md`](docs/abi-v2-design.md) — the C ABI between the two DLLs, and the spikes
  that settled its shape.
- [`docs/phase-0-spikes.md`](docs/phase-0-spikes.md) — hot reload, the export pipeline, and why the
  scope is flat.
- [`docs/property-export.md`](docs/property-export.md), [`docs/editor-tooling.md`](docs/editor-tooling.md)
  — the research behind `@export` and behind the debugger/LSP story.
- [`CLAUDE.md`](CLAUDE.md) — the map of the source tree and the working rules.
