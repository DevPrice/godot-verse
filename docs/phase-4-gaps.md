# Phase 4a — where the implementation and the design disagree

**Status:** 2026-09-13 · **the record of what Phase 4a actually built**, against what
[`phase-4-design.md`](phase-4-design.md) said it would. Every claim below was checked against the
code or measured by running it; none is recalled. Where a previous document said something that
turned out to be false, that is noted and the document has been corrected.

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

---

## 1. The gaps, at a glance

Sized as **S** (an afternoon), **M** (a day), **L** (more, or needs a decision first).

| id | gap | kind | size |
| --- | --- | --- | --- |
| **G1** | a struct signal payload compiles, registers a bogus signal, and emits nothing | accepted-but-broken | M |
| **G2** | a `var` or non-`public` `godot_signal` member is silently absent from the signal list | accepted-but-broken | S |
| **G3** | a `godot_signal` on a class never bound to a handle is only reported at emission | accepted-but-broken | S |
| **G4** | a payload the wire cannot carry is refused at *emission*, not at the member | accepted-but-broken | S |
| **G5** | `_make_function` and `_can_make_function` are stubs — R-SIG-4's editor half | unbuilt | M |
| **G6** | the thread guard covers 2 of 31 entry points | unbuilt | S |
| **G7** | `godot_signal()` has no zero-argument alias | unbuilt | S |
| **G8** | `godot_array` has no `AddObject` | unbuilt | S |
| **G9** | a callback does not remember its `FContentScope` | unbuilt | M |
| **G10** | `@statics` emits neither of the two diagnostics the design promised | unbuilt | S |
| **G11** | 106 of 114 `@GlobalScope` utilities are undispatched | unbuilt | L |
| **G12** | 367 math methods and 261 operators are absent with nothing recorded | unbuilt | M |
| **G13** | math exists for 4 of 16 types; `snapped`, `min`/`max`, `floor`/`ceil`/`round` unwritten | narrower | M |
| **G14** | `GetClassOf` answers only the Godot class | structural | — |
| **G15** | there is no `godot_callback` native class; a callback is `(handle, decorated name)` | structural | — |
| **G16** | `vh_signal` is non-parametric; the payload lives in a host-side table | structural | — |
| **G17** | the callback native takes `any`, not a typed function parameter | structural | — |
| **G18** | `@statics` names its class as a string, not an identifier | structural | S |
| **G19** | no behavioural test for `_CanDropData` / `_HasPoint` | test gap | S |
| **G20** | R-EXP-5's body overstates; R-AUD-2 never edited | doc | S |

**G14–G17 are working as built and are not bugs.** They are here so a fresh reader does not
"restore" them to the design's shape without knowing why they differ.

---

## 2. Accepted-but-broken

The worst category: a script writes something the compiler accepts, and it does not work. All four
are signals, all four have the same fix shape, and the design already specified it — §6.2's *"a
payload the wire cannot carry is refused **at the member**, reusing R-EXP-3's machinery rather than
failing at the emission."* That sentence answers G1, G3 and G4 together, and G2 belongs with them.

### G1 — a struct signal payload does not work

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

### G6 — the thread guard covers 2 of 31 entry points

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

### G9 — a callback does not remember its content scope

**Design:** §5. *"Epic's `FVerseEventCallbackList` carries the other half of the discipline and is
worth copying: each callback remembers the `FContentScope` it was subscribed in, and a terminated
scope drops it. That is what keeps a raise (R-DIAG-3) from leaving callbacks that can never run
again."*

**What is there.** Nothing. `FCallbackTarget` is `{OwnerHandle, DecoratedName}` and the host has one
process-wide `GContentScope`. After a raise terminates the scope, `EnterVerse` revives it at the next
`vh_tick` — so in practice the callbacks *do* run again, which is why nothing failed. Whether that is
correct or merely benign is not established.

**Worth deciding before Phase 5**, which adds `Await` and more callbacks with longer lives.

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

### G12 — the math tail is absent *and* unrecorded

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

1. **G2 + G3 + G4** — one validation pass in `GetClassSignals`, three silent failures become editor
   diagnostics. Highest value per hour in the list, and the design already specifies it.
2. **G1** — the struct payload, which the same pass can either implement or refuse cleanly.
3. **G12** — record the math skips. Restores R-SCN-2's promise over the largest surface still missing
   from it, and tells the *next* person what is absent without them having to read a Verse file.
4. **G6** — finish the thread guard. Small, and it is a correctness claim the spec already makes.
5. **G5** — `_make_function`. Needs the by-hand checklist to verify, so it pairs with running that.
6. **G13**, **G11**, **G7**, **G8**, **G10**, **G19**, **G20** — ordinary work, in whatever order the
   next phase makes convenient.
7. **G9** — decide before Phase 5 rather than after.

`G14`–`G18` need no action unless a requirement changes; read them before touching the code they
describe.
