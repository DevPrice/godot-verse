# Phase 4 — parity: signals, virtuals, and the rest of Godot's model

**Status:** Draft 1 · 2026-09-12 · **4a is built; 4b is not.** Read
[`phase-4-gaps.md`](phase-4-gaps.md) before trusting §6 or §8: it is the record of where the
implementation and this document disagree, and several things below read as delivered and are not.
§13 summarises it. Everything else here is the design as it was written, kept so the reasoning can
be checked against what happened.

**Originally:** designed, not built. The three spikes §2 names were run
*before* this document was written rather than during the phase, so what is below rests on compiler
output and a host build rather than on reading. Two of the three moved the design, and one of them
retired an idea this document would otherwise have been built around.
**Companion to:** [`roadmap.md`](roadmap.md) §"Phase 4", [`spec.md`](spec.md) §5.3–§5.5 and §8,
[`dodge-the-creeps.md`](dodge-the-creeps.md) — whose eight walls are this phase's scope and whose
closure is its gate.

The phase splits in two. **4a** is everything scene code touches every day: the failable cast,
container creation, a Callable made from a Verse function, signals, the full virtual set, Godot's
constants and statics, and the value-type methods. **4b** is the editor's data model — instantiation
without a node, custom Resources, autoloads, icons, RPC config, and the rest of `@export`. 4a's gate
is a yardstick measurement; 4b's is a checklist.

---

## 0. Where the decisions came from

Settled by interview across nine rounds before any design, so the reasoning is not reconstructed
later. Three of them were settled by *prior art* rather than by preference, and one was overturned
by a spike — both are marked.

| decision | answer | consequence |
| --- | --- | --- |
| the exit gate | **re-port Dodge the Creeps idiomatically** | "no GDScript in it" is already true; the honest gate is that the eight walls are gone. Measured as a diff against today's port |
| the re-port | **rewrite in place, wall by wall** | one yardstick, one maintenance burden, and each stage ends with a wall visibly falling |
| phase shape | **4a / 4b** | 4a is what the yardstick measures; 4b is the editor's data model, which the yardstick never touches |
| order against Phase 5 | **4b first, as the roadmap has it** | parity before concurrency; R-ASYNC-4 has been survivable since Phase 3 |
| first stage | **the cast (R-SCN-6)** | measured as the most expensive absence; everything written after it is written differently |
| virtual set | **generate every one** | consistency over analysis latency, whose real fix is Phase 7's cooked route |
| Callable from a Verse function | **full, in 4a** | R-SIG-3 and R-INT-4 close together, and signals' `Subscribe` needs it |
| cast identity | **script object identity, mirror wrappers fresh** | not an optimisation: without it `player[GetNode(…)]` can never succeed |
| signal declaration | **a typed member, Verse's own shape** | *prior art* — Epic writes `DamagedEvent:listenable(float)` as a class member. Revised by spike S-B: shaped like `listenable`, not implementing it |
| signal payload | **one payload, tuples above arity 1** | *spike* — a multi-parameter Verse function satisfies a tuple-parameter callback, so Godot still sees N arguments and a handler still reads them as N parameters |
| signal name in ClassDB | **the Verse spelling, verbatim** | *prior art* — Godot C# registers `Hit`, not `hit`, and our scene connections already reach `OnMobTimerTimeout` |
| engine signals | **generate typed accessors** | *prior art* — C# generates an `event` per engine signal plus `SignalName` constants. 489 signals across 149 classes |
| signal list source | **the built class only** | one source of truth; a brand-new script shows its signals after a Build, as an `@export` default already does |
| math methods | **pure Verse** | *prior art* — C# reimplements `Vector2.Length()` in managed code and dispatches only the engine utilities. §8.3 bounds what that costs |
| `@GlobalScope` utilities | **Verse wins, except random** | the random family is dispatched so `seed()`/`randomize()` steer one stream, which is what C# does; everything else keeps R-AUD-2 |
| constants and statics | **one suffixed module per class** | `NodeStatics.NotificationReady`, `Vector2Statics.Zero`. *Overturned mid-interview*: unsuffixed `Node`/`Tree` modules would break `Tree := GetTree[]`, which the yardstick writes today |
| a script's own statics | **a module with a declared association** | `@statics(player)` on the module, so the class↔module link is checkable rather than a naming convention that fails silently |
| export hints | **type-driven first, attributes for the rest** | keeps the existing rule that a range comes from the type's own `where` clause, so the slider and the type cannot disagree |
| `@tool`'s editor virtuals | **4a, riding the mechanism** | if R-NODE-7 is general they cost a test, and R-EXP-5 stops being `part` |
| signal payload | **tuple, or a struct when you want names** | Verse tuples cannot name their elements, and Godot's signal list and `_make_function` both want names. A tuple keeps the N-parameter handler S-B proved; a struct maps top-level fields to named arguments (§6.2) |
| emission timing | **immediate, a stated exception** | every other void mutation defers to commit; emission does not, so "emit then read what the handler changed" works and a Verse emission matches a GDScript one. It joins the audited set Phase 4.5 owns (§6.4) |
| `Subscribe` and rollback | **compensated with `Stm::OnRollback`** — *wrong, and Phase 4.5 §11.1 says why: that API is a no-op here. It is `AutoRTFM::OnAbort<SameAsClosed>` now* | it mutates Godot and returns a value, so it can be neither deferred nor ignored. Epic's own event does the same in reverse. The host's first compensation |
| connect semantics | **reference equality, duplicates allowed, idempotent `Cancel`, no flags** | *prior art, twice*: Godot's lambda callables compare by reference and UEFN's event inserts one entry per subscribe with an idempotent cancel (§5, §6.3) |
| callback binding | **a bound method only, in 4a** | Godot's self-capturing lambda dies with its object; its plain lambda is anchored to the script and is Godot's own known leak. We take the half that does not leak; unbound functions are **OQ-16** |
| foreign-thread calls | **refused, in this phase (R-ASYNC-8)** | VerseVM asserts `IsInGameThread()` and then carries on, so today a worker-thread call is a logged callstack followed by undefined behaviour. A mutex cannot fix thread *identity*; the guard belongs where the exposure grows (§5) |
| effect-semantics review | **its own phase, 4.5** | asked for as a phase rather than an open-question row, and inserted without renumbering so the phase records written earlier stay true |
| virtual names | **Godot's spelling, `_Ready`** | measured, not preferred: plain names collide with a *signal* on Node, CanvasItem, Control and BaseButton, and with a method 834 times. The underscore Godot already uses is the disambiguation, and C# keeps it too (§7.1) |
| the rename it causes | **all at once, in stage 5** | `Ready` → `_Ready` across demo, tests and the yardstick in one commit; pre-1.0, no deprecation window |
| script-level hooks | **`_Notification` only** | it is not in `extension_api.json`, so it is hand-declared rather than generated. `_ToString`, `_Get`, `_Set`, `_GetPropertyList`, `_ValidateProperty` become **R-NODE-10** and land in 4b (§7.3) |
| the latency number | **no threshold** | feature parity first, performance goals later. It is recorded when stage 5 lands and acted on when the editor feels slow, not when a number is crossed |
| statics and abstract | **4a** | Godot's own 114 statics are a visible hole in a phase about parity |
| the `<transacts>` trap | **deferred** | not this phase; transaction semantics get a review of their own later (**OQ-15**) |
| ABI | **v4.0, break freely** | pre-1.0, both DLLs rebuild together, and the mismatch is caught at `vh_init` |
| editor-side testing | **a written checklist now** | automation only if a flow regresses twice; the checklist becomes the manual's raw material |
| spikes | **three, before this document** | §2 |

---

## 1. What the prior art offers — read before designing around it

### 1.1 Godot C#, which has solved this problem once already

Read out of `modules/mono/editor/Godot.NET.Sdk/Godot.SourceGenerators/ScriptSignalsGenerator.cs`,
`bindings_generator.cpp`, and `GodotSharp/Core/`.

