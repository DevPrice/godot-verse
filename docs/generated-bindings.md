# Generated bindings for classes the mirror does not carry

The mirror is generated from `extension_api.json`, which describes core Godot and nothing else. Two
kinds of class are therefore invisible to a Verse script: a class a **third-party GDExtension**
registers in ClassDB, and a class a **script** declares with `class_name` — GDScript or C#. This
document is the design for generating Verse for both, and the record of the decisions that shaped
it.

**§10 is the section written after the spikes, and it corrects §4 twice.** Read it before
trusting the decision table: the `<reads>` row does not work as written, and the volume row has a
hard ceiling §4 did not know about. The rest of the body stands and is still the record of why
each shape was chosen.

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

## 10. What the spikes came back with

All five of §7 ran on 2026-09-17, against engine `203d764` and ABI 11.0. Two of them change §4.

Spikes 1 and 3 needed a throwaway host patch, and both were reverted; each is described where its
reading is, in §10.1 and §10.3, closely enough to write again. Spike 2 needed none, so its fixtures
are kept -- `tests/verse_probe/final_probe.verse` and `final_reject.verse`.

### 10.1 A project class survives the adoption path (spike 1)

Yes, and nothing new was needed underneath it. A `/user@localhost` class named as the answer to
`GetClassOf` crossed in as itself, downcast, and ran a method:

```
spike_binding<public> := class(node):
    Hit<public>(Power:int)<transacts>:int = Power * 2

Probe<public>(N:node)<transacts>:int = if (B := spike_binding[N]) then B.Hit(21) else -1
```

`Probe` answered **42** with `--class-of spike_binding`, and **-1** with both controls —
`--class-of Timer`, where the mirror's own row wins, and `--class-of NoSuchClass`, where nothing
resolves. So `NewMirroredWrapper` takes a project `UClass` unmodified: an `FAdoptPeerScope` and a
`NewObject<UObject>` are the whole of it, and §3's "the construction path underneath needs nothing
new" is confirmed rather than assumed.

The patch was three lines — `MirroredClassForHandle` falling through to `FindGodotClass` when the
mirror has no row. The real fourth question is keyed differently (§3), but it builds on exactly this.

### 10.2 `<final>` is available, and glitch 3569 is its number (spike 2)

`class<final>(object)` compiles in a `/user@localhost` package, and the subclass is refused:

```
error 3569: Class `final_derived` cannot be a subclass of the class `final_base` which has the
`final` attribute.
```

§4 guessed at `ErrSemantic_FinalSuperclass` and had no number. The decision not to use it stands and
is now grounded rather than inferred: the refusal fires at the *subclass's* declaration, so a
generated `boss := class(mob)` would report the error against a generated file the author cannot
edit — which is worse than the bridge's own refusal in §5, not better.

### 10.3 A separate package digests, and survives being replaced (spike 3)

Both halves, decisively.

A second source package added at runtime went **External with a digest at the first build**, beside
the attribute package that already does this:

```
package GodotAttributes   verse=/Godot.org/Godot     role=External digest=  2248 B snippets=1
package GodotBindings_1   verse=/Godot.org/Bindings  role=External digest=   159 B snippets=1
package GodotScripts_1    verse=/user@localhost      role=Source   digest=    29 B snippets=1
```

Replacement mid-session works when the package is **generational the way the script package is** — a
name no publish has used, with the retiring one removed from the source project first. That is not a
nicety: §6 already records that publishing one Source package name twice asserts inside
`AsyncLoading2.cpp` rather than reporting anything, and a fresh name per roster change is what
avoids it. Two builds in one process, with the bindings source swapped between them, and a script
that names only what the *second* roster carries:

```
[probe] ...:6:51: error 3506: Unknown member `Damp` in `rapier_body`.
[probe] ...:7:38: error 3506: Unknown identifier `rapier_joint`.
[probe] compile 1: status 4, generation 0, 2 error(s)
[probe] compile 2: status 0, generation 1, 0 error(s)
[probe] call AskDamp: status 0, int 42
[probe] call AskSlack: status 0, int 5
```

