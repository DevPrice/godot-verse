# How a script declares a signal

The research behind moving a script-declared signal from a `signal(t)` member to `@export_signal` over an
ordinary `event(t)`, and the staged plan that follows from it. Not a phase record: this is the
document to read before touching `GetClassSignalsLive`, `BindSignals`, or anything that tests
`IsSignalClass`.

`docs/spec.md` R-SIG-1 carries the requirement and the per-stage status. This carries the reasoning
and the measurements, including the ones that say what **cannot** be done.

## 1. Where this starts

The shipped mechanism reads a signal off the member's *type*:

    player := class(area2d):
        Hit<public>:signal(tuple(int, string)) = signal(tuple(int, string)){}

`signal(t)` is ordinary parametric Verse over the native `vh_signal`, holding a `/Verse.org/Verse`
`event(t)` in its `Ev` field. It works, and R-SIG-1 through R-SIG-6 are green on it.

What it costs is a bridge type in the author's vocabulary where Verse already has one. A script that
wants to hand a signal to any Verse code taking an `awaitable(t)` or a `listenable(t)` cannot: a
`signal(t)` is shaped like those interfaces and implements none of them. The target is that the
member *is* a `event(t)` — the type Verse's own concurrency vocabulary is built on — and an
attribute says it is also a Godot signal:

    player := class(area2d):
        @export_signal
        Hit<public>:event(hit_payload) = event(hit_payload){}

## 2. The one thing that cannot move

**Emission has to stay the bridge's verb.** Three separate facts force it, and none of them is about
the field's type:

- `signalable.Signal` carries the default effect set, which contains `no_rollback`. Every Godot
  callback reaches Verse inside a transaction, so a `<transacts>` body may not call it. Measured:
  `tests/verse_probe/event_probe.verse` Q2 records the refusal as uLang glitch **3512**, naming
  `(/Verse.org/Verse/event:)Signal`. A specifier-less body *can* call it (Q3, which compiles and
  runs), and that is the whole of the exception.
- Godot has to be the dispatcher. A GDScript connection fires only if `emit_signal` runs, and the
  ordering between Verse and GDScript handlers has to be Godot's decision rather than the VM's.
- Nothing can intercept `VEvent::Signal`. The engine owns it; there is no hook, and a forwarder task
  looping on `Await` would miss an emission raised during its own resumption.

So the author's emit verb is an extension method the bridge owns — `Hit.Emit(Payload)` — and the
field's own `Signal` is left alone. This is not a Verse workaround: C# reached the same split, for
the second reason (§5).

## 3. What `listenable` is, and why the first plan was wrong

The first shape considered was `@export_signal` over *any* field implementing `listenable`. It does not
work, and the reason is worth keeping because it is one line of the engine:

    listenable<public><native>(payload:type) := interface(awaitable(payload), subscribable(payload)) {}

`Engine/Plugins/Verse/Verse/Source/Verse/Verse/Verse/Listenable.native.verse`. It is `Await` plus
`Subscribe` and nothing else — **`signalable` is not among its supertypes**, so a `listenable` field
gives the host no way to deliver a Godot emission *into* it and no way to hear one coming *out*.

Two smaller facts finish it off:

- `event(t) := class(event_base_intrnl, signalable(t), awaitable(t))` — no `subscribable`, so a
  plain `event(t)` does not implement `listenable` either. The type that does is
  `subscribable_event_intrnl`, which is `<epic_internal>` and whose own comment says it "should be
  deleted and use event instead once we get event API changes complete." Do not build on it.
- A field whose `Await` and `Subscribe` are the *author's* implementation would register with Godot
  and then never hear it: subscription has to go out through `connect`, not into a local list (§5).

So the attribute marks an `event(t)`, and the host tests the member's class chain for
`event_base_intrnl` rather than for an interface.

**The attribute gates both spellings, not just the new one.** `@export_signal` is what registers a
member with Godot, full stop — a `signal(t)` without it is no more registered than an `event(t)`
without it. What differs is what silence means, and that is the only reason the two types are told
apart in `GetClassSignalsLive` rather than folded into one test:

| member | with `@export_signal` | without |
| --- | --- | --- |
| `event(t)` | registered | not a signal, absent from the list, no complaint |
| `signal(t)` | registered | listed and refused, `VH_SIGNAL_NEEDS_ATTRIBUTE` |