- **A signal is declared as an attribute on a nested delegate**: `[Signal] delegate void
  HitEventHandler(int damage)`. The generator strips `EventHandler` and registers the signal as
  **`Hit` — PascalCase, unchanged**. No snake_casing anywhere: a C# method is `OnPressed` in ClassDB
  too. `GetGodotSignalList()` is what Godot reads.
- **Emission is a generated typed method**, `EmitSignalHit(int damage)`, whose body is
  `EmitSignal(SignalName.Hit, [damage])`. C# buys its checked emission spelling with a source
  generator, which is a tool we do not have and, per §6, do not need.
- **Engine signals are generated too**: `public event Action Pressed { add => Connect(SignalName.Pressed, Callable.From(value)); remove => Disconnect(...) }`,
  one per signal per class, plus a `SignalName` constant. That is the precedent for §6.4.
- **The math types are reimplemented in the language.** `Vector2.Length()` is
  `Mathf.Sqrt((X * X) + (Y * Y))` in C#; `Mathf.cs` redefines the constants "with Decimal precision
  and cast down". No engine round trip for any of it.
- **The engine utilities are dispatched.** `GD.BytesToVar`, `GD.Hash`, `GD.Print` and — worth
  noticing — `GD.Randf` all call `NativeFuncs.godotsharp_*`, so the engine's RNG is one stream.

One fact reframes "exact parity" for §8: Verse's `float` is 64-bit and Godot's `real_t` is 32-bit in
a standard build, so a `vector2`'s components are truncated on every crossing whatever we do.
Dispatch does not buy bit-exactness. It buys *edge-case semantics*, which is a much narrower prize.

### 1.2 Verse's own event vocabulary, and why we cannot implement it

`/Verse.org/Verse` and `/Verse.org/Concurrency` already have the shape a Godot signal has:

```
signalable(payload)  := interface:  Signal(Val:payload):void
subscribable(t)      := interface:  Subscribe(Callback(:t):void)<transacts>:cancelable
awaitable(payload)   := interface:  Await()<suspends>:payload
listenable(payload)  := interface(awaitable(payload), subscribable(payload))
```

Epic's own engine code writes one as a class member — `DamagedEvent<public><native><final>:listenable(float)`
— so a signal as a typed data member is Verse's normal spelling rather than an invention.

**But two of the four cannot be implemented by us, and the compiler says so** (spike S-B):

- `signalable.Signal` is `no_rollback`. Every Godot callback runs inside a transaction, so a
  `Signal` inherited from that interface is uncallable from precisely the place a script emits from.
- `subscribable.Subscribe` fixes its callback type at `(:t)->void`, which is a **no_rollback
  callback** — a handler that could not itself touch Godot. Declaring the callback `<transacts>`
  produces a *different domain*, which Verse reads as an ambiguous overload rather than an override:
  "Function `(/user@localhost/probe_signal:)Subscribe` must have a distinct domain from these other
  functions with the same name."

`awaitable` survives — its `Await()<suspends>:payload` is exactly what Phase 5 will want — and is the
one of the four worth implementing when concurrency lands.

So the design keeps Verse's **vocabulary** (`Signal`, `Subscribe`, `cancelable`, `Await`) and drops
its **interfaces**. R-AUD-2 is satisfied by the spelling an author writes, which is what it is about.

### 1.3 Verse facts found by compiling, not by reading

All from the spike driver in §2, each one either load-bearing below or a correction to something
this repository already believed.

- **Type-based extension methods exist**: `(V:vector2).Length<public>()<reads>:float`, and they work
  on the *mirror's* structs from project code. Epic's `SpatialMath/Vector2.native.verse` is the
  worked example.
- **Operators are definable**: `operator'+'(Left:vector2, Right:vector2)<computes>:vector2`,
  `operator'*'(:vector2, :float)`, `prefix'-'`. **OQ-11's open half is answered: yes.**
- **The mirror's math structs are not `<computes>`-constructible.** `vector2{X := …, Y := …}` inside a
  `<computes>` function is refused — "This archetype instantiation constructs a class that has the
  'transacts' effect". Epic's own vector2 is `struct<concrete><computes><persistable>`; ours is not,
  and §8.3 has to fix that in the generator before any of the math can be `<computes>`.
- **Inline modules exist and need no `using`**: `NamedColors<public> := module:` with members reached
  as `NamedColors.White`. This is what §8.1's class modules are made of.
- **Verse rejects a definition that resolves ambiguously against anything visible**
  (`SemanticAnalyzer.cpp`, `RequireUnambiguousDefinition`), and the external-package carve-out
  exempts definitions *in* the external package, not a user's local that collides with one. So a
  module named `Tree` would break `if (Tree := GetTree[])`, which `main.verse` writes today. Hence
  the `…Statics` suffix.
- **A module-scoped `var` must be a `weak_map`** — there is no global mutable state in Verse, which
  matters for anything tempted to keep a registry in project code.
- **An explicit effect specifier replaces the default set, and the default set is wider than
  `<transacts>`** — it contains `no_rollback`. A function with no specifier may call anything; one
  that says `<transacts>` may not call a no-specifier function. That is why `Ready<override>():void =`
  works in every script in the repo while `CallIt<public>()<transacts>:int = NoSpecifierMethod()` is
  refused: the caller's narrowing is what fails, not the callee's declaration.
- **What forbids `no_rollback` is failure, not the host's transaction** — and this corrects
  `dodge-the-creeps.md` wall 8, which recorded the cause as "every Godot callback runs inside a
  transaction". It does, but AutoRTFM is a runtime arrangement the Verse effect checker cannot see:
  a `Ready` override calling a specifier-less module helper compiles. What refuses one is a **failure
  context** — the condition of `if (X := F[])`, an option unwrap `Slot?`, a failable index — because
  failure has to unwind, so what it invokes must be rollbackable. `<decides>` alone does not make a
  function rollbackable (it replaces the default set, which contained `no_rollback`), so a failable
  helper needs `<decides><transacts>` — and *that* narrowing is what then refuses its own call to a
  specifier-less helper. One failable function pulls `<transacts>` onto everything beneath it. All
  four steps were re-run against the compiler rather than reasoned about; the probe files are the
  shape of `tests/verse_probe/example.verse`.
- **This is why the mirror's methods must keep `<transacts>`.** Every cast R-SCN-6 adds is a failure
  context, so a generated method that carried the default set instead would be uncallable inside the
  one construct the phase exists to introduce.

### 1.4 The surface, counted

From `godot-cpp/gdextension/extension_api.json`, which is what the generator reads:

