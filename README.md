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
        set Position = vector2{X := Position.X + Delta * Speed, Y := Position.Y}
```

Godot's API is mirrored as a Verse class hierarchy under `/Godot.org/Godot`, generated from
`extension_api.json` by `tools/gen_verse_api.py`. Only `object` is a `<native>` class with a
C++ shadow; everything above it is ordinary Verse whose methods bottom out in a handful of native
primitives, so mirroring another hundred Godot classes costs no C++ at all. A method that mutates
the scene defers its write to transaction commit, and of the 1236 methods that used to carry
Verse's `<decides>` effect only the 132 returning an object still do — see the lifetime note
below for why the rest gave it up.

**A Godot property is a writable Verse member**, not a get/set pair. 686 of them across the
mirrored classes are emitted as `var Position<public>...:vector2`, and the `get_position` and
`set_position` they were built from are gone — two spellings of one thing is worse than one.
The mechanism is uLang's `<getter(...)>`/`<setter(...)>`, whose attribute classes are
`epic_internal` and so reachable on the same footing as `@editable`: use, not authorship. The
accessor functions themselves must be `epic_internal` too, since a definition may be no more
accessible than the `accessor` type it takes.

Three kinds stay methods. A read-only property has no setter and the attributes must come in
pairs. An object-typed one would need a getter that cannot fail, which is exactly what a null
Godot object is. And a `string` is `[]char`, so the compiler asks it for element accessors as
well — what `set Node.Text[99] = 'x'` should do has no answer worth inventing.

The compiler asks a *struct*-typed property for a field-named overload of each accessor, so that
`set Node.Position.X = 1.0` could resolve. Those are emitted and are dead: it walks a struct's
fields without checking any is assignable, and a Verse struct may not contain a `var`. Assign the
whole vector.

**Class names are unprefixed**, because `/Godot.org/Godot` is already the namespace: the mirror is
`node2d`, `timer`, `control`. Verse reports a name that two `using`s both define at the *use* site
rather than at the import, and the fix belongs there too — `(/Godot.org/Godot:)timer` — which is
how Epic's own libraries keep their two `vector3` types apart. Across all 1023 Godot classes, no
unprefixed name collides with a Verse reserved word or with any type Epic ships.

**A reference to a freed node is an error, not a failure.** Verse has no null, so this had to be
given a meaning. An `object` holds a Godot instance id, which outlives the object it names, and
reaching through a stale one raises a Verse *runtime error* — the same unrecoverable class of
fault as reading a `var` out of a dead UE object. It unwinds to the root failure context and
aborts every enclosing transaction, so the deferred scene writes the call had already queued are
discarded rather than half-applied.

Failure was the wrong tool for it. `<decides>` is how Verse says *this value may legitimately be
absent*, and an `if` that swallows it reads as handling a known case; a use-after-free is neither
legitimate nor absent, and silently skipping the branch every frame is how a dead reference stays
invisible. What is genuinely absent — `GetParent()` at the scene root, a `FindChild` that misses —
returns an object and still fails, which is the one thing `<decides>` is left doing.

Test `IsInstanceValid()` before reaching through a handle the scene may have dropped;
`demo/scripts/lifetime.verse` holds a child, frees it, and keeps looking.

*Known limitation:* the host runs a single `verse::FContentScope` for the whole project, and a
runtime error terminates the scope's task group — so one dead-object access kills every suspended
async task in every script, not just the offending one. Per-script scopes (or per-invocation, if
Godot ever reaches Verse off the main thread) are the right shape and are not built yet.

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

**`@global_class` registers a script's class with Godot**, the way C#'s `[GlobalClass]` does: the
class appears in Create New Node and can be named as a type from GDScript. It takes no argument
because there is nothing to say — *One top-level name per file* already pins the class name to the
file stem, so that name is what Godot registers. `<abstract>` on the class comes across as Godot's
`is_abstract`.

**The registered name is PascalCase**, so `player_controller` registers as `PlayerController`.
This is the one place the bridge does not hand Godot the Verse spelling, and C# is no guide here
because C# type names are already PascalCase. Godot's global class names share one namespace with
the engine's own types, and a snake_case name both reads wrong beside them in the class picker and
is what collides there. Only the Godot-facing name changes: the script still instantiates through
its file stem, so nothing on the Verse side has to know. Words split on underscores, plus one
narrow rule for Godot's dimensional suffix: a `d` closing a word with a digit before it
uppercases, so `enemy2d` registers as `Enemy2D` the way `Node2D` and `Camera3D` are spelled.
Nothing else about digits is special — `add` and `vector2i` come through untouched.

This is the one attribute the bridge owns rather than borrows, and it could not have been declared
beside the rest of the API. `AddSuperType` refuses `class(attribute)` unless the definition's
verse path is in `CSemanticProgram::_EpicInternalModulePrefixes` — access to `epic_internal` is
what the `InternalUser` package scope buys, and authorship is not. Adding `/Godot.org/` to that
list is reachable, but only from inside this process, and `host/Verse` is compiled by VNI at build
time where nothing the host does can reach. So the attribute lives in a second source package the
host adds at runtime, sharing the verse path of the native one — which is why a script needs no
import beyond the `using { /Godot.org/Godot }` it already has. `HostScript.cpp` carries the
mechanism and the ordering constraint that goes with it.

**The registry is answered from the file's text, not from the host.** `EditorFileSystem` asks for
every `.verse` in the project, from its scan thread, during the startup scan — and every ABI entry
point must be called on the `vh_init` thread, while `vh_compile_project` may run only once per
process. A filesystem scan is the last thing that should be able to spend that one build. So
`src/verse_class_decl.cpp` scans for the declaration directly, deferring every comment and string
decision to the same lexer the highlighter uses, so `@global_class` inside a comment or a string
is not mistaken for the attribute. GDScript answers the same question the same way, from a
tokenizer-only pass; C# is the one that reads compiled metadata, and pays for it by needing a
build before a new `[GlobalClass]` appears. `bin/verse_class_decl_test.exe` covers the scanner.

**`base_type` is the nearest global ancestor**, following C#: a script deriving from another
*global* script nests under it in the class picker, and only when no ancestor is global does the
walk fall through to the Godot class the mirrored Verse superclass stands for. The same walk
answers `_get_instance_base_type`, which is why a `class(node2d)` script now attaches at `Node2D`
rather than at the `Node` floor.

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

**Ctrl+click and hover resolve a symbol through the compiler.** Godot routes the ctrl-hover
underline, ctrl+click and the documentation tooltip through one `_lookup_code` call, and the
position arrives *inside* the buffer rather than beside it — the editor splices U+FFFF in at the
cursor, which is the only thing that can tell two same-named locals apart, since the symbol Godot
also passes is just the word under the pointer. `vh_lookup_symbol` answers off the semantic
program: it walks the AST for the innermost identifier node whose source range contains the
cursor and reports what that identifier resolved to, with the definition's own location and its
type spelled back as Verse source.

The tooltip's prose is the comment block sitting immediately above the definition, which is as
close as Verse gets to a doc comment — the language has no `///` form, so the convention is
simply what precedes a definition.

