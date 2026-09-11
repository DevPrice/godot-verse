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

## What works

A `.verse` file is a Godot script. Attach one to a node the way you would a GDScript: it compiles
when the project loads, its `Ready()` runs on `_ready`, and its `Process(Delta:float)` runs every
frame. `PhysicsProcess(:float)` maps to `_physics_process`. Several scripts on several nodes work
independently.

A script *is* a class named after its own file, and the node it is attached to is `Self`. That is
the only shape a script has: a `.verse` file the compiled project has no such class for is not a
valid script, and Godot reports it as one that failed to compile.

```verse
using { /Godot.org/Godot }

mover := class(node2d):

    @export
    @export_group("Movement")
    var Speed<public>:type{_X:float where 0.0 <= _X, _X <= 500.0} = 60.0

    @export
    Greeting<public>:string = "Verse is running inside Godot, as a class."

    Ready<override>():void =
        Print(Greeting)

    Process<override>(Delta:float):void =
        set Position = vector2{X := Position.X + Delta * Speed, Y := Position.Y}
```

Godot's API is mirrored as a Verse class hierarchy under `/Godot.org/Godot`, generated from
`extension_api.json` by `tools/gen_verse_api.py`. Only `object` is a `<native>` class with a
C++ shadow; everything above it is ordinary Verse whose methods bottom out in a handful of native
primitives, so mirroring another hundred Godot classes costs no C++ at all. A method that mutates
the scene defers its write to transaction commit, and of the 1236 methods that used to carry
Verse's `<decides>` effect only the 132 returning an object still do — see the lifetime note
below for why the rest gave it up.

**The package exports what a script should call and nothing else:** `Print`, `IsInstanceValid`,
the three value types, and the mirrored classes with their singleton accessors. The plumbing under
them carries no access specifier at all — the `Vh…` primitives, the `variant` tuple they pass, its
`Tag…` constants, and `object`'s `Handle` — which in Verse means the enclosing module, and the
enclosing module is exactly the three files in `host/Verse`. Reaching past the typed layer to call
`VhSetValue(Handle, "position", …)` is the untyped spelling of `set Position`, trading a compile
error for a runtime type mismatch. Hiding them also keeps some fifty names out of every completion
list, and stops the wire tuple's shape from being an API commitment. The generator already worked
this way — every accessor it emits is `epic_internal` — and this is that rule applied to what
those accessors call. `Handle` is hidden for a second reason as well: it is the one thing that
outlives what it names, so a script holding a handle rather than the object holds a number no
guard can check.

Godot hands a singleton out by name rather than through the scene, so `input` and `engine` would
otherwise be classes no script could obtain an instance of. `gen_verse_api.py` reads
`extension_api.json`'s singleton list and emits `GetInput()`, `GetEngine()` and `GetTime()` for
the three mirrored classes that appear in it — `<decides>`, because `Engine::get_singleton`
answers nothing for a name this build did not register.

`IsInstanceValid` is a free function rather than a method, the way GDScript spells
`is_instance_valid(node)`. It is hand-written rather than generated with the rest of
`@GlobalScope`, for two reasons. Godot's own signature takes a `Variant`, so a faithful mirror
would read `IsInstanceValid(Value:variant)` and hand a script the one type this package keeps to
itself. And nothing can reach `@GlobalScope` at all yet: every mirrored call rides
`VhCallValue(Handle, …)`, a utility function has no handle, and Godot's C interface exposes those
functions only through `variant_get_ptr_utility_function`, a ptrcall needing per-signature
marshalling. Little would survive the trip anyway — the 78 math and 8 random names collide with
`/Verse.org/Simulation`, which every script imports, and 21 of the 28 general ones are
`Variant`-typed or vararg.

**A Godot property is a writable Verse member**, not a get/set pair. 686 of them across the
mirrored classes are emitted as `var Position<public>...:vector2`, and the `get_position` and
`set_position` they were built from are gone — two spellings of one thing is worse than one.
The mechanism is uLang's `<getter(...)>`/`<setter(...)>`, whose attribute classes are
`epic_internal` and so reachable on the same footing as `<native>`: use, not authorship. The
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

Test `IsInstanceValid(Node)` before reaching through a reference the scene may have dropped;
`demo/scripts/lifetime.verse` holds a child, frees it, and keeps looking.

*Known limitation:* the host runs a single `verse::FContentScope` for the whole project, and a
runtime error terminates the scope's task group — so one dead-object access kills every suspended
async task in every script, not just the offending one. Per-script scopes (or per-invocation, if
Godot ever reaches Verse off the main thread) are the right shape and are not built yet.