Build 1 refuses both names; build 2 resolves and runs both. `GodotBindings_1` (159 B) is replaced by
`GodotBindings_2` (306 B) with nothing asserting in between.

Two things the patch had to get right, and both are load-bearing for the real thing. The bindings
package's **dependencies must exclude the script package's own generations** — the script package
depends on this one, so the edge runs one way only. And the bindings package must exist **before**
the first `PrepareGenerationPackage`, because `AddScriptPackage` takes the script package's
dependency list fresh from whatever else the project holds at that moment.

### 10.4 `--stage-only` is more than §1 claimed (spike 4)

§1 argued that the relink is for the generated C++ tables and not for the Verse. That is right, and
understated: a **whole new `.verse` file** dropped into the mirror package and staged with
`build_host.py --stage-only` — no UBT run, no VNI, no manifest edit — was compiled, resolved and
called by the runtime compiler. So was a class in it deriving from a mirrored class:

```
spike_mob<public> := class(timer):
    Hit<public>(Power:int)<transacts>:int = Power * 2
```

`GodotPeerClassFor` walked past it to `Timer` and minted the right peer, because the walk takes the
nearest ancestor with a row in the generated name table and a staged class has none.

What this does *not* do is make the mirror the right home. It sharpens §1's real argument instead:
the obstacle was never the build, it is that the mirror is **one package in the engine tree, shared
by every project on the machine**. That reasoning now carries the whole decision on its own.

### 10.5 The cost is linear, and there is a ceiling §4 did not know about (spike 5)

Measured with synthetic binding classes shaped the way §2 shows — a subclass of a mirrored class
with five properties, an enum, a declared signal and eight methods — compiled as one Source package.
Absolute numbers are a cold one-shot process and are pessimistic against R-PERF-2's steady state;
the **deltas** are the reading.

| classes | parse | semantic | analysis | Δ over 0 | per class | generation |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 346.0 | 1005.8 | 1351.8 | — | — | 2332.9 |
| 10 | 348.1 | 1016.4 | 1364.5 | +12.7 | 1.27 | 2361.5 |
| 50 | 355.1 | 1040.0 | 1395.1 | +43.3 | 0.87 | 2407.2 |
| 200 | 369.1 | 1175.6 | 1544.7 | +192.9 | 0.96 | 2678.2 |
| 500 | 399.6 | 1393.1 | 1792.7 | +440.9 | 0.88 | 3193.5 |
| 1000 | 454.5 | 1763.3 | 2217.8 | +866.0 | 0.87 | 4074.9 |

All in ms. **A binding class costs ~0.87 ms of analysis, linearly**, which is about what a mirrored
class costs — `phase-2-design.md` §3.1 measured the mirror at 2.6 ms per class over 60 and 0.83 ms
over 1022. §4's volume decision is therefore justified by the numbers rather than by analogy: carried
as Source, 200 binding classes add ~193 ms to *every* per-keystroke analysis against R-PERF-2's
520 ms whole-project figure, and 1000 add ~866 ms. The digest is what makes "bind everything"
affordable, exactly as §4 said.

**The ceiling is the finding §4 did not have.** At 600 classes the build failed with a diagnosed
runtime error, and at 1000 with a fatal one:

```
ErrRuntime_MemoryLimitExceeded: Exceeded memory limit(s). (Ran out of memory for allocating
`UObject`s while attempting to construct a Verse object of type event!)

Maximum number of UObjects (131072) exceeded when trying to add 1 object(s), make sure you update
MaxObjectsInGame/MaxObjectsInEditor/MaxObjectsInProgram in project settings.
```

Every Verse class is a `UVerseClass` with a `UFunction` per method, and the mirror's own 1036 are
already in that pool. Declared signals are not the driver — 1000 classes without one fail the same
way — they are only what happens to allocate last, because a `signal(t)` member constructs an `event`
UObject during module evaluation.

It is raisable, and raising it works. `UObjectBase.cpp:1274` reads
`[/Script/Engine.GarbageCollectionSettings] gc.MaxObjectsInProgram` from `GEngineIni`, defaulting a
Program to 100K. Adding
`-ini:Engine:[/Script/Engine.GarbageCollectionSettings]:gc.MaxObjectsInProgram=500000` to the
`GEngineLoop.PreInit` line in `VerseHost.cpp` took 1000 binding classes from fatal to a clean build.
So the ceiling is a **decision to take deliberately**, in the host's own PreInit, rather than a wall
— but a project that installs a large addon hits it at a few hundred classes if nobody takes it, and
the failure mode at the fatal end names UE's setting and nothing about Verse or this bridge.