An event is useful purely between Verse tasks, so opting out is an ordinary thing to mean. A
`signal(t)` has no purpose but Godot, so opting out is far likelier to be a forgotten line — and the
alternative to the warning is a Node panel that is empty for no stated reason, which is the failure
this whole rejection pass exists to remove. The reject sits after `NO_GODOT_OWNER`, `IS_VAR` and
`NOT_PUBLIC` and before the payload reasons: an attribute is the wrong edit to suggest for a member
with no Godot owner, and a payload complaint about a member Godot was never told about is noise
before the edit that matters.

**The attribute is `@export_signal`, and `@signal` is unavailable rather than unwanted.** The
attribute package shares `/Godot.org/Godot`'s verse path so that a script's existing
`using { /Godot.org/Godot }` reaches it, which puts a bare marker attribute's *class* in the same
module namespace as `signal(t)` and its `signal() := signal(tuple())` alias. A third definition of
that name is glitch **3532** — *"The class ... is ambiguous with these definitions: function
`sig(:t)`, function `sig()`"* — measured in `tests/verse_probe/attribute_name_probe.verse`. There is
no second spelling to fall back on: `@export_category` can be a `<constructor>` function beside
`export_category_attribute` only because it takes an argument, and a bare marker has none.

`@export_signal` was chosen over renaming `signal(t)` out of the way. It joins the nine `@export*`
attributes an author already writes, and it says what it does — the member exists either way, and
the attribute is what sends it to Godot, exactly as `@export` sends a member to the inspector.
**`signal(t)` is therefore not renamed at any stage**, which retires the `engine_signal(t)` idea in
§7 and leaves Stage 4 with nothing to rename.

## 4. What is settled by measurement

Every row below was run, not read. The probe fixtures stay so each can be re-run.

| question | answer | where |
| --- | --- | --- |
| Is `event(t)` nameable and constructible from a script package? | Yes | `event_probe.verse`, `event_resume.verse` |
| Does a `<suspends>` body await one and get `t` back typed? | Yes | `event_probe.verse` Q1 |
| Can a `<transacts>` body call `Ev.Signal`? | **No** — glitch 3512 | `event_probe.verse` Q2 |
| Can a specifier-less body? | Yes | `event_probe.verse` Q3 |
| Can a `<suspends><transacts>` body await? | **No** — glitch 3512 | `event_resume.verse` Q1 |
| Does a task suspended on an event resume on a later call? | Yes | `event_resume.verse` |
| Is `listenable(t)` implementable from a user package? | Yes, `<override>` on both methods | `signal_listenable_probe.verse` Q1 |
| Is such a class usable *as* the interface, and awaitable through it? | Yes | `signal_listenable_probe.verse` Q1a, Q1b |
| Does a parametric extension method on a native parametric class work? | Yes | `signal_listenable_probe.verse` Q2 |

The last row also carries a spelling worth not re-deriving: `where t:type` goes **inside the
receiver's parens**, as `/Verse.org/Verse` spells its own —

    (Ev:event(t) where t:type).Emit<public>(Val:t)<transacts>:void

Written after the receiver instead, it is glitch **3502**, "Can't access a data definition's value
from a preceding expression," reported at the receiver's type rather than at `t`. `Array.native.verse`
line 17 and `ClassifiableSubset.native.verse` line 124 are the precedents, the second being the
parametric-class case.

**Still unmeasured, and the first thing to assert when implementing:** whether
`SignalPayloadType` finds a populated substitution table on an `event(t)` member. `signal(t)` is
declared in `/Godot.org/Godot`; `event(t)` is `/Verse.org/Verse`, which is read from a 32 KB digest
after the first build. The instantiation is created while analysing the script's own member, so it
should be there — but a null answer does not fail, it produces an empty shape, which registers the
signal with **zero arguments** and emits nothing. The first fixture is a struct payload, and the
assertion is the registered argument count and names, not that the signal exists.

## 5. What C# does, and what of it is reachable

Godot's C# binding is the closest prior art, and it is closer than expected. `[Signal]` targets a
**delegate declaration** (`modules/mono/glue/GodotSharp/GodotSharp/Core/Attributes/SignalAttribute.cs`),
and `ScriptSignalsGenerator.cs` turns it into four things: a cached `StringName`, a private
`backing_X` delegate with a public `event` over it (`:262-283`), a `protected void EmitSignalX(...)`
wrapper calling `EmitSignal(SignalName.X, [...])` (`:287-325`), and a `RaiseGodotClassSignalCallbacks`
override the engine calls to deliver an emission.

Four things it confirms:

- **The emit verb belongs to the binding.** C# does not make raising the C# event emit the signal; it
  generates a separate method that calls the engine. Same decision as §2, arrived at without Verse's
  effect system being involved.