**`@export` puts a data member in the inspector.** The property list Godot shows comes out of the
*semantic program* the last analysis pass left behind, not out of the running bytecode —
`vh_class_export_list` walks `CClass::GetDefinitionsOfKind<CDataDefinition>()` and keeps the
members whose `CDefinition::HasAttributeSubclass` matches `/Godot.org/Godot/export`. That choice is
what makes the list refresh live: analysis re-runs on every keystroke, while code generation may
happen once per process, so anything read from the VM would be frozen at startup.

`export` is declared in the same runtime-compiled package `@global_class` is (below), under the
same authorship grant, rather than borrowed from Epic's own vocabulary the way it once was. Epic's
`editable` is `@customattribhandler`, so applying it calls a handler lookup and obliges the host to
load `VerseSimulationMetadata` and submit every member to that handler's rules — rules written for
a different inspector, which refuse what that inspector cannot draw. A `color` member is refused
outright; Godot draws colours fine. An attribute with no handler is just a type applied to a
definition, which is the whole of what exporting a member needs, so `export` carries no
`@customattribhandler` at all.

**A hint comes off the member's declared type, not off a second attribute.** A bounded
`type{_X:float where 0.0 <= _X, _X <= 500.0}` is already a range: the compiler enforces those exact
bounds at every assignment, so a slider built from the same numbers cannot disagree with the
language, the way a `@clamp_min` written beside a plain `float` could. An `enum` is already a list
of choices and a mirrored class is already the node or resource a slot will accept, so both hints
are read off the type the same way. A bound on one side only (`type{_X:int where 0 <= _X}`) becomes
`or_greater`/`or_less` rather than a missing hint. A strict `<` needs no special case at all: the
analyser normalises it to the adjacent double, and every bound is rounded inward onto the step the
inspector moves on — so `_X < 500.0` shows 499.999 and `_X <= 500.0` shows 500, without anything
having to remember which was written.

**`@export_category`, `@export_group` and `@export_subgroup` reach Godot's three nesting depths.**
Properties reach the inspector in the order the class declares them, since Godot has no
per-property section field: a member opens the section it names and every member declared after it
joins that section until another opens one, the same positional rule C#'s `[ExportGroup]` and
GDScript's `@export_group` follow. Where a member names more than one, the outermost wins — a
member cannot be the first of a group and the first of the category above it at once.

Above all three sits the script's own heading, which the script pushes at the head of its property
list the way `GDScript::_update_exports` does — there is no instance to ask for one, since a
non-tool script gets a placeholder in the editor and a placeholder only replays the list it was
handed. It names the registered class where the script has `@global_class` and the file otherwise,
which is a deliberate step past GDScript: `Script::get_class_category` reads `Resource::get_name()`
and so shows `player.gd` even for a `class_name Player`, where a heading that reads `Mover` is the
name Godot uses for that script everywhere else.

**A member that cannot cross is harvested with a reason, not dropped.** `vh_class_export_list`
lists every `@export` member whether or not it can reach the inspector, each with the line it was
declared on: a Godot reference must be spelled `?node2d`, because nothing can force a value into an
inspector slot and the declaration that compiles without the option, `node2d{}`, is a handle of 0 —
dead from birth and indistinguishable from one freed later; an `option` around anything else is
refused the other way, since the inspector has no empty slot for a number; and a reference to one of
the project's own classes that never registered itself is refused with the fix, which is to register
it — Verse will let a member be typed as any class in the project, but the inspector filters a slot
by a *Godot* class name, and only `@global_class` gives the class one.

Each refusal reaches the author as a warning on the line that declared it, the way the script
editor marks a GDScript warning. Without one the member is simply absent: Godot draws the property
list, and a member that never enters it leaves nothing behind to explain itself. The warnings are
harvested when an analysis lands rather than when the editor validates, because reading the export
list waits out any analysis in flight and `_validate` runs on the editor's thread while one usually
is — asking there would hand back the stall the background check exists to remove.

**Values cross both ways, through the VM's shape rather than the export list — a value exists
nowhere but the VM.** `vh_instance_get_field` reads a member off a live instance,
`vh_class_default_field` reads its declared default off a throwaway one (the Verse constructor
runs in `NewObject`, not in CDO construction), and `vh_instance_set_field` writes one. Matching
the VM's storage exactly is most of that work: a `var` of a scalar type keeps its value inside a
reference cell, a `var` of a container type keeps a *mutable* container, and a write that lands on
the slot rather than through it corrupts the member silently — the read agrees, and only the
interpreter ever objects.

That the defaults come from the VM has one consequence an author meets early: **editing an
initializer does not change the default the inspector shows until the editor restarts.** Evaluating
`= array{60.0, 120.0}` needs generated code, and code generation happens once per process, so every
declared default on screen dates from the session's first build. The *shape* of the list does keep
up — a new member, a changed type, a range, a hint all come from the analysis, which re-runs on every
keystroke — which is what makes the two look like they should refresh together. A value an author has
actually typed into the inspector is unaffected either way: it lives in the scene, not in the script.

