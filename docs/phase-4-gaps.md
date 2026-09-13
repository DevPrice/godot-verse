# Phase 4a — where the implementation and the design disagree

**Status:** 2026-09-13 · **the record of what Phase 4a actually built**, against what
[`phase-4-design.md`](phase-4-design.md) said it would. Every claim below was checked against the
code or measured by running it; none is recalled. Where a previous document said something that
turned out to be false, that is noted and the document has been corrected.

**Seven entries have since been closed** — G1, G2, G3, G4, G6, G12 and G21 — and G9 was answered
with a "do not build this". Closing them took the ABI to **v5**. G21 was raised *and* half closed in
the same pass: G1 exposed it, and the half it was actually about turned out to need no ABI change at
all. What is left of it is R-LANG-2, a MUST that has been at `part` since Phase 2. Each closed entry keeps its original diagnosis below its **Built** note, because the
diagnosis is the part worth re-reading: §0 is why.

**Companion to:** [`phase-4-design.md`](phase-4-design.md) (the design, and §13's short note pointing
here), [`spec.md`](spec.md) (per-requirement status), [`by-hand-checklist.md`](by-hand-checklist.md)
(what no headless run can see).

**Who this is for.** Someone picking Phase 4 back up with no memory of building it. The phase is
green — `tools/run_tests.py` is 7/7 with 201 integration cases and the yardstick's 30 headless
checks pass — so nothing here is a broken build. What is here is the difference between "the tests
pass" and "the design was delivered", which are not the same thing and were not the same thing at
the end of this phase.

---

## 0. Read this first: reason less, probe more

Two of the entries below were written down *wrong* the first time, and both were wrong in the same
way — I reasoned about what the code would do instead of running it, and the reasoning was
plausible and false.

- **G1** was recorded as "a struct payload maps to one argument, not one per field" and dismissed as
  cosmetic. Probing it took two minutes and showed it registers a bogus signal and emits nothing.
- **G12** was recorded in a source comment as "`gen_verse_api.py` reports each as a skip with a
  reason". It does not, and never did.

`tests/verse_probe` exists precisely for this (see `CLAUDE.md`), and adding a temporary fixture to
`tests/integration` and reading what Godot prints costs one test run. **Before you write down what
something does, make it do it.**

Applied before the fact rather than after it, twice, while closing the entries above — and both
times the reading changed the plan:

- **G1** looked finished once a struct payload reached GDScript as named arguments. Reading
  `InstanceCall`'s arity check instead of assuming the inbound path was symmetric found **G21**: N
  Godot arguments against a one-parameter Verse handler is `VH_ERR_ARGUMENT`, from a call the author
  never wrote. Half a feature, shipped, would have been a *new* accepted-but-broken entry.
- **G9** said to copy Epic's `FVerseEventCallbackList`. Reading it found that the rule drops
  callbacks on *termination* and that UEFN never revives a terminated scope — so copying it into a
  host with one revived process-wide scope would have regressed the very bug the revive fixed.

---

## 1. The gaps, at a glance

Sized as **S** (an afternoon), **M** (a day), **L** (more, or needs a decision first).

| id | gap | kind | size | state |
| --- | --- | --- | --- | --- |
| **G1** | a struct signal payload compiles, registers a bogus signal, and emits nothing | accepted-but-broken | M | **closed** |
| **G2** | a `var` or non-`public` `godot_signal` member is silently absent from the signal list | accepted-but-broken | S | **closed** |
| **G3** | a `godot_signal` on a class never bound to a handle is only reported at emission | accepted-but-broken | S | **closed** |
| **G4** | a payload the wire cannot carry is refused at *emission*, not at the member | accepted-but-broken | S | **closed** |
| **G5** | `_make_function` and `_can_make_function` are stubs — R-SIG-4's editor half | unbuilt | M | open |
| **G6** | the thread guard covers 2 of 31 entry points | unbuilt | S | **closed** |
| **G7** | `godot_signal()` has no zero-argument alias | unbuilt | S | open |
| **G8** | `godot_array` has no `AddObject` | unbuilt | S | open |
| **G9** | a callback does not remember its `FContentScope` | unbuilt | M | open |
| **G10** | `@statics` emits neither of the two diagnostics the design promised | unbuilt | S | open |
| **G11** | 106 of 114 `@GlobalScope` utilities are undispatched | unbuilt | L | open |
| **G12** | 367 math methods and 261 operators are absent with nothing recorded | unbuilt | M | **closed** |
| **G13** | math exists for 4 of 16 types; `snapped`, `min`/`max`, `floor`/`ceil`/`round` unwritten | narrower | M | open |
| **G14** | `GetClassOf` answers only the Godot class | structural | — | as built |
| **G15** | there is no `godot_callback` native class; a callback is `(handle, decorated name)` | structural | — | as built |
| **G16** | `vh_signal` is non-parametric; the payload lives in a host-side table | structural | — | as built |
| **G17** | the callback native takes `any`, not a typed function parameter | structural | — | as built |
| **G18** | `@statics` names its class as a string, not an identifier | structural | S | as built |
| **G19** | no behavioural test for `_CanDropData` / `_HasPoint` | test gap | S | open |
| **G20** | R-EXP-5's body overstates; R-AUD-2 never edited | doc | S | open |
| **G21** | a user struct cannot cross the wire *inbound*, so a Verse handler cannot take one | found closing G1 | M | **closed for signals**; the rest is R-LANG-2 |