### 10.6 The one that changes §4: a binding cannot spell `<reads>`

§4 says the classification rules are `gen_verse_api.py`'s, exactly, including *"`<reads>` where Godot
says `is_const` and the method answers something"*. **A binding in a package of its own cannot spell
that**, and the refusal comes four ways at once:

```
error 3512: This invocation calls a function (`(/Godot.org/Godot/object:)Call`) that has the
'transacts' effect, which is not allowed by its context.
error 3593: Invalid access of internal function `(/Godot.org/Godot:)VhToInt` ...
error 3593: Invalid access of internal function `(/Godot.org/Godot:)VhCallValueConst` ...
error 3593: Invalid access of internal data `(/Godot.org/Godot/vh_object:)Handle` ...
```

Every `object.Call` and `Callv` overload is `<transacts>`; there is no `<reads>` call verb anywhere
in the public surface. And the three things the mirror's own `<reads>` bodies use — the const-call
native, the packers, and `Handle` itself — all carry no `<public>`.

**Verse's internal access is scoped by verse path, not by package.** That is the fact that opens the
fork, and the attribute package is the existing proof of it: a *second* package at
`/Godot.org/Godot` reaches everything the mirror's own files do. Three options, all three run:

| | `<reads>` bindings | a binding name colliding with a mirrored one |
| --- | --- | --- |
| `/Godot.org/Bindings` (§4's choice) | **impossible** — the four refusals above | an ambiguity at the *use* site, spellable `(/Godot.org/Bindings:)timer` |
| `/Godot.org/Godot` (the attribute package's path) | **works** — `VhCallValueConst`, `VhToInt` and `Handle` all resolve | glitch **3532** at the generated declaration, against a file the author cannot edit, unspellable |
| `/Godot.org/Bindings` **plus one addition to the mirror** | **works** | unchanged from §4 |

The third is the recommendation, it was run end to end, and it is now **built** — `CallConst`
and `CallvConst` in `GodotApi.native.verse`, six arities plus the `godot_array` spelling,
asserted in the integration and export layers and put to the runtime compiler in
`tests/verse_probe/call_const_probe.verse`. One `<public>` extension method on `object`, which
needs no new native because `VhCallValueConst` already exists:

```
(Target:object).CallConst<public>(Method:string, Args:[]variant)<reads>:variant =
    VhToVariant(VhCallValueConst(Target.Handle, "call", array{VhFromStringName(Method)} + Args))
```

With it staged, a binding at `/Godot.org/Bindings` declaring
`Tag<public>()<reads>:int = CallConst("tag", array{}).AsInt[] or 0` compiled and ran. `Target` rather
than `Self`, which is V3514 — *"Cannot use reserved identifier `Self` as definition name"*.

It is a widening of the public surface and should be taken as one: a script could then declare
`<reads>` over a Godot call that mutates. That is no worse than what the mirror already does for its
3996 `<reads>` methods, where the honesty comes from Godot's `is_const` rather than from the
language — and for a binding it would come from the same place, the addon's own
`METHOD_FLAG_CONST`. A GDScript class has no const methods at all, so script-class bindings are
`<transacts>` throughout and never reach this. It also closes a gap a script has today: R-INT-2's
`Callv` escape hatch has no `<reads>` spelling, so reading a third-party property from a `<reads>`
function is wall 8's cascade for no reason.

### 10.7 Six more traps, for §6

- **A method parameter may not share a name with an inherited mirrored property.**
  `Apply(Power:int, Scale:float)` on a `class(node2d)` is glitch 3532 — *"The data
  `(/user@localhost/bind_class_0/Apply:)Scale` ... is ambiguous with ... `(/Godot.org/Godot/node2d:)Scale`"*.
  §6 has the member case and CLAUDE.md has the module-level case; this is a third, and the generator
  meets it on every method it emits, because an addon's parameter names are not ours to choose. The
  filter is the whole inherited property and signal set of the binding's base, which
  `verse_api::methods` already carries by declaring class.
- **The readers are `AsBool`, not `AsLogic`.** Godot's `bool` is 568 predicates spelled
  `<decides>:void` and 306 `logic`-answering methods, but the *variant* reader is one function and it
  is `AsBool`. The doc's own opening line says `AsLogic[]`, which does not exist.
- **A class with no members is spelled `class(base) {}`, and a binding with no members is ordinary.**
  It is the *indented* form that needs a member — `class(node2d):` followed by nothing is a parse
  error — and the brace form has no such requirement. The emitter first read that as "Verse has no
  empty class body" and wrote a `Bound<public>()<reads>:logic = true` filler instead, which is a
  member of the author's own class that is in the binding and not in their `.gd`. The case is not
  rare: a GDScript declaring only Godot virtuals and `@export` variables has nothing else to carry,
  because a leading underscore is skipped (§6) and properties are R-INT-9's remainder.
  `tests/verse_probe/empty_class_probe.verse` is the measurement, and it checks the three things a
  binding has to do — declare, serve as a parameter type, and serve as a downcast target.
- **A binding is in no map the analysis snapshot carried, and half the editor asks that map
  first.** `vh_class_members` reads `GSnapshot->Classes`, which `TakeAnalysisSnapshot` fills by
  walking the **script package alone** — so every answer the editor takes from the snapshot
  rather than from a resolved position knew nothing about a binding, and `main_script{}.` opened
  an empty completion popup that filled only when the analysis for that keystroke landed
  (`by-hand-findings.md` B33). Bindings are harvested now, into a map of their own: beside
  `Classes` rather than in it, because every other question that map answers — abstractness,
  exports, whether the published generation carries the class — is about a class the project
  declares, and because a binding sharing a name with a script class would otherwise take its
  place there.
- **An object is a type a binding has to be able to name, and naming one wrong refuses the whole
  package.** `verse_type_for` could type an object only as a class the *mirror* carries, which
  cost a method naming a script class its place in the binding and cost a method answering a
  mirrored class rather more: it was emitted with a declared result and a `variant` body, and a
  package that does not compile is every binding in the project (`by-hand-findings.md` B35). Two
  things make it work. The roster is collected in full before any member is typed — Verse
  resolves module-scope definitions in any order, so the dependency order the old comment said
  was needed is not — and an object result is `<decides>`, over `AsObject[]` and a downcast,
  because a class has no value standing for “Godot answered nothing”.
- **And an object *argument* is optional, because nothing here can say otherwise.** The mirror
  reads `"meta": "required"` off the API dump and declares the 112 arguments Godot marks as the
  class itself; a binding has no such metadata in either source — GDScript carries no
  nullability annotation and `ClassDB.class_get_method_list` carries none of the dump's — and
  Godot accepts null for both, so every object argument of a binding is declared `?class`
  (`by-hand-findings.md` B37). They pack through `VariantMaybeObject`, which is public for the
  reason `CallConst` is: a bindings package sits at `/Godot.org/Bindings` and reaches nothing
  internal to `/Godot.org/Godot`.

### 10.8 What §9 should now say

**B-1 has its first number.** A binding class costs what a mirrored class costs, so "curated mirror
plus generated bindings" trades ~0.87 ms of per-keystroke analysis per class for ~0.87 ms per class —
nothing, until the digest is in it, and then everything, because the mirror is digested and a Source
bindings package is not. The question is really *how early the bindings package gets its digest*, and
§10.3 says it gets one at the first build.

**The ceiling is a question of its own**, and it went to `spec.md` §14 as **OQ-19** rather than into
§9: it is not about bindings, it is about what a Program pre-sizes its UObject pool to, and generated
bindings are only what makes the answer urgent. §10.5 shows the wall is real at a few hundred binding
classes and that one flag on `PreInit` removes it. The number is not a detail — the pool is
pre-sized, so it is memory spent whether or not a project has an addon.

### 10.9 What the last five members corrected — one of them a §4 decision

§4 says the classification rules are `gen_verse_api.py`'s, exactly, *"properties as writable members
except where a nested struct or a container forces a getter/setter pair"*. **A property here cannot
be a member at all, and the reason is the package rather than the property.** The compiler says it in
as many words:

```
error: Data members with `<getter(...)>` and `<setter(...)>` must be either uninitialized
       or initialized with `= external{}`.
error: external{} macro must not be used in regular Verse code. It is a placeholder allowed
       only in digests.
```

So a source package has exactly one of the two spellings available, and it costs the class its
archetype: an uninitialized member must be supplied by every archetype, so `mob{}` — which R-INT-12
needs and `tests/integration` asserts — becomes *"Object archetype must initialize data member
`Speed`"*. The mirror escapes this because VNI compiles it, where `external{}` is legal.

What follows is better than a workaround. **A ClassDB property needs nothing:** it is *defined* by a
getter and a setter method (`ADD_PROPERTY` names both), and those are in the class's method list, so
a binding already answers `GetProcessCallback()` and `SetProcessCallback()`. Only a GDScript `var`
has no pair, and it is given the one Godot would have given it — `GetSpeed()`, `SetSpeed(V)`. That
spelling also carries a `string` var, which the member spelling could not have: `string` is `[]char`,
and a container-typed member is asked for `TagGetter(:accessor, :int):char`, the indexed overloads
the mirror skips 403 properties rather than write. The names are invented, which is done almost
nowhere else in this project; the alternative was to bind no GDScript `var` at all.

Four more, each measured rather than reasoned about (headless Godot 4.7):

- **A GDScript static is reachable, and not through ClassDB.** `script.call("make", 4)` answers
  `12` on the script *resource*, which is what `Thing.make()` does underneath. A script class has no
  ClassDB entry, so `ClassDB.class_call_static` — what a bound GDExtension class uses — cannot reach
  one. The generated body loads the script and calls through it, which is the only spelling of the
  five that needs a failable guard before the call.
- **Godot's enum metadata is the same for both kinds of class.** An enum-typed argument, result or
  property is an `int` whose `class_name` is `Thing.State` with `PROPERTY_USAGE_CLASS_IS_ENUM`
  (65536) in its usage. A GDScript `enum State { IDLE, BUSY }` arrives in the constant map as a
  *Dictionary* value, `{"IDLE": 0, "BUSY": 1}`, which is what tells an enum from a constant.
- **A ClassDB enum property carries no class name.** `Timer.process_callback` reports an `int` with
  an empty `class_name` and `"Physics,Idle"` as a hint string, so it is typed `int` and nothing is
  lost: the enum itself is still bound and `ToInt` still spells a value for it.
- **A predicate belongs to ClassDB classes alone.** The rule reads a test out of Godot's own naming,
  which Godot chose for its own C++ API. A GDScript author writing `func is_alive() -> bool` has
  made no such claim, and `<decides>` would change how every caller spells the call. `PREDICATE_EXTRA`
  — the 26 predicates whose Godot name carries no prefix — is not ported, because every entry names a
  *mirrored* class and a mirrored class is never bound.

**And the member-shadow rule is not optional once properties exist.** `Mob extends RigidBody2D`
declaring `var mass` is the ordinary case, not an exotic one, and a member that shadows an inherited
mirrored one is glitch 3532 against a line the author cannot edit — which refuses the package, and
with it every binding in the project (B35). Every method, property and signal is checked against the
whole mirrored ancestry (`verse_api::methods`, keyed by declaring class) before it is emitted, and a
collision drops that member rather than the package.

**The differential test §4 promised is not built.** It would run the C++ classifier over the mirrored
classes and assert it reproduces `GodotClasses.native.verse`, and the C++ side cannot read
`extension_api.json`: there is no JSON parser on that side of the repository. What stands in for it
is the integration layer, where the whole generated package is compiled by the real Verse compiler on
every run — a wrong classification is a build failure, not a silent drift — plus the assertions that
call through each of the five member kinds. The gap is that the *mirror's* rules and the bindings'
can still drift apart without a test saying so.

## 11. What the editor says about a binding

Completion worked from the first generation. Hover, ctrl+click and syntax highlighting answered
nothing at all, and one fact runs under all three: **a binding's Verse declaration is not a file.**
The package is a synthetic snippet, read back from its digest in the engine tree after the first
build (§10.3), so what a lookup answers for `mob` is a path no editor can open and a comment nobody
wrote. Measured with `tools/probe_hover.py` against `tests/integration`:

| hovered | the host answers | the editor drew |
| --- | --- | --- |
| `mob` | class, at `Digests/GodotBindings_1/GodotBindings_1.digest.verse` | nothing — no class to name, no prose to read and no location to jump with, which `hide_if_empty` turns into `ERR_UNAVAILABLE`, so there was no ctrl-hover underline either |
| `Hit` | function, owner `mob`, same path | "Local Constant Hit: `type{_(:int)<transacts>:int}`", and nowhere to click |
| `node2d` beside them | class, at `GodotClasses.native.verse` | Godot's class documentation |

**Completion was never in this**, which is worth saying because it looks like the same question: at
`M.` the popup carries `Hit(…)`, `Label()` and `Heavy()` among the 387 the base contributes, because
it resolves against the semantic program rather than against a table on this side. Everything that
*did* fail resolved correctly too, and then had nothing on the consumer's side to turn into an
answer.

So the fix is not to make the generated Verse reachable. It is to stop routing through it: what the
author wrote is either the GDScript the binding stands for or nothing at all, and each of those has
a page.

- **A script binding answers its own script.** `LOOKUP_RESULT_CLASS`, `class_name` the global name
  (`Mob`), the `.gd` and `location` 0 — which is GDScript's own answer for a global class name,
  `gdscript_editor.cpp`'s `ScriptServer::is_global_class` arm. Both halves are needed: the help
  viewer is skipped for a **script** doc (`script_text_editor.cpp` tests `is_script_doc`), so the
  click falls through to the location, and the tooltip still comes from the class doc Godot
  generates out of the script's `##` comments. The script is named **twice**, as `script` and as
  `script_path`, because Godot renamed that field between 4.7 and 4.8 and a result carrying one of
  them is, in the other editor, a location with no script beside it — which means *a line in the
  file being edited*. B29 is what that looked like.
- **A GDExtension binding answers the Godot class**, with no location, so the click opens the
  documentation the way `node2d` already does. There is no source under `res://` for it to open.
- **A member answers under its *Godot* name**: `Hit` documents nothing, `hit` is what GDScript
  declared. A signal is a `signal(t)` data member, so the Verse kind cannot tell a method from a
  signal and the roster does.

That needs the roster kept on the consumer's side, which `refresh_bindings` was throwing away —
`VerseScriptLanguage::BindingInfo`, one row per binding with the Godot class, the script's global
name and path, and the Verse-to-Godot member map. R-INT-11's sidecar wants the same table in an
exported game.

**The colours are the third surface and they are a table, not a lookup.** The highlighter's
`type_names` was four generated tables plus the project's own Verse classes, and a binding is in
none of them, so it drew as plain text — which is how a name the editor does not know reads. Filling
it in was the moment to split the one colour into the three the editor theme carries, the way
GDScript does (`gdscript_highlighter.cpp:776-818`):

| colour | Verse names |
| --- | --- |
| engine type | the mirrored classes ClassDB knows, the 793 mirrored enums, a binding for a **GDExtension** class |
| user type | the project's own Verse classes, an enum declared in the open file, a binding for a script's **`class_name`** |
| base type | the 16 math types and `rid`, the exported types (`variant`, the containers, `callable`, the signal types), and Verse's own |

`verse_api::classes` needs one test to split it, because it carries the math types and `rid` beside
the 1036 engine classes and ClassDB has heard of none of those — which is B28's test exactly, and is
the same line GDScript draws between a Variant type and an engine class. `int`, `float`, `string`,
`logic` and `void` stay in the keyword colour, because Verse's compiler reserves them and Godot's
does not.

**The GDExtension arm of all three is unmeasured here**, and will stay that way until a fixture
project carries a real GDExtension: `tests/integration` has only script bindings, and
`tests/host_smoke`'s `rapier_body` roster is the host's side of the wire, where no editor is
involved.