- **Attribute plus a type declaration is the proven shape.**
- **Per-parameter rejection is the right granularity.** C# refuses a non-void return, `ref`/`out`
  parameters and unmarshalable parameter types, each at the parameter's own location (`:152-173`).
  `vh_signal_desc.Reject` is the same idea; a Verse tuple element has no location of its own, so the
  member's line is as close as this can get.
- **The bypass hazard is an accepted cost, not a Verse peculiarity.** Inside the declaring class,
  `backing_X` is invokable and would run subscribers without telling Godot. The generator's own
  comment says so and has not resolved it: *"This can confuse users. Maybe we should directly connect
  the delegates, as we do with native signals?"* Our equivalent is a direct `Ev.Signal(5)` from a
  specifier-less body, and §9 accepts it the same way.

**Two mechanisms, deliberately.** C# uses a local delegate list for declared signals and a real
per-handler connection for engine signals — `add => Connect(SignalName.X, ...)` /
`remove => Disconnect(...)`, `modules/mono/editor/bindings_generator.cpp:3305-3321`. That is the same
asymmetry §7 chooses, shipped in another binding for the same reason.

**What is not reachable: dispatch without connections.** C#'s local list works because
`RaiseGodotClassSignalCallbacks` exists on the instance. The GDExtension script-instance vtable has
no equivalent — `classdb_register_extension_class_signal` registers a signal on an extension *class*,
not a delivery path into a script instance. So `Subscribe` has to `connect` a Callable per subscriber,
and that is a constraint rather than a choice.

**One thing it reverses.** An earlier sketch had `@export_signal("health_changed")` renaming the signal for
Godot's benefit. C# registers under the member's own spelling — the delegate name minus the mandatory
`EventHandler` suffix, PascalCase preserved, no snake_case conversion — and GDScript connects with
that spelling. `@export_signal` is a bare marker with no argument, which is also what R-SIG-1's
"no second place to spell it" rule already asked for.

**One thing it does better and we cannot copy.** C# declares the payload as a parameter list, so
parameter names reach the registered signal (`ScriptSignalsGenerator.cs:476`) and the connect dialog
shows `newHealth`. A Verse payload is a type argument, and a tuple cannot name its elements, so a
tuple payload gets `Arg0`/`Arg1`. The existing answer is a struct payload (§6), and the conclusion is
a documentation one: make the struct the *default* advice for any signal with more than one argument
rather than the advanced option.

## 6. The struct payload is the part that changes least

`DescribePayload` (`HostScript.cpp:5567`) takes a payload **type**, not a signal. Its three branches —
Tuple, Struct, Bare — key off what that type is and know nothing about how the member was declared.
So the named-argument mechanism survives untouched:

| direction | mechanism | under `@export_signal` |
| --- | --- | --- |
| registering | `CollectStructFields` → one argument per field, named by the field | unchanged |
| emitting | `EPayloadShape::Struct` reads each field by decorated key | unchanged; only the shape *lookup* changes |
| delivering to an awaiter | host rebuilds the struct against the recorded shape, signals the event | shorter — the event is the member, not a field of one |
| invoking a handler | `InvokeCallback` ends at `InstanceCall`, so N arguments satisfy one struct parameter | unchanged |
| export | sidecar records the shape per member name | unchanged |

The nested-struct refusal (`VH_SIGNAL_PAYLOAD_NESTED_STRUCT`, one level and no more) stays where it
is and still reaches `_validate` at the member's line.

**One asymmetry the struct advice has to name.** At runtime a struct payload is loose: `InstanceCall`
packs N Godot arguments into one struct parameter, so an editor-made connection to
`OnHit(Damage:int, By:string)` works. At compile time `Subscribe(Callback(:t):void)` with a struct `t`
type-checks against one struct parameter, and a two-parameter function satisfies a one-*tuple*-parameter
callback but not a one-*struct*-parameter one. So `Hit.Subscribe(OnHit)` needs `OnHit(P:hit_payload)`.
True today, unchanged by any of this, and exactly the seam the "use a struct for names" advice walks
into.

## 7. Engine accessors stay on `signal(t)`

The 503 generated engine-signal accessors answer a `signal(t)`:

    Timeout<public>()<transacts>:signal(tuple()) = signal(tuple()){Id := VhSignalBind(Handle, "timer", "Timeout", "timeout")}

They are **not** moving, and the decision is recorded here because it looks like an inconsistency:

- A script's own signal always has a receiver, so one connection per member for the instance's life
  is the right model. An engine signal on a foreign object has no receiver until someone asks, so
  connect-while-awaiting is the right model. Two jobs, two mechanisms — which is what C# ships (§5).
