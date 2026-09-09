# godot-verse

Epic's Verse as a Godot scripting language, as a GDExtension.

Two DLLs meet at a C ABI:

- `verse_host.dll` — a monolithic Unreal Program target built by UBT with the AutoRTFM clang
  driver. It boots `FEngineLoop`, owns VerseVM, compiles `.verse` sources through Solaris and
  runs them.
- `godot_verse.dll` — the GDExtension, built by SCons against godot-cpp with MSVC. It loads the
  host, feeds it a table of Godot callbacks, and pumps it once per frame.

`include/verse_host_abi.h` is the contract between them. Nothing else crosses.

## Layout

    include/     the C ABI header, shared by both sides
    host/        Unreal Program target sources (staged into the engine tree to build)
    src/         GDExtension sources
    tools/       build_host.py, the keyword generator and friends
    tests/       host smoke test (loads verse_host.dll with no Godot involved)
    demo/        Godot project

## Building

Requires a UE source checkout with the Verse toolchain (`../UnrealEngine`), Visual Studio 2022,
Godot 4.7, and Python with SCons.

    python tools/build_host.py            # stages host/ into the engine tree, runs UBT
    scons target=editor                   # builds the GDExtension
    python tools/build_smoke.py           # builds the standalone ABI test

Run the smoke test (no Godot involved), then the demo:

    bin/host_smoke.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine .
    godot --path demo

The engine ships a hard SDK gate in `Engine/Config/Windows/Windows_SDK.json`: Windows SDK
10.0.26100 and MSVC 14.44.35211. This machine has 10.0.22621 and 14.44.35207, so that file was
edited locally to accept them (the original is beside it as `Windows_SDK.json.orig`). Installing
the Windows 11 SDK 26100 component and updating VS 17.14 is the real fix; until then the gate is
relaxed rather than satisfied.

## What works

A `.verse` file is a Godot script. Attach one to a node the way you would a GDScript: it compiles
when the project loads, its `Ready()` runs on `_ready`, and its `Update(Delta:float)` runs every
frame. `PhysicsUpdate(:float)` maps to `_physics_process`. Several scripts on several nodes work
independently.

Compiler diagnostics land in Godot's output with file, line and column, and reach the script
editor through `_validate`. Syntax highlighting comes free from the language's metadata virtuals,
over 156 reserved words generated from the Verse compiler's own `ReservedSymbols.inl` by
`tools/gen_verse_keywords.py`.

The Verse side reaches Godot through a hand-written module at `/Godot.org/Godot`
(`host/Verse/Godot.native.verse`) — free functions over instance-id handles, covering `logic`,
`int`, `float`, `char`, `string`, `[]t`, `[k]v`, `tuple` and `<decides>` in both directions. Real
class hierarchies — `player := class(godot_node2d)` — are Phase 4.

`VerseTicker` from Phase 2 still works, but nothing needs it: the script language pumps `vh_tick`
from `_frame`, so every scripted node is driven rather than one hand-placed one.

## Five constraints worth knowing

**The project is the compilation unit, not the file.** Verse compiles a whole package at once,
and the host can only do it once per process — a second `BuildAll` re-notifies already-loaded
native Verse packages and aborts inside UE's async loader. So the first script that needs
compiling scans `res://` for every `.verse` file and builds them together. Scripts added while
the editor is running are not picked up until it restarts.

**Each script wraps itself in a module named after its file.** Every file in the project shares
one flat `/user@localhost` scope and Verse forbids shadowing, so two scripts that both define a
top-level `Ready()` are a compile error rather than two scripts. `mover.verse` opens with
`mover := module:` and its functions resolve under `(/user@localhost/mover:)`. A lone unwrapped
script still works — the host falls back to the flat scope — which is why the smoke test's
`hello.verse` needs no module.

**The host must be loaded from `Engine/Binaries/Win64`.** VNI records each Verse package's source
directory relative to the loaded module, and the Verse compiler reads those `.verse` files at
runtime. A copy of the DLL anywhere else compiles against an empty package set, and every
identifier in a script is unknown. `bin/` gets a copy for the smoke test, not for Godot.

**The host module is never unloaded.** A monolithic UE runtime does not survive `FreeLibrary`
after `FEngineLoop::AppExit`. `vh_shutdown` tears the engine down; the module stays resident for
the life of the process.

**Every Godot callback is invoked through `AutoRTFM::Open`.** The callbacks live in a DLL the
AutoRTFM compiler never instrumented, so calling one from closed Verse code is a fatal
"could not find function" at runtime. Writes go further and defer to `AutoRTFM::OnCommit`, so a
failed Verse expression does not leave the scene half-written.
