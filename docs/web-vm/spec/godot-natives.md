# `godot-natives.md`: the contract of `host/Verse/Godot.native.verse`

Status: reviewed by the lead 2026-09-24 — no VerseVM structure or code; the code blocks are
declarations quoted from this project's own `Godot.native.verse`. Room: dirty. Sources read: `docs/phase-7.5-design.md` (§3,
§6, §8 and the rest), `CLAUDE.md` (`variant`, `rid`, transactions, "Objects that are not nodes",
and the ABI header sections), `host/Verse/Godot.native.verse`, `include/verse_host_abi.h`,
`host/Private/GodotBindings.cpp`, `host/Private/GodotClasses.cpp`, `host/Private/GodotClasses.h`,
`host/Private/GodotMathLayout.gen.h`, `host/Private/GodotClassNames.gen.h` (header and excerpt),
`host/Private/HostScript.cpp` (the sections implementing signal/event binding and delivery,
object-identity and class-resolution, `InstanceCall`, `ValueToWire`/`WireToValue`,
`ReadSelfDescribingValue`, `ReadRidStruct`), `host/Private/HostEventLoop.cpp` (`Sleep`
scheduling and the pump), `src/verse_script_instance.cpp` (the consumer-side dispatch of the
four script-level hooks and the `<decides>`-as-`true`/`false` rule).

This document restates, in prose and tables, everything a clean-room implementer of `vm/` and
`src/verse_vm_host.cpp` needs in order to implement the 46 `<native>` free functions currently
declared in `host/Verse/Godot.native.verse`, the four native-shadowed types (`vh_object`,
`variant`, `godot_ref`, `vh_signal`), and the calling-convention rules the runtime host applies
around every one of them. Nothing here describes VerseVM's internal structure; everything is
stated as an observable contract over the `vh_godot_api` callbacks in `include/verse_host_abi.h`.

`docs/phase-7.5-design.md` §4 counted "the 39 Godot natives" from an earlier survey; the file as
it stands today (`host/Verse/Godot.native.verse`) declares **46** top-level `<native>` functions,
listed in full below. The discrepancy is not investigated further here — implement what is
actually declared in the file, not the historical count.

## 1. How to read this

Every native's entry below states, in order:

- **Verse signature** — quoted verbatim from `Godot.native.verse`, which is this project's own
  public source and may be quoted directly.
- **What it does** — which `vh_godot_api` callback(s) it drives and what is passed.
- **Outcomes** — the return value, the `<decides>` failure condition if any, whether it can raise
  a Verse runtime error (with the raised message, verbatim, when the source names one), and
  whether it can suspend.
- **Transaction behaviour** — whether the Godot-visible effect happens immediately, is deferred to
  commit, or is compensated on abort, and what "on abort" does.
- **Object-identity / handle notes**, where relevant.

Two rules apply throughout and are not repeated per native:

- **Every native runs on the thread that called `vh_init`.** Nothing in this file is reachable
  from a background analysis thread; the natives are called only from a live VM entry
  (`vh_instance_call`, `vh_callback_invoke`, `vh_tick`, `vh_run_main`), all of which are documented
  in `include/verse_host_abi.h` as single-threaded to the `vh_init` caller.
- **A native that reaches `vh_godot_api` and finds the callback pointer null does the least
  surprising thing it can**: a `<reads>`/`<transacts>` value-returning native answers a default
  `variant` (Tag 0, i.e. Nil) or `0`; a `<decides>` native declines; a void native does nothing.
  This is what lets an embedder bring the ABI up incrementally, exactly as the UE host's own
  `GodotBindings.cpp` does — it is not a native-specific rule, so it is stated once here rather
  than under each entry.

## 2. The effect-specifier rule, restated

`Godot.native.verse`'s own header comment states the rule that decides every specifier in the
file: **a native is `<reads>` if calling it cannot change Godot, and cannot change any reference
the bridge is already holding.** Building a *fresh* reference is not a change by that rule, because
nothing outside the call can observe it yet — which is why `VhRefNew` and the `VhRefFrom...`
family are `<reads>` while `VhRefSet` is not. Everything that reaches Godot to mutate it, or that
hands Godot something of Verse's own (`VhCallableFrom`, the whole signal family), is `<transacts>`.

Two additional facts, applied throughout §8:

- **`<decides>` does not narrow the effect set.** A `<decides>` native still carries the wide
  default effect set unless it is *also* marked `<transacts>` or `<reads>`, exactly as a script's
  own `<decides>` virtuals do (`CLAUDE.md`, "A virtual that answers Godot a `bool`...").
- **A raise is not the same outcome as a `<decides>` failure.** A `<decides>` native declines
  silently, letting an `if`/`?` in the caller handle it. A raise unwinds to the root failure
  context, drops every deferred write the enclosing transaction had queued, and is reported
  through `OnRuntimeError` with a stack. The rule that decides which of the two a given host-side
  problem gets is: a script mistake or a stale reference is a decision-family failure only for
  things the caller has an ordinary way to test for (`IsValid`, a missing key); reaching through
  something the caller was never in a position to test — a freed object, a value with no wire
  representation, a member the mirror and this build of Godot disagree about — is a raise.

## 3. The three native-shadowed types, and `vh_signal`

### 3.1 `vh_object`

```
vh_object<public><native> := class:
    var Handle<native>:int = 0
    block:
        set Handle = VhAdoptOrMint(Self)
    _Notification<public>(What:int):void = {}
    _Get<public>(Property:string):variant = variant{}
    _Set<public>(Property:string, Value:variant)<decides>:void = false?
    _GetPropertyList<public>():godot_array = godot_array{}
    _ValidateProperty<public>(Property:dictionary):void = {}
```

`vh_object` is the one native class in the whole API and the root of every mirrored Godot class.
Its `Handle` field is the Godot instance id: written once, at construction, by the `block:`
clause's call to `VhAdoptOrMint(Self)` (§8.12), and never written again. It is not `<public>`,
because a handle is the one thing that outlives what it names — reading it directly instead of
through the object would give a script a number no guard can check.

The `block:` clause runs for **every** class derived from `vh_object`, with `Self` already typed
as the *derived* class, so a plain Verse class deriving from a mirrored Godot class gets a live
Godot peer with no code the deriving class has to write. §4 covers what `VhAdoptOrMint` decides
between minting a fresh peer and adopting one the host is already holding.

The five method bodies are the whole of Godot's script-level hook surface that
`extension_api.json` has no row for — Godot offers these to *scripts*, not to `ClassDB`-registered
methods, so no amount of mirror generation produces them. Every body shown above is the default,
inherited implementation; §11 covers what happens when a script overrides one. All five carry no
effect specifier of their own (the widest default set) except `_Set`, whose `<decides>` is
explained in §11.

### 3.2 `godot_ref`

```
godot_ref<public><native> := class<computes>:
    Ref<native>:int = 0
```

The C++ shadow behind every reference-type wrapper — `godot_array`, `dictionary`, `callable`,
`signal_ref`, and the packed-array wrappers `GodotApi.native.verse` builds over them. `Ref` is an
id in a table the *consumer* (the embedder) owns — the same table an `Object`'s instance id does
not need, because an instance id is already Godot's own stable name for an object. `<computes>`
(rather than `<transacts>`) is what lets the four public container archetypes (`godot_array{}`,
`dictionary{}`, and the two that ride `godot_ref`) be built from a `<reads>` or `<computes>`
context; see §8.29 for how the mint actually happens through a data-member default rather than
through this type's own construction.

Its only behavioural contract for a reimplementation: **the moment a Verse value of this type (or
of a class deriving from it) becomes unreachable and is collected, the id it names must be
released** — i.e. the embedder's `ReleaseRef` callback must be called exactly once per id that was
ever handed to Verse through `NewRef`, a `VH_TYPE_REF` value, or minted implicitly by `VhRefNew`/
`VhRefNewDefault`/`VhRefFrom...`. §12.4 states the identity rule this depends on.

### 3.3 `variant`

Covered in full in §5 below — every lane, and the tag-to-lane table.

### 3.4 `vh_signal`

```
vh_signal<public><native> := class:
    Id<native>:int = 0
```