**A reference is two mechanisms, told apart by which package declares the class.** A member typed
`?sprite2d` names one of the generated mirrors, and its value is a Verse wrapper the host builds
around the handle Godot hands over — `NewObject` against the class's `UClass`, the same call that
instantiates a script, because a mirrored class is ordinary Verse over the one native `object` and so
its instance *is* a UObject. A member typed `?mover` names a class the project declares, and there
the object already exists: it is the one that node's own script instance built, and it crosses as an
instance rather than as a handle (`vh_instance_set_field_instance`). Building a second would give one
node two Verse objects, two sets of members, and no way for the author to tell which they were
looking at. The rule then runs the other way too: a mirrored member assigned a node that *does* carry
a Verse script holds that script's own object rather than a second wrapper around the same handle.

Drawing the slot needs two names, not one, and that is worth separating carefully. What the slot
*filters by* is the name Godot knows the class as — the registered `Mover` for a script class. Which
*kind* of slot it is — a node picked out of the scene, or a resource off disk — Godot decides from the
class's ClassDB ancestry, and a registered class is not in ClassDB: `is_parent_class("Mover", "Node")`
is false, and an object property with neither hint is drawn as a resource picker. So the host sends the
nearest mirrored class in the referenced class's own superclass chain (`node2d`) beside the registered
name, which is a name ClassDB does know. GDScript splits the same two answers the same way.

**A struct crosses as the numbers it is made of.** `vector2`, `vector3` and `color` are hand-written
Verse structs, and each is carried as a tuple tagged with the Godot type to rebuild from it — the same
encoding a method argument already used. Building one needs the only piece of real VM object
construction in the bridge, and one non-obvious thing about it: a field the class declares with an
initializer is *raised to the shape* as a constant shared by every instance, so there is no
per-instance slot to write. The archetype has to ask for object fields explicitly, which is what
`VNativeRef::FromNativeStruct` does to hand a native struct to ordinary Verse code.

**An array becomes the packed container Godot has for its element, and an `Array` where it has none.**
`[]float` is a `PackedFloat64Array`, `[]int` a `PackedInt64Array` (a Verse int is 64 bits; narrowing
it would discard half of a value the language allows), `[]string` a `PackedStringArray`, `[]vector2` a
`PackedVector2Array`. `[]logic` has no packed form, so it becomes an `Array` carrying its element type
beside it — an array editor that was not told what it holds offers the author a row of anything.

One rule about building those is worth knowing, because getting it wrong is fatal rather than wrong:
an element's mutability follows its container's. Reading a `var` container hands out an immutable
snapshot, made by freezing each element in turn, so an element has to be freezable — and a `VArray` is
not. Every `VArrayBase` constructor sets the deeply-mutable flag and nothing clears it, while `VArray`
has no `FreezeImpl`, so the elements of a `var []string` have to be mutable arrays too.

**An enum crosses as its ordinal**, which is what GDScript and C# store too, with the enumerators as
the choices the inspector's dropdown offers — including the trap that reordering them reinterprets
every scene already saved. The value in the slot is an enumerator rather than a number, so a write
takes the enumeration from the one already there; the ordinal is bounded against the enum the author
*declared* rather than against the VM's count, and one outside it is refused rather than clamped,
because a scene saved against a longer version of the enum will carry one.

Reading is uniform — the option is unwrapped and the handle reported — with one wrinkle that decides
the shape of the whole read path. Verse spells an empty `option` and `logic` false with the same
cell, so a member holding nothing cannot be told from a member holding false by looking at it. The
read asks the semantic program what the author declared, the same way the write path asks whether a
member is a `var`.

A `.tscn` stores a node-typed property as a `NodePath` and applies it in the deferred pass at the end
of `PackedScene::instantiate`, after every node exists — which falls inside the window where the
instance is still unsealed, so even a non-`var` reference gets filled in. A resource needs one thing
more: a Verse handle is a number and holds no reference, so the script instance keeps a
`Ref<Resource>` per exported reference member. Without it, a resource whose only other holder was the
inspector is freed the moment the write returns, leaving the script a handle to nothing.

**A file that stops compiling keeps its inspector.** Godot gives a non-tool script a
`PlaceHolderScriptInstance` in the editor, and that placeholder holds its own copy of the property
list and the values on it — `update()` *erases every value whose name the new list omits*, so
handing it an empty list over a typo clears the inspector and drops the values out of the `.tscn`
on the next save. GDScript's answer is `placeholder_fallback_enabled`: an analysis that fails
leaves the last good list standing and flips the flag, which is Godot's cue to read the inspector
out of the placeholder's copy rather than asking the script. `VerseScript` does the same, from
`refresh_exports`. Values typed before the break survive the break, and a scene loaded against a
script that does not analyse hands its stored values back through `property_set_fallback` once it
does.