**G14–G18 are working as built and are not bugs.** They are here so a fresh reader does not
"restore" them to the design's shape without knowing why they differ.

**The closed entries shipped together** and the ABI went to **v5** with them — `vh_signal_desc`
grew three fields, which is a layout change, so both sides must be rebuilt (`tools/run_tests.py
--build`, or `host_smoke` refuses at `vh_init` with no useful sign of why). `tools/run_tests.py` is
7/7 and the yardstick's 30 headless checks pass. What each one actually built is under its own entry.

---

## 2. Accepted-but-broken — **all four closed**

The worst category: a script writes something the compiler accepts, and it does not work. All four
are signals, all four had the same fix shape, and the design already specified it — §6.2's *"a
payload the wire cannot carry is refused **at the member**, reusing R-EXP-3's machinery rather than
failing at the emission."* That sentence answered G1, G3 and G4 together, and G2 belonged with them.

**What shipped, once, for all four.** `vh_signal_desc` carries a `Reject` and a `RejectDetail` the
way `vh_export_desc` carries its own (ABI v5), filled by one validation pass in `GetClassSignals`.
`VerseScript::_get_script_signal_list` and `_has_script_signal` drop a rejected signal, so Godot is
never told about one nothing can emit; `refresh_script_warnings` — renamed from
`refresh_export_warnings`, since it now covers both — turns the reason into a warning at the
member's own line. The binding keeps its reject too, so emitting or subscribing to a refused signal
reports *its* reason rather than the generic "names nothing", which is the only report a build
running outside the editor gets.

The five reasons, all decidable from the declaration:

| `vh_signal_reject` | the declaration | was |
| --- | --- | --- |
| `IS_VAR` | a `var` member | G2 |
| `NOT_PUBLIC` | not `<public>` | G2 |
| `NO_GODOT_OWNER` | a class that does not derive from `object` | G3 |
| `PAYLOAD_UNSUPPORTED` | an argument with no Godot type | G4 |
| `PAYLOAD_NESTED_STRUCT` | a struct payload whose field is itself a struct | G1 |

Tests: `tests/integration/scripts/signal_rejects.verse` (the four member-level refusals, plus one
good signal on the same class so the pass is shown to reject individually rather than wholesale) and
`signal_no_owner.verse` (the class with no base, which needs a file of its own because the thing
being tested *is* the base). The *editor* sentences need a window and are on
[`by-hand-checklist.md`](by-hand-checklist.md); the runtime ones each have an emitter, because "it
was rejected" and "it was rejected for the right reason" are different claims and only the second
helps an author. As of this pass they read:

```
The signal `Unseen` was never registered with Godot: a `godot_signal` member must be
  `<public>` for anything outside the class to connect to it. Nothing was emitted.
The signal `Reassignable` ...: a `godot_signal` member must not be `var`.
The signal `Nested` ...: its payload field `Inner` is itself a struct, and a payload
  decomposes one level only.
The signal `Maybe` ...: its payload argument `Value` has no Godot type.
```

### G1 — a struct signal payload does not work · **closed, and it found G21**

**Built:** `DescribePayload` has a struct branch. A `CClass` whose `IsStruct()` is true and which has
no generated `FStructLayout` — that second test is what keeps the sixteen math types *out*, since a
`vector2` payload is one Vector2 argument and not two floats — decomposes into one argument per
top-level field, named by the field, walking the struct's own inheritance chain base-first. Each
field's decorated key (`(/user@localhost/strike_report:)Damage`) is computed once at description
time and stored on the binding, so the emission reads fields by name rather than reconstructing
paths. `EmitSignal` grew a `Struct` arm beside its tuple and bare ones.

One level only, which is what "decompose flat only" was chosen to mean: a field that is itself a
user struct is `PAYLOAD_NESTED_STRUCT` at the member, because Godot has no argument shape for a
struct and silently dropping the field would be the same accepted-but-broken failure in a new place.