| | count | where it lands |
| --- | --- | --- |
| virtual methods | **1413**, declared by 106 classes | §7, generated into the mirror as `_Ready`-style names |
| …of those, names that collide with a member or signal on the same class | **834** + **8** | §7.1 — the reason the underscore stays |
| signals | **489**, across 149 classes | §6.4, generated accessors |
| static methods | **114** | §8.2, by-name dispatch |
| class constants | **161** (46 of them `Node`'s `NOTIFICATION_*`) | §8.1 |
| math-type constants | **210** across the 16 math types | §8.1 |
| utility functions | **114**, of which ~28 have no `/Verse.org` counterpart | §8.2 |
| math-type methods / operators | **367** / **261** | §8.3, the largest hand-written surface in the phase |

`global_constants` in the JSON is **empty**: `PI`, `TAU`, `INF` and `NAN` are not in the extension
API at all, so the handful that matter are hand-written rather than generated (§8.1).

---

## 2. The three spikes, and what they answered

The driver is `tests/verse_probe`, built by `tools/build_verse_probe.py`: it loads the host, compiles
an arbitrary list of `.verse` files, prints every diagnostic, and instantiates a class and calls its
zero-argument methods. It was written for these three and kept, because the questions it answers
recur — `tests/verse_probe/example.verse` is S-B's fixture, so the finding below can be re-run rather
than believed. What was *not* kept is the prototype each spike put in `host/`: the phase-0 rule holds,
those were reverted, the host rebuilt clean, and `tools/run_tests.py` re-run — **7 passed, 0 failed,
0 skipped.**

### S-A — can the host bind a class-typed member at construction? **Yes, and it already does.**

The signal design needs the host to fill a member's owner and name the way it fills `Handle`. Two
production paths already do exactly this: `NewMirroredWrapper` builds a mirrored-class instance
around a handle and the export path writes it into a declared member, and `NewReferenceWrapper` does
the same for a native class carrying C++ state (`godot_ref`). Dodge the Creeps then calls
`Sprite?.Play()` on the result, so the write survives and dispatches. What is new in §6 is only
*which* members to fill and *when*: chosen by declared type, inside `Instantiate`, before the
instance seals.

### S-B — is the signal shape spellable, and does a tuple payload reach a handler as N parameters? **Yes.**

Three rounds of probes. The third compiles clean and runs:

```
godot_signal<public>(t:type) := class:
	Signal<public>(Val:t)<transacts>:void = {}
	Subscribe<public>(Callback(:t)<transacts>:void)<transacts>:cancelable = probe_cancel{}

probe3 := class(object):
	Hit<public>:godot_signal(int) = godot_signal(int){}
	Struck<public>:godot_signal(tuple(int, string)) = godot_signal(tuple(int, string)){}
	OnStruck<public>(Damage:int, By:string)<transacts>:void = {}
	WireAll<public>()<transacts>:void = { Unsub := Struck.Subscribe(OnStruck); Unsub.Cancel() }
```

**`Struck.Subscribe(OnStruck)` typechecks** — a two-parameter Verse function satisfies a
one-tuple-parameter callback, because a Verse function's parameter *is* its tuple. So the payload
question dissolves: the author writes `OnStruck(Damage:int, By:string)`, Godot sees two arguments,
and nothing in between needs a per-arity family of signal types. A zero-argument signal is
`godot_signal(tuple())`, whose handler decorates as `OnEmpty()` with no parameters at all.

What this spike *cost* was the `listenable` interfaces (§1.2). The first two rounds tried to
implement them and were refused; the third stopped trying and compiled.

### S-C — can a Verse function value reach C++ and be invoked there? **Yes, verified in our own package.**

The engine's own answer came first: `subscribable_event_intrnl::Subscribe(TVerseFunction<void(FVerseValue const&)>)`
is native, `FVerseEventCallbackList` stores each callback with the `FContentScope` it was made in,
and `Signal` invokes them inside `FDecidesContext::Transact`. VNI's macro set has
`V_MARSHAL_PARAM_FUNCTION` for exactly this.

Rather than trust the analogy, a probe was added to *our* native surface —
`VhProbeCallNow<public><native>(Callback(:int)<transacts>:void, Val:int)<transacts>:void`, implemented
as `void VhProbeCallNow(TVerseFunction<void(int64)> Callback, int64 Val)` — and both a **bound method
reference** and a **module-scope free function** marshalled and ran:

```
[spike] instantiated
method callback ran with 21
[spike] call Run -> status 0 value int 42      # the callback's `set Seen = V * 2` landed
free function ran with 5
```

The guessed C++ signature compiled first try, which is itself the answer: VNI's function-parameter
marshalling is available to any native package, not only Epic's. What remains unproven, and is
therefore §5's risk rather than its assumption, is the *Godot* half — a `CallableCustom` (which
godot-cpp does expose, `variant/callable_custom.hpp`) round-tripping through the ABI.

---

## 3. Stage 1 — the cast, and object identity (R-SCN-6)

The port pays for this absence in seventeen `@export` slots, three stringly-typed `Object.Set` calls
and an `?option` unwrap around every node lookup. Phase 2 spiked it and left it: `node2d[Value]` with
`Value:object` compiles, and instantiating a mirrored class needs no new machinery.

**The mechanism, and why identity is part of it rather than an optimisation.** `GetNode("Player")`
returns an `object`. If the host answers that with a fresh mirror wrapper, its Verse class is `node`
and `player[…]` can never succeed — the cast fails on exactly the case wall 1 exists for. So the
object a handle crosses as must be *the script's own Verse object* whenever that handle carries one.

- The GDExtension already keeps the map: instance id → `VerseScriptInstance` → `verse_object`, which
  is how `set_field` avoids building a second Verse object around a node (`verse_script_instance.cpp`).
  A new Godot callback answers, for a handle, both its Godot class name and — when it has one — the
  qualified Verse class name of the script on it.
- With a Verse class name, the host returns that instance's own object. Without one, it builds a
  mirror wrapper for the Godot class, `FindMirroredClass` + `NewMirroredWrapper` as today.
- A **handle→class cache** keeps this from being a `get_class` round trip per crossing. Its
  invalidation rule is the instance id: ids are never reused within a run.
- Mirror wrappers stay **uncached** — a fresh one per crossing, equality by handle. A wrapper cache
  is a later optimisation `tools/build_bench.py` can ask for, and it would need a rule for what
  happens across hot-reload generations, which nothing yet needs.

**What a script writes:**

```
if (Sprite := animated_sprite2d[GetNode("AnimatedSprite2D")]):
	Sprite.Play()
```

and, for the mob that wall 5 configures through strings:

```
if (Mob := rigid_body2d[Scene.Instantiate[…]]):
	set Mob.LinearVelocity = Velocity
```

**Done when:** `dodge-the-creeps` loses its export slots for node lookups and the three `Object.Set`
calls, and an integration case casts a node it was handed by Godot, a node carrying another script,
and a node whose class does not match (which must fail rather than raise).

---

## 4. Stage 2 — making a container (R-TYPE-2's other half, R-INT-2)

The smallest item in the phase and the one that unblocks the most: `godot_array{}` compiles today and
holds reference 0, which crosses as `Nil`. So `Callv` cannot be given arguments, `AddUserSignal` is
unusable, and the 90 mirrored methods taking an `Array` plus the 83 taking a typed one can only be
passed a container Godot supplied.

`VhRefNew` is already the native function; what is missing is a public way to reach it and typed
element accessors on the way in. The generator already emits the accessors for reading.

- `MakeArray()`, `MakeDictionary()`, and the typed forms the generator can spell, each minting a
  reference id through the existing table.
- `AddInt`, `AddFloat`, `AddString`, `AddObject`, … symmetric with the existing `GetInt`/`GetFloat`
  readers, because a script cannot spell a `variant` (R-TYPE-7).
- The lifetime rule is unchanged: the id is released when the Verse value holding it is collected,
  through `godot_ref::BeginDestroy`.

**Done when:** the integration suite calls a GDScript method with arguments a Verse script built
itself, and R-INT-2 goes from `part` to `done` in the spec.

---

## 5. Stage 3 — a Callable made from a Verse function (R-TYPE-3's other direction, R-INT-4, R-SIG-3)

S-C proved the host half. The design is symmetric with the reference table the GDExtension already
keeps, pointing the other way:

- A native function taking `Callback(:t)<transacts>:void` receives a `TVerseFunction`. The host keeps
  it in a `godot_callback` native class — a UObject, so the VM traces it and `BeginDestroy` is the
  release signal, exactly as `godot_ref` does — and mints an id.
- A new Godot callback, `MakeCallable(CallbackId)`, asks the GDExtension for a `Callable` wrapping a
  `CallableCustom` that holds that id. `godot-cpp` exposes `CallableCustom` (`variant/callable_custom.hpp`).
- Invoking it crosses back through a new entry point, `vh_callback_invoke(CallbackId, Args, …)`, and
  the `CallableCustom`'s destructor releases through `vh_callback_release`.