Because the list comes from the analysis rather than the build, it also comes back after a build
that *never succeeded* — the case a one-shot code generator would otherwise strand for the whole
session. The script still cannot run until the editor restarts, so the failed build says so once.

**A non-var is an initializer, not a constant.** Both kinds of `@export` are editable in the
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

**A landed analysis asks the editor to look again.** "Replaced on the next validate" is only true
if there *is* a next validate, and Godot has no reason of its own to run one: it validates when the
text changes and then stops. Fix the last error and the sequence is idle timer fires, we answer
from the analysis before the fix, the fresh analysis lands 100ms later with nothing to report --
and no one asks. The line stays underlined, the error bar stays red, and both clear only when the
author types the next character. Documentation has the same shape and a coarser timer: Godot
republishes a script's class doc when the script is *saved*, so a doc built from the analysis a
save did not wait for stays a save behind.

So the frame that lands a result the editor has already drawn something from -- diagnostics that
differ from the previous analysis, or a script that settled its validity on this one -- asks the
script editor for both again. `CodeTextEditor::validate_script` is the signal its own idle timer
emits for the first, and `CodeTextEditor` is reachable because the `CodeEdit` that
`ScriptEditorBase::get_base_editor` hands out is its child; it is not in the extension API, so the
extension checks for the signal rather than assuming it, and an engine build that moves it costs
the stale underline back, not a crash. `update_docs_from_script` is the ask for the second, and is
the same pair of calls a save makes. Only the visible editor is refreshed -- Godot validates a
script when its tab is opened and republishes its documentation when it is saved, so the rest come
back current on their own. The ask is made from `_frame` rather than from the poll itself, because
completion also reaps analyses, and re-entering the editor from there would rebuild its error list
mid-popup.

**Saving does not block on it either.** Saving is where a script's validity and its export list
are decided, so the answer has to be about the text being saved rather than the one before it --
but waiting for that is a ~100ms freeze on every Ctrl+S, which is exactly the hitch GDScript does
not have. So a save queues the analysis and returns, and the script keeps the validity and exports
the previous one gave it until the new one lands. Nothing regresses in the meantime: a stale
answer is the answer from a moment ago, not a wrong one, and it is never the *superseded* answer
that would flag a mistake the author has already undone.

`VerseScriptLanguage` keeps a list of the live `VerseScript`s for this, and tells each one when a
result is published; a script that queued an analysis and recognises the published text as its own
re-derives validity and republishes its export list then. Usually there is nothing to wait for at
all: Godot validates the buffer on its idle timer well before the author reaches for Ctrl+S, so by
the time the save arrives the host is already holding that exact text and the whole thing is a
string compare.

**Queueing an analysis is not the same as starting one, and only `_frame` may start one.** Every
host entry point that reads the semantic program joins the analysis thread before it answers --
`vh_has_class`, `vh_class_members`, `vh_class_export_list`, all of them. So an analysis started in
the middle of the editor's work is one the editor waits out, and it does not matter that the call
that started it returned immediately. Saving is where that bites, because
`ScriptEditor::save_current_script` asks the script for its documentation the instant
`save_resource` returns: an analysis begun inside the save is paid for by
`update_docs_from_script`, three lines later, and Ctrl+S hitches for the full ~100ms with nothing
in the extension appearing to block. So `request_check` only records the buffer, and `_frame`
hands it to the host after its own poll, refresh and tick are done -- a queued buffer waits a
frame; a blocked editor would wait the whole analysis.

That leaves the documentation Godot published during the save describing the program from before
it, so the frame that lands the result asks the script editor for both again -- see the note on
re-validating above, which is the same mechanism and the same trigger.

Completion is the one caller that still waits (`settle_checks`). It has to: it asks the host about
a buffer the host must already be holding, and an analysis finishing afterwards would be recorded
as the text the host holds when by then it would not be.

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
construct it precedes — but for a member sitting behind several lines of `@export`, `@export_group`
and friends, the node that begins the construct is the attribute clause and not the member, and
the comment is reachable from the definition only by guessing at the shape of the syntax tree
around it. The definition's line is already known, the file's text is already to hand, and
walking up from that line while the lines are comments needs none of that.