**What this exposed — G21, since closed.** Shipping the outbound half alone would have left the
inbound one broken: Godot invokes a handler with N arguments, a Verse `Subscribe(Callback(:t))` takes
the payload as one value, and `InstanceCall`'s arity check answers `VH_ERR_ARGUMENT` from a call the
author never wrote. It was refused explicitly for one commit and then built; **G21** has the
mechanism. A Verse handler takes the struct as one value now, so both halves of §6.2's row are real.

**Original diagnosis, kept because the method is the point:**

**Design:** §6.2's fourth row. A `godot_signal(my_struct)` should report **one Godot argument per
top-level field**, named by the field, so the connect dialog and `_make_function` get real names;
the Verse subscriber receives one value and reads `P.Damage`.

**What happens** (measured, by adding a `godot_signal(probe_payload)` to
`tests/integration/scripts/signals.verse` and emitting it):

```
[probe] Struck2 args = [{ "name": "Value", "class_name": &"", "type": 0, ... }]
[probe] handler saw: <null>
ERROR: The payload of signal `Struck2` has no representation on the Godot wire, so nothing was emitted.
```

One argument named `Value` of type NIL, and **no emission at all**.

**Why.** `DescribePayload` (`host/Private/HostScript.cpp`) has two branches: a tuple decomposes, and
everything else becomes one argument described by `DescribeType`. `DescribeType` fills
`FMemberType::Struct` from `FindStructLayout(name)`, which is a lookup into the **generated**
`verse_math::MathLayouts` — Godot's own sixteen math types. A user struct is not in it, so `Struct`
stays null, the descriptor carries no type, and `ValueToWire`'s struct branch is
`if (!Declared.Struct) return false`.

**Why it reads as cosmetic in the design.** §6.2 presents the row as a *trade* — "a tuple keeps the
ergonomic N-parameter handler and loses names in the editor; a struct gets names in the connect
dialog" — which sounds like two working spellings and a preference. It is not.

**To fix.** A Verse struct is a `uLang::CClass`; `GetDefinitionsOfKind<CDataDefinition>` walks its
fields, which is exactly what `GetClassSignals`' sibling `GetClassExports` already does for a class.
So:

1. `DescribePayload` grows a struct branch: detect a `CClass` whose `EStructOrClass` is `Struct` and
   that is not a mirrored math type, and describe each field with `DescribeType`.
2. `FSignalBinding` carries the field *names* alongside `ArgTypes` (it only carries types today).
3. `EmitSignal` reads the `VValueObject` field by field — `ReadStructValue` already does this for
   math structs, driven by an `FStructLayout`; this wants the same loop driven by the field list.
4. An integration case: a struct payload reaches GDScript as named arguments. This is §6.9's case 8,
   whose tuple half is tested and whose struct half is not.

**Or** close it as G4 does, by refusing it at the member — which is cheaper and strictly better than
today, but loses the feature.

### G2 — a `var` or non-`public` signal member is silently absent

**Design:** §6.6. *"A `godot_signal` member must not be `var`, and must be `<public>` to be
registered. A private one is a diagnostic rather than a silently absent signal."*

**What happens.** `GetClassSignals` checks neither. It collects every data member whose declared
type reaches `vh_signal`. A `var` one would be bound and emitted through a reference cell, which is
untested; a module-scoped one is registered with Godot anyway, which is the opposite of the design's
rule.

**To fix.** `CDataDefinition::IsVar()` and the member's access level are both on hand in
`GetClassSignals` — `GetClassExports` reads `IsVar()` a few lines away. Refuse and report.

### G3 — a signal on a never-instantiated class is reported late

**Design:** §6.6. *"A class with no Godot object has nowhere to bind, so a `godot_signal` member on a
class that is never instantiated against a handle is a diagnostic at the member."*

**What happens.** `BindSignals` reports `"The signal ... could not be bound"` — but only when an
instance is actually made, and through the runtime error channel. A class that is never attached to
a node says nothing at all.

### G4 — a bad payload is refused at emission

**Design:** §6.2. *"A payload the wire cannot carry is refused **at the member**, reusing R-EXP-3's
machinery rather than failing at the emission: the rejection reasons already spelled in
`vh_export_reject` answer the same question about the same lanes."*

**What happens.** `EmitSignal` reports it at the first emission, at runtime, after the signal has
already been registered with Godot.

**To fix (G2–G4 together).** One validation pass in `GetClassSignals` that returns a per-signal
*reject reason* alongside the descriptor, in the shape `vh_export_desc` already uses for
`vh_export_reject`. `VerseScript::_get_script_signal_list` then drops rejected signals, and
`_validate` turns the reason into a sentence at the member's own line — which is machinery
`src/verse_script_language.cpp` already has for exports (R-SCN-2, R-EXP-3). **This is the single
highest-value item in this document**: it converts three runtime surprises into editor diagnostics
and costs one pass.

---

## 3. Unbuilt

### G5 — `_make_function` is a stub (R-SIG-4's editor half)