- **Only a function bound to a script instance is accepted in 4a**, and the reason is Godot's own
  design rather than caution. GDScript has no free functions, but its lambdas are the analogue and
  Godot answers this question twice, differently:

  | | `get_object()` | `is_valid()` | dies with the object? |
  | --- | --- | --- | --- |
  | `GDScriptLambdaSelfCallable` (captures `self`) | the captured object | `CallableCustom::is_valid()` — consults ObjectDB | **yes** |
  | `GDScriptLambdaCallable` (plain) | **the script resource** | overridden to `function != nullptr` | **no** — anchored to a strong `Ref<GDScript>` |

  The second row is Godot's own known leak — the `GDScriptLambdaCallables` TODO and GH-102327, which
  this repo already met from the other end (`phase-2-design.md` §11, the exit-time segfault). So the
  bound case is the half that does not leak: `get_object()` is the script instance's node, `is_valid()`
  consults ObjectDB, and a freed node drops its subscriptions with no work from us. An unbound
  function is refused with a diagnostic, and **OQ-16** carries what answering it will require.
- **Equality is by reference**, which both prior arts agree on: Godot's lambda callables compare
  `p_a == p_b` ("Lambda callables are only compared by reference") and Epic's event inserts a new
  entry per `Subscribe`. So two subscriptions of the same method are two connections with two
  independent `cancelable`s, rather than the duplicate Godot refuses for `Callable(node, "method")`.
- Epic's `FVerseEventCallbackList` carries the other half of the discipline and is worth copying: each
  callback remembers the `FContentScope` it was subscribed in, and a terminated scope drops it. That
  is what keeps a raise (R-DIAG-3) from leaving callbacks that can never run again.

With this, R-SIG-3 takes a Verse function; R-INT-4 crosses a Callable in both directions; and any
callback-taking engine API — `sort_custom`, tween callbacks, `Array.filter` — becomes reachable.

### 5.1 The thread guard this stage makes necessary (R-ASYNC-8)

A `Callable` is a value, and an author may hand it to a `WorkerThreadPool` task. So this stage is
what turns "Godot could in principle call us from another thread" into a surface the bridge itself
offers, and the guard belongs with it.

**VerseVM asserts the game thread** — `VVMEnterVMInline.h`, at the top-level VM entry:
`ensure(IsInGameThread() && (!IsInAsyncLoadingThread() || …))`, above the comment "Verse bytecode and
AutoRTFM transactions must run on the game thread." It is thread *identity*, so serialising entry
does not satisfy it, and AutoRTFM's transaction state is per-thread besides. It is an `ensure`, not a
`check`, so the current behaviour is a logged callstack and then undefined behaviour — the worst of
the available failure modes.

The guard is small: record the `vh_init` thread at init, compare at every entry point, answer
`VH_ERR_THREAD` having run nothing, and report through the diagnostic callback with the script and
method named. The GDExtension can check on its own side too, so the message can say which node it
came from. Serving such a call rather than refusing it is OQ-6's, and the spec now records the
deadlock a blocking hand-off invites.

The precedent is already in the host: while a background analysis owns the program, `vh_tick` is a
no-op and the entry points that read the semantic program block, because proceeding trips
`ensure(!bBlockAllExecution)` and takes the process down.

---

## 6. Stage 4 — signals (R-SIG-1, R-SIG-2, R-SIG-3, R-SIG-4, R-SIG-6)

Sequenced after Stage 3 (§5) because `Subscribe` is a Callable made from a Verse function.

### 6.1 What a script writes

```
player := class(area2d):

	Hit<public>:godot_signal() = godot_signal(){}

	Struck<public>:godot_signal(struck_payload) = godot_signal(struck_payload){}

	OnBodyEntered<public>(Body:node2d):void =
		Hit.Signal(())
		Struck.Signal(struck_payload{Damage := 10, By := "spike"})

struck_payload<public> := struct:
	Damage<public>:int = 0
	By<public>:string = ""
```

and, elsewhere, in Verse rather than in a scene file:

```
	_Ready<override>():void =
		if (P := Player?):
			Unsub := P.Hit.Subscribe(OnPlayerHit)
```

`godot_signal()` is the alias for `godot_signal(tuple())`, the way `listenable()` is for
`listenable(tuple())`.

**There is no `@signal` attribute.** The member's *type* is the marker, and the host reads declared
types out of the semantic program already. A bridge attribute exists where the text is the only
source — `@global_class` survives without compilation because Godot asks about files it has only
scanned — and §6.5 says the signal list is not one of those cases.

### 6.2 The payload, and the argument names Godot needs

Godot's signal list carries an argument *name* per parameter, and `_make_function` writes those names
into the handler it generates. **Verse tuples cannot name their elements** — `tuple(Damage:int, …)`
is refused with "Expected a type, got data definition instead" — so the payload type is what decides
whether the editor sees names:

| payload | Godot arguments | a Verse subscriber writes |
| --- | --- | --- |
| `tuple()` | none | `OnHit(P:tuple())` |
| a bare type, `godot_signal(int)` | one, named for the type — `Int` | `OnHit(Value:int)` |
| `tuple(int, string)` | two, `Arg0` and `Arg1` | `OnHit(Damage:int, By:string)` — a multi-parameter function satisfies a tuple parameter (S-B) |
| a **struct** | one per **top-level field**, named by the field | `OnHit(P:struck_payload)`, reading `P.Damage` |

Mapping is **top level only**: a `vector2` field is one `Vector2` argument, not two floats. The trade
is explicit and per signal — a tuple keeps the ergonomic N-parameter handler and loses names in the
editor; a struct gets names in the connect dialog and in the generated stub, and hands the Verse
subscriber one value.

A payload the wire cannot carry is refused **at the member**, reusing R-EXP-3's machinery rather than
failing at the emission: the rejection reasons already spelled in `vh_export_reject` answer the same
question about the same lanes.

### 6.3 What it is underneath

`godot_signal(t)` is a `<native>` parametric class in the Godot package whose C++ shadow holds two
things: the **owner handle** and the **signal name**. Both are written by the host at construction —
`Instantiate` walks the script class's data members for the ones whose declared type is this class,
exactly where `Shadow->Handle.Init` runs today (S-A). The member's own name is the signal's name, so
there is no second place to spell it and nothing to drift.

| the author writes | it becomes |
| --- | --- |
| `Hit.Signal(Payload)` | Godot's `emit_signal`, **immediately** (§6.4), payload unpacked per §6.2 |
| `Hit.Subscribe(F)` | `ConnectSignal` with a Callable made from a Verse function (§5), answering a `cancelable` |
| `Unsub.Cancel()` | `DisconnectSignal`, **idempotent** — a second `Cancel` does nothing, as `event_subscription::Cancel` does in UEFN |
| `Hit.Await()` — Phase 5 | `awaitable(t)`, the one Verse interface whose domain fits (§1.2) |

**Subscription goes through Godot, not through a Verse-side list.** It costs a Callable per
subscription and it is the only arrangement in which a signal emitted *from GDScript* reaches a Verse
subscriber, which R-SIG-6 requires. `Subscribe` takes no flags in 4a; one-shot belongs with `Await` in
Phase 5, where the idiom that wants it lives.

### 6.4 Emission is immediate, and that is a stated exception

Every other void mutation in the mirror defers to `AutoRTFM::OnCommit`, which is what makes
`<transacts>` literally true for 6813 methods. Emission does **not**: handlers run synchronously, as
they do in GDScript, so "emit, then read what the handler changed" behaves the way a Godot author
expects, and a Verse emission is indistinguishable from a GDScript one. The cost is stated rather
than hidden — if the emitting transaction later aborts, the handlers have already run. That puts
emission in the same documented set as the 1354 value-returning mutators, and **Phase 4.5** is where
that whole set is audited.

**`Subscribe`, by contrast, is compensated.** It mutates Godot *and* returns a value, so it can be
neither deferred nor ignored; the native registers `Verse::Stm::OnRollback` to disconnect. **That
last clause is wrong** — `Stm::OnRollback` is the Solaris interpreter's STM and never ran from this
bridge, which Phase 4.5's S-3 measured and fixed; it is `AutoRTFM::OnAbort<SameAsClosed>` now. The
paragraph is otherwise right, and the rest of it is why the compensation exists at all. That is
Epic's own pattern in the other direction — `subscribable_event_intrnl`'s unsubscribe re-subscribes on
rollback — and without it a failed transaction leaves a live connection the script believes it never
made. This is the host's first rollback compensation and the shape to copy for anything later that
mutates Godot and cannot defer.