A definition answers at its own name too, so hovering `Ready` where it is declared describes it
rather than declining. That needs the locus narrowed all the way down to the name. A definition's
own span runs from its first attribute to the end of its body, so matching against that would
resolve every blank column inside a function to the function — but narrowing to its first VST
child is not enough either: for `PhysicsProcess<override>(Delta:float):void` that child still
covers the specifier, the parameter list and the return type. None of those means "this
definition", and hovering any of them used to describe the method. So the narrowing descends the
leading edge of the tree — a type spec's first child is what is being typed, a call's is the
callee — until it reaches the identifier, whose own locus excludes the attributes, which hang off
its Aux rather than its children.

What that leaves is exact: `<public>` and `<override>` resolve to nothing and so show no tooltip
at all, which is right, because an access specifier is not a definition and this project has no
documentation of Verse's own to offer for one.

**A parameter is described by itself, not by the method it belongs to.** It is not in the AST the
walk covers — analysis moves parameters onto the function's signature and leaves only their types
behind — so they are asked of the enclosing function instead, after the walk, which is also the
only place the cursor can be for one to matter.

Where it is *declared*, though, the editor shows nothing at all, which is what GDScript does: the
declaration is the line the cursor is already on, so there is nowhere to jump and nothing to say
that the line does not. And a parameter carries no prose anywhere, because its source line is the
line its whole function is declared on — the comment block "above" it is the function's, so
reading one would describe an argument with the method's documentation. The host reports which
definitions are parameters rather than leaving that to be guessed from the scope.

**An override is documented by what it overrides.** `Ready<override>()` in a script has nothing
to say about itself, so the lookup carries the definition it overrides alongside the one under the
cursor, and the editor falls back to the parent's documentation when the declaration carries no
comment of its own — a comment the author *did* write always wins. The click goes to the parent
too, since this definition's own line is the one the cursor is already on.

`object`'s `Ready`, `Process` and `PhysicsProcess` are in the method map as `Node._ready`,
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

A local or a parameter is reported as one of Godot's two *local* lookup results, which is not
where it looks like it belongs. `SCRIPT_LOCATION` is the honest label and it jumps correctly, but
the tooltip path drops it in a `// Nothing to do.` branch and shows nothing at all. `LOCAL_VARIABLE`
and `LOCAL_CONSTANT` are the only results that carry a description and a type straight off the
lookup, rather than going to documentation for them — the click path never reads `type`, it jumps
on `location` alone as long as `class_name` is empty, so leaving that key unset is load-bearing.
Verse's `var` split maps onto the two exactly.

**A member of the script's own class is a property, not a local.** Godot has the right labels —
`CLASS_PROPERTY` and `CLASS_METHOD` — but they cost something: they describe the symbol out of
*documentation* rather than out of the lookup result, so naming one without registering any leaves
the tooltip saying "Property" and nothing else. So the script registers its own, out of
`vh_class_members` and the comment blocks above each member, and the tooltip gets the label and
the prose both.

Naming the class is also what would normally divert ctrl+click into the help viewer instead of
jumping. It does not here, and the reason is one flag: Godot diverts only for a class whose
documentation is *not* a script doc, and `is_script_doc` is settable through the dictionary a
GDExtension language returns. GDScript relies on exactly the same thing.

The cost is at startup. Godot loads every file of a language that supports documentation during
the editor's filesystem scan, and loading a `.verse` builds the project — so the host now boots
when the project is opened rather than when the first script is. For a project whose scene already
runs Verse that is the same work moved earlier; for one where nothing does, it is new. Setting
`_supports_documentation` back to `false` gives the old startup and the old "Local Variable" label
together; there is no way to have one without the other.

**A name from the mirrored API resolves to Godot's own documentation instead.** Ctrl+clicking
`node2d`, `Position` or `GetChild` opens the class reference for `Node2D`, `Node2D.position` or
`Node.get_child`, and hovering any of them shows the description Godot already ships. A mirrored
property arrives as a `var` like any `@export` member, so the routing keys off the *owner* —
only a mirrored class appears in the table. `vector2`, `vector3` and `color` are in it too, and so
are their fields: they are Godot builtins that happen to be hand-written in
`GodotApi.native.verse` rather than generated, and without an entry the editor calls them local
constants and the `X` of `Position.X` does nothing at all when clicked — it resolves fine, but to
a definition in the engine tree, which has no `res://` file to jump to. `object` — the base every
script names in its own class header — is the same problem reached from the other end: Godot's
`Object` is the one class `gen_verse_api.py` skips outright, because the hand-written `object` in
`Godot.native.verse` already stands exactly where it stands, so the class it should be paired with
is the one class the table cannot contain. It is named for the documentation lookup on its own,
kept apart from the generated table because "part of the mirrored API" is a different question that
the completion path asks and needs the old answer to. Nothing here writes that
prose: both paths key off `class_name`, which is precisely the key that diverts the click away from a jump
and into the help viewer, and which the tooltip uses to fetch the description out of the same doc
data. There is nothing to jump to anyway — the generated API is compiled from the engine tree,
not from the project.