That is read out of the source rather than asked of the compiler, which is not where you would
expect to find it. The parser does keep comments, hanging each one off the node that begins the
construct it precedes — but for a member sitting behind four lines of `@editable`, `@clamp_min`
and friends, the node that begins the construct is the attribute clause and not the member, and
the comment is reachable from the definition only by guessing at the shape of the syntax tree
around it. The definition's line is already known, the file's text is already to hand, and
walking up from that line while the lines are comments needs none of that.

A definition answers at its own name too, so hovering `Ready` where it is declared describes it
rather than declining. That needs the locus narrowed: a definition's own span runs from its first
attribute to the end of its body, and matching against that would resolve every blank column
inside a function to the function. The name sits in the definition's first VST child, which is
tight enough to mean the author pointed at it.

**An override is documented by what it overrides.** `Ready<override>()` in a script has nothing
to say about itself, so the lookup carries the definition it overrides alongside the one under the
cursor, and the editor falls back to the parent's documentation when the declaration carries no
comment of its own — a comment the author *did* write always wins. The click goes to the parent
too, since this definition's own line is the one the cursor is already on.

`object`'s `Ready`, `Update` and `PhysicsUpdate` are in the method map as `Node._ready`,
`_process` and `_physics_process`. They are hand-written rather than mirrored — the generator
skips virtuals — but they exist to be the Verse spelling of Godot's, so overriding one opens
Godot's page for it.

The redirect is deliberately confined to declarations. A call site already resolves to the
implementation that will run, and sending *that* to the parent would be wrong rather than merely
unhelpful, so the host reports whether the cursor was on a definition and only fills the override
in that case.

**An error marks its line**, in the gutter, the error bar and the line background. That is all
Godot's, but it hinges on one detail: `ScriptTextEditor::_validate_script` moves every error whose
path is not exactly the script's own — `res://scripts/mover.verse` — into a separate
depended-errors list, which is *listed* but never marked. The host reports the absolute path it
was handed, so diagnostics are rewritten to the `res://` path they came from before they reach
Godot. Both the compile and the analysis path go through one function, so both mark.

Two things make that safe rather than merely possible.

- **A stale answer is refused rather than shown.** Diagnostics can lag the buffer by one analysis
  because the editor replaces its error list wholesale, but a lagging *locus* is a confident jump
  to the wrong line — every row below an insertion has moved. So the lookup answers only when the
  buffer matches what analysis last saw, which is the same comparison `_validate` already uses to
  skip a re-analysis, and otherwise declines. Declining costs an underline that does not appear
  for a few frames.
- **The host refuses unless the program it holds can be walked at all.** Code generation hangs an
  IR package off every module, and the AST accessors this needs assert rather than degrade when
  they find one — a lookup at the wrong moment would take the process down rather than return
  nothing. The host tracks which kind of build produced its current program instead of trusting
  the caller, and `vh_compile_project` now ends by re-analysing what it just built, so a symbol
  resolves on the first hover of a session rather than only after the first edit.