### 6.5 What Godot is told

A new `vh_class_signal_list(ClassNameUtf8, …)` answers a name and argument descriptors per signal,
read out of the semantic program exactly as `vh_class_method_list` and `vh_class_export_list` are, so
it refreshes per keystroke rather than per build. `VerseScript::_get_script_signal_list` (:512) and
`_has_script_signal` (:508) answer from it; both exist and return empty today.

The name Godot sees is the **Verse spelling verbatim** — `Hit`, `MobSpawned` — following C# and
following what a Verse method already does in every scene connection the port carries. A signal
declared in a script that has never been built appears after the next Build, the same bargain an
`@export` *default* already makes.

### 6.6 The rules that will otherwise be found by accident

- **Signals inherit.** A script class deriving from another script class has that class's signals, and
  both the list and the construction-time binding must walk the whole chain. Phase 2 shipped this bug
  once already, for `@export` on a base script class, in two places that had been correct right up
  until a script could derive from a script.
- **A `godot_signal` member must not be `var`**, and must be `<public>` to be registered. A private one
  is a diagnostic rather than a silently absent signal.
- **A class with no Godot object has nowhere to bind**, so a `godot_signal` member on a class that is
  never instantiated against a handle is a diagnostic at the member.
- **Two subscriptions of one handler are two connections** (§5, equality by reference), each with its
  own `cancelable`. Cancelling one leaves the other alive.
- **A freed node drops its subscriptions**, because `get_object()` is the subscriber's node and Godot
  cleans up connections when an object is freed. Nothing in the host tracks that.
- **Generations**: an instance made against generation N keeps generation N's class and therefore its
  signals. Nothing adopts, which is R-ITER-4 and needs no work here.

### 6.7 Godot's own 489 signals

Generated per class, as C# generates them: an accessor returning a `godot_signal(t)` bound to that
handle and that Godot name, with the payload built from the signal's declared arguments — which Godot
*does* name, so these get real names and need no struct.

```
	_Ready<override>():void =
		if (Timer := MobTimer?):
			Unsub := Timer.Timeout().Subscribe(OnMobTimerTimeout)
```

A method rather than a data member, because a mirror wrapper is built per crossing and a member would
have to be filled on each one. The eight cases where the accessor name would have collided —
`Node.ready`, `CanvasItem.draw`, `Control.gui_input`, `BaseButton.pressed` among them — are collisions
with a **virtual**, not with a method, so §7.1's underscore dissolves all eight and no signal needs an
invented name.

This is also what makes wall 3 answerable in Phase 5: `Timer.Timeout().Await()` is a spelling only
because the accessor exists.

### 6.8 The editor's half of R-SIG-4

Connecting through the Node panel and `_make_function` are the untested half of a requirement whose
load-bearing half the port leans on. `_make_function` (`src/verse_script_language.cpp`:596) writes a
handler into the script the way R-TOOL-12 writes a `using` line —
`ScriptEditor::get_current_editor()->get_base_editor()` is the `CodeEdit`, which Phase 3 established a
GDExtension can write into — with parameters spelled from the same descriptors §6.5 reports:

```
	OnStartButtonPressed<public>():void =
		# TODO
```

### 6.9 What proves it

Integration cases, each one line in `tests/integration/test_main.gd`:

1. GDScript reads `get_signal_list()` on a node carrying a Verse script and finds the declared signal
   with the right argument count and names.
2. GDScript connects to a Verse-declared signal and receives it when the Verse script emits.
3. A Verse script `Subscribe`s to a signal a **GDScript** node emits, and the handler runs.
4. A Verse script subscribes twice and cancels once; the surviving subscription still fires.
5. `Cancel` twice is not an error.
6. A scene-file connection to a Verse method still works — regression, because that is the port's
   existing wiring and this stage must not disturb it.
7. Freeing the subscriber's node stops delivery, with no error on the next emission.
8. A struct-payload signal reaches GDScript as named arguments and a tuple-payload one as positional.

By hand, because nothing headless can see them: the Node panel lists the signal, connecting through it
writes a working handler, and `_make_function` names the parameters from §6.2's mapping.

---

## 7. Stage 5 — the full virtual set, `_Notification`, and `@tool`'s editor surface

R-NODE-7 asks for a mechanism general enough that a virtual added by a future Godot version needs no
code change here. **Most of that mechanism already exists** and has since Phase 1: `GodotVirtualNameOf`
turns `PhysicsProcess` into `_physics_process` by rule rather than by table, `vh_method_desc` carries
the Godot name of the virtual a method overrides, and the script instance answers `has_method` and
`get_method_list` with it — which is what decides whether Godot puts the node in the process list.

What is missing is the **declarations to override**. The generator skips all 1413 `is_virtual`
methods, and the three that exist — `Ready`, `Process`, `PhysicsProcess` — are hand-written on
`vh_object`, the native root, which is why every script has them whether or not its base class does.

### 7.1 The name: `_Ready`, not `Ready`

Counted before it was decided, because the plain spelling looked free and is not:

| collision | count | where |
| --- | --- | --- |
| a virtual against a **method or property** of the same PascalCase name | **834**, in 41 classes | almost entirely server-extension classes nobody derives from: `TextServerExtension` 235, `PhysicsServer3DExtension` 175, `PhysicsServer2DExtension` 119. On classes a game script derives from: `Control` 2 (`_get_minimum_size`, `_get_tooltip`), `Resource` 4, and **zero** on Node, Node2D, CanvasItem, Object |
| a virtual against a **signal** of the same name (§6.4) | **8** | `Node.ready` vs `_ready`, `CanvasItem.draw` vs `_draw`, `Control.gui_input` vs `_gui_input`, `BaseButton.pressed` vs `_pressed`, `Range.value_changed` vs `_value_changed`, `CollisionObject2D/3D.input_event` vs `_input_event`, `BaseButton.toggled` vs `_toggled` — that is, on the most-used classes in the engine |

The second row is what decides it. A virtual and a method are two roles that rarely meet — `_get_length`
is what a subclass *implements*, `get_length()` is what a caller *invokes* — and if that were the whole
problem the plain spelling would be defensible. But `Node.ready` is a **signal** and `_ready` is a
**virtual**, and both are things an ordinary script touches.

**So Godot's own spelling wins: a virtual keeps its leading underscore, PascalCased.** `_Ready`,
`_Process`, `_PhysicsProcess`, `_Input`, `_Draw`, `_GuiInput`, `_GetMinimumSize`. Every collision of
both kinds disappears by construction, because Godot's names are *already* disambiguated by the
underscore and we stop throwing that information away. A leading-underscore method name is legal
Verse — compiled in the spike driver §2 describes, not assumed. It is also what a Godot developer
types in GDScript and what C# generates (`public override void _Ready()`), so R-AUD-1 is served
rather than strained, and `GodotVirtualNameOf` stays a rule: strip nothing, lowercase each capital
with an underscore before it.

The cost is a rename of the three virtuals every script overrides. It lands **all at once, in this
stage** — `demo/`, `tests/integration/`, `tests/host_smoke/` and the yardstick in one commit, with the
old names gone rather than deprecated. Pre-1.0, no compatibility obligation; the roadmap's standing
rule, and the phase is rewriting the yardstick anyway.

### 7.2 The generator work

- Emit each class's virtuals as ordinary Verse methods with a default body, on the class that
  declares them — `_Input` on `node`, `_Draw` on `canvas_item`, `_GuiInput` on `control`,
  `_GetConfigurationWarnings` on `node`. A script overriding one says `<override>` and Verse's own
  redeclaration rules supply the error when the signature is wrong.
- **No effect specifier**, exactly as the three hand-written ones carry none: the default set is
  wider than `<transacts>` (§1.3), so an overriding body may call whatever Godot it likes.
- A virtual with a return type needs a default: `array{}`, `false`, `0.0`. Where Godot's own default
  is "not handled", that is what the default body must mean, and the ones that gate engine behaviour
  (`_CanDropData`, `_HasPoint`) get a test each.