- `event(t)` has no field to carry an id, and the class cannot be reopened to add one, so an accessor
  could not construct the event it answers. The host would have to cache one per `(object, signal)`
  and hand it back through `any` plus a cast, making every accessor failable or giving it a silent
  dead-event fallback.
- A permanent connection per accessed pair replaces today's connect-only-while-awaiting, so every
  emission of every accessed signal would cross into the VM forever, awaiters or not. Unmeasured, and
  not worth measuring for this.
- It keeps `CollectEngineSignalTypes` and `BindEngineSignal` — the path that makes
  `Timer.Timeout().Await()` work in an export — entirely untouched.

What closes the interop gap instead is **`signal(t)` implementing `listenable(t)`**, which §4 shows
is possible: `signal.Subscribe` already matches `subscribable.Subscribe` exactly, and
`Await()<suspends>:t` matches `awaitable`. Adding the interfaces gives engine signals the same
interop without touching the connection model. `GodotApi.native.verse`'s "shaped like `listenable`
rather than implementing it" comment argues from `signalable.Signal` being `no_rollback`, which is
true and does not reach `listenable`, because `listenable` does not extend `signalable`.

**The decision stays reversible.** `Timer.Timeout().Await()` reads identically under both designs, so
moving the accessors later is a generator and host change. The exception is a script that stores an
accessor's result by type (`Sig:signal(tuple()) = Timer.Timeout()`), which is rare.

## 8. The staged plan

Each stage is shippable and testable on its own.

**Stage 0 — probes. Done**, §4.

**Stage 1 — `signal(t)` implements `listenable(t)`. Done.** The interop win, independent of
everything below. The one variable §4 did not cover was whether a class whose super is the native
`vh_signal` may also carry interfaces; it can, and `tests/verse_probe/signal_listenable_probe.verse`
Q3/Q4 assert both a declared signal and an engine accessor satisfying the interface, including as an
argument to a function that has never heard of Godot.

**Stage 2a — the attribute, required on both spellings. Done.** `export_signal` in
`AttributePackageSource`; `IsEventClass` beside `IsSignalClass`; `GetClassSignalsLive` gating on the
attribute and listing a bare `signal(t)` with `VH_SIGNAL_NEEDS_ATTRIBUTE`; the sentence on both
sides of the ABI; `VH_ABI_VERSION` 10.0 → **10.1** for the new enumerator, which is a minor by the
header's own policy. The 19 existing declarations carry the attribute, and
`tests/verse_probe/export_signal_probe.verse` records the analysis half of both spellings.

*This is where §4's open question closed.* An `event(strike_report)` member reports **two arguments
named `Damage` and `By`**, so the type-variable substitution reads fine off a digest-loaded
`event(t)` and the struct branch of `DescribePayload` is reached through an event exactly as through
a signal. That was the risk that could have sunk the design, and it is measured rather than assumed.

**Stage 2b — the `event(t)` runtime half. Done.** `BindSignals` casting to `verse::event` when the
member is not a `vh_signal`; `VhEventEmit`/`VhEventSubscribe` taking the event through `any`, with
`Emit` and `Subscribe` as parametric extension methods over them; **one Godot connection per
`@export_signal` event member**, held for the instance's life; a
`GEventBindingIds` table keyed by the event object, which is what `event(t)` has instead of the `Id`
field `vh_signal` carries; `ReleaseInstance` dropping the binding, the callback row, the reference
and the strong pointer together; and `CollectDeclaredTypes` recording the shape for a runtime host.
Nine cases in `tests/integration`, all running in an export too.

**Where that connection can be made is not a free choice, and two plausible answers are both wrong.**
Not inside `vh_instantiate`: the consumer builds the Verse object *before* it installs the script
instance on the node, and `Object::has_signal` answers off the installed instance, so Godot refuses
with *"Attempt to connect nonexistent signal"*. Not at the end of the consumer's `create()` either —
the object does not hold the script instance until `_instance_create` has **returned** to Godot,
which is later still and not a point this side can name. Both were built and both failed the same
way, and the failure is silent in the worst possible shape: the member registers, emissions still
reach Godot, and only delivery *back into the event* is missing, so every await on it hangs.
`ConnectDelivery`'s failure is now reported rather than ignored, which is how the second attempt was
caught in one run instead of by reading.

What works is **the first entry into the instance** (`EnsureEventConnections`, from `InstanceCall`).
It is late enough because a call into the instance is Godot dispatching to an installed script
instance, and early enough because a Verse awaiter can only exist after Verse code has run on the
object — and running Verse code on it *is* an entry. The one ordering left uncovered, Godot emitting
before any Verse code runs on that node, has nothing on the Verse side to deliver to.

