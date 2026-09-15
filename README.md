# godot-verse

Epic's [Verse](https://dev.epicgames.com/documentation/en-us/uefn/verse-language-reference) as a
scripting language for Godot 4, as a GDExtension.

The goal is a first-class language, not a demo: anything a GDScript author can express about a
Godot project, a Verse author can express — and Verse's own features work, concurrency and failure
contexts and parametric types included. Godot's model stays Godot's: a node is a node, a signal is
a signal, a Resource is a Resource. The intended user is a Godot developer who wants a better
language than GDScript and does not already know Verse.

## Status: experimental. Do not build a game on this.

This is a research build: expect a compiler crash, a rough diagnostic, or an untested corner of
the ABI. A build happens on Play, not on save — pressing Play compiles the whole project and runs
the edited code; saving only refreshes diagnostics and completion. There is a
`Project > Tools > Build Verse` for when there is no run to hang it on.
[`docs/spec.md`](docs/spec.md) carries the per-requirement status; where this file and the spec
disagree, believe the spec.

**Works today**

| Feature | Status |
| --- | --- |
| Scripting | A `.verse` file attaches to a node like GDScript; every Godot virtual runs, spelled the way Godot spells it (`_Ready`, `_Process`, ...); `@export` properties show in the inspector; `helper{}` makes an object that is not a node, with Godot's own three lifetimes |
| Interop | Every `Variant` type crosses in both directions, containers by reference; GDScript can call any method a script defines; a runtime error names a file, a line and a Verse call stack |
| Signals & concurrency | Signals are typed members; `Await` works on any Godot signal; `spawn`, `Sleep` and `race` each get a task scope tied to the script instance |
| Engine surface | All 1036 Godot classes and their enums are mirrored |
| Project & editor | Hot reload on Play; modules via `.vmodule`; `@tool` scripts; imports the editor writes for you; syntax highlighting, live diagnostics, completion, symbol lookup and a step debugger |
| Export | Windows: the export dialog produces a runnable game with no manual copying, and the exported game runs its Verse |

**Coming next**

- Linux and macOS, for both the editor and exported games
- Android and iOS exported games
- The editor's data model: custom Resources, autoloads, the remaining `@export` surface

**Blocked on Epic licensing the Verse toolchain for redistribution**

- A download-and-unzip addon install — building requires your own Unreal Engine source checkout
  plus Visual Studio, and the resulting host DLL cannot be redistributed
- Prebuilt host binaries for any platform
- CI (a hosted runner cannot hold a licensed UE checkout)

**Blocked on Unreal's own tooling**

- Web export — UBT has no wasm build target to build a runtime host against

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
against nothing. Tell Godot where your checkout is, once per machine, under **Editor Settings →
Verse → Host**:

    verse/host/engine_dir    …/UnrealEngine

That is all that is normally set: `verse/host/dll_path` and `verse/host/cooker_path` are derived
from it and are there for a build that put them somewhere else. The setting lives in Editor
Settings rather than in the project because it names one machine, and a project.godot carrying it
is your local state in everybody else's clone. `UE_ROOT` in the environment wins over both, which
is how the test harness points a headless Godot at a checkout without writing to anything.

Then run the demo — `godot --path demo` — or copy `demo/addons/godot-verse/` into your own
project; there is nothing per-project to set.

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