The result is reported as one of Godot's two *local* lookup results, which is not where it looks
like it belongs. `SCRIPT_LOCATION` is the honest label and it jumps correctly, but the tooltip
path drops it in a `// Nothing to do.` branch and shows nothing at all; the `CLASS_*` types do
produce a tooltip, by routing into Godot's own class documentation, which has nothing to say
about a Verse definition. `LOCAL_VARIABLE` and `LOCAL_CONSTANT` are the only pair that both jump
and describe — the click path never reads `type`, it jumps on `location` alone as long as
`class_name` is empty, so leaving that key unset is load-bearing. Verse's `var` split maps onto
the two exactly.

**A name from the mirrored API resolves to Godot's own documentation instead.** Ctrl+clicking
`node2d`, `Position` or `GetChild` opens the class reference for `Node2D`, `Node2D.position` or
`Node.get_child`, and hovering any of them shows the description Godot already ships. A mirrored
property arrives as a `var` like any `@editable` member, so the routing keys off the *owner* —
only a mirrored class appears in the table. `vector2`, `vector3` and `color` are in it too: they
are Godot builtins that happen to be hand-written in `GodotApi.native.verse` rather than
generated, and without an entry the editor calls them local constants. Nothing here writes that
prose: both paths key off `class_name`, which is precisely the key that diverts the click away from a jump
and into the help viewer, and which the tooltip uses to fetch the description out of the same doc
data. There is nothing to jump to anyway — the generated API is compiled from the engine tree,
not from the project.

Turning a Verse name back into a Godot one is a generator problem, not a runtime one. The class
transform is reversible by table, but the method transform is not reversible at all: Verse's
`SetPosition` came from `set_position` by dropping the underscores that separated the words, and
nothing in the name says where they were. So `gen_verse_api.py` emits the mapping alongside the
class table it already wrote, keyed by the class that *declares* the method — which is what the
compiler reports as a resolved definition's enclosing scope, and is the right key, because an
inherited call resolves to the declaring class rather than the one it was called through.

Syntax highlighting is a real lexer, which is what lets nested `<# #>` block comments,
dedent-terminated `<#>` comments and comments inside string interpolation all colour correctly —
none of which a delimiter matcher can express. On top of the comment/string/number/keyword
classes it colours operators and punctuation, call positions (`Print(`, and the bracketed
`GetChild[` of a `<decides>` call), `.member` accesses, and both attribute spellings — suffix
`<public>` and prefix `@editable`.

A definition's name colours apart from a call to it, the way GDScript separates `func foo` from
`foo()`. Verse has no `func` keyword, so the discriminator is the `=` that follows the parameter
list: a call never has one, and the `=` of a comparison or of an interpolated string sits inside
the brackets the scan has already passed. The name must also start its line, which is where every
Verse definition begins.

The colours come from GDScript's theme keys — `gdscript/function_definition_color` and
`gdscript/annotation_color` alongside the language-neutral ones. Verse has no keys of its own, and
matching is the point: a `.verse` file open beside a `.gd` one should not colour the same idea two
different ways.

**Type names colour as types, from a set of names rather than from positions.** A bare identifier
that names a class is coloured as one: the mirrored Godot API — `node2d`, `timer`, `control` —
plus the class each `.verse` file in the project defines, which is its own stem. Verse's
primitives need no help, being reserved words already.

Deliberately not from the compiler, even though ctrl+click above proves it could answer. Every
visible line is recoloured on every keystroke, and a source range goes stale the instant a line
is inserted above it, so colouring from loci would make the colours crawl a line behind the text.
A *name* does not move when a line is inserted. The two sources of names are also both available
before anything has been compiled, which matters because the highlighter has to colour a script
that has never been built — and neither one needs the host to be loaded at all.

**Known limitation: ctrl+hover inside a string interpolation underlines the whole string.** The
lookup itself is right — `{Position.X}` resolves to `Position` and clicking it navigates — but
`CodeEdit::get_lookup_word` returns the entire literal whenever the cursor is inside a registered
string delimiter, deliberately, so that `preload("res://…")` yields a path. The only lever from
outside the engine is to stop declaring `"` a string delimiter, which also drives auto-closing
quotes, string-aware folding, indentation and completion suppression. Not worth an underline.

What that costs is precision the compiler would have given for free: a local shadowing a class
name colours as the class, and a type alias or a nested class is not in the set, so it stays
plain text. Those are the cases where the next step is the scope-aware name set the analysis
could hand over per file.

**Completion offers the same two name sets, plus the reserved words.** It is coarse for the same
reason and to the same extent: which names are in scope at a point, and what a value's members
are, are questions only the compiler can answer, and only about text it has already analysed.
Completing on a bare cursor is declined — offering the whole mirrored API as one undifferentiated
list is not help — so an option appears once at least one character has been typed, and the
prefix is filtered here rather than handed over in full for Godot to filter after paying for it.

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
