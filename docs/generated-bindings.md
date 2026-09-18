# Generated bindings for classes the mirror does not carry

The mirror is generated from `extension_api.json`, which describes core Godot and nothing else. Two
kinds of class are therefore invisible to a Verse script: a class a **third-party GDExtension**
registers in ClassDB, and a class a **script** declares with `class_name` — GDScript or C#. This
document is the design for generating Verse for both, and the record of the decisions that shaped
it.

R-INT-2 is already done and is not what this is about: `Target.Callv("hit", Args).AsLogic[]` works
today. What this buys is a declared type — completion, hover, an argument hint, and a compile error
at the call rather than an empty `variant` at runtime.

## 1. Why this is not "add them to the mirror"

The first instinct is to regenerate `GodotClasses.native.verse` with the extra classes in it. That
is the wrong place, and the reason is worth writing down because the usual explanation — "adding a
class means rebuilding `verse_host.dll`" — is only half true.

**The rebuild is not to compile the Verse.** `phase-2-design.md` §3.1 measured it:

> **Where M1's time lands: nowhere.** The mirror is *ordinary* Verse — only the ~30 declarations in
> `Godot.native.verse` are `<native>` — so VNI generates no C++ for it and the 2.8 MB never reaches
> the C++ compiler.

The mirror's `.verse` files are read off disk by the **runtime** compiler, out of the staged copy at
`Engine/Source/Programs/VerseHost/Verse/`. That is why the host must load from
`Engine/Binaries/Win64` at all: VNI records each package's source directory relative to the loaded
module. `build_host.py --stage-only` puts new sources there without invoking UBT.

What forces the relink is the generated **C++ tables**, not the Verse:

- `host/Private/GodotClassNames.gen.h` — the Godot-class-to-Verse-class table that
  `MirroredNameForGodotClass` binary-searches (`HostScript.cpp:4322`). A class with no row there
  cannot cross as itself.
- `host/Private/GodotMathLayout.gen.h` — the math types' field trees.

Even if that cost were zero, the mirror would still be wrong for this: **it is one package in the
engine tree, shared by every project on the machine**, and these classes are per-project and change
when someone installs an addon. The project side already recompiles on every build with no rebuild
of anything.

## 2. The shape: a binding is a subclass, not a wrapper

The first proposal was a wrapper class holding a `Target:rigid_body2d`. It was rejected on the spot
and correctly: `AddChild(M)` does not compile, and every call reads `M.Target.GetName()`. The
bridge's own code says the same thing one level up, above `GInstancesByHandle`
(`HostScript.cpp:4072`):

> a fresh mirror wrapper's class is `node`, and no downcast to a script class can succeed against
> one.

A binding is therefore what a Verse script class already is — **a subclass of the mirrored base,
living in a Verse package of its own**:

```
mob<public> := class(rigid_body2d):
    Speed<public>()<reads>:float = ...
    Hit<public>(Power:int)<decides><transacts>:logic = ...
```

`Call` and `Get` are inherited from `object`, so no body needs a stored handle. `mob` passes
anywhere a `node` is accepted, inherits all 3312 mirrored properties and 503 signal accessors, and
`mob[SomeNode]` is Verse's own downcast with no new cast machinery.

## 3. What the host has to learn

One thing, and it is narrow. `GodotVerse::ObjectForHandle` (`HostScript.cpp:4398`) asks three
questions of an incoming handle: is a Verse script instance bound to it, did Verse mint it, and
otherwise what does `get_class()` say. A GDScript-scripted `RigidBody2D` answers `RigidBody2D` and
gets a mirror wrapper.

It needs a fourth question: **does this handle's class, or its script, name a binding class?** The
construction path underneath needs nothing new:

- `NewMirroredWrapper(UClass*, int64)` is class-agnostic — `NewObject<UObject>` inside an
  `FAdoptPeerScope` (`HostScript.cpp:4053`).
- `FindGodotClass` already resolves a project class by module-qualified name in the script package
  (`HostScript.cpp:1896`), which is what `vh_instantiate` uses for every Verse script.

The two keys differ. A ClassDB class is keyed on `get_class()` and needs no new Godot callback. A
script class is not — `get_class()` on a node carrying `mob.gd` answers `RigidBody2D` — so the
script's global name has to be read as well, either through a new callback beside `GetClassOf` or
supplied by the consumer at the crossing.

## 4. Decisions

Taken in the requirements interview, with the reason each rests on.