**A global function resolves the same way, through `@GlobalScope`.** `Print` and
`IsInstanceValid` belong to no class, so there is no owner to key the method table on, and the
file they are declared in is in the engine tree — which left them, before this, described by their
own comment and with nowhere for a click to go. Godot keeps its global functions on
`@GlobalScope`, a class the documentation has and `ClassDB` does not, and GDScript sends a click
on `print(` to exactly that, so naming it costs nothing and gets the whole class reference entry.
The three singleton accessors — `GetEngine`, `GetInput`, `GetTime` — are documented by the class
they hand out instead, which is the only thing one of them can be said to be.

Telling a global apart from a member is a question about the *owner*, again: a definition at the
top level has no class to be owned by and reports the file it was written in instead, because a
snippet scope carries its path as its name. So the owner agreeing with the path is what says "top
level", and the path saying `Godot.native.verse`, `GodotApi.native.verse` or
`GodotClasses.native.verse` is what separates the package from the project — whose one flat scope
would otherwise let a script's own `Print` be documented as Godot's.

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
`<public>` and prefix `@export`.

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

One name has to be spelled out beside the table: `object`, the only type `/Godot.org/Godot`
exports that no generated entry stands behind. It is hand-written rather than mirrored precisely
because Godot's `Object` is the one class `gen_verse_api.py` skips, and it is also the type name a
script is likeliest to write, since it is the base in its own class header. The rest of the
package needs nothing: `Print`,
`IsInstanceValid` and the singleton accessors are calls, and a call is coloured from its position
rather than from any list.

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

**Completion comes from the compiler, including after a `.`.** `vh_complete_symbol` answers three
questions off the semantic program: the members of the expression at a position, every name a
scope there admits — the locals declared above the cursor, the enclosing class and its
superclasses, and each scope's `using`, which is how the whole mirrored Godot API arrives — and
the attributes among those. So `Position.` offers `X` and `Y` and nothing else, `Self.` offers the
script's own methods beside `node2d`'s properties and `node`'s methods two classes up, and a bare
identifier offers what is actually in scope rather than a list of every class that exists.

Completion is only ever asked about text that does not compile: the member being typed does not
exist yet. Two things make that answerable.

- **The receiver survives an expression that fails.** uLang's `CExprError` holds onto its analysed
  children precisely so that a well-formed sub-expression is still usable inside a parent that is
  not, so `Position.` — a syntax error — still has `Position` in the tree with its type on it.
  That is why the position handed over is the *receiver's* last byte and not the cursor's: the
  cursor is on a name that resolves to nothing by construction.
- **It runs its own analysis, and throws the diagnostics away.** There is no moment at which an
  analysis of half-typed text already exists, so `vh_complete_symbol` takes the buffer the way
  `vh_check_project` does and analyses it. Its errors describe a line the author is in the middle
  of writing and are discarded rather than reported.

That analysis costs the same ~100ms a validate does, and Godot re-asks on every keystroke while
the box is open. What makes that affordable is that the buffer handed over has the half-typed
identifier replaced by a fixed placeholder, so every prefix of one identifier is the same
question and the whole of a completion context is one analysis. Substituting rather than deleting
is deliberate: `set X = ` is a parse error that can take the enclosing function's AST down with
it, while an identifier nothing defines costs one diagnostic nobody sees.

Inside a comment nothing is completed at all. Godot raises the popup on its own the moment an
option matches what is being typed, so a scope walk answered there puts the Godot API over the
middle of a sentence — and the sets that need no compiler, the class names and the keywords, would
do it even with the host unloaded. Whether the cursor is in one is a question for the lexer the
highlighter already runs: a `<#` several lines above is what makes the line a comment at all, and
the dedent that ends a `<#>` one is not a delimiter to search for. It is asked of the character
*behind* the cursor, because a caret sitting on the `#` it just typed is still in code.

**An `@` completes to attributes and nothing else.** The scope at that position is the same one a
bare identifier completes against — three hundred names, of which five are legal after an `@` —
so the mode exists to narrow it. What survives is two shapes: an attribute class, and the
`<constructor>` function beside one that carries a payload. `@export` and `@global_class` name
their classes directly, since neither takes an argument; `@export_category("Movement")` names the
constructor, because Verse builds `export_category_attribute` out of its argument through a
function of that name. The compiler-generated constructor every class has is dropped everywhere
else for being unspellable, and this is the one place a constructor *is* what the author writes.

