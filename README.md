# Godot Verse

Godot Verse is a GDExtension that adds Epic's
[Verse](https://dev.epicgames.com/documentation/en-us/uefn/verse-language-reference) to Godot 4 as a
scripting language.

The goal is a first-class language, not a demonstration. Anything you can express about a Godot
project in GDScript, you can express in Verse, and Verse's own features work, including concurrency,
failure contexts, and parametric types. Godot's model doesn't change: a Node is a Node, a Signal is
a Signal, and a Resource is a Resource. Use Godot Verse if you develop in Godot and want a stronger
language than GDScript, or if you want to experiment with Verse.

## Project status

Godot Verse is experimental. Don't build a game with it. It's a research build, so expect compiler
crashes, unclear diagnostics, and parts of the ABI that no test covers.

For the status of each requirement, see [`docs/spec.md`](docs/spec.md). If this file and the
specification disagree, the specification is correct.

### Supported features

| Area | What works |
| --- | --- |
| Scripting | You attach a `.verse` file to a node the same way you attach a GDScript. Every Godot virtual method runs, spelled the way Godot spells it: `_Ready`, `_Process`, and the rest. `@export` properties appear in the inspector. `helper{}` creates an object that isn't a node, with Godot's three lifetimes. |
| Interoperability | Every `Variant` type crosses in both directions, and containers cross by reference. GDScript can call any method that a Verse script defines. A runtime error reports a file, a line, and a Verse call stack. |
| Signals and concurrency | Signals are typed members. `Await` works on any Godot signal. `spawn`, `Sleep`, and `race` each get a task scope that's tied to the script instance. |
| Engine API | All 1036 Godot classes and their enums are mirrored in Verse. |
| Project and editor | Hot reload when you run the project, modules through `.vmodule` files, `@tool` scripts, imports that the editor writes for you, syntax highlighting, live diagnostics, code completion, symbol lookup, and a step debugger. |
| Export | On Windows and Web, the export dialog produces a runnable game with no manual copying, and the exported game runs its Verse code. For details, see [Web export](#web-export). |

### When a build happens

A build compiles the whole project. It happens when you run the project, not when you save a file.
When you click **Play**, Godot compiles the project and runs the edited code. Saving a file only
refreshes diagnostics and code completion.

To build without running the project, click **Project > Tools > Build Verse**.

### How runtime errors behave

A Verse runtime error, such as `Err("...")` or an integer overflow, behaves like a GDScript runtime
error. Godot reports the file, the line, and the Verse call stack. The call that raised the error
stops, and the game keeps running.

One difference matters: every call that Godot makes into Verse runs as a transaction. If a call
raises an error, **the extension undoes everything that the call changed before the error**.
GDScript keeps those changes. The exceptions are Godot methods that can't be undone, which
[`docs/nonatomic-methods.md`](docs/nonatomic-methods.md) lists.

Because of this undo, an error can repeat. In the following `_Process` method, the counter never
gets past the value that fails, because each failing call undoes its own increment:

```verse
_Process<override>(Delta:float):void =
    set Frames += 1
    if (Frames = 2):
        Err("fails at frame 2, and then at every frame after it")
```

If a change must survive an error, make the change in a call that can't raise one.

### Limitations

Know these limitations before you start. Each one names what blocks it.

The following aren't built yet:

- Linux and macOS, for both the editor and exported games.
- Android and iOS exported games.
- Parts of the editor's data model: custom Resources, autoloads, and the rest of the `@export`
  surface.

The following are blocked until Epic licenses the Verse toolchain for redistribution:

- An addon that you download and unzip to install. To build the extension, you need your own
  Unreal Engine source checkout and Visual Studio, and you can't redistribute the host DLL that the
  build produces.
- Prebuilt host binaries, on any platform.
- Continuous integration, because a hosted runner can't hold a licensed Unreal Engine checkout.

### Web export

The Unreal host can't be built for the web, so a Web export runs your Verse code on a second
runtime: an interpreter of Verse's compiled bytecode that the extension carries.

Web exports use the interpreter by default. The **Project Settings > Verse > Runtime > Backend**
setting has a Web override set to `vm`, in the same way that Godot defaults Web to the
Compatibility renderer. If you change that override to `host`, the Web export fails with an error.

To use the interpreter in a Windows export too, set **Project Settings > Verse > Runtime > Backend**
to `vm`. A Windows export that uses the interpreter ships no Unreal binary.

In your Web export preset, turn on **Extensions Support** and turn off **Thread Support**.

To build the Web libraries, do the following:

1. Install Emscripten 4.0.11. `tools/emsdk_env.py` finds an `emsdk-4.0.11` checkout next to this
   repository, or the checkout that the `VERSE_EMSDK` environment variable names.
2. Build the release variant:

   ```sh
   python tools/emsdk_env.py -- scons platform=web arch=wasm32 threads=no target=template_release
   ```

3. Build the debug variant:

   ```sh
   python tools/emsdk_env.py -- scons platform=web arch=wasm32 threads=no target=template_debug
   ```

The Web build doesn't use threads, so a server that can't set cross-origin isolation headers can
serve it. It runs slower than a native build: `dodge-the-creeps` runs at about two-thirds of real
time in Chrome.

A Web export ships Verse code that Epic's compiler produced, so Epic's terms still apply to it. For
details, see [Licensing](#licensing).

## Example script

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

To attach a script, select a node, click **Attach Script**, and then select **Verse** as the
language. The template that Godot writes names the class after the file, which is the only shape a
script can have. If a `.verse` file doesn't define a class named after itself, Godot reports that the
file failed to compile.

`@global_class` also registers the PascalCase name with Godot. In this example, `Mover` appears in
the **Create New Node** dialog, and GDScript can use it as a type.

The `/Godot.org/Godot` package mirrors Godot's API as a Verse class hierarchy. Properties are
writable members, such as `set Position = …`, rather than pairs of get and set methods.

### Modules

Each top-level name must be unique within its module. To make a directory a module, add a
`<name>.vmodule` file to it. The marker is explicit because `res://` is an asset tree, and its
directory names were chosen to organize assets, not code. In a project with no markers, every file
is in one root module, which suits a small project. Whatever the module layout, a class must still
be named after its file to be attachable to a node.

## Build the extension

This section describes how to build the extension on Windows.

### Before you begin

Install the following:

- An **Unreal Engine source checkout** that includes the Verse toolchain. To access the source, you
  need a GitHub account that's linked to an Epic account. This project is developed against Epic's
  main branch, which reports version 6.0.
- **Visual Studio 2022** with the C++ workload.
- **Godot 4.7** or later.
- **Python 3** with **SCons**.

Before you ship anything that you build from this repository, read [Licensing](#licensing).

### Build and run

1. Set the `UE_ROOT` environment variable to the path of your Unreal Engine checkout.
2. Build the host. The script stages `host/` into the engine tree and runs the Unreal Build Tool.

   ```sh
   python tools/build_host.py
   ```

3. Build the GDExtension. The build writes it to `demo/addons/godot-verse/bin`.

   ```sh
   scons target=editor
   ```

4. In Godot, open **Editor Settings > Verse > Host**, and set `engine_dir` to the path of your
   Unreal Engine checkout:

   ```none
   verse/host/engine_dir    …/UnrealEngine
   ```

   You set this once per computer. `verse/host/dll_path` and `verse/host/cooker_path` are derived
   from `engine_dir`, so set them only if your build puts those files somewhere else.

5. Run the demo project:

   ```sh
   godot --path demo
   ```

   To use the extension in your own project instead, copy `demo/addons/godot-verse/` into it. The
   extension has no per-project settings.

Godot must load the host from the engine tree's `Engine/Binaries/Win64` directory. At runtime, the
Verse compiler reads each package's source files relative to the loaded module, so a copy of the
host anywhere else compiles against an empty package set.

The host path is an editor setting rather than a project setting because it's specific to one
computer. If `project.godot` stored it, your local path would appear in everyone else's clone. The
`UE_ROOT` environment variable takes precedence over both settings, which lets the test harness
point a headless Godot at a checkout without writing to a file.

### Run the tests

The test suite covers the unit layer, the C ABI, three headless Godot projects, and export checks
on Windows and Web. To run it, use the following command:

```sh
python tools/run_tests.py
```

## Licensing

This repository's own source code is licensed under the [MIT License](LICENSE). The license covers
the code in this repository and nothing else.

The Verse host is built from Unreal Engine source code and links it, so **a game that you ship with
the host is subject to Epic's Unreal Engine EULA, including royalties.** This requirement comes from
the engine's license, not from this project, and the MIT License doesn't change it.

You also can't redistribute binaries built from Unreal Engine source code to anyone who doesn't hold
their own license. For this reason, this repository doesn't include prebuilt binaries.

The interpreter that the `vm` backend and every Web export use differs from the host in one way and
matches it in another:

- **It contains no Unreal Engine source code.** It was written in a clean room from this
  repository's own specification in `docs/web-vm/`, so the MIT License alone covers it.
- **The program that it runs is Epic's compiler output.** `program.vbc` is compiled by Epic's Verse
  compiler, and most of it is Epic's own Verse library code in compiled form.

A game on the `vm` backend ships no Unreal Engine binary, but it still ships Epic's compiler output.
Treat it as subject to Epic's terms until Epic releases the Verse toolchain under a license that
says otherwise.

## Design documentation

- [`docs/spec.md`](docs/spec.md): the numbered requirements for the finished software, with the
  status of each. Section 14 lists the open questions.
- [`docs/roadmap.md`](docs/roadmap.md): the requirements, sequenced into phases.
- [`docs/abi-v2-design.md`](docs/abi-v2-design.md): the C ABI between the two DLLs, and the
  experiments that settled its shape.
- [`docs/phase-0-spikes.md`](docs/phase-0-spikes.md): hot reload, the export pipeline, and why the
  scope is flat.
- [`docs/property-export.md`](docs/property-export.md) and
  [`docs/editor-tooling.md`](docs/editor-tooling.md): the research behind `@export`, the debugger,
  and the language server.
- [`CLAUDE.md`](CLAUDE.md): a map of the source tree and the working rules for contributors.