| Question | Decision | Why |
| --- | --- | --- |
| Which classes | Every ClassDB class the mirror does not carry, plus every script class with a `class_name` | Godot enforces global-name uniqueness, so the Verse class name is collision-free by construction. A script with no `class_name` is skipped and said to be skipped. |
| Trigger | Any change to the class roster | Not a GDScript-specific feature. `EditorFileSystem.script_classes_updated` is language-agnostic, so C# comes free; `GDExtensionManager.extensions_reloaded`, `extension_loaded` and `extension_unloading` cover ClassDB. Plus a scan at startup. |
| Where the files live | Under `.godot/`, never committed, never shown | An author should not have to think about them. Costs an explicit arm in `find_project_files`, which skips every dot-directory (`verse_script_language.cpp:3665`). |
| Roster change mid-session | Regenerate and rebuild silently | The new classes are completable within a second or two of the addon loading. |
| Volume | Bind everything, then digest the package | Binding classes are re-analyzed from source per keystroke where the mirror is read from its digest. Phase 2 measured 60 classes at 158 ms and 1022 at 850 ms, so the digest is what makes "bind everything" affordable. |
| Package | Its own Verse package: `using { /Godot.org/Bindings }` | A digest is made of a package, so separateness is what the volume decision rests on. A collision with a mirrored name is spelled `(/Godot.org/Bindings:)timer` — verbose, unambiguous, and nothing is ever refused for its name. |
| Classification rules | `gen_verse_api.py`'s, exactly | `<decides>` for object returns, `<decides>:void` for predicates, `logic` for the 306 that answer a value, `<reads>` where Godot says `is_const` and the method answers something, properties as writable members except where a nested struct or container forces a getter/setter pair. |
| Where those rules live | Ported to C++, with a differential test | The units layer runs the C++ classifier over the mirrored classes and asserts it reproduces `GodotClasses.native.verse`. Drift is caught by a test rather than prevented by structure, which is how `CONST_OVERRIDES` and the skip table are already kept honest. |
| Enums, constants, statics | All three, matching the mirror | A third-party physics class is unusable without its enums. Costs what `phase-2-design.md` §3.1 measured for the mirror's 793, now per project. |
| Signals | Typed `signal(t)` from the start | A binding signal should be indistinguishable from `Timer.Timeout()`. Needs `BindEngineSignal` taught about non-mirror packages and the shapes recorded in the sidecar. |
| Construction | Yes for both, by the Godot spelling | A ClassDB class mints through `ClassDB.instantiate(name)`; a script class mints its base and then `set_script`, which `spec.md:442` already names as the Godot spelling for R-INT-1. |
| `<final>` | Not used | It exists — `ErrSemantic_FinalSuperclass`, *"Class `X` cannot be a subclass of the class `Y` which has the `final` attribute"* — but a GDScript hierarchy forces `boss := class(mob)`, which `<final>` on `mob` forbids. Inheritance is made to work instead, and the one case that cannot work is refused by the bridge rather than by the language (§5). |
| Exported games | MUST work | A game shipping a physics addon needs its bindings at runtime, so the class-to-binding table joins the sidecar beside the declared types and the 503 signal payloads. The export layer asserts it. |
| Mirror's own scope | Open | Per-project bindings can cover any ClassDB class, which makes "curated mirror plus generated bindings" a coherent option for the first time. Answer it with numbers once the generator exists, the way §3 answered `--all`. |
| Paperwork | This document only | Spec requirements and a roadmap phase wait until the spikes come back. |

## 5. Inheritance, and the one case that cannot work

**How GDScript does script-to-script inheritance.** One script instance on the object, and a chain
in the *Script resources*. `GDScriptInstance::callp` (`gdscript.cpp:1963`) walks
`sptr = sptr->base.ptr()` looking for the method and calls it with `this` — the same single
instance — as the receiver. Member variables work because `member_indices` carries the base's
members in one flat layout on that one instance.

That mechanism is GDScript-internal at both ends. `GDScriptFunction::call` takes a
`GDScriptInstance*` and indexes members by slot, so it cannot run against a `VerseScriptInstance`;
`base` is `Ref<GDScript>` (`gdscript.h:83`), so GDScript cannot extend a Verse class either; and an
`Object` holds exactly one `script_instance`, so there is no seam to add one.

Three cases follow, and only the third is a problem:

- **`boss := class(mob)`** — works properly. It stays inside GDScript's own chain: a node carrying
  `boss.gd` really does have `mob.gd`'s methods, and `Call("hit")` finds them exactly as `callp`
  does. Binding-to-binding inheritance mirrors the GDScript hierarchy.
- **`player := class(rapier_character_body)`** — works properly. Ordinary Godot inheritance from an
  engine class, the same shape as `class(node2d)`.
- **`player := class(mob)`** — cannot work. The inherited methods forward to `Call("hit")` on an
  object whose script is `player.verse`, and Godot has no `hit` there.

**The third case is refused in `_validate`**, at the class's own line, naming R-INT-6 and the
one-script-per-node rule. The alternative — let it compile and raise at the first inherited call —
puts the failure a long way from its cause, and the cause is not something the author can fix in
their own file. The check belongs in the pass that already validates a script's base type, and the
test is whether the base resolves to a binding for a *script* class rather than for a ClassDB one.

## 6. Traps