The binding half of a script-declared `signal(t)` member (the typed half, `signal(t)` itself, is
ordinary parametric Verse over this, declared in `GodotApi.native.verse` and out of this
document's scope — it is not itself `<native>`). `Id` is written once, at construction, the way
`vh_object::Handle` is: it names a row the host keeps recording the owning object's handle, the
signal's Godot name, and how its payload decomposes into Godot arguments — all of which is decided
at the point a script's class is instantiated, by walking the class's declared signal members
(§9.1). A `signal(t)` a script builds for itself outside that walk (e.g. a local variable) holds
`Id = 0` and names nothing; every native in the signal family treats `Id == 0` as "not bound" and
reports the specific sentences in §9.

## 4. Object identity: minting, adopting, releasing, crossing back

This section is the shared machinery behind `VhAdoptOrMint`, `VhObjectOf`, `VhSingletonObject`,
and every place a Godot handle becomes a Verse value or a Verse-constructed object becomes a
Godot peer. It corresponds to R-NODE-3.

### 4.1 Two ways a `vh_object` gets a peer

When any class derived from `vh_object` is constructed, its inherited `block:` clause runs and
calls what this document spells as **`AdoptOrMint(Self)`** (the native `VhAdoptOrMint`, §8.12).
That call decides between exactly two outcomes, checked in this order:

1. **Adopt.** If the *embedder* is in the middle of constructing this exact object for a handle it
   already holds — i.e. the host is running `vh_instantiate` for a node Godot made, or crossing a
   handle in through `VhObjectOf`/`VhSingletonObject`/a container element and just built the
   wrapper object — the pending handle is consumed and returned. "Consumed" is load-bearing: the
   adoption record is a single pending `(class, handle)` pair, cleared the instant it is read, so
   that if the class being adopted has its *own* member of a `vh_object`-derived type, that
   member's own construction (which runs during the same outer construction) does not also adopt
   the outer handle — it falls through to minting instead, because the pending record no longer
   matches.
2. **Mint.** Otherwise, a fresh Godot object is made: the embedder is asked to instantiate the
   *nearest Godot class that mirrors the object's actual Verse class* (§4.3), and the resulting
   handle is recorded as "minted by this Verse object."

A **class default object** (the throwaway instance the export-default machinery constructs to
read a class's declared defaults, and any similar reading device) is never minted through the
ordinary path: it is flagged before construction and the peer call answers "no peer" for it and
for everything nested under it while the flag is set (§4.4). It is also never an adoption target.

### 4.2 Suppressing the mint (`FSuppressMintScope` / `IsMintSuppressed`)

A reading device that only wants a class's *declared defaults* — never a live object — sets a
scoped flag before constructing the class default object and clears it afterward. While the flag
is set:

- The `vh_object` construction path answers "no peer" (handle `0`) rather than minting one.
- `VhRefNewDefault` (§8.34) answers `0` rather than minting an empty container.

Both are the same fact stated twice, because a container-typed member's *default* mints through a
different mechanism than an ordinary `vh_object`-typed member's does (§8.29), and both mechanisms
have to honour the same suppression. Without it, a class with a member such as `Held:node2d =
node2d{}` would mint one real Godot node per read of its declared defaults — which happens on
every keystroke's analysis, for every exporting class, and the nodes are never freed (§4.5).

### 4.3 Which Godot class a mint asks for

Minting asks the embedder to instantiate a Godot class by name. The name is resolved from the
Verse class actually being constructed by walking its ancestor chain (most-derived first) and
taking the **first ancestor that names a Godot class**, where "names a Godot class" is checked in
this order:

1. **A generated-binding class** (one the roster passed to `vh_set_bindings` names — see
   `docs/generated-bindings.md`). This has to be checked before the mirror, because a binding
   exists exactly when Godot has a class the mirror does not carry, and falling through to the
   mirror would mint the wrong, less-derived Godot class silently.
2. **A mirrored class**, resolved through the same table `GetClassOf` reads for the reverse
   direction (§4.6): the generated table mapping every Godot class name to the Verse class that
   mirrors it (or, for a class outside a `--classes-file` subset, its nearest emitted ancestor).

If the walk reaches `vh_object` itself without finding either, there is no Godot class to
instantiate — minting answers "no peer" (handle `0`), which is correct for `vh_object` itself and
for the class-default-object case, and is a raise for anything else (§8.12's raised message).

If the embedder's instantiate callback answers "no such object" for a name that *did* resolve
(Godot refuses to construct an abstract class, a class it only ever hands out as a singleton, or a
name this build of Godot does not have), that is also a raise (§8.12).

### 4.4 Release: only what was minted

Every `vh_object` is told when it is collected. On that notice, the object's own handle is looked
up in the *minted-peer table* — the table `AdoptOrMint` added a row to when it minted (never when
it adopted). Two outcomes:

- **Not in the table**: this `vh_object` crossed in from Godot (adopted, not minted, whether as a
  script instance or as a mirror wrapper around some other handle) — nothing is released, because
  the object is one the scene itself owns.
- **In the table, and this object is the one recorded as having minted it**: the table row is
  removed and the embedder's release callback is called, telling it whether the release is an
  ordinary collection (the peer may or may not be freed immediately, per the embedder's own
  ownership rules for that Godot class) or a **discard** (the transaction that minted it aborted,
  so nothing outside the transaction can ever have observed the object — it must be freed
  outright, not merely dereferenced).

  The "and this object is the one recorded" clause matters: the record is keyed by handle, so
  releasing must not fire for an unrelated `vh_object` that happens to share... — in practice this
  guards the abort-compensation path, whose release call names no object at all (it runs after the
  transaction has already unwound), against ever matching a *different* live object that has since
  reused bookkeeping for the same handle.

Compensation on abort is registered at mint time (§8.12's transaction behaviour), not discovered
at release time: if the transaction that minted a peer aborts, the compensation removes the row
and discards the peer *immediately*, rather than waiting for the Verse value to be collected —
which may never happen, since nothing outside an aborted transaction ever held a reference to it.

### 4.5 A container's mint is not this mechanism

`godot_ref`-derived containers (`godot_array{}`, `dictionary{}`, and their siblings) do **not** go
through `AdoptOrMint`. §8.29 (`VhRefNewDefault`) is the mechanism a *default* uses; an explicit
`VhRefNew` call (§8.28) is the mechanism anything else uses. Both check the same suppression flag
as §4.2. What a dropped container leaks, if the suppression is not observed, is a table entry
rather than a Godot object — released the next time the value is found unreachable, which an abort
also produces (dropped containers do not accumulate the way a leaked minted node does).

### 4.6 Crossing a handle *in*: which Verse object it becomes

Whenever a Godot instance id has to become a Verse value (a method's return, a property read, a
container element, `VhObjectOf`, `VhSingletonObject`), the same algorithm decides which Verse
object it is, checked in this order:

1. **A live script instance bound to that handle.** If the handle names a Godot object this VM has
   an active `vh_instance` for, that instance's own Verse object is the answer — never a fresh
   wrapper. This is what makes `Self` inside a script's own methods the same object a caller who
   already holds a reference to that node sees.
2. **A live minted peer.** If the handle is one this VM itself minted (§4.1), the very Verse
   object that minted it is the answer. **This is the round-trip identity rule**: an object a
   script constructed, handed to Godot (say, through an `Array` or a signal payload), and later
   receives back — from that same Array, or from a different call that happens to return the same
   handle — is `=`-equal to the object it started as, not a second wrapper around the same handle.
3. **A fresh wrapper of the handle's most-derived resolvable class.** Otherwise, a new Verse
   object is built, of whichever class §4.7 resolves the handle's Godot class name to (or of the
   caller-supplied fallback class, for the one caller — `VhSingletonObject` — that knows the class
   before asking Godot for the handle). This case is **never cached**: two separate crossings of
   the same un-scripted, un-minted handle answer two distinct (but handle-equal) Verse objects.
   Building this wrapper runs the target class's own `block:` clause, which must **adopt** (not
   mint) — the pending-adoption record (§4.1) is what makes that happen, set immediately before
   the wrapper is constructed.
4. **A bare `vh_object`, naming no Godot peer at all**, only when step 3 could not resolve any
   class for the handle (a class outside the mirror entirely, or a handle Godot has already
   freed and so will not even name a class for) — never mint anything in this case; a bare
   `vh_object` is what a caller's downcast is expected to decline against.

### 4.7 Resolving a handle's own class name (`GetClassOf`)

To classify a handle whose class is not already known from context (§4.6 step 3), ask the
embedder for the Godot class name the handle currently answers (the `GetClassOf` callback), then
resolve that name to a Verse class in this order, caching the final answer per handle (instance
ids are never reused within a run, so the cache is safe for the life of the process):

1. **A script class.** Ask the embedder whether the handle carries a script with a registered
   global class name (the `GetScriptClassOf` callback — this is *not* the same question as
   `GetClassOf`, because a node whose script is `mob.gd` with `class_name Mob` answers `Node2D`
   from `GetClassOf` and `Mob` only from `GetScriptClassOf`). If it does, and that name is one of
   the roster passed to `vh_set_bindings` as a script-class binding, that binding's Verse class is
   the answer.
2. **A ClassDB binding.** Otherwise, if `GetClassOf`'s answer is itself one of the roster's
   ClassDB-class bindings, that binding's Verse class is the answer.
3. **The mirror.** Otherwise, look `GetClassOf`'s answer up in the generated table mapping every
   Godot class name to the Verse class that mirrors it, or to its nearest emitted ancestor when a
   `--classes-file` build does not carry that exact class (§5's sibling file,
   `host/Private/GodotClassNames.gen.h`, is the generated table itself — sorted by Godot class
   name for binary search, one `{godot_name, verse_name}` row per Godot class known to
   `extension_api.json`, every row present even when only a subset is emitted, each such row
   naming its nearest *emitted* ancestor instead). **This generated header may be read directly by
   the clean room** — it is machine-generated data, not hand-written host logic.
4. **Nothing resolves.** A handle `GetClassOf` cannot describe (freed already) or whose Godot class
   the mirror does not carry at all falls through to §4.6 step 4.

### 4.8 Singletons (`VhSingletonObject`) are a shortcut on step 3

A singleton's *name* is a mirrored class by construction — the accessor knows which Verse class an
engine singleton mirrors before asking Godot for anything, because the generator emitted the
accessor from the same `extension_api.json` row. So `VhSingletonObject` skips `GetClassOf`
entirely and resolves the mirrored class straight from the singleton's own name string (the same
mirror table as step 3 above), passing that resolved class as the fallback the handle is wrapped
with. This is the mechanism `CLAUDE.md`'s "`Object::get_class()` can answer a class
`extension_api.json` has never heard of" note is about: `Engine.get_singleton("IP")` would answer
a driver class (`IPWindows`) that resolves to nothing through `GetClassOf`, so naming the class
from the singleton's own name sidesteps that entirely.

## 5. The `variant` struct: lanes and tags in full

`variant` (§3.3) is one Godot `Variant` on the wire, as a fixed-width struct with 22 scalar lanes:
`Tag`, `Ref`, four integer lanes `I0`–`I3`, sixteen float lanes `F0`–`F15`, and one string lane
`Text`. `Tag` is Godot's own `Variant::Type` numbering, given in full by `vh_variant_tag` in
`include/verse_host_abi.h` (`VH_VARIANT_NIL = 0` through `VH_VARIANT_PACKED_VECTOR4_ARRAY = 38`,
`VH_VARIANT_MAX = 39`); those numeric values are Godot's own and must never be renumbered.

The field order for every math type below comes from `host/Private/GodotMathLayout.gen.h`, a
generated header the clean room **may read directly**: it is machine-generated from the same
`extension_api.json` table that emits the Verse struct declarations, so it cannot drift from them,
and it carries no hand-written host logic. Its shape: for each of the sixteen math types, an
ordered list of named fields, each either a scalar (float, or integer for the four "…I" types) or
"nested" — carrying the `vh_variant_tag` of another math type in the same table, whose own field
list is then flattened in place, in order. `Basis`'s three fields (`X`, `Y`, `Z`) are each a
`vector3`, so it occupies 9 float lanes; `Transform3D` nests a `Basis` (9) then a plain `vector3`
`Origin` (3), for 12; `Projection` nests four `vector4`s, for 16 — the widest value type in the
struct.

| Tag | Godot type | Lanes occupied, in order |
| --- | --- | --- |
| 0 `NIL` | — | none |
| 1 `BOOL` | `bool` | `I0` (0 or 1) |
| 2 `INT` | `int` | `I0` |
| 3 `FLOAT` | `float` | `F0` |
| 4 `STRING` | `String` | `Text` |
| 5 `VECTOR2` | `Vector2` | `F0`=X, `F1`=Y |
| 6 `VECTOR2I` | `Vector2i` | `I0`=X, `I1`=Y |
| 7 `RECT2` | `Rect2` | `F0`=Position.X, `F1`=Position.Y, `F2`=Size.X, `F3`=Size.Y |
| 8 `RECT2I` | `Rect2i` | `I0`..`I3` = Position.X, Position.Y, Size.X, Size.Y |
| 9 `VECTOR3` | `Vector3` | `F0`=X, `F1`=Y, `F2`=Z |
| 10 `VECTOR3I` | `Vector3i` | `I0`=X, `I1`=Y, `I2`=Z |
| 11 `TRANSFORM2D` | `Transform2D` | `F0`,`F1`=X.X,X.Y; `F2`,`F3`=Y.X,Y.Y; `F4`,`F5`=Origin.X,Origin.Y |
| 12 `VECTOR4` | `Vector4` | `F0`=X, `F1`=Y, `F2`=Z, `F3`=W |
| 13 `VECTOR4I` | `Vector4i` | `I0`..`I3` = X, Y, Z, W |
| 14 `PLANE` | `Plane` | `F0`..`F2`=Normal.X,Y,Z; `F3`=D |
| 15 `QUATERNION` | `Quaternion` | `F0`=X, `F1`=Y, `F2`=Z, `F3`=W |
| 16 `AABB` | `AABB` | `F0`..`F2`=Position.X,Y,Z; `F3`..`F5`=Size.X,Y,Z |
| 17 `BASIS` | `Basis` | `F0`..`F2`=X.X,X.Y,X.Z; `F3`..`F5`=Y.X,Y.Y,Y.Z; `F6`..`F8`=Z.X,Z.Y,Z.Z |
| 18 `TRANSFORM3D` | `Transform3D` | `F0`..`F8`=Basis (as row 17, flattened); `F9`..`F11`=Origin.X,Y,Z |
| 19 `PROJECTION` | `Projection` | `F0`..`F3`=X (Vector4); `F4`..`F7`=Y; `F8`..`F11`=Z; `F12`..`F15`=W |
| 20 `COLOR` | `Color` | `F0`=R, `F1`=G, `F2`=B, `F3`=A |
| 21 `STRING_NAME` | `StringName` | `Text` (Verse spells this and `String` and `NodePath` all as `string`; only `Tag` tells them apart) |
| 22 `NODE_PATH` | `NodePath` | `Text` |
| 23 `RID` | `RID` | `I0` — see the scalar-encoding note below |
| 24 `OBJECT` | `Object` | `Ref` = the object's own Godot **instance id** (not a table id — no release is ever owed for this lane) |
| 25 `CALLABLE` | `Callable` | `Ref` = an id in the consumer's reference table |
| 26 `SIGNAL` | `Signal` | `Ref` = an id in the consumer's reference table |
| 27 `DICTIONARY` | `Dictionary` | `Ref` |
| 28 `ARRAY` | `Array` | `Ref` |
| 29–38 the ten packed-array tags (`PACKED_BYTE_ARRAY` … `PACKED_VECTOR4_ARRAY`) | — | `Ref` |

Integer vector components (`Vector2i`/`3i`/`4i`, `Rect2i`) ride in the integer lanes rather than
the float ones deliberately: a `double` stops representing every 32-bit integer exactly above
2<sup>53</sup>, and Godot's are 32-bit signed values that must round-trip intact.

### 5.1 `rid`'s scalar encoding is its own case, not a math-type row

`rid` is **not** one of the sixteen rows in `GodotMathLayout.gen.h` and must not be treated as
one. A Godot `RID` crosses as a **scalar** — `I0` alone, under `VH_VARIANT_RID` — never as a
component array the way every math type above does. The reasons this matters for a
reimplementation, both measured the hard way in the existing host:

- A `RID` is an ordinary wrapped integer: nothing mints it, nothing releases it, and it indexes a
  server's own table rather than naming a Verse-side reference — which is exactly why it does not
  belong in `Ref` alongside objects and containers, despite `Ref` otherwise being "the identity
  lane." Reading it out of `Ref` by the same code path that reads `Object` would silently answer
  the wrong number.
- Verse's own `rid` struct (declared elsewhere, in the ordinary mirror, not in this file) has to be
  claimed by name — its class must be recognised as `rid` specifically — everywhere a struct value
  is classified, or it falls into the generic mirrored-struct arm (asking Godot for one argument
  per lane the way `vector2` does) or the generic reference arm (treating the handle as a table
  id), either of which produces a wrong answer with no diagnostic (`CLAUDE.md`'s "`rid` is the
  second instance of that trap").

### 5.2 The string family

`String`, `StringName` and `NodePath` all carry their payload in `Text` and are told apart **only
by `Tag`** — Verse spells all three as its own `string`, so the distinguishing information a wire
value carries is entirely in the tag, never in the shape of the payload.

## 6. Wire conversion: `variant` ⇄ `vh_value`

This is the rule implemented by `VariantFromWire` and `VariantToWire` in the existing host (and by
their sibling, the general-purpose numeric coercion used throughout the marshalling code). A
reimplementation must reproduce both directions exactly, because a mismatch between them is a
silent value corruption rather than a refusal (a lane written by one rule and read by a different
one is *plausible nonsense*, not an error).

### 6.1 `variant` → `vh_value` (`VariantToWire`)

Given a `variant` (its `Tag` and lanes), the corresponding `vh_value` is built as follows, tested
in this order:

| Tag | `vh_value` produced |
| --- | --- |
| `BOOL` | `VH_TYPE_LOGIC`, `Logic` = (`I0` != 0) |
| `INT` | `VH_TYPE_INT`, `Int` = `I0` |
| `FLOAT` | `VH_TYPE_FLOAT`, `Float` = `F0` |
| `STRING`, `STRING_NAME`, `NODE_PATH` | `VH_TYPE_STRING`, pointing at a UTF-8 copy of `Text` that outlives the call |
| `OBJECT` | `VH_TYPE_INT`, `Int` = `Ref` (the instance id, unwrapped from the `Ref` field into a plain int — the wire's `VH_TYPE_REF` is never used for an object) |
| `RID` | `VH_TYPE_INT`, `Int` = `I0` |
| `NIL` | `VH_TYPE_VOID` |
| any tag whose lane table (§5) says "no float or int lanes" (i.e. `CALLABLE`, `SIGNAL`, `DICTIONARY`, `ARRAY`, and the ten packed-array tags) | `VH_TYPE_REF`, `Ref` = the variant's `Ref` field, unchanged |
| every other tag (the fourteen remaining math types) | `VH_TYPE_TUPLE`, whose items are the tag's lanes read out **in the exact order §5's table gives them** — integer lanes first (as `VH_TYPE_INT` items), then float lanes (as `VH_TYPE_FLOAT` items). Every `vh_value` this produces carries `VariantTag` set to the source `Tag`, so a consumer reading the tuple back still knows which Godot type it names. |

In every case, `VariantTag` on the produced `vh_value` is set to the `variant`'s own `Tag`, even
for the scalar cases — this is what lets the two forward-only rules below (numeric coercion,
inferred tag) stay honest about what type a value that later needs to be turned back into a
`variant` should become.

### 6.2 `vh_value` → `variant` (`VariantFromWire`)

Given a `vh_value`, the tag to rebuild is **`VariantTag` if it is not `VH_VARIANT_NIL`, otherwise
inferred from the `vh_value`'s own `Type`** (`VH_TYPE_LOGIC`→`BOOL`, `VH_TYPE_INT`→`INT`,
`VH_TYPE_FLOAT`→`FLOAT`, `VH_TYPE_STRING`→`STRING`; every other untagged `Type` infers `NIL`,
deliberately — a tuple is equally a `Vector2`, a `Vector2i` or a `Rect2`, and guessing among them
would silently misread whichever the caller meant). Given the resolved tag:

| Resolved tag | Lanes filled |
| --- | --- |
| `NIL` | nothing |
| `BOOL` | `I0` = the value coerced to an int (§6.3), nonzero → 1, zero → 0 |
| `INT` | `I0` = the value coerced to an int |
| `FLOAT` | `F0` = the value coerced to a double |
| `STRING`, `STRING_NAME`, `NODE_PATH` | `Text` = the value's UTF-8 bytes, only when `Type` is actually `VH_TYPE_STRING` (otherwise `Text` is left empty rather than guessed at) |
| `OBJECT` | `Ref` = the value coerced to an int (an instance id arriving as a plain `VH_TYPE_INT`, not wrapped in `VH_TYPE_REF`) |
| `RID` | `I0` = the value coerced to an int |
| any reference tag (`CALLABLE`, `SIGNAL`, `DICTIONARY`, `ARRAY`, the ten packed arrays) | `Ref` = the value's `Ref` field if `Type` is `VH_TYPE_REF`, otherwise the value coerced to an int (so a bare table id crossing as a plain int is still accepted) |
| any of the fourteen remaining math tags | Only when `Type` is `VH_TYPE_TUPLE` or `VH_TYPE_ARRAY`: walk the tag's lane table (§5) in order, filling each integer lane and then each float lane from successive items of the sequence, coercing each item with §6.3. **A sequence shorter than the tag's lane count leaves the remaining lanes at their zero default rather than reading past the end** — this is a deliberate leniency, not an error condition. |

### 6.3 Numeric coercion (used by both directions above)

A payload that needs to become an integer coerces: `VH_TYPE_LOGIC`→0 or 1, `VH_TYPE_FLOAT`→
truncated toward zero, `VH_TYPE_REF`→the reference id itself, anything else→its `Int` field
verbatim. A payload that needs to become a double coerces: `VH_TYPE_LOGIC`→0.0 or 1.0,
`VH_TYPE_INT`→the exact (or nearest-double) value, anything else→its `Float` field verbatim. This
is what lets a method the mirror types as `float` accept an integer-valued `Variant` from Godot's
own API (Godot answering a bare `0` where a `float` was documented is common) without a special
case at every call site: by the time a `variant` exists, the lane its tag names is already filled
with the coerced value.

### 6.4 What is *not* symmetric

`VariantToWire` never produces `VH_TYPE_OPTION` — that shape only ever appears on the
*declared-type-directed* marshalling path used for script members and method parameters/results
(§12.3), which is a different, wider conversion than the `variant`-specific one this section
describes. A native's own `variant` parameters and results only ever cross as the shapes in §6.1
and §6.2.

## 7. Transaction behaviour, restated per outcome

Recapping `CLAUDE.md`'s rule for this bridge specifically as it applies to the natives in this
file:

- **A write that reaches Godot defers to the enclosing transaction's commit**, unless the native
  is one of the two stated exceptions below. A raise anywhere in the computation that produced the
  write drops it, unread by Godot, because the transaction that would have committed it aborts
  instead.
- **The two immediate exceptions** are `VhSignalEmit` (and, through it, `VhEventEmit`) and
  `VhRefSet`. Both take effect the instant they are called, not at commit:
  - **Signal emission** must be immediate so that handlers — Verse's own included — run
    synchronously and observe a Godot state consistent with "the emit already happened," the way a
    GDScript author's own `emit_signal` does. The stated cost: if the emitting transaction itself
    later aborts, the handlers have already run and cannot be un-run.
  - **A container write** (`VhRefSet`) must be immediate because `VhRefGet` and `VhRefSize` are
    themselves immediate, unconditionally — so a deferred `VhRefSet` would leave a container
    disagreeing with itself inside a single Verse expression (`A.SetInt(0, 5)` immediately followed
    by `A.GetInt[0]` would read the old value), and because the append pattern a freshly built
    container needs (`VhRefNew` then repeated `VhRefSet` at successively larger indices) only works
    if each write is visible to the next read.
- **A mutation that also answers a value the caller needs immediately cannot be deferred, and
  cannot simply be dropped on abort either — it is *compensated*.** The pattern: the mutation runs
  immediately (inside whatever `Open`/unchecked context the embedder callback needs), and an abort
  handler is registered *as if the call had been made from ordinary transactional code*, so that if
  the enclosing transaction aborts, the compensating action (a disconnect, or discarding a minted
  peer) runs even though the call that made the original change was itself made from inside an
  unchecked context. The natives this applies to: `VhSignalSubscribe`, `VhEventSubscribe`,
  `VhSignalRefSubscribe`, and `VhAdoptOrMint`'s minting branch. **This is *not* the VM's own
  ordinary rollback log** — it is a separate, narrower mechanism reserved for exactly these cases,
  because the VM's own rollback has no way to reach back into an embedder callback that already
  ran outside of transactional bookkeeping.
- **Everything else** — every plain `<reads>` native, and every `<transacts>` native with nothing
  above applying to it — simply runs (immediately, for a `<reads>` native, since there is nothing
  to defer or compensate) or defers its single write to commit (for a `<transacts>` native with no
  value to hand back this call).

## 8. The natives

### 8.1 `Print`

```
Print<public><native>(Message:string)<transacts>:void
```

Drives the `Print` callback with the message's UTF-8 bytes. Deferred to commit (it is an ordinary
`<transacts>` write with no stated exception). No failure path, no raise, cannot suspend. If the
callback pointer is null, nothing happens (§1).

### 8.2 `VhIsValid`

```
VhIsValid<native>(Handle:int)<reads>:logic
```

Drives `IsValid` with the handle and answers its boolean verbatim (`false` if the callback is not
wired up). Immediate — there is nothing to defer, since it answers a value and changes nothing.
Never raises, never suspends. This is the native the Verse-side `IsInstanceValid` wraps, and the
one the raised messages elsewhere in this file (§8.4, §8.9) tell the author to call first.

### 8.3 `VhTypeMismatch`

```
VhTypeMismatch<native>(Expected:string, Value:variant)<reads>:void
```

Never returns normally: it **always raises** a runtime error and unwinds to the root failure
context. Raised message, verbatim (`Value.Tag` is the variant's numeric tag, `Expected` the type
name the caller wanted):

> `Godot returned a value tagged %lld where the Verse bridge expected `%hs`. The type table in tools/gen_verse_api.py and this build of Godot disagree.`

It exists so that the typed readers generated in `GodotApi.native.verse` (`AsInt`, `AsVector2`,
and so on) can be **total functions** despite bottoming out in an array index that would otherwise
be failable: each reader checks the variant's `Tag` and calls this to raise rather than returning
a `<decides>` failure, because a tag mismatch there means the mirror and the running Godot build
have drifted apart — a bug in the bridge, not an ordinary miss the caller has a spelling for.

### 8.4 `VhCallValue`

```
VhCallValue<native>(Handle:int, Method:string, Args:[]variant)<transacts>:variant
```

Converts each argument `variant` to a wire `vh_value` (§6.1) and drives `CallMethod` with the
handle, the method name, and the argument array; converts the result back (§6.2) and returns it.

**Total: every failure mode raises rather than declining**, because (per the file's own comment) a
dead receiver, an unknown method, and an argument or result with no wire representation are all
the caller's or the mirror's mistake, not an ordinary miss — there is nothing left for a caller to
handle once one of them happens. The three raised messages, verbatim (`%s` is `Called`, the verb;
`%hs` is the method name; `%lld` is the handle):

> `%s `%hs` on Godot object %lld, which Godot has already freed. Test IsInstanceValid[...] before reaching through a reference the scene may have dropped.`
> (when the handle names a freed or never-valid object)

> `%s `%hs` on Godot object %lld, and the value has no representation on the Verse bridge. This is a gap in the type table in tools/gen_verse_api.py.`
> (when an argument or the result has no wire encoding)

> `%s `%hs` on Godot object %lld, which has no such member. The generated Verse mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py.`
> (when the object is alive but has no such method)

Deferred to commit is **not** how this native behaves: because it must answer a `variant` result
immediately, the call runs immediately, inside the same unchecked context every embedder callback
needs (there is no compensation registered for it — a *method call*'s side effects on Godot are
Godot's business to make consistent, unlike the specific "mutate and also connect/mint" shapes
§7 compensates). If the callback pointer is null, this also raises (the internal/generic raise, not
one of the three sentences above, since there was no status code to interpret).

### 8.5 `VhGetValue`

```
VhGetValue<native>(Handle:int, Property:string)<decides><reads>:variant
```

Drives `GetProperty` with the handle and property name; if the property comes back as "no such
member," this **declines** (the `<decides>` case) rather than raising — Godot genuinely cannot
tell an absent property from a nil one, so a miss here is an ordinary failure the caller is
expected to guard. Any other non-success status (a dead receiver, a value with no wire
representation) still raises, with the same three sentences as §8.4 (verb `Read`). On success,
answers the property's value converted through §6.2. Immediate (a read has nothing to defer).

### 8.6 `VhVariantFromAny`

```
VhVariantFromAny<native>(Value:any)<decides><reads>:variant
```

Drives no `vh_godot_api` callback at all — this is a pure host-side type dispatch, over what
`ReadSelfDescribingValue` decides a value is, tested in this exact order (a value matching an
earlier test never reaches a later one):

1. **An object** — a Verse value wrapping a `vh_object`-derived instance — becomes `VH_VARIANT_
   OBJECT` with the object's own handle. Checked before the logic test below, because Verse's
   `true` is represented as an option wrapping `false`, and an object test has to be settled before
   anything is read as a logic.
2. **An integer** — `VH_VARIANT_INT`.
3. **A float** — `VH_VARIANT_FLOAT`.
4. **A string** (an array of `char8`/`char32`) — `VH_VARIANT_STRING`.
5. **One of the sixteen mirrored math structs** (matched by the Verse class's own base name against
   the layout table of §5) — decomposed into its lanes per §5's field order.
6. **A `rid`** (matched by class name specifically, distinct from the math-struct test above) —
   `VH_VARIANT_RID`, scalar-encoded per §5.1.
7. **A `logic`** — `VH_VARIANT_BOOL`. Checked *last* among the value-shape tests (after the math
   structs and `rid`, but conceptually "the same family" as the object test at the top) because a
   struct value and a logic value are otherwise both plain cells with nothing forcing an order
   between them except that a struct match must win when a struct is what was written.

Anything else — a tuple, a map, a class of the author's own that is none of the above, an *empty*
array (which carries no element type and so cannot say what it holds) — **declines**. This is the
whole contract: a value with no self-describing Godot meaning is the caller's mistake, and
declining is a better answer than a `variant` holding nothing, which would be indistinguishable
from a deliberately empty `variant{}`. Immediate; never raises; cannot suspend.

### 8.7 `VhCallValueConst`

```
VhCallValueConst<native>(Handle:int, Method:string, Args:[]variant)<reads>:variant
```

Bit-for-bit the same behaviour as `VhCallValue` (§8.4) — same callback, same argument and result
conversion, same three raised sentences, same "total, no `<decides>` outcome" rule. The only
difference between the two natives is the Verse-side effect specifier they carry, which lets a
`<reads>` caller call this one and not the other. **A reimplementation must not skip calling
`Host.Godot.CallMethod` for this native on the theory that "`<reads>` means nothing happens"** —
the C++ behind both is identical; only what the compiler will let a caller be differs. Nothing in
the runtime host verifies that the named method is actually `const` on Godot's side — that is the
generator's promise for a mirrored method and the `METHOD_FLAG_CONST` check for a generated
binding, never a runtime check here — so a hand-written or mis-generated call through this native
that in fact mutates Godot will do so without deferring, and a failure that should have undone the
write will not.

### 8.8 `VhCallVoid`

```
VhCallVoid<native>(Handle:int, Method:string, Args:[]variant)<transacts>:void
```

Checks `IsValid` on the handle **immediately** (not deferred) and raises the dead-object sentence
from §8.4 right away if it fails — this is what lets a write to a freed object fail at the exact
point the script made the call, rather than silently vanishing when the deferred write runs after
the transaction has already committed and nothing is listening for its outcome. If the handle is
live, the actual `CallMethod` invocation (with the same argument conversion as §8.4) is deferred to
commit; its result is discarded (this native answers nothing) and any status the deferred call
itself returns is not reported — a defect in a deferred call is invisible by construction, which is
exactly why the liveness pre-check exists.

### 8.9 `VhSetValue`

```
VhSetValue<native>(Handle:int, Property:string, Value:variant)<transacts>:void
```

Same shape as `VhCallVoid`: an immediate `IsValid` pre-check (raising the §8.4 dead-object sentence
on failure, verb `Wrote`), then a deferred `SetProperty` call with the value converted through §6.1.

### 8.10 `VhSingletonObject`

```
VhSingletonObject<native>(Name:string)<reads>:vh_object
```

Drives `GetSingleton` with the name; if it answers a nonzero handle, wraps it per §4.6/§4.8 with
the *singleton's own name* resolved straight to a mirrored class as the fallback (skipping
`GetClassOf` — §4.8). If `GetSingleton` answers zero (no such singleton in this build), the handle
is wrapped with **no** fallback class, so it becomes a bare `vh_object`, which any cast then
declines — this is deliberately not a raise: "this singleton does not exist in this build" is an
ordinary thing for a cast-based caller to test for. Never raises on its own account. Immediate.

### 8.11 `VhObjectOf`

```
VhObjectOf<native>(Handle:int)<reads>:vh_object
```

Wraps `Handle` per §4.6 with **no** fallback class (unlike `VhSingletonObject`) — a handle Godot
has freed, or whose class the mirror does not carry, becomes a bare `vh_object`. **Total: never
declines and never raises.** The caller is expected to have written a failable cast against the
result; a bare `vh_object` is that cast's failure case, and raising here would answer a different
question than the one the caller asked. Immediate.

### 8.12 `VhAdoptOrMint`

```
VhAdoptOrMint<native>(Object:vh_object)<transacts>:int
```

The peer-resolution native described in full in §4.1–§4.3. Called from `vh_object`'s own inherited
`block:` clause and from nowhere else — `Object` is always `Self` of whatever is under
construction. Returns the resolved or minted handle, or `0` for a class default object, for
`vh_object` itself, or (with mint suppressed, §4.2) while a reading device is active.

Otherwise, if minting was attempted and Godot refused to construct the resolved class name, this
**raises** (naming the refused class, `%hs`):

> `Godot would not make a `%hs`, so this class has no object to be. A Godot class that is abstract, or that the engine only ever hands out as a singleton, cannot be constructed -- derive from one that can, or reach the singleton through its accessor.`

On a successful mint, the row is recorded (§4.4) and an **abort compensation** is registered
(§7): if the enclosing transaction later aborts, the peer is released as a discard (freed outright
where the embedder's own ownership rules would otherwise have left it alive, since nothing outside
the aborted transaction can ever have observed it).

### 8.13 `VhCallableFrom`

```
VhCallableFrom<native>(Callback:any)<transacts>:int
```

`Callback` is checked for being a Verse function **bound to a live script instance** — the only
shape a Godot `Callable` can safely carry, because Godot's own unbound-lambda spelling is anchored
to the script resource rather than to an instance and is a known Godot leak this bridge
deliberately does not reproduce. If it is not that shape, answers `0` and does nothing further (no
raise — this is `<decides>`-shaped behaviour spelled as "answer 0," not an actual `<decides>`
native, matching the Verse declaration's own `int` result rather than a failable one).

If it is that shape, the bound object's handle and the method's decorated name are recorded
against a fresh callback id, and `MakeCallable` is driven with that id and the owning handle;
its result (a reference-table id for the Godot `Callable`, or `0` if the embedder could not make
one) is returned directly. Not deferred — a `Callable` value is needed immediately by the caller.
No compensation is registered for this one (unlike the subscribe-shaped natives below): making a
`Callable` value does not itself connect anything to Godot, so there is nothing to undo on abort
beyond the callback-id bookkeeping, which is harmless to leave in place.

### 8.14 `VhSignalEmit`

```
VhSignalEmit<native>(Id:int, Payload:any)<transacts>:void
```

Looks `Id` up in the signal-binding table (§9.1). If `Id` is `0` or otherwise names no binding,
**raises**:

> `A signal was emitted through an unbound `signal`. One a script built for itself rather than declared as a member of a class Godot instantiated names nothing, the way `godot_array{}` does.`

If the binding exists but was refused at analysis time (§9.4's reject codes), raises instead
(composed from the reject reason, verbatim reason text depends on which `vh_signal_reject` applies
— see `vh_signal_reject`'s own comments in `include/verse_host_abi.h` for the reason clauses):

> `The signal `<name>` was never registered with Godot: <reason>. Nothing was emitted.`

Otherwise, decomposes `Payload` according to the binding's recorded payload shape (§9.1 — Bare,
Tuple, or Struct) into the Godot argument list, and drives `EmitSignal` with the owning handle,
the signal's Godot name, and that argument list. If the payload cannot be decomposed into the
recorded shape (a mismatch the analysis-time check should already have prevented, but is checked
again here), raises:

> `The payload of signal `<name>` has no representation on the Godot wire, so nothing was emitted.`

**Immediate — one of the two stated exceptions to deferred writes (§7).** Handlers, including any
Verse `Subscribe`/`Await` on the same signal, run synchronously inside this call.

### 8.15 `VhSignalSubscribe`

```
VhSignalSubscribe<native>(Id:int, Callback:any)<transacts>:int
```

Same binding lookup and same two raised sentences as `VhSignalEmit` share for an unbound or
rejected signal (verb: "Subscribe was called on..." / "Cannot subscribe to..."):

> `Subscribe was called on an unbound `signal`, which names nothing.`
> `Cannot subscribe to `<name>`: <reason>`

Otherwise, makes a `Callable` for `Callback` exactly as `VhCallableFrom` does (§8.13; if that
fails, answers `0`), then drives `ConnectSignal` with the owning handle, the signal's Godot name,
that `Callable`, and flags `0` (an ordinary, repeating connection — not one-shot). Records a
subscription row and answers a fresh subscription id.

**Compensated, not deferred** (§7): the connect happens immediately, because it must answer an id
the caller can later cancel; an abort of the enclosing transaction registers `VhSignalCancel`
(§8.16) against this subscription id, so a rolled-back computation is left with no live connection
it believes it never made.

### 8.16 `VhSignalCancel`

```
VhSignalCancel<native>(Subscription:int)<transacts>:void
```

Looks the subscription up; if it is not found, does nothing (**idempotent** — a second cancel is a
no-op, matching UEFN's own `event_subscription.Cancel`). Otherwise removes the row and drives
`DisconnectSignal` with the recorded handle/name/`Callable`, then releases the `Callable`'s
reference-table id via `ReleaseRef`. Immediate (it is the compensating action itself in the abort
case, and an ordinary void write the rest of the time — there is nothing gained by deferring an
unsubscribe).

### 8.17 `VhEventEmit`

```
VhEventEmit<native>(Ev:any, Payload:any)<transacts>:void
```

Extracts the underlying object from `Ev` (an `event(t)` instance) and looks it up in the
event-binding table (§9.1, populated only for `@export_signal` event members at construction). If
it names no binding, raises:

> `Emit was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal. An event a script builds for itself is a Verse event and nothing more -- `Signal` is how tasks are resumed through one.`

Otherwise delegates to the exact same emission as `VhSignalEmit` (§8.14) against the bound signal
id — same payload decomposition, same immediate timing, same handler-runs-synchronously behaviour.

### 8.18 `VhEventSubscribe`

```
VhEventSubscribe<native>(Ev:any, Callback:any)<transacts>:int
```

Same binding lookup as `VhEventEmit`; on a miss, raises:

> `Subscribe was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal.`

Otherwise delegates to `VhSignalSubscribe` (§8.15) against the bound signal id, with the same
compensation behaviour.

### 8.19 `VhSignalBind`

```
VhSignalBind<native>(Handle:int, Class:string, Accessor:string, Name:string)<transacts>:int
```

Binds one of Godot's **own** 489 native signals (as opposed to a script-declared one) for a
generated per-class accessor such as `Timer.Timeout()`. `Class`/`Accessor` identify the generated
Verse accessor method (used only to look up the payload shape, and only once per distinct
`(Class, Accessor)` pair — cached, so calling the same accessor repeatedly, e.g. in a loop, does
not grow anything); `Name` is Godot's own signal name, the string `connect`/`emit_signal` use.

Keyed by `(Handle, Name)`: a second call for the same object and signal name answers the same
binding id rather than minting a new row, so `Timer.Timeout()` called repeatedly on the same timer
does not grow the binding table. The payload shape is discovered by resolving the accessor's
declared return type (a `signal(t)` instantiation) against the semantic program — in an editor
host — or against the sidecar's recorded payload shapes — in a runtime host, which has no semantic
program to ask (§12.3's general rule: never read declared types from the live program). Answers
the (possibly freshly minted, possibly cached) binding id. Immediate; drives no `vh_godot_api`
callback itself — it is pure bookkeeping, preparing a row `VhSignalEmit`/`VhSignalSubscribe`/
`VhSignalAwait` will later act on. Never raises.

### 8.20 `VhSignalAwait`

```
VhSignalAwait<native>(Signal:vh_signal)<transacts>:int
```

Reads `Signal`'s `Id` field and looks it up. If unbound, raises:

> `Await was called on an unbound `signal`, which names nothing and so will never be emitted.`

If bound but rejected, raises:

> `Cannot await `<name>`: <reason>`

Otherwise registers a wait (§9.2) against the binding's owning handle and signal name, connecting
with the **one-shot** flag (`VH_CONNECT_ONE_SHOT`), and answers a token naming the connection. The
signal object itself (not merely its id) is held by the host for the life of the token — this
matters because an accessor such as `Timer.Timeout()` mints a **fresh** `signal(t)` value on every
call while several such values can share one binding id, so only holding the object (not the id)
say which particular wait this is. `0` if the connect could not be made (the owning handle is dead,
or the callback wiring is incomplete) — in which case the wait never resumes, exactly as awaiting a
signal nobody emits never resumes anywhere else in this bridge.

### 8.21 `VhSignalAwaitEnd`

```
VhSignalAwaitEnd<native>(Token:int)<transacts>:void
```

**Idempotent** — a token not found (already ended, whether by resuming or by a previous end) does
nothing. Otherwise: removes the wait row, unregisters its content-scope cleanup hook if one was
attached (§9.2), disconnects the one-shot connection (harmless if Godot has already dropped it
because it already fired — the resumed case), and releases the `Callable`'s reference-table id.
Reached from the awaiting body's own `defer`, so a cancelled wait — the task that was awaiting was
itself cancelled before the signal ever fired — leaves no connection behind.

### 8.22 `VhSignalRefAwait`

```
VhSignalRefAwait<native>(Ref:int, Waiter:vh_signal)<transacts>:int
```

The same registration as `VhSignalAwait`, for a Godot `Signal` **value** the mirror has no
accessor for (one a GDScript or C# script declared, or one made with `add_user_signal`) rather
than for a mirrored engine signal. Resolves `Ref` to an owning handle and Godot signal name via the
embedder's `SignalTarget` callback; if that fails (the reference is not actually a `Signal`, or the
consumer predates the callback), raises:

> `Await was called on a Signal value that names no object and signal.`

Otherwise registers the wait exactly as `VhSignalAwait` does, with no binding id (this wait is not
tied to any `vh_signal` row — `Waiter`'s payload is always the untyped Godot-Array shape, §9.3).

### 8.23 `VhSignalRefSubscribe`

```
VhSignalRefSubscribe<native>(Ref:int, Callback:any)<transacts>:int
```

Resolves `Ref` the same way as `VhSignalRefAwait`; on failure, raises:

> `Subscribe was called on a Signal value that names no object and signal.`

Otherwise requires `Callback` to be a bound-instance method (as `VhCallableFrom` does); on failure:

> `Subscribe was given a Verse function that is not a method bound to a live script instance, which is the only shape a Godot Callable can carry without outliving what it names.`

On success, connects exactly as `VhSignalSubscribe` does (compensated on abort), with the
handler's incoming payload always the untyped Godot-Array shape (§9.3), since nothing declares a
foreign signal's arguments.

### 8.24 `VhSignalRefFor`

```
VhSignalRefFor<native>(Handle:int, Name:string)<reads>:int
```

The inverse of `VhSignalRefAwait`/`SignalTarget`: drives `MakeSignalRef` with the handle and name
and answers the resulting reference-table id (Godot's own `Signal(object, "name")`), or `0` for a
handle Godot has already freed or if the callback is absent. Immediate; never raises.

### 8.25 `Sleep`

```
Sleep<public><native>(Seconds:float)<suspends>:void
```

Covered in full in §10.

### 8.26 `VhCallStatic`

```
VhCallStatic<native>(Class:string, Method:string, Args:[]variant)<transacts>:variant
```

For Godot's static methods (no receiving object). Converts arguments as §6.1, drives `CallStatic`
with the class name, method name and argument array, converts the result as §6.2. On any non-success
status, raises using the same three-sentence family as `VhCallValue` (§8.4), with the handle
argument fixed at `0` (there is no receiving object to name) and verb `Called static`. If the
callback pointer is null, answers a default (Nil) `variant` rather than raising — unlike
`VhCallValue`, which raises when its callback is missing (`VhCallStatic`'s existing implementation
treats a missing callback as "nothing to call" rather than as a bridge fault; a reimplementation
should match this asymmetry rather than "fix" it, since fixing it would change observable
behaviour the differential harness (§10.2 of the phase document) would catch as a regression against
the reference host). Not deferred — it answers a value immediately.

### 8.27 `VhCallUtility`

```
VhCallUtility<native>(Name:string, Args:[]variant)<transacts>:variant
```

For Godot's `@GlobalScope` utility functions (also no receiving object). Same conversion and same
"missing callback answers Nil rather than raising" behaviour as `VhCallStatic`; drives `CallUtility`
with the name and argument array; raises with verb `Called` and handle `0` on a non-success status
other than "callback missing."

### 8.28 `VhCallUtilityConst`

```
VhCallUtilityConst<native>(Name:string, Args:[]variant)<reads>:variant
```

Identical behaviour to `VhCallUtility` — same callback, same conversion, same raised sentences.
Only the effect specifier differs, for the reason `VhCallValueConst` (§8.7) states: this is judged
one utility function at a time by the generator (`extension_api.json` carries no per-utility
"const" flag), not inferred, because some utilities that look like pure lookups actually move
global state (`randf`, `randomize`) and must stay `<transacts>` to keep a replay honest. A
reimplementation does not need to reproduce *which* utilities get which specifier — that judgment
is baked into which generated Verse call site uses which of these two natives, not into the natives
themselves, which behave identically either way.

### 8.29 `VhAdoptRef`

```
VhAdoptRef<native>(Value:godot_ref)<reads>:void
```

Does nothing observable beyond the fact of being called with `Value`. Its entire contract is
existential: calling it is what forces the `godot_ref`-derived Verse value to acquire whatever
native representation your reimplementation uses to detect "this value became unreachable" (in the
existing host, a `UObject` shadow — in `vm/`, whatever the collector's finalization hook is). A
`godot_ref`-derived Verse value that is never passed through this native has no such
representation and so **nothing will ever release the table entry it names** when it is dropped.
Every `godot_ref`-derived wrapper built by the generated `GodotApi.native.verse` layer routes
through this exactly once, at the point it wraps a fresh id.

### 8.30 `VhRefGet`

```
VhRefGet<native>(Ref:int, Key:variant)<decides><reads>:variant
```

Converts `Key` as §6.1, drives `RefGet` with `Ref` and the wire key. An absent key (a Dictionary)
or an out-of-range index (an Array or packed array) **declines** (`<decides>` failure) — this is
the whole reason a container read composes naturally into a Verse guard. Any other non-success
status raises using the reference-family sentences (§8.31's table). On success, converts the
result as §6.2. Immediate.

### 8.31 `VhRefSet`

```
VhRefSet<native>(Ref:int, Key:variant, Value:variant)<transacts>:void
```

Converts both `Key` and `Value` as §6.1, drives `RefSet`. **Immediate — the second of the two
stated exceptions to deferred writes (§7).** An absent-key/out-of-range status is treated the same
as success here — **silently** — because this native has no `<decides>` shape to report a miss
through; only `VhRefGet` (and, separately, an explicit bounds check a caller might write) can ever
observe that a write did nothing. Any *other* non-success status raises. The two reference-family
raised sentences, used by every reference native below that can raise (verb varies: `Read`,
`Wrote`, `Sized`, `Called`, "Called a method on"):

> `%s a Godot container that names nothing. A container built in Verse -- `godot_array{}` and the like -- holds no Godot value; one has to come back from Godot.`
> (specifically for `Ref == 0` — a container a script constructed itself, never obtained from Godot)

> `%s a Godot container the bridge no longer holds (reference %lld). A reference is released when the Verse value holding it is collected, so this is a handle kept past the object that owned it.`
> (for a nonzero `Ref` the table no longer recognises)

### 8.32 `VhRefSize`

```
VhRefSize<native>(Ref:int)<reads>:int
```

Drives `RefSize`; answers `0` if the callback is absent. Any non-success status raises (§8.31's
sentences, verb `Sized`) — unlike `VhRefGet`, there is no "ordinary miss" case here, since asking a
live container for its size cannot fail short of the reference itself being invalid. Immediate.

### 8.33 `VhRefNew`

```
VhRefNew<native>(Tag:int)<reads>:int
```

Drives `NewRef` with `Tag` (a `vh_variant_tag` naming which reference type — Array, Dictionary, or
one of the ten packed-array types), answering the freshly minted, already-claimed id, or `0` if the
callback is absent or `Tag` does not name a reference type. Immediate. Never raises.

### 8.34 `VhRefNewDefault`

```
VhRefNewDefault<native>(Tag:int)<converges>:int
```

Identical to `VhRefNew`, except that while mint suppression (§4.2) is active it answers `0`
instead of minting. This is the mechanism a container-typed data member's **archetype default**
uses — `godot_array{}` as a default value — because a data-member default may be neither a
divergent call nor one that reads the instance, which rules out calling `VhRefNew` (an ordinary
`<reads>` native) directly from a default position; `<converges>` is a specifier only a native may
carry, and this native exists solely to let a default claim it honestly. Any other caller should
call `VhRefNew` instead, which states its width (`<reads>`) truthfully.

### 8.35 `VhRefInvoke`

```
VhRefInvoke<native>(Ref:int, Args:[]variant)<transacts>:variant
```

Converts `Args` as §6.1, drives `InvokeCallable` with `Ref` and the argument array, converts the
result as §6.2. Any non-success status raises (§8.31's sentences, verb `Called`). This is what
makes a Callable-typed engine parameter (a callback the engine will invoke later) reachable at all
— the API a script uses to *invoke* a `callable` value it is holding, as opposed to `VhCallableFrom`,
which *makes* one out of a Verse function. Not deferred — answers a value immediately.

### 8.36 `VhRefInvokeVoid`

```
VhRefInvokeVoid<native>(Ref:int, Args:[]variant)<transacts>:void
```

Exactly `VhRefInvoke`, with the result discarded. Provided so a caller with no use for the return
value is not forced to declare one.

### 8.37 `VhRefCall`

```
VhRefCall<native>(Ref:int, Method:string, Args:[]variant)<transacts>:variant
```

For a method of the **builtin type** a reference names (`Signal.emit`, `Callable.bind`,
`Callable.call_deferred`, and anything else of an Array/Dictionary/Callable/Signal that the mirror
does not wrap) — reachable only this way, because `VhCallValue` takes a handle naming an `Object`,
and none of Godot's builtin (non-Object) types has one. Converts arguments as §6.1, drives `RefCall`,
converts the result as §6.2. A method name this build of Godot's builtin-type dispatch does not
recognise raises a dedicated sentence (unlike a *container* miss, which is an ordinary `<decides>`
failure elsewhere — a missing key is data, a misspelled method is a bug):

> `Godot has no method `<method>` on the value reference %lld names.`

Any other non-success status raises using §8.31's two general sentences (verb "Called a method
on"). Not deferred.

### 8.38 `VhRefCallVoid`

```
VhRefCallVoid<native>(Ref:int, Method:string, Args:[]variant)<transacts>:void
```

Exactly `VhRefCall`, with the result discarded.

### 8.39–8.42 The bulk readers: `VhRefInts`, `VhRefFloats`, `VhRefStrings`, `VhRefValues`

```
VhRefInts<native>(Ref:int)<reads>:[]int
VhRefFloats<native>(Ref:int)<reads>:[]float
VhRefStrings<native>(Ref:int)<reads>:[]string
VhRefValues<native>(Ref:int)<reads>:[]variant
```

All four drive `RefContents` once (asking for the *whole* container as one sequence) and then
reshape the result:

- **`VhRefInts`**: each element coerced to an int (§6.3) — for `PackedInt32Array`/
  `PackedInt64Array`/`PackedByteArray` and any other integer-shaped container.
- **`VhRefFloats`**: each element coerced to a double, **except** that an element which is itself a
  tuple/array (a packed *vector* array — `PackedVector2Array`, `PackedVector3Array`,
  `PackedVector4Array`, `PackedColorArray` — where `RefContents` answers one component-tuple per
  element) is **flattened**: its own components are appended in order rather than the tuple being
  coerced as one number. So a three-element `PackedVector2Array` answers six floats, in the same
  per-element field order §5 gives each math type.
- **`VhRefStrings`**: each element taken verbatim if it is already a string, otherwise an empty
  string is substituted (no coercion attempted) — for `PackedStringArray`.
- **`VhRefValues`**: each element converted through §6.2 with no reshaping — for a plain `Array`
  holding heterogeneous `Variant`s.

If the callback is absent or the reference names nothing the embedder recognises, all four answer
an **empty array** rather than raising or declining — a container the script is iterating is not
the moment to discover the bridge is down (this is a considered exception to §8.31's raise rule;
a raise here would break an ordinary `for` loop over a possibly-empty container). Immediate.

### 8.43–8.46 The bulk builders: `VhRefFromInts`, `VhRefFromFloats`, `VhRefFromStrings`, `VhRefFromValues`

```
VhRefFromInts<native>(Tag:int, Values:[]int)<reads>:int
VhRefFromFloats<native>(Tag:int, Values:[]float)<reads>:int
VhRefFromStrings<native>(Tag:int, Values:[]string)<reads>:int
VhRefFromValues<native>(Tag:int, Values:[]variant)<reads>:int
```

The reverse of §8.39–8.42: mint a fresh container of `Tag` (`NewRef`) and fill it by repeated
`RefSet` calls at successively increasing integer-index keys, `0`, `1`, `2`, …, in order — which is
also what appends to a freshly minted packed array in Godot's own resize-on-set behaviour. Each
element is converted to a wire `vh_value` as: `VhRefFromInts` — `VH_TYPE_INT`/`VH_VARIANT_INT`
verbatim; `VhRefFromFloats` — `VH_TYPE_FLOAT`/`VH_VARIANT_FLOAT` verbatim (no un-flattening — a
caller building a packed vector array from loose floats is not something these four provide; the
generated layer that wants that packs its own tuples through `VhRefFromValues` instead);
`VhRefFromStrings` — `VH_TYPE_STRING`/`VH_VARIANT_STRING`; `VhRefFromValues` — each `variant`
converted through §6.1. If `NewRef` answers `0` (bad `Tag`, or the callback is missing), answers
`0` immediately without attempting any `RefSet` calls. Immediate; never raises (a `RefSet` failure
mid-fill is not surfaced — the resulting container may simply be shorter or emptier than the input,
matching `VhRefSet`'s own silent-miss behaviour, §8.31).

## 9. Signal and event plumbing, restated as one story

This section pulls together the mechanism behind §8.14–§8.24 into the shape a reimplementation
should build once, rather than once per native.

### 9.1 What a signal binding row holds, and how a payload decomposes

Every signal binding — whether for a script-declared `signal(t)`/`event(t)` member or for a
mirrored engine signal accessor — is one row keyed by a host-assigned integer id, holding: the
owning object's handle, the signal's Godot-visible name, and a **payload shape**, which is one of
exactly three kinds, decided once (at construction time for a script member, at first-accessor-use
for an engine signal, cached thereafter) from the member's or accessor's declared payload type:

- **Bare**: the payload is one value, and it is the whole of one Godot argument. `signal(int)`,
  `signal(node2d)`.
- **Tuple**: the payload is `tuple(...)` (including the empty tuple, `signal()`'s default) — one
  Godot argument per element, positionally named `Arg0`, `Arg1`, … (Verse tuples cannot name their
  own elements).
- **Struct**: the payload is a Verse struct with no struct-typed fields of its own (nested structs
  are rejected at the declaration, §9.4) — **one Godot argument per top-level field, named by the
  field**. This is what gives the connect dialog and `_make_function` real argument names, and it
  decomposes only one level deep by design.

**Emitting** (outbound) walks the payload value according to its shape and produces exactly that
many Godot arguments, each converted through §6.1. **Receiving** (inbound — an emission delivered
back to a waiter or a handler) is the exact inverse: Bare requires exactly one incoming argument and
converts it back to the payload's declared type; Tuple requires exactly as many arguments as the
tuple has elements and rebuilds a Verse array (Verse tuples are arrays at runtime); Struct packs
however many arguments arrived into **one struct value with that many fields**, using the identical
"N loose arguments satisfy one struct parameter" rule §12.1 states for `vh_instance_call` — this is
the same rule applied a second time, once for a Godot method calling into a script and once for
Godot delivering a signal emission, and a reimplementation should share one routine for both rather
than writing it twice.

A binding row that was refused at analysis time (§9.4) is still bound — never silently dropped —
specifically so that emitting or subscribing to it later can say the analysis-time reason again
(§8.14/§8.15's second raised sentence), for the sake of a game running outside the editor, which
never saw the editor's own warning.

### 9.2 The lifetime of one `Await`

`Await` on a `signal(t)`/foreign `Signal` value is ordinary Verse layered over `/Verse.org/Verse`'s
own `event(t)` — the two natives that back it, `VhSignalAwait`/`VhSignalRefAwait` and
`VhSignalAwaitEnd`, are the entirety of what the native layer owes it:

1. **Connect**, with the **one-shot** flag (`VH_CONNECT_ONE_SHOT`) — a single `Await` call resumes
   exactly once, so letting Godot drop the connection itself when it fires is both correct and
   saves an explicit disconnect on the resuming path.
2. **Hold the signal object** (not merely its binding id) for the life of the wait, because an
   accessor such as `Timer.Timeout()` mints a fresh `signal(t)` value on every call while several
   such values can share one binding id — only the object identifies *this* wait.
3. **Anchor the wait to the awaiting task's own content scope.** If the wait is begun while a
   content scope is active (it always is, inside an ordinary script call), a cleanup hook is
   registered on that scope that calls `VhSignalAwaitEnd` if the scope is torn down before the
   signal ever fires — which is what makes a `race`'s losing branch, or an instance that is
   released mid-wait, disconnect cleanly rather than leaking a connection forever.
4. **`defer`-disconnect on the Verse side** is what actually invokes step 3's cleanup on the
   ordinary "the wait ended some other way" paths (a `race` losing, the awaiting task being
   cancelled) — the native's own idempotence (§8.21) is what makes it safe for both the scope's
   cleanup hook and an explicit `defer` to end up calling it.

`VhSignalAwaitEnd` is therefore reached from up to two independent triggers for the same
token — the scope-cleanup hook, and the resuming/cancelling code path itself — and must tolerate
either running first and the other being a no-op.

### 9.3 The untyped path: a foreign `Signal`'s payload

Nothing declares the arguments of a Godot `Signal` the mirror has no accessor for (one a GDScript
or C# script declared, or one made with `add_user_signal`). Both `VhSignalRefAwait`'s waiter and
`VhSignalRefSubscribe`'s handler therefore always receive the emission's arguments packed as **one
Godot `Array`** (a `godot_array` reference value) rather than as a decomposed payload — there is no
declared shape to decompose against. This is the one payload "kind" not listed in §9.1's three,
because it is not a *signal binding's* payload shape at all; it is what happens when there is no
binding (and so no recorded shape) to consult.

### 9.4 `@export_signal` construction-time binding, and `EnsureEventConnections`

At the moment a script instance is constructed (`vh_instantiate`, before that instance is called
into for the first time), every member the class declares that is either a `signal(t)` carrying
`@export_signal` or a bare `event(t)` (which needs no attribute — an event is useful purely between
Verse tasks and is simply absent from Godot's signal list without one) is walked and bound:

- A `signal(t)` member's binding is validated the way `vh_class_signal_list` validates it for the
  editor (payload shape supported, no nested struct field, the class actually derives from
  `vh_object` so there is something to register on) — a rejected one is still bound, carrying its
  reject code, per §9.1's rule.
- An `event(t)` member's binding is recorded the same way, keyed additionally by the underlying
  `event` object itself (since `event` is Verse's own class and cannot carry a host-visible `Id`
  field the way `vh_signal` can) — a table from `event` object identity to binding id stands in for
  `vh_signal::Id`.

**The Godot-side connection for an `@export_signal` event member is deliberately not made at this
point.** `vh_instantiate` runs before the consumer has installed the script instance on the Godot
object, and Godot's own `has_signal`/`connect` answer off the *installed* instance — connecting
here would be refused with Godot's own "Attempt to connect nonexistent signal." Instead, the
connection is made once, lazily, the **first time the instance is called into for any reason**
(the very first `vh_instance_call` on it) — a step called `EnsureEventConnections`, run
unconditionally and cheaply (a no-op for every instance whose event bindings are already connected,
and for every instance that declares none) at the top of every instance call. This moment is both
late enough (a call into the instance only happens once Godot has finished installing it) and early
enough (a Verse `Await` on the event can only be reached by running Verse code on the instance,
which *is* a call). Reconnecting is skipped for a binding that is already connected or was rejected
at binding time; a binding that fails to connect for some other reason produces:

> `The signal `<name>` was registered but could not be connected, so awaiting it would never resume.`

(silence here would be strictly worse: the member still emits normally, but nothing ever delivers
back to a Verse `Await`, and nothing else would ever say why.)

A `signal(t)` member (as opposed to an `event(t)` one) is bound the **same** way, at
instantiation — §9.1's row exists for both member kinds identically; only the event-specific
lazy-connect step is unique to `event(t)`, because a `vh_signal`'s connection lifetime is entirely
per-`Await` (§9.2) rather than a standing connection the instance holds for its whole life. The one
standing connection an `@export_signal` **event** member holds (for its whole life, made once at
first entry) is a different lifetime than the per-wait connection an ordinary `signal(t)`
`Await` makes and drops (§9.2) — the two coexist because `signal(t)` connects and disconnects once
per `Await` call while an `event(t)` member's Godot connection has to stay open across every
`Await`/`Subscribe` that might ever be made against it, since the event object itself, not a
per-wait connection, is what actually fans the emission back out to however many Verse waiters are
listening at the moment it fires.

Releasing an instance (`vh_release_instance`) tears down every standing `@export_signal` event
connection it holds: releases the `Callable`'s reference-table id, drops the callback-id row, and
removes the event-binding-table entry — none of this happens automatically otherwise, because the
binding row holds a strong reference to the event object and nothing else would ever end it, which
would otherwise be a garbage-collection root leaked per scripted node.

### 9.5 Delivery back in

Every connection made in this file (an `Await`'s one-shot, a `Subscribe`'s standing connection, or
an `@export_signal` event's standing connection) is fed through the same `Callable`-invocation path
the embedder calls back into the host on. Dispatch on the callback id's recorded purpose:

1. **An await token** — deliver the emission's arguments to whatever is waiting on that token
   (§9.2), resuming it **synchronously, inside this very call** — nothing is queued, exactly the
   way GDScript's own coroutine resume happens inside the signal that woke it. This runs in its
   own nested transaction (nested inside whatever transaction the *emitting* call is already in),
   so a raise in the resumed task unwinds only its own writes rather than the emitter's.
2. **An `@export_signal` event's standing connection** — the same synchronous-delivery shape as
   (1), against the bound event object directly rather than against a per-wait registration.
3. **An ordinary method-call subscription** (`VhSignalSubscribe`/`VhEventSubscribe`) — invoke the
   bound script method through the same call path `vh_instance_call` uses (§12.1), packing the
   emission's arguments per the binding's recorded payload shape if there is one (a mirrored/
   script-declared signal), or as the single untyped `godot_array` argument (§9.3) if there is not
   (a foreign `Signal`).

In every case, if the target instance has since been released, or the wait/binding has since ended,
delivery is a silent no-op — not an error — because the gap between Godot queueing an emission and
the host delivering it is exactly the same window an ordinary cancelled wait closes in.

## 10. `Sleep`

```
Sleep<public><native>(Seconds:float)<suspends>:void
```

`Sleep` drives **no** `vh_godot_api` callback. It is scheduled and resumed entirely inside the
runtime host's own event pump (what `vh_tick` calls), because it must work in a context with no
scene tree at all (a headless smoke test, the analysis-probe tooling, a `@tool` script that never
receives a frame).

- **`Seconds < 0.0`** resumes the calling task **immediately**, without ever suspending it at all.
- **`Seconds >= 0.0`** (including exactly `0.0`, "resume at the next tick" — the cheapest way to
  yield one frame) records a deadline as **the current wall-clock monotonic time plus `Seconds`**,
  using the same clock source the embedder's own `vh_tick`-driven pump reads (`FPlatformTime::
  Seconds()` in the existing host — any monotonic wall-clock source is equivalent), and suspends
  the calling task.
- **The clock deliberately ignores everything about simulated or paused time**: it does not read
  `Engine.time_scale`, and it keeps counting even while the game (or the whole engine) is paused.
  For anything that should respect the engine's own clock — pausing, `time_scale` — the author is
  expected to await a `Timer` or `GetTree[].CreateTimer[Seconds].Timeout()` instead; `Sleep` is
  explicitly the low-level, scene-tree-independent primitive.
- **Resumption happens from the tick pump, not from any Godot event.** Every `vh_tick` call first
  finds every sleeping task whose deadline has already passed, removes all of them from the
  sleeping set *before running any of them* (so a task that sleeps again inside its own resumption,
  e.g. `loop { Sleep(0.0) }`, does not spin the same tick forever), sorts the due set by deadline
  (earliest first, for a deterministic resumption order when several deadlines land in the same
  tick), and resumes each one in that order. **This is not subject to `vh_tick`'s budget** — the
  budget governs arbitrary queued work; a sleeper whose deadline has passed is due regardless of how
  much of the tick's time budget is left, and holding it over would be a frame of drift with no way
  for the author to observe why.
- **The deadline survives an arbitrarily long gap between ticks** (e.g. the whole pump stalling for
  a background analysis in the editor) without producing a burst of double-resumptions: because the
  deadline is a fixed timestamp rather than something accrued per tick, a task whose deadline has
  long since passed still only resumes once, the next time anything ticks.
- **Cancellation**: resuming a sleeping task re-enters that task's own content scope (§4 of
  `CLAUDE.md`'s "Signals, tasks and awaiting" section — restated here because `Sleep` is exactly
  where it is observable) and **declines to run anything** if that scope was terminated in the
  meantime (the owning script instance was released, or a runtime error terminated its scope). A
  sleeping task on a since-freed node is therefore silently dropped at its resumption point, doing
  no work and reporting nothing — there is no separate "cancel a sleep" operation; scope
  termination *is* the cancellation mechanism, uniformly, for every suspended Verse operation this
  bridge has, not only `Sleep`.
- The resumption itself runs inside its **own** nested transaction (nested inside whatever the pump
  itself is running inside), for the same reason signal delivery does (§9.5): a raise inside a
  resumed sleeper must roll back only that sleeper's own writes, never anything unrelated the pump
  happened to be doing around it.

## 11. `vh_object`'s script-level hooks, and how a reimplementation must dispatch them

§3.1 gave the four hook bodies (`_Notification` is a fifth, notification-only hook with no return
value to speak of). This section is about **how the consumer decides whether to bother calling
into the VM for one at all**, and about the one non-obvious result-encoding rule among them.

### 11.1 "Declared, not merely inherited" is the whole cost model

Every one of Godot's 1413 mirrored virtuals — and these five hand-written hooks — exists as a
method on some class in the hierarchy with a default, do-nothing body. A script that overrides
**none** of them must not pay for a VM entry on every notification or every property miss. The
mechanism: a class's own method list (what `vh_class_method_list` reports) carries only what that
class **declares** — an inherited empty body from a mirrored ancestor is not itself a declaration
on the deriving class — so "does this decorated name resolve on the instance" is *not* the test (it
resolves for everything, always, because the empty body is inherited); the actual test compares the
resolved method's underlying **procedure** against the same lookup performed starting one class
higher in the hierarchy: if they are the same procedure, the instance did not override it and the
call should never be made. A reimplementation must reproduce this identity test at whatever
granularity its own bytecode representation has for "the same compiled body," or every script pays
a VM entry for `_notification` on every notification regardless of whether it overrides `_Notification`.

### 11.2 `_Set`'s `<decides>` result crosses as `true`/`false`, written by the consumer, not inferred

`_Set` is declared `<decides>:void`, so its Verse-side value is always empty (a decline, or a
success carrying Verse's empty tuple) — there is nothing in the *value* that distinguishes "took
the write" from "declined it." The distinction the consumer needs (Godot's `_set` returns a bare
`bool`) has to come from the **call's status**, not from any value:

- The call **succeeded** (declared-`<decides>`-and-ran-to-completion) → the consumer writes `true`.
- The call **declined** (the `<decides>`-failure outcome) → the consumer writes `false`.

Both arms are written explicitly by the consumer; neither is inferred by leaving Godot to read an
empty result, because Godot reads a script virtual's boolean result through its own
"is this exactly zero/empty" test, under which an empty result **always** reads as `false` — so
inferring rather than writing would make a successful `_Set` that took the write silently look, to
Godot, exactly like a decline. This is not unique to `_Set`: it is the general rule for **every**
Godot-facing virtual whose mirror type is `<decides>:void` (i.e. every one of the 161 generated
`bool`-returning virtuals, plus this hand-written one) — a reimplementation's script-instance call
path must apply it uniformly, keyed on "is this a `<decides>`, no-declared-result virtual that
overrides a Godot-visible boolean hook," not specifically on the name `_Set`.

### 11.3 Ordering against `@export`

`_Get`/`_Set` are consulted **only** for a member name that the script's own declared members (its
`@export`ed fields) did not already answer — Godot's own order for a script's property hooks, and
the order that keeps an `@export` and a same-named `_Set`/`_Get` override from racing: the member
always wins, and the hook is never even asked about that name. `_GetPropertyList`'s entries are
**appended after** the declared members' own property-list entries, for the identical reason.
`_ValidateProperty` is the one hook that is purely additive — it is given a property dictionary
that already names an existing property (declared or hook-served) and may only mutate it in place;
there is no "validate a property that does not exist" case.

## 12. What the runtime host does around a call that is not itself one of these natives

Three facts a reimplementation must reproduce even though none of them is a `<native>` declared in
`Godot.native.verse` — they are what the runtime host does *around* dispatching into a script
instance, which every one of §8–§11's natives is ultimately called from.

### 12.1 Struct-argument packing: N loose Godot arguments satisfy one struct parameter

When Godot (or a Callable invocation reached from this file's signal/event delivery, §9.5) calls a
script method that declares **exactly one parameter, and that parameter's declared type is a
user-defined struct with exactly as many fields as there are incoming arguments**, the incoming
arguments are packed positionally into that one struct value (field order following the struct's
own declared field order) rather than being rejected for an arity mismatch. This is the identical
rule, applied at the call boundary, that already lets a signal's struct payload decompose into one
Godot argument per field on the way out (§9.1): Godot invokes the handler with N arguments because
that is what buys the connect dialog and `_make_function` real per-field names, so the handler side
has to accept N arguments for the one struct parameter the declaration actually has. **This packing
is available only through a Callable-mediated call** (a signal connection, an await delivery, an
explicit `Callable.call`) — a direct, arity-checked dispatch path (Godot's own `Object.call("...")`
by name) checks arity against the declared parameter count *first* and never reaches the packing
rule at all, answering an ordinary "wrong number of arguments" instead. A reimplementation's script
dispatch must therefore apply this packing specifically on the callback/Callable-invocation path,
not on every call path uniformly.

### 12.2 The `<decides>` result, in general, is written by the caller from the call's status

§11.2 stated this for `_Set` specifically; restated here as the general rule a reimplementation's
whole script-call boundary (not only the four hand-written hooks) must apply: **for a Godot bool
virtual — any generated `<decides>:void` override of a Godot-declared boolean hook — the answer
Godot receives is written by the consumer from the call's outcome (success → `true`, decline →
`false`), never left to whatever the Verse value happened to be**, because an empty/void Verse
result and a `<decides>` decline are otherwise indistinguishable once they reach a boolean read on
Godot's side.

### 12.3 Declared parameter and result types come from the recorded snapshot, never from the live program

Whenever a call needs to know a method's (or a signal payload's) **declared** parameter and result
types in order to marshal arguments (§6, §9.1), that information must come from a **recorded
snapshot** taken at a point before code generation ran — in an editor host, the semantic-analysis
snapshot the last successful analysis or build left behind; in a runtime host, which never
analyses, the equivalent table the cook recorded into the sidecar. It must **never** be read by
walking the *live*, currently-loaded program's own class shape, because generating executable code
from a semantic program **rewrites** it: a method whose signature includes a struct-typed parameter
of the shape §12.1 describes gets a second, *coerced* version of itself generated alongside the
original, decorated under the same name but with the parameter list flattened to one synthetic
parameter — so a live walk finds the wrong arity for the original declaration and either refuses a
call that should have worked, or (worse, silently) accepts an internal generated signature the
author never wrote and answers a wrong default. `sidecar.md` is the exhaustive field list for what
the sidecar actually carries and how each `vh_class_*` read is answered from it; this document
states only the rule that a reimplementation's marshalling code must never bypass by reading
whatever program the interpreter currently has loaded and executing.

### 12.4 Handle round-trip identity, restated as an implementation obligation

§4.6's step 2 is not optional colour: if a reimplementation's `ObjectForHandle`-equivalent ever
builds a *fresh* wrapper for a handle that this same VM previously minted (rather than returning
the original minting Verse object), two round-trips through Godot of what the author wrote as one
object will produce two Verse values that compare unequal by `=`, silently, with no diagnostic
anywhere — this is a correctness property the differential harness (`docs/phase-7.5-design.md`
§10.2) is expected to catch via a fixture that mints an object, threads it through an `Array` or a
signal payload, and asserts `=` on what comes back.

## 13. Open questions this survey did not pin down

- **The exact reject-reason sentence text for each `vh_signal_reject` value** (used to compose the
  second raised sentence in §8.14/§8.15/§8.20) is not quoted here verbatim — the enum's own
  comments in `include/verse_host_abi.h` give the *meaning* of each code precisely, but the
  human-readable sentence each one is rendered as (`SignalRejectReason` in the existing host) was
  not located and read in this pass. A clean-room implementer needing the exact wording should ask
  for a follow-up spec addition rather than guess at it.
- **Whether `VhCallStatic`/`VhCallUtility` answering a default `variant` (rather than raising) when
  their callback pointer is null is an intentional asymmetry against `VhCallValue`'s raise-on-null
  behaviour, or an oversight never exercised in practice** (because a fully wired-up embedder never
  leaves any of these three null) is noted in §8.26 as "match it, do not fix it," but the *reason*
  for the asymmetry was not found stated anywhere and may be worth a lead follow-up.
- **The precise set of Godot "const-and-answering" utility functions that get `VhCallUtilityConst`
  versus `VhCallUtility`** is generator policy (`tools/gen_verse_api.py`), not host behaviour, and
  is out of this document's scope by design (§8.28 says so) — flagged again here so it is not
  mistaken for an omission.

## 14. Exact error-string inventory (for cross-checking against `tests/`)

Every user-visible string quoted verbatim in §8–§9 above, gathered in one place:

1. `%s `%hs` on Godot object %lld, which Godot has already freed. Test IsInstanceValid[...] before reaching through a reference the scene may have dropped.`
2. `%s `%hs` on Godot object %lld, and the value has no representation on the Verse bridge. This is a gap in the type table in tools/gen_verse_api.py.`
3. `%s `%hs` on Godot object %lld, which has no such member. The generated Verse mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py.`
4. `%s a Godot container that names nothing. A container built in Verse -- `godot_array{}` and the like -- holds no Godot value; one has to come back from Godot.`
5. `%s a Godot container the bridge no longer holds (reference %lld). A reference is released when the Verse value holding it is collected, so this is a handle kept past the object that owned it.`
6. `Godot would not make a `%hs`, so this class has no object to be. A Godot class that is abstract, or that the engine only ever hands out as a singleton, cannot be constructed -- derive from one that can, or reach the singleton through its accessor.`
7. `Godot returned a value tagged %lld where the Verse bridge expected `%hs`. The type table in tools/gen_verse_api.py and this build of Godot disagree.`
8. `Godot has no method `<method>` on the value reference %lld names.`
9. `A signal was emitted through an unbound `signal`. One a script built for itself rather than declared as a member of a class Godot instantiated names nothing, the way `godot_array{}` does.`
10. `The signal `<name>` was never registered with Godot: <reason> Nothing was emitted.`
11. `The payload of signal `<name>` has no representation on the Godot wire, so nothing was emitted.`
12. `Subscribe was called on an unbound `signal`, which names nothing.`
13. `Cannot subscribe to `<name>`: <reason>`
14. `Emit was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal. An event a script builds for itself is a Verse event and nothing more -- `Signal` is how tasks are resumed through one.`
15. `Subscribe was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal.`
16. `Await was called on an unbound `signal`, which names nothing and so will never be emitted.`
17. `Cannot await `<name>`: <reason>`
18. `Await was called on a Signal value that names no object and signal.`
19. `Subscribe was called on a Signal value that names no object and signal.`
20. `MakeCallable was given a Verse function that is not a method bound to a live script instance. Only a bound method can be made into a Callable today (OQ-16): Godot's own unbound spelling is anchored to the script resource and is its known leak.`
21. `MakeCallable was given a value that is not a Verse function.`
22. `Subscribe was given a Verse function that is not a method bound to a live script instance, which is the only shape a Godot Callable can carry without outliving what it names.`
23. `The signal `<name>` was registered but could not be connected, so awaiting it would never resume.`
