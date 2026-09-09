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
    tools/       build_host.py, the API and keyword generators and friends
    tests/       host smoke test (loads verse_host.dll with no Godot involved), lexer test
    docs/        notes on the editor toolchain
    demo/        Godot project

## Building

Requires a UE source checkout with the Verse toolchain (`../UnrealEngine`), Visual Studio 2022,
Godot 4.7, and Python with SCons.

    python tools/build_host.py            # stages host/ into the engine tree, runs UBT
    scons target=editor                   # builds the GDExtension
    python tools/gen_verse_api.py         # regenerates the Verse mirror of Godot's API
    python tools/build_smoke.py           # builds the standalone ABI test
    python tools/build_lexer_test.py      # builds the standalone lexer test

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

A script defines a class named after its own file, and the node it is attached to is `Self`:

```verse
using { /Godot.org/Godot }

mover := class(godot_node2d):

    Speed<public>:float = 60.0

    Ready<override>():void =
        Print("Verse is running inside Godot, as a class.")

    Update<override>(Delta:float):void =
        if (P := GetPosition[]):
            SetPosition(vector2{X := P.X + Delta * Speed, Y := P.Y})
```

Godot's API is mirrored as a Verse class hierarchy under `/Godot.org/Godot`, generated from
`extension_api.json` by `tools/gen_verse_api.py`. Only `godot_object` is a `<native>` class with a
C++ shadow; everything above it is ordinary Verse whose methods bottom out in a handful of native
primitives, so mirroring another hundred Godot classes costs no C++ at all. A method that can fail
carries Verse's `<decides>` effect, and one that mutates the scene defers its write to transaction
commit.

**A reference to a freed node fails rather than dangles.** Verse has no null, so this had to be
given a meaning. A `godot_object` holds a Godot instance id, every accessor is `<decides>`, and
once Godot frees the object those accessors stop resolving — `demo/scripts/lifetime.verse` holds a
child, frees it, and keeps calling.

A script may still be written the older way, as a `module` of free functions that find their own
node by path; the host picks between the two shapes on whether the class exists.

Compiler diagnostics land in Godot's output with file, line and column, and the script editor gets
them live: `_validate` re-analyses the project against the unsaved buffer rather than replaying
what the last build said. Syntax highlighting is a real lexer — nested `<# #>` block comments,
dedent-terminated `<#>` comments and comments inside string interpolation all colour correctly,
which no delimiter matcher can do.

`VerseTicker` from Phase 2 still works, but nothing needs it: the script language pumps `vh_tick`
from `_frame`, so every scripted node is driven rather than one hand-placed one.

## Editor tooling

The Verse debugger the host links (`Verse::SocketDebugger`) is reachable, but not driven by
anything yet. The language server is not reachable at all — the engine checkout has no build of
it. Full research and citations are in `docs/editor-tooling.md`.

**Debugger.** Set the project setting `verse/host/enable_debugger` to `true` (defaults to
`false`) before the game starts. That makes `vh_init` call `Verse::SocketDebugger::Listen()`,
which opens a socket on port 1963 (the engine's `verse.DebuggerPort` console variable). The port
is real and confirmed by reading the engine source. Whether any VS Code extension can actually
attach to it is not: the socket frames each message as a 4-byte length prefix plus raw JSON, not
the `Content-Length`-framed transport the standard Debug Adapter Protocol uses, and no adapter
bridging the two is known to exist. `.vscode/launch.json` records the port with this caveat rather
than a config presented as working.

**Language server.** `tools/run_verse_lsp.py` looks for a `uLangLSP`-derived executable and tells
you exactly why it can't find one: `uLangLSP` in the UE checkout is a message-type library
(`LSP.h`/`LSP.cpp`), not a Program target, and nothing links it into a binary. There is no build
command for it, unlike `verse_host.dll`. `.vscode/settings.json` still associates `*.verse` with a
`verse` language id and matches `demo/scripts`' indentation (4 spaces — one of its three files
uses literal tabs instead, which is inconsistent, not a style choice to follow), so syntax
association and formatting work independently of the language server question.

**Known to work:** the debugger's port and the flag that opens it. **Not known to work:**
whether any VS Code debug extension can speak this socket's framing, and there is currently no
way to run the language server at all.

## Five constraints worth knowing

**The project is the compilation unit, not the file.** Verse compiles a whole package at once, and
the host can only *generate* once per process: publishing a compiled package sets
`EInternalObjectFlags::LoaderImport` on every export, and a second pass over the already-loaded
native Verse packages trips an assertion on that flag inside UE's async loader. So the first script
that needs compiling scans `res://` for every `.verse` file and builds them together, and scripts
added while the editor is running are not picked up until it restarts.

Analysis is not subject to that. A build configured with `bSemanticAnalysisOnly` and no digests,
code or bytecode publishes nothing and can be run as often as you like, which is what gives the
script editor live diagnostics. What it cannot do is replace the bytecode a running program is
already executing, so hot reload is narrower than it looks rather than flatly impossible.

**One top-level name per file.** Every file in the project shares one flat `/user@localhost` scope
and Verse forbids shadowing, so two files that both define a top-level `Ready()` are a compile
error rather than two scripts. Naming the class after the file is what keeps that from happening —
file names are already unique. A script written as a `module` instead needs the same treatment for
the same reason, which is why `mover := module:` was the shape before classes existed.

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