- The three hand-written ones move off `vh_object` onto `node`, where Godot declares them. A
  `Resource`-derived script stops having a `_Process` it can never receive.
- `@tool`'s editor-only surface (R-EXP-5) is then a *test*, not a feature: `_GetConfigurationWarnings`
  and the gizmo virtuals are among the 1413.

### 7.3 What the generator cannot reach: the script-level hooks

`_notification` **is not in `extension_api.json`** — `Object` declares zero virtuals there, and
neither `_to_string`, `_get`, `_set` nor `_get_property_list` appear anywhere in it. They are hooks
Godot offers to *scripts* rather than methods it registers in ClassDB, so no amount of generator work
produces them and R-NODE-8 does **not** ride R-NODE-7 after all.

`_Notification(What:int):void` is therefore hand-declared on the native root beside today's three,
and the GDExtension's existing `notification_func` calls it through `vh_instance_call` — **no new
ABI**. Its `What` is an int, and §8.1's `NodeStatics.NotificationReady` is what makes it readable.

The rest of that set — `_ToString`, `_Get`, `_Set`, `_GetPropertyList`, `_ValidateProperty` — becomes
a requirement of its own (**R-NODE-10**) rather than being quietly skipped, and sits in **4b**, where
`_Get`/`_Set` can be designed against the export machinery they overlap with.

### 7.4 The cost

Analysis latency, already 1190 ms with 1023 classes and 758 enums. The decision is to generate
everything and **not** to set a threshold: feature parity first, performance goals later, with the
number recorded by `tools/build_bench.py` when the stage lands (OQ-14). The real fix remains the
cooked route Phase 7 owns.

---

## 8. Stage 6 — constants, statics, `@GlobalScope` and the math types (R-SCN-3, OQ-11, R-NODE-4's Godot half)

### 8.1 Class modules

Verse has no constant on a type, and Epic hit the same wall (`Zero2()` with a TODO wishing for
`vector2.Zero`). What Verse does have is inline modules with qualified access, so:

```
NodeStatics<public> := module:
	NotificationReady<public>:int = 13
	PrintOrphanNodes<public>()<transacts>:void = …

Vector2Statics<public> := module:
	Zero<public>:vector2 = vector2{}
	Up<public>:vector2 = vector2{X := 0.0, Y := -1.0}
```

The suffix is not decoration. A module named `Node` or `Tree` would be a top-level name in the
author's scope, and Verse refuses a local that resolves ambiguously against one — `Tree := GetTree[]`
in `main.verse` would stop compiling (§1.3). `…Statics` is a name nobody reaches for.

`PI`, `TAU`, `INF` and `NAN` are not in the extension API at all, so a small hand-written
`GodotStatics` module carries the few that Verse's own stdlib does not already spell — and per
R-AUD-2 the ones it does spell are not duplicated.

### 8.2 Dispatch with no handle

Godot's 114 statics and the ~28 utility functions with no `/Verse.org` counterpart share one problem
and get one solution: a by-name call that carries no object. Two new Godot callbacks —
`CallStatic(ClassName, Method, Args)` and `CallUtility(Name, Args)` — and the generator emits Verse
free functions and module members over them.

The **random family is the one exception to R-AUD-2** and is dispatched rather than answered by
Verse's `GetRandomFloat`: `randf`, `randi`, `randf_range`, `randi_range`, `randfn`, `seed` and
`randomize` all steer the engine's stream, and a Verse-side RNG would silently ignore `seed()`. C#
does the same thing for the same reason. Everything else keeps the rule: where Verse has a
counterpart, Verse's spelling wins, and the 78 math and 8 random names that would collide are not
mirrored under Godot's names.

### 8.3 The math types, in Verse

Value-type methods become extension functions and operators, with **Verse bodies**:

```
(V:vector2).Length<public>()<computes>:float = Sqrt(V.X * V.X + V.Y * V.Y)
operator'+'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X + R.X, Y := L.Y + R.Y}
```

Three things this needs before it can be written:

1. **The mirror's math structs must be declared `<computes>`** (and `<concrete>`), or none of these
   bodies can construct their result in a `<computes>` context — the spike hit this immediately.
   Epic's own `vector2` carries `struct<concrete><computes><persistable>`; the generator does not.
2. **A bound on how much is hand-written.** 367 methods and 261 operators across 16 types is C#'s
   `Vector2.cs`-sized problem, and C# solved it by writing them all. This phase does not: the
   arithmetic operators, and the methods the yardstick and ordinary scene code actually reach —
   length, normalized, distance, dot, cross, lerp, clamp, abs, sign, floor/ceil/round, rotated,
   angle, snapped, min/max — are written; the long tail (`bezier_interpolate`, `cubic_interpolate`,
   `orthonormalized`, `slerp`, the Basis/Quaternion conversions) is **recorded as a skip with a
   reason**, which is machinery R-SCN-2 already has and which makes the gap say so in the editor
   rather than being silently absent.
3. **Each written body must match Godot's edge cases**, not merely its formula: `normalized()` of a
   zero vector is zero, not NaN. The reference is `../godot/core/math/*.h`, and the test is a table
   of inputs whose answers are read from Godot rather than derived.

When this lands, `dodge-the-creeps/scripts/vectors.verse` is deleted — the file OQ-11 was opened
about — and `position += velocity * delta` has its ordinary spelling back.

### 8.4 A script's own statics and constants (R-NODE-4), and abstract classes (R-NODE-5)

Godot asks a script class for `_has_static_method` and `_get_constants`, and Verse has no `static`
keyword. The same inline module that carries Godot's constants carries a script's, in the script's
own file, with the association **declared** rather than inferred from a name:

```
@statics(player)
PlayerStatics<public> := module:
	MaxSpeed<public>:float = 400.0
	Describe<public>()<transacts>:string = "the player"
```

A naming convention (module `Player` for class `player`) was the first proposal and was rejected in
the interview for a good reason: a typo produces a silently empty statics module rather than an
error, and a file may declare any number of `@global_class` classes, so there is no single obvious
name to convene on. With the attribute, a module naming a class that does not exist — or two modules
naming one class — is a diagnostic.