**Design:** §6.8. Connecting through the Node panel with "Make Function" checked should write a
handler into the script with parameters spelled from the same descriptors `vh_class_signal_list`
reports:

```
	OnStartButtonPressed<public>():void =
		# TODO
```

**What is there.** `VerseScriptLanguage::_can_make_function` returns `false` and `_make_function`
returns `String()` (`src/verse_script_language.cpp`). Untouched by the phase.

**Note the spec understates this.** R-SIG-4 says the editor flow is "untested". It is *absent*.

**To fix.** Phase 3 established that a GDExtension can write into the editor's `CodeEdit`
(`ScriptEditor::get_current_editor()->get_base_editor()`), and R-TOOL-12 already writes a `using`
line that way. The argument names come from `class_signals()`, which `VerseScript` already caches.
The indentation must be tabs (Godot's editor writes tabs, and Verse rejects mixed tabs and spaces).

### G6 — the thread guard covers 2 of 31 entry points · **closed**

**Built:** 26 more entry points carry the two-line prologue; three do not, and each says why where it
is defined — `vh_abi_version` (a consumer calls it *before* `vh_init`, so there is no recorded thread
to compare against, and it reads a compile-time constant), `vh_init` itself, and
`vh_callback_release`.

**`vh_callback_release` was the one that needed deciding, and deciding it found a real race.** It
stays unguarded, because a Godot `Callable` is destroyed on whatever thread dropped its last
reference and refusing that would leak the row rather than protect anything. But "releasing only
touches a map" was never a reason it was *safe*: `TMap::Remove` from an arbitrary thread against
`Find` and `Add` on the game thread can rehash and free under the reader. `GCallbacks` is under an
`FCriticalSection` now, and `InvokeCallback` copies its `FCallbackTarget` out under the lock instead
of holding a pointer into the map across a call that runs Verse.

Four entry points return `vh_bool` and have no error value, so a refused call answers `0` — which
reads as "no such class" rather than "refused". The diagnostic carries the difference. Recorded
rather than fixed: widening them is a major ABI change and nothing has needed it.

The GDExtension-side check the design also asked for ("so the message can say which node it came
from") is still absent.

**Original entry:**

**Design:** §5.1. *"Record the `vh_init` thread at init, compare at **every entry point**, answer
`VH_ERR_THREAD` having run nothing."* Also: *"The GDExtension can check on its own side too, so the
message can say which node it came from."*

**What is there.** `WrongThread()` guards `vh_instance_call` and `vh_callback_invoke`. There are 31
`extern "C"` entry points in `host/Private/VerseHost.cpp`. `vh_tick`, `vh_compile_project`, the
field accessors, and every list call are unguarded. The GDExtension-side check does not exist.

**Why it is not as bad as it sounds.** The two guarded ones are the two that *execute Verse*, which
is what `ensure(IsInGameThread())` in `VVMEnterVMInline.h` is about. The unguarded ones mostly read
the semantic program. But `vh_tick` runs the VM and `vh_compile_project` publishes a generation, so
the guard is incomplete on its own terms.

**To fix.** Add `WrongThread(...)` to each; it is a two-line prologue. Decide deliberately whether
`vh_callback_release` stays unguarded — it is deliberately unguarded today, because a `Callable` can
be destroyed on whatever thread dropped the last reference and releasing only touches a map.

### G7 — `godot_signal()` has no alias

**Design:** §6.1. *"`godot_signal()` is the alias for `godot_signal(tuple())`, the way `listenable()`
is for `listenable(tuple())`."*

**What is there.** Only `godot_signal(t:type)`. Every zero-payload declaration in the repo spells
`godot_signal(tuple())`, twice per line:

```
	Hit<public>:godot_signal(tuple()) = godot_signal(tuple()){}
```

**To fix.** Find out how `/Verse.org/Concurrency` spells `listenable()` — it is in
`Engine/Plugins/Verse/Verse/Source/Verse/Verse/Verse/Listenable.native.verse` — and copy it. It is
cosmetic but it is on the line every author writes.

### G8 — no `AddObject` on `godot_array`

**Design:** §4. *"`AddInt`, `AddFloat`, `AddString`, `AddObject`, … symmetric with the existing
`GetInt`/`GetFloat` readers."*

**What is there.** `CONTAINER_ELEMENTS` in `tools/gen_verse_api.py` has Variant, Logic, Int, Float,
String, the four math types, Array and Dictionary — and **no object entry**, so `godot_array` has no
object accessor in either direction. `typed_array(t).Add` covers the typed case, and
`AddVariant` can carry one if the caller can get a `variant`, which R-TYPE-7 says a script cannot.

**To fix.** Adding an object row to `CONTAINER_ELEMENTS` is one line, but check why it is absent
before adding it: the reader half (`GetObject`) would have to answer `object`, and
`element_converters` special-cases `VhFromObject` already (see the comment there about taking the
lane packer's branch and handing back the base `object`).

### G9 — a callback does not remember its content scope · **answered: do not copy it, and here is why**

**Design:** §5. *"Epic's `FVerseEventCallbackList` carries the other half of the discipline and is
worth copying: each callback remembers the `FContentScope` it was subscribed in, and a terminated
scope drops it. That is what keeps a raise (R-DIAG-3) from leaving callbacks that can never run
again."*

**What is there.** Nothing. `FCallbackTarget` is `{OwnerHandle, DecoratedName}` and the host has one
process-wide `GContentScope`. After a raise terminates the scope, `EnterVerse` revives it at the next
`vh_tick` — so in practice the callbacks *do* run again, which is why nothing failed.

**What Epic actually does**, read rather than recalled:

- The list is literally a sparse array of `TTuple<TWeakPtr<FContentScope>, TVerseFunction<...>>`
  (`Engine/Plugins/Verse/Verse/Source/Verse/Public/VerseEvent.h:21`), the scope taken from
  `FContentScopeGuard::GetActiveScope()` at subscribe time (`VerseEvent.cpp:50`).
- The trigger is **termination, not destruction**. `FContentScopeImpl::Terminate()` broadcasts
  `OnContentScopeCleanup` and then `Clear()`s it (`VerseContentScope.cpp:128-137`); each
  subscription's cleanup lambda does `SubscribedCallbacks.RemoveAt(CallbackID)`
  (`VerseEvent.cpp:169-188`). `Signal` re-checks `ShouldExecuteCodeWithThisScope()`, which is
  `!WasTerminated()`, as a second line (`VerseEvent.cpp:83-87`).
- **`ResetTerminationState()` is never consulted by that path.** Its only implementation clears the
  flag and rebuilds the task group; it restores no cleanup delegates and no callbacks
  (`VerseContentScope.cpp:139-146`). Subscription loss is permanent for that scope.
- **UEFN never revives a terminated scope for new work.** `ContentScopeRepository` hands back the
  cached scope only while `ShouldExecuteCodeWithThisScope()`, and otherwise **makes a new one**
  (`VerseEngine/.../ContentScopeRepository.h:80-92`). Scopes are per owner UObject, per entity, per
  world, per Sequencer evaluation — never one per process, except in the standalone host programs,
  of which this bridge is one.

**So the discipline is not separable from the granularity, and copying it here would be a
regression.** Epic can drop every callback in a scope on termination because a scope is one entity or
one world, and because the next subscription gets a *fresh* scope. This host has one scope for the
whole project and deliberately *revives* it — `ReviveContentScope` exists because without it one
script's first raise ended Verse for the process, silently, for a phase. Attaching Epic's rule to
this architecture would mean: any script raises, and every subscription in the project dies for good.
That is strictly worse than today, and it is worse in exactly the way the revive was written to fix.

**The real content of G9 is R-ASYNC-4** — narrowing the blast radius from the project to something
smaller. Get scopes per owner first, and Epic's rule becomes correct *and* free. Until then the
recording would be dead weight: one scope, so every callback would record the same always-live
pointer.

Nothing to build here. **What to do in Phase 5:** implement R-ASYNC-4 first; the callback→scope link
is a consequence of it, not a precursor.

### G10 — `@statics` has no diagnostics

**Design:** §8.4. *"With the attribute, a module naming a class that does not exist — or two modules
naming one class — is a diagnostic."* That was the argument *for* the attribute over a naming
convention.

**What is there.** `GetClassStatics` returns false when the class is absent, which the GDExtension
turns into "no statics". Two modules naming one class both contribute, silently. Neither case
reports anything.

**To fix.** Both checks belong in the same walk `GetClassStatics` already does over the root module's
`CModule` definitions. The reporting channel is `ReportDiagnostic`, with the module's own location
from `FillLocation`.

### G11 — 106 of 114 utilities are undispatched

**Design:** §8.2. *"Two new Godot callbacks — `CallStatic(...)` and `CallUtility(...)` — and the
generator emits Verse free functions and module members over them."* The design expected ~28 to need
dispatching.

**What is there.** `CallStatic` is generic over `ClassDB.class_call_static`, so **all 114 statics
work** with no per-method code. `CallUtility` is a **fixed if-chain of eight** in
`VerseRuntime::api_call_utility` — the random family, which §8.2 names as mandatory. The other 106
are recorded as `utility_not_dispatched` skips in `src/verse_api_skipped.h`.

**Why.** The GDExtension interface offers **no by-name utility call that takes Variants**. The only
route is `variant_get_ptr_utility_function`, which hands back a *ptrcall* wanting typed argument
pointers and a signature hash. godot-cpp binds each utility as an ordinary C++ function instead.

**To fix, if it is worth it.** Generate the if-chain from `extension_api.json` into a
`src/verse_api_utilities.h`, one branch per utility, with a Godot-type → C++-type table in the
generator. Mechanical but new machinery. **Check first**: for most of the 106, Verse's own stdlib is
the right answer under R-AUD-2 and the skip is correct rather than a gap. The ones actually missing
are the `@GlobalScope` names with no Verse counterpart — `print_rich`, `var_to_bytes`, `hash`,
`type_string`, `instance_from_id` and the like.

### G12 — the math tail is absent *and* unrecorded · **closed (the recording half)**

**Built:** `gen_verse_api.py` reads `host/Verse/GodotMath.native.verse`, extracts what it defines —
extension methods from `^\(X:type\).Name`, operators from `^operator'sym'(A:type, B:type)` and
`^prefix'sym'(V:type)` — and records every `builtin_classes` method and operator for a `MATH_TYPES`
entry that is not among them. **585 rows**, 342 methods and 243 operators, against 43 written.

Reading the file rather than maintaining a list is the whole requirement: add a method to `GodotMath`
and its skip disappears on the next generation. The generator test asserts the *negative* property —
a method that is written must not be recorded as absent — because that is the one that rots.

Two details worth keeping:

- Godot files `float * Vector2` under `Vector2`, and the Verse overload serving it is written with
  the float on the left. An operator is therefore recorded under *whichever* side is a math type, or
  every reversed-operand overload would read as missing while being present.
- Resolving one in the editor needed a second path. `ClassDB` has never heard of `Vector2` — it is a
  Variant type, not a class — so `skipped_member_for`'s chain walk answers nothing for `vector2`, and
  it now matches on the Verse class name the skip row already carries.

The sentence is deliberately unlike every other skip's: not "the bridge cannot carry this" but "the
math types are ordinary Verse, this one has not been written yet, and here is the file it goes in".
Anything else would stop someone who could have added it in ten minutes.

**The writing half is still G13.** This closed R-SCN-2's promise over the surface, not the gap.

**Original entry:**

**Design:** §8.3 item 2. *"The long tail … is **recorded as a skip with a reason**, which is
machinery R-SCN-2 already has and which makes the gap say so in the editor rather than being
silently absent."*

**What is there.** Nothing records it. `tools/gen_verse_api.py` enumerates `api["classes"]`; it never
enumerates `builtin_classes[*].methods` or `[*].operators`. So **367 methods and 261 operators** are
absent with nothing said about them. Only the math types' *constants* reach
`src/verse_api_skipped.h` (20 rows), and only because those go through the statics module.

**A comment in `host/Verse/GodotMath.native.verse` used to claim otherwise. It was corrected on
2026-09-13** and now says what is true.

**This is the largest hole left in R-SCN-2's promise** — "every Godot class and every method on it is
reachable, or the reason it is not is reported".

**To fix.** Read the names `GodotMath.native.verse` defines — the extension methods match
`^\(X:type\)\.Name`, the operators `^operator'…'\(X:type` — and record every `builtin_classes`
method and operator for a `MATH_TYPES` entry that is not among them, as `math_not_written`. Reading
the file rather than hand-maintaining a list keeps it honest: add a method to `GodotMath` and its
skip disappears on the next generation.

---

### G21 — a user struct crosses outbound only · **half closed: signals both ways, the rest is R-LANG-2**

**Found closing G1**, by reading `InstanceCall` rather than assuming — §0's lesson applied before the
fact instead of after it.

**It is not an orphan.** It is the unbuilt half of **R-LANG-2**, a MUST at `part` since Phase 2,
whose own text already names the answer: *"a struct member as a Godot struct **or dictionary**"*.
Phase 2 recorded it as a wall; this is that wall found from the other side.

**What was wrong with this entry's first draft.** It listed "decide what a struct looks like on the
wire" as the blocker for all of it. That is the blocker for *half*, and the half it does not block is
the half G21 was about — because Godot delivers a struct payload as **N separate arguments**, so the
signal case needs no Godot representation at all. `Seq.Items` is `const vh_value*` already, so the
tuple lane was never scalar-only either; only the *math* path is, because math structs are.

**Built — no ABI change, no new Godot representation.**

- `FMemberType::UserStruct` holds an `FUserStructLayout`: the struct's decorated name and its fields'
  keys, names and declared types. Set by `DescribeType` where `ReferenceClass` used to be — calling a
  struct a reference is what sent it down the handle path to be refused there.
- One walk fills it, `CollectStructFields`, and `DescribePayload` uses the *same* one. Two walks
  would be two chances to disagree about field order, and a disagreement there is a silent
  mis-assignment rather than an error.
- `WireToValue` grows a user-struct arm beside the mirrored-math one: `FindVClassByDecoratedName`
  (`FindMirroredVClass` without the assumption that the name is Godot's), an archetype of field keys,
  `NewVObject`, then **this same function** per field — so a field can be anything a field can be,
  where a math struct's can only be a scalar.
- `InstanceCall` packs N arguments into one tuple when the method's single parameter is a user struct
  with N fields. Verse already reads a multi-parameter function as satisfying a one-tuple-parameter
  callback for the same reason — a function's parameter *is* its tuple.
- The `SubscribeSignal` refusal is gone. The design's §6.2 sentence — *"the Verse subscriber receives
  one value and reads `P.Damage`"* — is true for the first time.

**The spike that had to come first** was whether a script-package struct's `VClass` is findable by
decorated name the way a mirrored one is. It is: `(/user@localhost:)strike_report` resolves. The
verse path rather than the package name is what decorates it, which is OQ-12's answer doing work
again — a generation's package is renamed on every publish while its verse path stays pinned.

**One claim this pass made and the test refuted.** Packing in `InstanceCall` was supposed to also let
a GDScript caller reach a struct-taking method positionally. It does not: `Object::call` checks arity
against the script's method list, which reports the one declared parameter, and answers *"Expected 1
argument(s)"* without entering the host at all. A Callable invocation is the difference — it arrives
through `vh_callback_invoke`, which Godot does not arity-check. The comment and the test say so now.

**What is left, and it is R-LANG-2's:** a user struct as a method parameter or return value in the
general case, and as an `@export`. That needs the representation decision this entry originally led
with, and the spec has already chosen: **a Dictionary keyed by field name**. It is GDScript's own
idiom for a record, self-describing, and order-independent — where positional would fail exactly the
way reordering a Verse enum silently reinterprets every saved scene.

Keep the two mechanisms separate when that lands. A Dictionary is a *reference* on this wire since
ABI v2, so it costs a ref-table entry per crossing; a signal emitted every frame should keep the
tuple path, which allocates nothing outside the arena.

---

## 4. Narrower than the design

### G13 — math coverage

**Design:** §8.3 names a written list — *"length, normalized, distance, dot, cross, lerp, clamp, abs,
sign, floor/ceil/round, rotated, angle, snapped, min/max"* — across 16 types.

**What is there.** `host/Verse/GodotMath.native.verse` covers **`vector2`, `vector2i`, `vector3` and
`color`**. The other twelve types have **no operators at all** — `vector4`, `rect2`, `rect2i`,
`plane`, `quaternion`, `aabb`, `basis`, `transform2d`, `transform3d`, `projection`, `vector3i`,
`vector4i`. Within the four, missing from §8.3's own list: **`snapped`**, **`min`/`max`**, and
**`floor`/`ceil`/`round`**.

`floor`/`ceil`/`round` have a reason worth keeping: Verse's own answer an `int` and are `<decides>`,
Godot's answer a vector of whole floats, and **Verse has no int-to-float conversion** to bridge them.
Solve that first (or write the conversion) before attempting them.

The rest is ordinary work. Match Godot's edge cases rather than its formulas —
`../godot/core/math/*.h` is the reference, per the memory note about that checkout — and remember
that every extension method name becomes a module-level name (see §5 below).

---

## 5. Structural deviations — working as built, do not "restore"

These differ from the design's *mechanism* and not its behaviour. Each was a deliberate call made
while building; the reasons are here so nobody spends a day reverting one.

### G14 — `GetClassOf` answers only the Godot class

**Design:** §3 — the callback answers a handle's Godot class name *and*, when it has one, the
qualified Verse class name of the script on it.

**Built:** it answers only the Godot class name. The Verse side comes from `GInstancesByHandle`, a
host-side `handle → FInstance*` map filled in `Instantiate` and cleared in `ReleaseInstance`.

**Why:** the host needs the script's *object*, not its name, so it needed that map regardless.
Sending the name too would be a second source of truth for the same fact.

### G15 — no `godot_callback` class

**Design:** §5 — a `godot_callback` native class holds a `TVerseFunction`, is traced by the VM, and
releases through `BeginDestroy` as `godot_ref` does.

**Built:** `FCallbackTarget` is `{int64 OwnerHandle, FUtf8String DecoratedName}` in a plain
`TMap`. Invoking goes through `InstanceCall`, the same path Godot's own dispatch takes.

**Why:** 4a accepts only a method bound to a script instance, and for one of those the pair is
complete. Nothing has to keep a VM cell alive, and argument conversion against declared parameter
types comes free. **If OQ-16 is answered** (unbound callbacks), this is the thing that has to change,
and the design's shape is probably right then.

**One fact this exposed, and it bites elsewhere:** recovering the decorated name compares
**procedures**, not function cells. A method is stored once per shape and `Bind` makes a fresh
`VFunction` on every field load, so two loads of one method are two cells that share their code. See
`DescribeBoundFunction`.

### G16 — `vh_signal` is not parametric

**Design:** §6.3 — `godot_signal(t)` is a `<native>` **parametric** class whose C++ shadow holds the
owner handle and the signal name.

**Built:** `vh_signal` is a non-parametric native class holding one `int64 Id`; `godot_signal(t)` is
ordinary parametric Verse deriving from it (`host/Verse/GodotApi.native.verse`). Owner, name and
payload shape live in `GSignalBindings`, keyed by that id.

**Why:** the same `vh_object`/`object` split, for the same reason — a native class cannot be
parametric, and Verse cannot reopen a class to add the typed API afterwards. **Whether VNI would
accept a parametric native class was never tested**; if someone wants the design's shape, that is
the spike to run first.

### G17 — the callback native takes `any`

**Design:** §5 — a native taking `Callback(:t)<transacts>:void`, marshalled through
`V_MARSHAL_PARAM_FUNCTION`.

**Built:** `VhCallableFrom<native>(Callback:any)`, which arrives as `FVerseValue`; the type check
lives in the parametric Verse wrapper `MakeCallable(t, Callback)` above it.

**Why:** one native has to serve every arity, and a native cannot be parametric. Verified by
building: `any` marshals as `FVerseValue` and `GetValue().DynamicCast<Verse::VFunction>()` recovers
the function.

### G18 — `@statics` takes a string

**Design:** §8.4 writes `@statics(player)`, unquoted.

**Built:** `@statics("player")`. `GetAttributeTextValue` is how uLang hands back an attribute's
argument, and there is no equivalent for a `type` — the unquoted form means walking the attribute's
own AST.

**The property §8.4 was buying survives**: a mistyped association is checkable rather than silently
empty. Except that the check is not implemented — see **G10**.

---

## 6. Test and documentation gaps

### G19 — no behavioural test for the virtuals that gate engine behaviour

**Design:** §7.2 — *"the ones that gate engine behaviour (`_CanDropData`, `_HasPoint`) get a test
each."*

**What is there.** `tests/verse_api_gen/test_gen_verse_api.py` asserts that `_HasPoint` is *emitted*
with a `false` default body. There is no test that Godot acts on the answer — which is what the
design meant, because a wrong default here changes engine behaviour silently.

### G20 — two spec entries that overstate or were not edited

- **R-EXP-5** is `part`, correctly, but its body now says the editor-only virtual surface "came with
  R-NODE-7 … so what was a feature is now a test". No such test was written; `_GetConfigurationWarnings`
  is exercised on a *non-tool* script by direct call. The by-hand checklist carries the real check.
- **R-AUD-2** was never edited to carry the random-family exception, which §11.2 asked for. The
  exception is recorded under R-SCN-3 instead.

---

## 7. What is owed by hand

[`by-hand-checklist.md`](by-hand-checklist.md), all of it. **Nothing on it has been run.** It needs a
windowed editor, which no automated layer in this repo starts. Phase 3's two owed checks are on it
too. **G5** cannot be verified without it.

---

## 8. Already corrected — do not re-report

Found during the review that produced this document, and fixed:

- The `GodotMath.native.verse` header claimed the math skips were recorded. They are not; the comment
  now says so (**G12**).
- `phase-4-design.md` §13 and `spec.md` R-SIG-1 both said a struct payload "crosses as one argument".
  It does not; both now say what it does (**G1**).

---

## 9. A suggested order

Items 1–4 are **done**, struck through, and their entries above say what shipped. What is left:

1. ~~**G2 + G3 + G4** — one validation pass in `GetClassSignals`.~~ Done, with **G1** folded in.
2. ~~**G12** — record the math skips.~~ Done.
3. ~~**G6** — finish the thread guard.~~ Done, and it found a `GCallbacks` race on the way.
4. ~~**G9** — decide before Phase 5.~~ Decided: **do not build it**, R-ASYNC-4 first. See the entry.
5. **G5** — `_make_function`. Needs the by-hand checklist to verify, so it pairs with running that,
   and it is now worth more than it was: a struct payload gives it real parameter names to write.
6. **G13** — the math bodies. Every one written deletes its own skip row, which is a rare shape for
   this kind of work: the measure of progress is generated rather than claimed.
7. **G11**, **G7**, **G8**, **G10**, **G19**, **G20** — ordinary work, in whatever order the next
   phase makes convenient.
8. ~~**G21** — needs an ABI decision before it is ordinary work.~~ The signal half needed no such
   decision and is done. What is left is **R-LANG-2**'s general case — a struct as a method
   parameter, a return value, an `@export` — and the spec has already chosen the Dictionary; it wants
   a phase, not a slot in this list.

`G14`–`G18` need no action unless a requirement changes; read them before touching the code they
describe.
