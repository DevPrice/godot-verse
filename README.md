# Godot Verse

Epic's [Verse](https://dev.epicgames.com/documentation/en-us/uefn/verse-language-reference) as a
scripting language for Godot 4, as a GDExtension.

The goal is a first-class language, not a demo. Anything you can express about a Godot project in
GDScript, you can express in Verse, and Verse's own features work, with concurrency, failure contexts,
and parametric types included. Godot's model stays Godot's: a Node is a Node, a Signal is a Signal,
and a Resource is a Resource. Write Verse here if you develop in Godot and want a stronger language
than GDScript, or if you just want to play with the language.

## Status: experimental

Don't build a game on this. It's a research build, so expect a compiler crash, a rough diagnostic,
or an untested corner of the ABI.

[`docs/spec.md`](docs/spec.md) carries the per-requirement status. Where this file and the spec
disagree, the spec is correct.

### What works

| Feature | Status |
| --- | --- |
| Scripting | A `.verse` file attaches to a node the way a GDScript does. Every Godot virtual runs, spelled the way Godot spells it (`_Ready`, `_Process`, and the rest). `@export` properties appear in the inspector. `helper{}` makes an object that isn't a node, with Godot's own three lifetimes |
| Interop | Every `Variant` type crosses in both directions, containers by reference. GDScript can call any method a script defines. A runtime error names a file, a line, and a Verse call stack |
| Signals and concurrency | Signals are typed members. `Await` works on any Godot signal. `spawn`, `Sleep`, and `race` each get a task scope tied to the script instance |
| Engine surface | All 1036 Godot classes and their enums are mirrored |
| Project and editor | Hot reload on Play, modules through `.vmodule`, `@tool` scripts, imports the editor writes for you, syntax highlighting, live diagnostics, completion, symbol lookup, and a step debugger |
| Export | On Windows, the export dialog produces a runnable game with no manual copying, and the exported game runs its Verse |

### When a build happens

A build compiles the whole project, and it happens on Play rather than on save. Press Play, and
Godot compiles the project and runs the edited code. Saving refreshes diagnostics and completion
only. To build without a run, click **Project > Tools > Build Verse**.

### What isn't supported

These limits are the ones worth knowing before you start. Each one names what blocks it.

Not built yet:

- Linux and macOS, for both the editor and exported games
- Android and iOS exported games
- Parts of the editor's data model: custom Resources, autoloads, and the rest of the `@export`
  surface

Blocked on Epic licensing the Verse toolchain for redistribution:

- A download-and-unzip addon install. To build, you need your own Unreal Engine source checkout
  and Visual Studio, and you can't redistribute the host DLL that build produces
- Prebuilt host binaries, on any platform
- Continuous integration, because a hosted runner can't hold a licensed Unreal Engine checkout

### Web export

A Web export runs your Verse on a second runtime: an interpreter of Verse's compiled bytecode that
the extension carries, because the Unreal host can't be built for the web. A Web export always uses
it, so there is nothing to set. To use it in a Windows export too, which then ships no Unreal binary,
set **Project Settings > Verse > Runtime > Backend** to `vm`.

A Web preset needs **Extensions Support** on and **Thread Support** off.

The Web build doesn't need threads, so a host that can't set cross-origin isolation headers can
serve it. It doesn't run at full speed yet: `dodge-the-creeps` runs at about two-thirds of real time
in Chrome. A Web export ships Verse code compiled by Epic's own compiler, so the redistribution
limits above still apply to it.

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

To attach the script, select a node, click **Attach Script**, and choose Verse as the language. The
template Godot writes names the class after the file, which is the only shape a script has. If a
`.verse` file defines no class named after itself, Godot reports it as a file that failed to
compile.

`@global_class` also registers the PascalCase name with Godot, so `Mover` appears in **Create New
Node** and GDScript can name it as a type.

Godot's API is mirrored as a Verse class hierarchy under `/Godot.org/Godot`, with properties as
writable members — `set Position = …` — rather than as get and set pairs.

### Modules

Each top-level name must be unique within its module. A directory becomes a module by carrying a
`<name>.vmodule` file. The marker is explicit rather than implied, because `res://` is an asset tree
whose directory names were chosen for sprites. A project with no markers holds every file in one
root module, which is what a small project wants. Whatever the module layout, naming the class after
the file is still what makes the class attachable to a node.

## Build the extension

Before you start, install the following:

- An **Unreal Engine source checkout** with the Verse toolchain. Access requires a GitHub account
  linked to an Epic account. This project is developed against Epic's main branch, which reports
  version 6.0.
- **Visual Studio 2022** with the C++ workload.
- **Godot 4.7** or later.
- **Python 3** with **SCons**.

Read [Licensing](#licensing) before you ship anything you build here.

To build:

1. Point the `UE_ROOT` environment variable at your engine checkout.
2. Build the host. This stages `host/` into the engine tree and runs the Unreal Build Tool.

       python tools/build_host.py

3. Build the GDExtension into `demo/addons/godot-verse/bin`.

       scons target=editor

4. Tell Godot where your checkout is. In **Editor Settings > Verse > Host**, set `engine_dir` to the
   path of the checkout:

       verse/host/engine_dir    …/UnrealEngine

   Set this once per machine. `verse/host/dll_path` and `verse/host/cooker_path` derive from
   `engine_dir`, so set them only if your build put those files somewhere else.

5. Run the demo project, or copy `demo/addons/godot-verse/` into a project of your own. There's
   nothing to set per project.

       godot --path demo

Godot must load the host from the engine tree's `Engine/Binaries/Win64`. The Verse compiler reads
each package's sources at run time, relative to the loaded module, so a copy anywhere else compiles
against an empty package set.

The host path lives in Editor Settings rather than in the project because it names one machine. A
`project.godot` that carries it puts your local state in everyone else's clone. `UE_ROOT` in the
environment takes precedence over both settings, which is how the test harness points a headless
Godot at a checkout without writing to a file.

To run the tests — the unit layer, the C ABI, two headless Godot projects, and an export check:

    python tools/run_tests.py

## Licensing

This repository's own source is [MIT](LICENSE). That covers what is written here and nothing else.

The Verse host is built from Unreal Engine source and links it, so **a game you ship with it is
subject to Epic's Unreal Engine EULA, royalties included.** That follows from the engine's
licensing, not from this project, and the MIT grant above doesn't change it.

You also can't redistribute binaries built from Unreal Engine source to anyone who doesn't hold
their own license, which is why nothing here ships prebuilt.

## Design notes

- [`docs/spec.md`](docs/spec.md) — what the finished software must do, numbered, with
  per-requirement status. §14 holds the open questions.
- [`docs/roadmap.md`](docs/roadmap.md) — those requirements sequenced into phases.
- [`docs/abi-v2-design.md`](docs/abi-v2-design.md) — the C ABI between the two DLLs, and the spikes
  that settled its shape.
- [`docs/phase-0-spikes.md`](docs/phase-0-spikes.md) — hot reload, the export pipeline, and why the
  scope is flat.
- [`docs/property-export.md`](docs/property-export.md) and
  [`docs/editor-tooling.md`](docs/editor-tooling.md) — the research behind `@export` and behind the
  debugger and LSP story.
- [`CLAUDE.md`](CLAUDE.md) — the map of the source tree and the working rules.