The names are offered without the `@`. CodeEdit walks back over identifier characters to find the
text it is filtering on and stops at the symbol, so the `@` is neither matched against nor
replaced on insert — GDScript strips it off its own annotations for the same reason. What is not
narrowed is where an attribute may be applied: `export` is `@attribscope_data` and is offered
above a class declaration all the same, because at the moment the question is asked the thing it
would attach to has not been written yet.

A call is completed with its brackets, and where the caret lands depends on whether there is
anything to type between them: a function with parameters inserts only the opening bracket, so
CodeEdit's brace completion closes it and leaves the caret inside, while one without inserts the
pair and leaves the caret past it. That is GDScript's rule exactly, ellipsis in the displayed name
and all, and it needs the parameter count to reach the editor with each completion item.

*Which* bracket is a second question, and the one Verse asks that GDScript does not: a `<decides>`
function is called `GetChild[0]`, so completing one with a parenthesis is an error the moment it
lands. The answer comes off the signature the item already carries — from the effect specifiers
alone, between the parameter list's closing parenthesis and the `:` before the result type, rather
than from the signature as a whole, because a parameter or a result can itself be a fallible
function *type* and spells `<decides>` too. The argument hint drawn above the caret has to agree
with the line under it, and takes its own answer from the bracket already written there:
`vh_signature_desc` reports a function's parameters but not its effects.

**Inside a class body, a method completes to its whole declaration.** GDScript answers a name
typed at class level with `func _ready() -> void:` rather than with `_ready`, and the Verse
spelling of the same idea is the base's signature with `<override>` after the name: typing `Up`
in a script's class body offers `Process<override>(Delta:float):void =`. The trailing ` =` is
where the body goes; nothing inserts a newline, so the editor's own indent takes the caret there.

That needs three things the compiler has and the editor does not. The first is the signature with
its parameters' *own* names — an override must match what it overrides, and the function type
`vh_complete_item` already carried spells the same definition `float->void`, names dropped. So
the item carries a `SignatureUtf8` spelled from `CFunction::_Signature` alongside it, effect
specifiers included, and those come out relative to the function default, which is why an
ordinary method's declaration carries none. The second is whether the compiler would accept the
override, which is `IsOverridable`: a class member — a module-level function has nothing to
override it from — that is neither `<final>` nor a class var's `<getter>`/`<setter>`, the three
things `DetectIncorrectOverrideAttribute` refuses one for. The accessors matter more than they
sound: the generated mirror is built out of them, and `node2d` alone contributes a dozen. The
third is free — a method the class has already overridden comes back owned by *that* class,
because the scope walk lets a subclass' copy win over its superclass' and drops the duplicate, so
comparing the owner against the class being edited is what stops an override that is already
written from being offered again.

**What the compiler would accept is not what is worth offering**, and the gap is the whole
mirrored Godot API. Every generated method is a class member with no `<final>` on it, so the
compiler would take an override of `GetName` — and it would do nothing, because
`gen_verse_api.py` *skips* Godot's virtuals: a generated method is never the Verse spelling of
one, only a concrete shim forwarding through the handle. Nothing dispatches back into it. So the
editor excludes the mirror wholesale, which the class table already answers, and the exclusion is
sound only because every emitted class lands in that table — both come off `emit_order`.

That leaves the two sets that mean something. `object` is hand-written rather than generated and
so is not in the class table: its `Ready`, `Process` and `PhysicsProcess` are the only Godot
virtuals the bridge carries at all, and the generated method table names exactly those three as
`object`'s — which is what the table lookup is for, since anything else that class ever grows
would be a helper nothing dispatches to. Everything else is a class the author wrote, and the
mirror never contains one of those. So a script on `node2d` is offered three declarations, less whichever it has already
written, plus whatever its own base script declares.

Which position counts as a declaration is read out of the text rather than the analysis, because
Godot asks on every keystroke and a half-written declaration does not report the class it belongs
to anyway. The cursor has to be on a line with nothing but indentation ahead of it, under the
file's class, and under nothing that class body itself opened — which the indentation already
encodes, since a line of code between the two indented *less* than the cursor would be the header
of the block the cursor is really inside. Only the file's top-level class is found, so a member
of a nested one completes as an ordinary name; one class per file is what the flat project scope
forces regardless. Everything else in scope is still offered there, so misreading the position
costs an option that was going to be in the list anyway.

The class-name and keyword sets are still offered alongside, for a bare identifier only. They
cover what a scope walk cannot: a class the file has not brought into view, and the reserved
words, which are not definitions at all. Completing on a bare cursor with no `.` before it is
still declined — offering the whole mirrored API as one undifferentiated list is not help — but
after a `.` there is no such worry, because the member set is bounded by the receiver's type.

