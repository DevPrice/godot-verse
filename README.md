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
    docs/        editor toolchain and property-export research
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
using { /Verse.org/Simulation }

mover := class(node2d):

    @editable
    @clamp_min("0.0")
    @clamp_max("500.0")
    var Speed<public>:float = 60.0

    @editable
    Greeting<public>:string = "Verse is running inside Godot, as a class."

    Ready<override>():void =
        Print(Greeting)

    Update<override>(Delta:float):void =
        if (P := GetPosition[]):
            SetPosition(vector2{X := P.X + Delta * Speed, Y := P.Y})
```

Godot's API is mirrored as a Verse class hierarchy under `/Godot.org/Godot`, generated from
`extension_api.json` by `tools/gen_verse_api.py`. Only `object` is a `<native>` class with a
C++ shadow; everything above it is ordinary Verse whose methods bottom out in a handful of native
primitives, so mirroring another hundred Godot classes costs no C++ at all. A method that can fail
carries Verse's `<decides>` effect, and one that mutates the scene defers its write to transaction
commit.

**Class names are unprefixed**, because `/Godot.org/Godot` is already the namespace: the mirror is
`node2d`, `timer`, `control`. Verse reports a name that two `using`s both define at the *use* site
rather than at the import, and the fix belongs there too — `(/Godot.org/Godot:)timer` — which is
how Epic's own libraries keep their two `vector3` types apart. Across all 1023 Godot classes, no
unprefixed name collides with a Verse reserved word or with any type Epic ships.

**A reference to a freed node fails rather than dangles.** Verse has no null, so this had to be
given a meaning. An `object` holds a Godot instance id, every accessor is `<decides>`, and
once Godot frees the object those accessors stop resolving — `demo/scripts/lifetime.verse` holds a
child, frees it, and keeps calling.

A script may still be written the older way, as a `module` of free functions that find their own
node by path; the host picks between the two shapes on whether the class exists.

**`@editable` puts a data member in the inspector.** The property list Godot shows comes out of
the *semantic program* the last analysis pass left behind, not out of the running bytecode —
`vh_class_export_list` walks `CClass::GetDefinitionsOfKind<CDataDefinition>()` and keeps the
members whose `CDefinition::HasAttributeSubclass` matches `/Verse.org/Simulation/editable`. That
choice is what makes the list refresh live: analysis re-runs on every keystroke, while code
generation may happen once per process, so anything read from the VM would be frozen at startup.
Subclasses count too, so `@editable_slider(float)` marks a member just as well as `@editable`.

The attribute is Epic's rather than ours because uLang guards inheriting from *any* attribute type
behind `CScope::IsAuthoredByEpic()` in `AddSuperType` — so neither a `godot_export` of our own nor
a `class(editable)` alias compiles. The `InternalUser` scope this package builds under unlocks
*access* to `epic_internal` definitions — which is what lets scripts say `<native>` — but not
authorship. The host therefore links `VerseSimulationMetadata` (every module it depends on was
already in the list) and scripts add `using { /Verse.org/Simulation }`. Authorship is only a
prefix test against `/Verse.org/`, `/UnrealEngine.com/` and `/Fortnite.com/`, so a package named
into one of those would pass the gate; `docs/property-export.md` records why that is not done.

**Hints come from Epic's metadata attributes too.** `@clamp_min("0.0")` and `@clamp_max("500.0")`
become a Godot range slider, `@category("Movement")` becomes an inspector group. They carry
strings rather than numbers — a single string argument is the one attribute payload the compiler
will hand back today — so godot-verse parses them, and a typo is a missing hint rather than a
compile error.

**Values cross both ways, through the VM's shape rather than the export list — a value exists
nowhere but the VM.** `vh_instance_get_field` reads a member off a live instance,
`vh_class_default_field` reads its declared default off a throwaway one (the Verse constructor
runs in `NewObject`, not in CDO construction), and `vh_instance_set_field` writes one. Matching
the VM's storage exactly is most of that work: a `var` of a scalar type keeps its value inside a
reference cell, a `var` of a container type keeps a *mutable* container, and a write that lands on
the slot rather than through it corrupts the member silently — the read agrees, and only the
interpreter ever objects.

**A non-var is an initializer, not a constant.** Both kinds of `@editable` are editable in the
inspector and stored in the `.tscn`. The difference is when the value may be applied: an instance
is *unsealed* between instantiation and the first call into it, which is exactly the window Godot
uses to push a scene's stored values, and while unsealed anything may be written. The first call —
`Ready`, in practice — seals it, and from then on only a `var` may be assigned. So Verse's
immutability holds where it means something: a non-var never changes once a script can observe it.
The host enforces this itself rather than trusting an inspector flag.

Full research, citations and the roadmap are in `docs/property-export.md`. `tests/host_smoke`
covers both directions — including, because a read/write round-trip cannot catch a value written
in the wrong representation, calling back into Verse to read and assign each member afterwards.

Compiler diagnostics land in Godot's output with file, line and column, and the script editor gets
them live: `_validate` answers for the unsaved buffer rather than replaying what the last build
said.

**`_validate` never blocks the editor.** Verse's compilation unit is the package, so there is no
such thing as re-analysing one file: the cheapest possible answer costs a whole-project semantic
analysis, about 100ms. Godot asks on every open, every tab switch, every idle tick and every save,
which is enough to make the editor feel broken. Two things keep it off the UI thread:

- **The answer is memoised on the source the host currently holds.** Opening, switching tabs and
  saving all validate a buffer nothing has touched, and those return the previous analysis for the
  cost of a string compare. The map is seeded from disk after the project build, so the first open
  of a file is free too, not just the repeats.
- **A buffer that really did change is analysed on a thread the host owns** (`vh_check_project_begin`
  / `_poll`), and `_validate` answers from the previous analysis until the new one lands a few
  frames later. Diagnostics lag the buffer by one analysis; the editor never stops drawing.

A lagging answer is shown but never *logged*. The script editor's error list is replaced wholesale
on the next validate, so a moment of staleness there costs nothing; the output log has no way to
retract a line, and an error the author already undid would sit in it for the rest of the session.
The log is written from the two places that are authoritative for the current text: a validate the
cache answered, and the frame a fresh analysis lands on.

Saving and reloading wait for the outstanding analysis instead of answering stale. Those are the
points where a script's validity is decided and where the author has just asked for something
explicitly, so a beat is worth more than a wrong answer -- at most one analysis, since a newer
buffer replaces a queued one rather than joining a line behind it.

The host will not execute Verse while an analysis is in flight, and enforces that itself rather
than trusting callers: VerseVM blocks execution for the length of a build, so a `vh_tick` that ran
anyway would trip `ensure(!bBlockAllExecution)` and then take the process down. A frame that lands
mid-analysis skips its tick; everything that reads the semantic program waits instead.

Syntax highlighting is a real lexer, which is what lets nested `<# #>` block comments,
dedent-terminated `<#>` comments and comments inside string interpolation all colour correctly —
none of which a delimiter matcher can express. On top of the comment/string/number/keyword
classes it colours operators and punctuation, call positions (`Print(`, the bracketed
`GetPosition[` of a `<decides>` call, and a definition like `Ready<override>()` whose specifiers
sit between the name and its parameter list), `.member` accesses, and both attribute spellings —
suffix `<public>` and prefix `@editable`.

What it does not do is resolve symbols: a bare identifier is a bare identifier, so type names
read as plain text. Telling a local from a field from a class needs the compiler, which is the
same thing the missing language server would provide.

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
`verse` language id and sets tab indentation to match what Godot's own script editor writes back
into a `.verse` file when it saves one — the two editors have to agree, because Verse rejects a
file that mixes tabs and spaces.

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