*§10's warning collected immediately, which is worth recording rather than quietly fixing.* Two of
the three call sites were moved and `CollectDeclaredTypes` was not, so the editor run passed all
nine cases and the **exported** run failed exactly the two with a struct payload: a runtime host has
no semantic program, reads its shapes from that table, found none for an event member, and emitted a
payload of nothing. Nothing in the editor could have caught it — the failure is only reachable where
the analysis is absent, which is the whole reason the export layer exists.

*The permanent connection belongs to this stage and only to this form.* An earlier draft of this
plan had it as a separate, "behaviour-preserving" stage applied to `signal(t)` members too. That was
wrong twice over: it is not behaviour-preserving — `tests/integration/test_cases.gd:2038-2048`
asserts that a declared signal's connection appears for the duration of a wait and is **gone again
after**, which R-SIG-5 states as a property — and it buys nothing, because `signal.Await` is the
bridge's own method and `VhSignalAwait` is exactly the hook a permanent connection would be working
around. A bare `event(t)` is the only case with no such hook, because its `Await` is Verse's native.
So `signal(t)` members keep connect-while-awaiting for good, alongside the engine accessors, and
only `@export_signal` members trade it for a connection that lives as long as the instance.

**Stage 3 — make it the spelling. Done.** Every fixture and both of `dodge-the-creeps`'s declarations
are `event(t)` now, and a `signal(t)` member draws a deprecation warning at its own line.

*The warning is not a `Reject`, and that decided how it travels.* A reject means "not registered
with Godot"; this member registers, emits, connects and is awaited exactly as before, so a reject
would be a lie. `vh_signal_desc` has no room for a second kind of complaint either — it is handed
back as an array, so growing it changes the stride an older consumer indexes by, which is what made
9.0 a major for `IsNamed`. What it uses instead is the diagnostic channel the compiler's own
warnings travel: a `VH_SEVERITY_WARNING` with the member's file and line, emitted from the snapshot
pass, which the consumer files into `compiler_warnings_by_path` for the gutter and
`log_build_diagnostics` pushes to the log for a test to read. Two reporters, one message, no ABI
change. `FSignalDesc` carries a host-side `bLegacyType` and `DeclaredIn` to make it; neither
crosses.

*What the port cost, which is the part worth knowing before doing it again.* Nine connection-count
assertions moved, all by exactly one, because an `@export_signal` event member holds a connection of
its own. `test_cases.gd` names that term `OWN_CONNECTION` rather than burying a `+ 1`, since the
whole point is that it is the member's and not the case's.

**Stage 4 — `signal(t)` keeps one job**: the engine accessors. It stops being something a script
declares.

## 9. Accepted costs

- **`Ev.Signal(5)` from a specifier-less body bypasses Godot.** Verse awaiters resume, Godot and
  GDScript hear nothing. There is no diagnostic for it — it is a legal call on a stdlib class, and
  catching it would mean scanning bodies. It is also legitimately useful on a *non*-`@export_signal` event.
  Documented in the `.verse` template rather than diagnosed, and C# carries the same hazard (§5).
- **Two declaration types during Stages 3–5**, which is what a staged port costs.
- **`signal(t)` survives** as the engine-accessor type (§7), keeping its name. Renaming it to say so
  was considered and rejected with the attribute's name (§3): `@export_signal` costs nothing and a
  rename costs every script that names the type.

## 10. What to be careful of when implementing

`IsSignalClass` has five call sites and only three of them move. This is the `variant` and `rid`
lesson applied to a new type: a type the bridge treats specially has to be claimed in every
classifier, or two of them quietly disagree and both say something useless.

| site | what it does now |
| --- | --- |
| `GetClassSignalsLive` | gates on `@export_signal`; accepts `signal(t)` and `event(t)`, refusing the first without it |
| `BindSignals` | casts to `verse::event` when the member is not a `vh_signal`, and connects once |
| `CollectDeclaredTypes` | records the payload shape for both, for a host with no semantic program |
| `CollectEngineSignalTypes` | **unchanged** — engine accessors stay `signal(t)` |
| `BindEngineSignal` | **unchanged** |

The failure mode if one is missed is a signal that registers and whose payload comes back empty, so
the assertion is the argument count and the names, never that the signal exists. `CollectDeclaredTypes`
was in fact the one missed, and §8's Stage 2b entry records what that cost and why only the export
layer could see it.