Every one of those sets is trimmed against what has been typed before it is handed over, because
Godot builds an option for each name it is given and re-asks on every keystroke. The trim is a
case-insensitive subsequence — `Prcs` reaches `Process` — rather than a prefix test, and that is
not a nicety: CodeEdit fuzzy-matches and ranks the options it receives, so a filter narrower than
its own decides the answer by itself and drops candidates the editor would have offered. What is
handed over is deliberately matched on the *name* rather than on the text the option displays,
which is the one place this stays narrower than Godot: an option that displays a whole
declaration would otherwise match on its parameter types, and `flt` is not what someone typing
`float` is looking for.

**A call shows its arguments while you write them.** `vh_signature_at` reports the parameters of
the function being called, and the editor draws them above the caret with the argument the cursor
is in highlighted — the same hint GDScript gives, spelled the way Verse declares it:
`GetChild(Index:int):object`, not the return type first.

Finding the callee is text work rather than compiler work: the scan walks back from the cursor,
closing brackets as it goes so a nested call's arguments do not read as the outer call's, and
stops at the first `(` or `[` nothing closed. That bracket's callee is the identifier ending just
before it. Counting the commas after it, again at depth zero, is which argument the cursor is in.
The position handed to the host is the callee's last byte, for exactly the reason members use the
receiver's: the arguments under construction do not analyse.

The hint is asked for before the options are, because the interesting moment — right after the
`(` — is a cursor with nothing typed, which is where completion itself declines. Both questions
are asked about one buffer, and the host answers the second off the first one's analysis: it
remembers which text the program it holds was built from and skips re-analysing when it matches.

**Completion spends the analysis a hover was relying on.** Leaving the host holding the
completion buffer is the price of analysing it, and every locus `vh_lookup_symbol` would then
report describes text with a placeholder spliced into it. So the language drops its record of
what the host holds for that file, which makes the lookup decline until the next validate
re-analyses. It also settles any analysis already in flight *before* asking, because one
finishing afterwards would be reaped later and recorded as the text the host holds — which by
then it would not be.

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
the host can only *generate* once per process. So the first script that needs compiling scans
`res://` for every `.verse` file and builds them together; scripts added while the editor is running
are not picked up until it restarts, and neither is a default an author changes in code — a declared
default is evaluated by generated code, so refreshing one means generating again.

The reason is a single `#if` in engine code, and it is worth knowing precisely because it says what
would have to change. `NotifyCompiledVersePackage`
(`CoreUObject/Private/Serialization/AsyncLoading2.cpp`) hands the loader's package ref a `UPackage`
only `#if !WITH_EDITOR`. Having one is what makes the *next* publish of that package call
`PinPublicExportsForGC`, which asserts `checkObject(!LoaderImport)` over the previous publish's
exports — and they carry that flag, because the previous publish set it on every one of them. The
editor rebuilds Verse all day because it compiles with `WITH_EDITOR=1` and never takes that path.

Three ways around it are closed. Unpinning first is the symmetric operation
(`ReleasePackageRef` → `UnpinPublicExportsForGC`), but it is private to that file and nothing ever
releases a *compiled* Verse package's ref, so the refcount never falls to 0. Renaming the old
`UPackage` does make `AddPackageRef` drop the stale ref, but by way of
`RemoveUnreferencedObsoletePackage`, which `check(PackageRefCount == 0)` as well. And
`bCompileAgainstEditor` does get a Program target `WITH_EDITOR=1` — UBT grants exactly that
combination — but it pulls the whole Engine in behind it and does not compile against
`bCompileAgainstEngine = false`, so buying it means giving up the lean host this target is.

What *does* work, and is worth knowing for anyone who picks this up: `IncrementalizeProjectSource`
with `EBuildMode::All` keeps the already-loaded native packages out of a second build, exactly as it
does for the editor. That narrows the obstacle from the whole program to the two packages the host
compiles at runtime — its scripts and its attributes. The remaining move would be to publish each
generation under a fresh package name so no ref is ever reused, at the price of leaking the previous
generation's classes, which stay pinned for the life of the process.

Analysis is not subject to any of this. A build configured with `bSemanticAnalysisOnly` and no
digests, code or bytecode publishes nothing and can be run as often as you like, which is what gives
the script editor live diagnostics — and why the *shape* of a script refreshes as you type while its
values do not. What analysis cannot do is replace the bytecode a running program is already
executing, so hot reload is narrower than it looks rather than flatly impossible.

**One top-level name per file.** Every file in the project shares one flat `/user@localhost` scope
and Verse forbids shadowing, so two files that both define a top-level `Ready()` are a compile
error rather than two scripts. Naming the class after the file is what keeps that from happening —
file names are already unique, and it is why a script is required to be shaped that way.

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