`@statics` is declared where `@global_class` and `@export` are: the attribute package the host adds
at runtime, **before the first `AddDataSource`** (`FSolarisIde::EnsureDataSourcePackageExists`
snapshots dependencies exactly once — CLAUDE.md's constraint, and the one that bites silently).

The host reads the module's members out of the semantic program the way it already reads a class's
exports and methods, and it reads their *values* the way `vh_class_default_field` reads an export's
default — no instance required, which is the whole point of a static.

**R-NODE-5** is the small one beside it: Verse has `class<abstract>`, so `_is_abstract` answers from
the semantic program instead of returning false unconditionally, and Godot stops offering to
instantiate a base script that was never meant to be attached.

---

## 9. ABI v4.0

`VH_ABI_VERSION` goes to 4000. Both sides rebuild; the mismatch surfaces at `vh_init`.

| addition | why |
| --- | --- |
| `vh_class_signal_list` | §6.3, the Node panel and GDScript |
| `vh_callback_invoke`, `vh_callback_release` | §5 |
| `vh_godot_api::MakeCallable` | §5, the GDExtension mints the `Callable` |
| `vh_godot_api::CallStatic`, `CallUtility` | §8.2 |
| `vh_godot_api::GetClassOf` | §3: a handle's Godot class and, when it has one, its Verse class |
| `VH_ERR_THREAD` | §5.1: an entry point called from a thread other than `vh_init`'s, having run nothing |

**Not added, deliberately:** emission (the v2 `EmitSignal` callback already does it), connection
(`ConnectSignal`/`DisconnectSignal`, likewise), and `_Notification` (a hand-declared virtual reached
through `vh_instance_call` from the vtable's existing `notification_func`). Three requirements land with no new wire at all, which is the ABI v2
design paying off.

---

## 10. 4b — the editor's data model

Designed here only far enough to say what it contains and what each depends on; it gets its own
stage-level design when 4a's exit is met.

- **R-NODE-3** — a Verse class instantiated without a node. Both `RefCounted` (Godot owns the
  lifetime, released when the Verse value is collected, `godot_ref`'s mechanism) and `Object`
  (explicit `Free()`, and Godot's own leak report if you do not) — parity with GDScript, footgun
  included, per the interview.
- **R-EXP-6** — custom Resources: a `.verse` extending `Resource`, saved to and loaded from `.tres`
  with its exports serialised. Needs R-NODE-3's non-node instances first.
- **R-EXP-7** — autoload singletons.
- **R-EXP-8** — `_get_class_icon_path` answering from an attribute.
- **R-EXP-9** — `_get_rpc_config`.
- **R-EXP-1's remainder** — flags, file/dir pickers, multiline, node paths, resource types,
  subgroups and categories. Type-driven where the Verse type can say it, attributes only where it
  cannot.
- **R-NODE-10** *(new)* — the remaining script-level hooks: `_ToString`, `_Get`, `_Set`,
  `_GetPropertyList`, `_ValidateProperty`. None are in `extension_api.json` (§7.3), so each is a
  hand-declared virtual on the native root, and `_Get`/`_Set` overlap the export machinery enough
  that they want designing beside it rather than before it.
- **R-NODE-5** — abstract classes: Verse has `class<abstract>`, so `_is_abstract` answers from the
  semantic program. Small, and in 4a with the statics.

---

## 11. Work order and exit

### 11.1 Stages

| stage | what | done when |
| --- | --- | --- |
| **0** | the three spikes | **done** — §2, and this document is written against them |
| **1** | the cast and object identity (R-SCN-6) | a node Godot hands back casts to its own script's class, and to its mirror class; a wrong cast fails |
| **2** | making a container (R-TYPE-2, R-INT-2) | a Verse script calls a GDScript method with arguments it built itself |
| **3** | Callable from a Verse function (R-INT-4), and the thread guard (R-ASYNC-8) | a Verse function connected to an engine signal runs when Godot emits it; a call from a worker thread is refused with a message rather than entering the VM |
| **4** | signals (R-SIG-1/2/3/4/6) | §6.9's eight integration cases pass, and the Node panel lists a Verse-declared signal by hand |
| **5** | the virtual set under `_Ready` naming, `_Notification`, `@tool`'s editor virtuals (R-NODE-7/8, R-EXP-5), and the rename of the existing three | `_Input`, `_Draw`, `_UnhandledInput`, `_Notification` and `_GetConfigurationWarnings` all reach a script; every script in the repo overrides `_Ready`; the analysis number is recorded |
| **6** | constants, statics, `@GlobalScope`, math (R-SCN-3, OQ-11, R-NODE-4/5) | `vectors.verse` is deleted and the port still plays |
| **7** | the idiomatic re-port | the eight walls, wall by wall, each in its own commit |

Stage 1 first because everything after it is written differently. Stage 3 before Stage 4 because
`Subscribe` is a Callable. Stage 6 after Stage 5 because both are generator work and the analysis
measurement is worth taking once the larger of the two has landed.

### 11.2 Exit criteria for 4a

- **Dodge the Creeps is idiomatic**, and the diff says so: `$Child` lookups are casts rather than
  seventeen inspector slots; `hit` and `start_game` are Verse-declared signals rather than two scene
  connections to one engine signal; `vectors.verse` is gone; the mob is configured through typed
  properties rather than `Object.Set` with string names. Wall 3 (`await`) is **not** closed here — it
  is Phase 5's, and the HUD keeps its state machine with a comment saying why.
- **The 29 headless checks still pass**, plus whatever the re-port adds.
- **`tools/run_tests.py` is green**, with a new integration case per requirement (R-QUAL-2).
- **The by-hand checks have been run** — Phase 3's two (a windowed yardstick run and an editor
  session) and this phase's: connecting a signal through the Node panel, letting `_make_function`
  write the handler, a `@tool` script's `_get_configuration_warnings` appearing on the node, and an
  `_input` handler receiving a key. [`by-hand-findings.md`](by-hand-findings.md) is the record; the
  list they were on is deleted.
- **The spec is edited in the same commits**: R-SIG-1/2/3/6 to `done`, R-SIG-4 to `done`,
  R-NODE-7/8 to `done`, R-INT-2/R-TYPE-2 to `done`, R-SCN-3 and OQ-11 closed, R-EXP-5 to `done`,
  R-AUD-2 carrying the random exception, and **R-NODE-10** added for the script hooks 4b inherits.

### 11.3 Where the code is

Line numbers are from the commit this document was written against and will drift; the names will
not. `CLAUDE.md` has the build and test commands, every stage ends with `python tools/run_tests.py`,
and the generated-files table there is binding — `GodotClasses.native.verse`, `verse_api_classes.h`,
`verse_api_skipped.h` and `GodotMathLayout.gen.h` are `gen_verse_api.py`'s output and are never
hand-edited.

**Stage 1 — the cast.** `host/Private/HostScript.cpp`: `NewMirroredWrapper` (:2239) and
`FindMirroredClass` beside it are what builds a wrapper today; `Instantiate` (:3968) is the shape to
copy for identity. The GDExtension already has the map — `VerseScriptLanguage::instance_for`
(`src/verse_script_language.cpp`:1916), used by `VerseScriptInstance::set_field`
(`src/verse_script_instance.cpp`:~460), which exists precisely so a node carrying a script is not
wrapped twice. The new `GetClassOf` callback goes in the table in `src/verse_runtime.cpp` and the
struct in `include/verse_host_abi.h`.

**Stage 2 — containers.** `VhRefNew` is already there (`host/Private/GodotBindings.cpp`:775) and
module-scoped; the public surface goes in `host/Verse/GodotApi.native.verse`, and the typed adders
in `tools/gen_verse_api.py` beside the accessors it already emits. The id lifetime is
`src/verse_ref_table.cpp` and `godot_ref::BeginDestroy` (`host/Private/GodotClasses.h`).

**Stage 3 — the Callable.** A `godot_callback` shadow beside `godot_ref` in
`host/Private/GodotClasses.h`; the native function taking `TVerseFunction` in
`host/Private/GodotBindings.cpp` (the reverted spike is §2's S-C, and Epic's
`FVerseEventCallbackList` in `Engine/Plugins/Verse/Verse/Source/Verse/Private/VerseEvent.cpp` is the
storage discipline worth copying); the `CallableCustom` subclass is new in `src/`, over
`godot-cpp/include/godot_cpp/variant/callable_custom.hpp`.

**Stage 4 — signals.** `godot_signal(t)` is declared in `host/Verse/Godot.native.verse` with its
shadow in `host/Private/GodotClasses.h`; the rollback compensation is `Verse::Stm::OnRollback`,
which the host has never used — `Engine/Plugins/Verse/VerseTags/…/TagContainer.cpp` and
`VerseEvent.cpp` are the two worked examples, and note their `if (!AutoRTFM::IsTransactional())`
guard does *not* apply to us, because nothing on the Godot side is instrumented; the construction-time binding goes in `Instantiate`
(`HostScript.cpp`:3968) beside `Shadow->Handle.Init`; the signal list is a new `GetClassSignals`
next to `GetClassMethods` (:2868) and `GetClassExports` (:2951), which are the two worked examples of
reading declarations out of the semantic program. On the Godot side, `VerseScript::_has_script_signal`
(:508) and `_get_script_signal_list` (:512) exist and answer empty today. `_make_function`
(`src/verse_script_language.cpp`:596) and `_can_make_function` (:513) are R-SIG-4's editor half.

**Stage 5 — virtuals.** All generator: `tools/gen_verse_api.py`:1238 is the `is_virtual` skip, and
`LIFECYCLE_METHODS` (:1935) is the table that exists because the three are hand-written today. The
host's rule is `GodotVirtualNameOf` (`HostScript.cpp`:2833) and `OverridesMirroredDefinition` beside
it. `_Notification` is hand-declared on the native root in `host/Verse/Godot.native.verse` (where
`Ready`/`Process`/`PhysicsProcess` are today, and from which they move to `node`), and the vtable
entry that will call it is `notification_func` (`src/verse_script_instance.cpp`:327). The rename
sweep touches `demo/scripts/`, `tests/integration/scripts/`, `tests/host_smoke/*.verse` and
`dodge-the-creeps/scripts/`.

**Stage 6 — constants, statics, math.** `tools/gen_verse_api.py` again, plus the math bodies as
ordinary Verse: a new file in `host/Verse/` rather than in the generated one, because they are
hand-written and the generated file is not. The struct specifiers that must change first are emitted at
`gen_verse_api.py`:1032 — `<name><public> := struct:`, which is where `<concrete><computes>` has to
appear before any math body can be `<computes>` (§8.3 item 1). `../godot`'s
`core/math/*.h` is the reference for the edge cases, per the memory note about that checkout.

---

## 12. Risks, and what this phase opens

| risk | mitigation |
| --- | --- |
| per-keystroke analysis, already 1190 ms, gets materially worse with 1413 virtuals, 489 signal accessors and ~1200 module members | recorded at Stage 5 with `tools/build_bench.py`, and **deliberately without a threshold**: parity first, performance goals later. This is **OQ-14**, and what it informs is whether Phase 7's cooked route is urgent or merely planned |
| the math surface is the largest hand-written body of code in the project, and every method is a chance to disagree with Godot quietly | tiered (§8.3): a written list, a recorded skip for the tail, and a test table whose expected values are read out of Godot rather than derived |
| `CallableCustom` round-tripping through the ABI is the one part of §5 no spike touched | Stage 3 is its own stage, before signals, precisely so that finding out is cheap. The fallback for R-SIG-3 is a method-name Callable, which already works and which would leave closures to Phase 5 |
| signals registered through `ScriptExtension` may not behave identically to GDScript's for the editor's Node panel | Stage 4's by-hand check is the first thing in it, not the last |
| a generated virtual with a wrong default return value changes engine behaviour silently — Godot asking "is this handled?" and being told yes | the default body for a value-returning virtual is chosen from Godot's own documented default, and the ones that gate engine behaviour (`_can_drop_data`, `_has_point`) get a test each |
| the re-port rewrites the yardstick, so a regression in the old shape stops being covered | the old spellings stay legal and the integration suite keeps a case for each — the export-slot path, `Object.Set`, and the scene-file connection |

**Open questions this phase opens or inherits:**

- **OQ-14** (new) — does per-keystroke analysis stay usable after the mirror grows? Recorded at
  Stage 5, with no threshold attached by decision. Blocks nothing; informs Phase 7's priority.
- **OQ-16** (new) — what anchors a callback that is *not* a bound method? 4a accepts only a method
  bound to a script instance, because that is the half of Godot's own design that does not leak.
  Answering it means choosing an owner for an unbound function — Godot anchors a plain lambda to
  the script resource and accepts the leak — and deciding whether the bridge should offer an
  explicit-owner spelling instead. Wanted by library-level handlers and by anything Phase 5's
  concurrency work hands to Godot.
- **OQ-17** (new) — **C# interop is asserted by four MUST requirements and has never been run.**
  R-SIG-6, R-INT-1, R-INT-2 and R-INT-5 all name C#, every test in the repo is GDScript, and
  exercising C# needs a .NET Godot build the harness does not have. Phase 4 makes the claim
  larger, not smaller, which is why it is written down here rather than discovered at 1.0.
- **OQ-15** (new) — Verse's transaction semantics deserve a review of their own: the `<transacts>`
  trap (wall 8), what `no_rollback` costs a library author, and whether the bridge should be saying
  something at the declaration. Deferred out of this phase deliberately.
- **OQ-15** — no longer a row waiting for a phase: it *is* **Phase 4.5**, between parity and
  concurrency, and §6.4 hands it emission plus the 1354 value-returning mutators as its subject
  matter. Whether Godot's 6766 const methods should carry `<reads>` instead of `<transacts>` —
  which would make the mirror's methods agree with its properties, and shrink the audited set to
  exactly those 1354 — is the first question that phase inherits, and it is **not decided here**.
- **OQ-11** — closed by §8.3 and §1.3: operators can be defined, extension methods work, and the
  answer is pure Verse.
- **OQ-13** — untouched; it is Phase 6's, and it wants R-ASYNC-4 first.


---

## 13. Where this design was wrong

Written after building 4a. **The full list lives in [`phase-4-gaps.md`](phase-4-gaps.md)**, which is
the single record of where the implementation and this document disagree and is written for someone
picking the phase back up with no memory of it. Twenty-one entries, each with what the design said,
what is there, why, and what closing it would take. **All of them are now closed, answered or
deliberately narrowed**, and the by-hand checks have since been run and what they found fixed
([`by-hand-findings.md`](by-hand-findings.md)). The phase owes nothing.

The short version, for a reader who is here rather than there:

- **The payload type is not where §6.3 implies.** A signal member's declared type comes back as the
  *generic* `godot_signal(t)` — its `Signal` method still has the type variable as its parameter — so
  the type argument has to be read off the class's `_TypeVariableSubstitutions`. The first
  implementation read the method and had every signal reporting one argument of unknown type. This
  is the only thing in the phase that had to be found by asking the compiler rather than by reading.
- **§6.7 depends on §7.1, so §11.1's stage order is wrong.** The eight colliding signal-accessor names
  collide with *virtuals*, and it is the underscore that dissolves them — so Godot's own 489 signals
  cannot be generated until stage 5 has landed. Stage 4 split in two to build it.
- **§6.2's fourth row did not degrade, it broke.** A struct payload read as a trade in that table and
  was not one: it registered a bogus signal and emitted nothing. Since fixed — it decomposes into one
  named argument per field, and a Verse handler receives it as one value. `phase-4-gaps.md` G1, G21.
- **Two latent bugs surfaced**, neither caused by this design. `GodotVirtualNameOf` turned `_Ready`
  into `__ready`; and `InstanceHasFunction` — which decides whether Godot puts a node in the process
  list at all — compared *function cells* rather than procedures, so every method had looked
  overridden since Phase 1. Both fixed.
- **§8.2's "one solution" is two.** `ClassDB.class_call_static` is generic enough for all 114 statics;
  the GDExtension interface has no by-name utility call taking Variants at all, so the utilities are
  a fixed table of eight.
- **§8.3 counted one cost as zero.** An extension method is a *module-level* definition, so
  `(V:vector2).Length()` makes `Length` ambiguous as a parameter or local name in every file that
  imports the package.

### 13.1 What the numbers came to

| | before Phase 4 | after 4a |
| --- | --- | --- |
| per-keystroke analysis (`vh_check_project`, median of 10) | 1190 ms | **1273 ms** |
| generation against the 5-file game | 1270 ms | 1395 ms |
| mirror size | — | 4364 KB |
| methods emitted | 8866 | 10149 |
| virtuals / signal accessors / constants / statics | 0 / 0 / 0 / 0 | 1283 / 489 / 352 / 114 |

**OQ-14's answer is "yes, comfortably".** §7.4 and §12's risk table both expected the 1413 virtuals
to be the thing that made per-keystroke analysis unusable. They cost about **45 ms**; the signal
accessors about 24; the constants and statics the rest. Eighty-three milliseconds for the whole of
the phase's mirror growth. No threshold is attached, by the decision §0 records, and Phase 7's
cooked route is informed rather than urgent.


### 13.2 What is still owed

- ~~**The by-hand checks**~~ — run, with Phase 3's two and Phase 5's, in one windowed session.
  [`by-hand-findings.md`](by-hand-findings.md) is what they found: nine defects, all fixed, and two
  things still open because nothing can automate them.
- **The twenty gaps in [`phase-4-gaps.md`](phase-4-gaps.md)**, §9 of which suggests an order.
- **4b**, which this document sketches in §10 and does not design.