Each of these is a way to get it wrong that costs a session to find.

- **`GodotPeerClassFor` walks to the nearest ancestor in `/Godot.org/Godot`** to decide what Godot
  class to mint (`HostScript.cpp:4292`). A script extending a binding class would mint the mirrored
  ancestor — a `RigidBody2D` where a `RapierBody2D` was meant. The walk has to learn about the
  bindings package, and CLAUDE.md's warning applies: add a fifth construction path and it leaks a
  Godot object per construction, silently, while a working scene looks entirely normal.
- **`GHandleClassCache` is keyed per handle** and caches what a handle crosses as. `get_class()`
  cannot change for a live object; a *script* can, so a script-keyed row needs invalidating on
  `set_script`.
- **The data-source package is snapshotted once.** `FSolarisIde::EnsureDataSourcePackageExists`
  snapshots the project's other packages as the script package's dependencies exactly once, so a
  package added after the first script is never depended on. Regenerating the bindings package
  mid-session has to replace it without losing that edge.
- **Only the generation's own package may be forced back to Source after a build.** The assembler
  publishes every Source package the program carries, and publishing one twice asserts inside
  `AsyncLoading2.cpp` rather than reporting anything. The bindings package goes External and is
  reached through its digest, which is what the volume decision wanted anyway.
- **`BindEngineSignal` resolves a payload only for classes under `GodotVersePath`**
  (`HostScript.cpp:6572`), and `VhSignalBind` carries no `<public>`, so a non-mirror package cannot
  reach it at all. Typed signals need both taught about the bindings package.
- **A script method list includes base classes.** `GDScript::get_script_method_list` calls
  `_get_script_method_list(r_list, true)` (`gdscript.cpp:316`) with no own-only flag, so the
  generator must diff against `get_base_script()` or re-declare inherited members and trip Verse's
  shadow rule.
- **The property list carries a category row** under `TOOLS_ENABLED` (`gdscript.cpp:342`). Filter on
  `PROPERTY_USAGE_SCRIPT_VARIABLE`.
- **An untyped GDScript declaration is `NIL` with `PROPERTY_USAGE_NIL_IS_VARIANT`**
  (`gdscript_parser.cpp:5417`) — the same convention `verse_value.cpp` pairs with
  `VH_TYPE_VARIANT`, so it maps straight to Verse `variant`. A script class with no `class_name`
  reports its native base instead of itself.
- **A member may not shadow an inherited mirrored one, signals included.** `verse_api::methods` in
  `src/verse_api_classes.h` is keyed by declaring class, which is exactly this check.
- **A script that does not compile reports no members.** Keep the previous binding and say so;
  emitting an empty class silently deletes an API.
- **Deleting `.godot/` deletes the bindings.** Godot regenerates that directory routinely, so
  generation has to run on project open, not only on a roster change.

## 7. Spikes to run first

Each is a `tests/verse_probe` fixture or a one-file experiment, and the repo's own rule applies: ask
the compiler, do not read `SemanticAnalyzer.cpp`.

1. Does a project class survive `ObjectForHandle`'s adoption path — build a handle as a
   `/user@localhost` class and call a method on it?
2. Is `<final>` available in a user package at all? Nothing in `AvailableAttributeUtils.cpp`
   restricts it, but that is an absence of evidence.
3. Does a separate data-source package digest and resolve the way the mirror's does, including
   after being replaced mid-session?
4. What does `--stage-only` alone do to a pure-Verse class added to the mirror? It decides whether
   §1's reasoning about the C++ tables is the whole story.
5. Cost: generation time and the analysis delta for a project with a real addon installed, measured
   the way `phase-2-design.md` §3.1 measured M1 and M2.

## 8. Testing

- **units** — the C++ classifier and emitter against hand-built descriptions, plus the differential
  test that reproduces `GodotClasses.native.verse` for mirrored classes. No Godot, no UE.
- **integration** — a `.gd` fixture with a `class_name`, a typed method, a property and a signal; a
  Verse case calls through the binding and asserts the result, which also proves a generated file in
  a separate package participates in the build. A second fixture for a class in the GDScript
  hierarchy, to prove `boss := class(mob)` resolves `mob`'s methods.
- **export** — the same cases run in an export and prove the sidecar carries the mapping. The counts
  named in `run_tests.py` change.
- **by-hand** — completion, hover and the argument hint on a binding member, into
  `docs/by-hand-findings.md`.

## 9. Open questions

| | Question | Bearing |
| --- | --- | --- |
| **B-1** | Does the mirror shrink to a curated core now that per-project bindings exist? Decide with numbers once the generator does, the way `phase-2-design.md` §3 decided `--all`. | R-SCN-2, R-PERF-2 |
| **B-2** | Does any of the C# half work? `script_classes_updated` is language-agnostic, so C# bindings generate with no new code — and OQ-17 still says no test in this repository has ever run C#. | OQ-17 |
