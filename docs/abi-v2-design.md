# ABI v2 — the design argument

**Status:** Draft 2 · 2026-09-11 · decided and built
**For:** roadmap Phase 1.1. **Requirements:** R-NODE-6, R-NODE-9, R-TYPE-1 … R-TYPE-7, R-DIAG-2.

Roadmap §1.1 says the header is rewritten once, against the whole spec, and that "a shape that has
to grow a second calling convention has failed." This document is the argument that precedes that
rewrite. It records what was read, what it ruled out, what the spikes measured, and what was
decided. Phase 1 is built against it; §5 is the summary of what the header became.

---

## 1. What godot-cpp does, and what it teaches

The instruction was to look at godot-cpp before inventing a wire format. godot-cpp is the closest
comparable: a foreign language binding, in-process, that has to carry every `Variant` type. It
does not have a wire format at all, and the reason is the lesson.

**Value types are copied; reference types are handles.** That split is the whole design.

| godot-cpp type | representation | cost of an operation |
| --- | --- | --- |
| `Vector2`, `Transform3D`, `Color`, `AABB` … | real fields, laid out exactly as the engine lays them out | none — it *is* the engine's memory |
| `PackedByteArray` … `PackedVector4Array` | 8-byte opaque + `ptrw()` onto the engine's buffer | bulk, shared |
| `String`, `StringName`, `NodePath`, `RID` | small opaque, engine-owned | one engine call |
| `Array`, `Dictionary` | **8-byte opaque handle**, engine-refcounted | one engine call per element |
| `Callable`, `Signal` | 16-byte opaque | one engine call |
| `Variant` | fixed opaque buffer in the engine's own layout | one engine call to convert |

`Array` and `Dictionary` are `uint8_t opaque[8]` with a `_MethodBindings` table of
`GDExtensionPtrBuiltInMethod` function pointers — `method_get`, `method_set`, `method_keys`,
`method_is_typed_key`. godot-cpp never enumerates a Dictionary to hold it; it holds a reference and
asks the engine. `TypedArray<T>` is not a distinct representation at all: it is `Array` plus a
`set_typed` call, so the element type lives in the engine's container rather than in the binding.

**Three consequences for us.**

1. **Nothing in Godot's type set is recursive by value.** A `Vector2` is two floats. A
   `Transform3D` is twelve. A `PackedVector3Array` is 3N floats. The only types that contain
   arbitrary other types — `Array`, `Dictionary`, and the two that carry an object reference,
   `Callable` and `Signal` — are exactly the types godot-cpp represents as handles. So a wire
   format that carries values only never needs to nest.
2. **`Array` and `Dictionary` have reference semantics**, and that is observable. A GDScript author
   who passes a `Dictionary` into a function and mutates it expects the caller to see the mutation.
   Any representation that copies silently converts reference semantics to value semantics.
3. **Our `object` already is this design.** `object.Handle` is a Godot instance id, and every
   member access is a native call through it. Extending the same treatment to `Array`,
   `Dictionary`, `Callable` and `Signal` is not a new mechanism; it is the existing one applied
   four more times.

### What this says about the three options that were on the table

The question was posed as three wire formats. godot-cpp says the question was aimed one level too
low: **the wire only ever has to carry values**, and the containers are not values.

- **Self-describing flat encoding** (a preorder node list with arities). Buys generality we would
  not use. Nesting only arises from `Array` and `Dictionary`; if those are handles, there is
  nothing left to nest. It also *loses* against handles on semantics — a nested encoding is a deep
  copy, which is consequence 2 above. **Rejected as over-built.**
- **Recursive Verse struct.** Same objection, plus an unproven one: `Godot.native.verse` records
  that `variant` is a plain tuple deliberately, "so that `variant` stays a plain tuple VNI can
  marshal without a struct on the wire." A recursive struct across a VNI boundary is a bet, and
  the thing it buys is the generality we just said we do not need. **Rejected.**
- **Opaque host-side token for everything.** ~40 new native functions and an allocation per value,
  which is what R-TYPE-6 exists to forbid. But *applied only to the reference types*, it is
  precisely godot-cpp's design. **Accepted in the narrow form, rejected in the broad one.**

### The proposal

> **Revised by the spike in §1a.** `variant` does not stay as it is: it becomes fixed-width, which
> is what makes `[variant]variant` legal. And the release hook this section assumed does not exist.
> Read §1a before acting on anything below it.

**Value types keep the existing tuple. Reference types ride as an id, exactly as `object` does.**

```
variant := tuple(int, []int, []float, []string)      # unchanged
```

- `Vector2i`, `Rect2i`, `Vector3i`, `Vector4i` move from `Floats` to `Ints`, which they should
  always have been in: above 2^53 a float is not an int.
- `Transform2D`, `Basis`, `Transform3D`, `Projection`, `AABB`, `Plane`, `Quaternion` are 6, 9, 12,
  16, 6, 4 and 4 floats. `FloatSeq` in `GodotBindings.cpp` already writes any count; the Godot side
  in `verse_value.cpp` already reads and writes all of them. **The wire is already done.** The gap
  is entirely the Verse side: the structs, the packers, and `gen_verse_api.py`'s type table.
- `Array`, `Dictionary`, `Callable`, `Signal` cross as their tag plus one id in `Ints(0)`, the way
  `Object` crosses as its tag plus its instance id. The id names an entry in a table the
  GDExtension owns.

So `vh_value`'s existing `Map` arm and `Seq`-of-`Seq` nesting become the *bulk conversion* path —
used when a script deliberately asks for a Verse copy — rather than the ordinary one.

### The cost, stated plainly

Three things get worse, and they should be on the record before the decision, not after.

1. **Lifetime.** A Godot `Array` handed to Verse must be kept alive while Verse holds it, and
   released when Verse drops it. Verse has no destructors, but a *native* Verse class is a
   `UObject` — `Godot.object.gen.h` shows `V_VALIDATE_CLASS(object, UObject)` — so a native
   `godot_ref` base with a `BeginDestroy()` override can release its table entry. Release is then
   on UE's GC, which is nondeterministic but bounded. **This is the one piece of the proposal that
   is not already proven in this codebase**, and it should be spiked before the header is
   rewritten, not after. Fallback if it fails: the table is cleared at a frame boundary and a
   handle held across frames is a diagnosable error, which is a real functional loss.
2. **Per-element access is an ABI round trip.** Iterating a 1000-element Godot `Array` from Verse
   is 1000 crossings. godot-cpp has the identical property and it is not what anyone complains
   about; and the bulk path is there for when it matters. Worth a benchmark under R-PERF-2 rather
   than a promise here.
3. **Writes defer to commit; reads do not.** `Dict.Set("a", 1)` followed by `Dict.Get["a"]` inside
   one transaction reads the old value, because mutations go through `AutoRTFM::OnCommit`. This is
   **not new** — `set Position` followed by reading `Position` has the same behaviour today — but
   applying it to a container makes it far more likely to be hit. It needs to be documented under
   R-ASYNC-3's ordering guarantee rather than discovered.

---

## 1a. What the spike found

Written, run against `ue6-main` at `203d76492e` with the host rebuilt, and reverted. Three
questions, and the third one changes the design.

### Q1 — does VNI marshal a 23-element fixed-width tuple? **Yes.**

A `tuple(int, int, int, int, int, int, float ×16, string)` was declared `<native>`, implemented in
`GodotBindings.cpp` as `verse::tuple<int64, …, verse::string>`, and round-tripped. The host added
1000 to the tag and doubled the last float; Verse read both back:

```
spike wide: tag=1007 last=5.000000 text=wide
```

So the "plain tuple VNI can marshal without a struct on the wire" property survives being made
wide. No struct on the boundary, no new risk.

### Q2 — is it hashable, so `[variant]variant` compiles? **Yes, at compile time and at runtime.**

`[spike_wide]int` compiled, and a lookup against a key built earlier in the same function found its
value:

```
spike keyed: found 42
```

The control — today's `tuple(int, []int, []float, []string)` — was rejected in the same build with
exactly the error §2 predicted: *"Use of 'tuple(int,[]int,[]float,[][]char)' as a map key is not
yet implemented."* The whole smoke suite stayed at **247/247** with the wide tuple present.

**This is the answer to "we should have hashers so `[variant]variant` becomes valid."** It is not
reachable by supplying a hasher — Verse hashability is a compile-time property of the type, decided
by `CArrayType::GetComparability`, with no hook to supply. It is reachable by removing every `[]T`
from `variant`'s shape, and that now has a measurement behind it rather than an argument.

A second consequence, unlooked for and worth more than the first: **a fixed-width variant allocates
nothing.** Today `VhFromFloat(1.0)` builds `array{Value}` and two empty arrays; a fixed tuple builds
none. R-TYPE-6 asks that marshalling not bake in per-call allocation, and this removes the
allocation that is already there.

### Q3 — does dropping a Verse reference reach the UObject shadow? **Yes, once the object has crossed to native.**

The first run of this spike said no, and it was measuring the wrong population. Both halves are
kept below, because the wrong answer is the instructive one: it is the difference between a Verse
value that has never been native and one that has.

#### First attempt, and why it misled

500 `object{Handle := …}` were created and dropped in Verse, then a full-purge `CollectGarbage`
was run:

```
spike lifetime: before live=1 destroyed=0
spike lifetime: after  live=1 destroyed=0
```

**The live `UObject` count did not move.** A Verse object constructed by Verse code is a VM cell; it
does not get a per-instance `UObject`, so there is no `BeginDestroy` to hang a release on. The
single live object throughout is the class default object.

The hook itself works — it is simply attached to the wrong thing. Later in the same run, the smoke
suite's own script instances (allocated host-side by `vh_instantiate` → `NewObject`) were released,
and the counter moved exactly as expected:

```
[spike] tick: live=16 -> 1, destroyed=15
```

So: `BeginDestroy` fires for UObjects **the host allocates**, and never for Verse values **Verse
allocates** — which is the entire population a container handle would consist of.

One further finding, found by deadlocking on it: `FHeap::RequestFreshCollectionCycle()` called from
inside running Verse code hangs the process. `VVMCollectionCycleRequest.h` warns about this for
`Wait` ("waiting for the GC in a running context is sure to deadlock") and it applies to the
request as well. **VM collection must be driven from outside any running Verse frame** — the host's
tick, or between calls. That is a constraint on the mechanism, not an objection to it.

#### Second attempt: what a native crossing actually hands you

Declaring `VhSpikeTrack<native>(Value:object):int` and letting the link fail printed the signature
VNI expects:

```
undefined symbol: __int64 verse::Godot::VhSpikeTrack(TNonNullPtr<verse::object>)
```

**A class parameter arrives as a `UObject` pointer.** So crossing to native materialises the shadow
that Verse-side construction does not. Implementing it to return that pointer as an integer and
calling it twice on one object:

```
spike identity: stable=1
```

The shadow is **stable across crossings** — it is cached on the cell, not minted per call. And with
500 objects each forced across the boundary once, then dropped:

```
[spike] tick: live=502 -> 1, destroyed=501
```

Every one was destroyed. The hook works; the first attempt simply never created a shadow to hook.

#### The soundness half: do referenced objects survive?

A release hook that also collects live objects is worse than none, so the sweep was moved to just
after a call on a live script instance — a moment when the instance is provably reachable from
Verse — and a full purge run there:

```
[spike] after a call on a live instance: live=4 -> 4, destroyed=412
[spike] after a call on a live instance: live=5 -> 4, destroyed=413
[spike] after a call on a live instance: live=4 -> 3, destroyed=414
[spike] after a call on a live instance: live=3 -> 3, destroyed=414   (x6)
[spike] tick:                            live=3 -> 1, destroyed=416
```

Nine full purges against live instances destroyed **nothing** that was still referenced; the two
drops of one are each the instance the harness had just released, and the final pair go when the
last are released. Referenced objects survive; unreferenced ones are collected at the next cycle.

### What survives

Both halves of §1's proposal stand, and one of them is cheaper than feared.

- **The fixed-width `variant` is confirmed and improved on**: it marshals, it is hashable, and it
  removes an allocation.
- **The reference-handle half has a proven lifetime mechanism**, and it is the simple one: a native
  class' `BeginDestroy`. `Verse::VWeakCellMap` is **not needed** — it was the fallback for a
  problem that turned out not to exist. The five facts it rests on, each measured above: a class
  parameter crosses as a `UObject`; that shadow is stable; it survives GC while Verse references
  it; its `BeginDestroy` fires when Verse drops it; and collection must be driven from outside a
  Verse frame, which `vh_tick` already is.

Two costs to carry forward rather than rediscover:

1. **Release is deferred to a collection cycle**, so a released id's table entry outlives the Verse
   value by up to one GC. Bounded and fine for a table of `Variant`s, but the table's memory
   pressure is invisible to UE's GC trigger (`TickGC` collects on object-array pressure), so the
   host should request a cycle when the table grows rather than wait to be asked.
2. **A handle type must cross to native at least once to acquire its shadow.** For a reference
   type this is automatic — the id is minted by a native call — but it is a real invariant, and a
   Verse-constructed value that never crosses has no shadow and therefore no release.

---

## 2. `dictionary`, or `[variant]variant`?

The open question was whether a Godot `Dictionary` should be a mirrored Godot class or should
marshal to and from Verse's own map type, and whether `Data["hp"]` can be made to work.

### `Data["hp"]` works either way

`SemanticAnalyzer.cpp:15161` rewrites `<expr>[<args>]` on a non-function callee to
`operator'()'[<expr>, (args)]`. So a free function in the Godot package

```
operator'()'<public>(Dict:dictionary, Key:string)<decides><transacts>:variant = ...
```

makes `Data["hp"]` an ordinary failable index on a mirrored class. Ergonomics do not decide this.

### `[variant]variant` does not compile

`CArrayType::GetComparability` returns `Comparable` and **not** `ComparableAndHashable` for any
non-string array — "FArrayProperty doesn't support hashing. See SOL-2126"
(`SemanticTypes.h:831`). `CTupleType::GetComparability` takes the least comparable element
(`SemanticTypes.cpp:204`). `variant` is a tuple containing `[]int`, so `variant` is
`Comparable`-but-not-hashable, and `AnalyzeMapType`'s deferred check rejects exactly that case with
*"Use of 'variant' as a map key is not yet implemented"* (`SemanticAnalyzer.cpp:13947`).

Making `variant` a `unique` class would make it hashable — **by identity**, so
`Data[MakeString("hp")]` would never find a key stored under a different but equal object. That is
not a dictionary.

So `[variant]variant` is unavailable, and the honest alternatives are narrower:

| option | for | against |
| --- | --- | --- |
| **`dictionary` mirrored class** | Reference semantics match Godot, so a dictionary passed to GDScript and mutated behaves the way every Godot author expects. Typed dictionaries keep their key and value types, which is half of R-TYPE-2. No size limit on the wire. `Data["hp"]` via `operator'()'`. | Every access is an ABI call. Writes defer to commit, so a write is not visible to a read in the same transaction. Needs the lifetime mechanism in §1. |
| **`[string]variant` marshalled** | Hashable, so it compiles. Idiomatic Verse: `for (K -> V : Data)`, map literals, coherent within a transaction. Most Godot dictionaries really are string-keyed. | A copy: mutating it does not affect Godot's dictionary, silently. Refuses a dictionary with any non-string key, which Godot permits and uses (`Vector2i` keys in tilemaps, int keys everywhere). Loses typed-dictionary information. |
| **`dictionary` plus explicit bulk conversion** | Both, and the author chooses which they meant: `Dict.ToMap[]` gives `[string]variant` where the keys allow it, `dictionary.FromMap(...)` goes back. Reference semantics stay the default, which is the safe default. | Two spellings of one concept, and a choice the author has to understand. |

The same argument applies unchanged to `Array` versus `[]variant`.

---

## 3. R-TYPE-4: one spelling for absence

`<decides>` was the preferred answer, and the rest of this document supports it: a `dictionary`
lookup that misses is already `<decides>` through `operator'()'`, which is the same spelling a null
object return already uses. The rule then reads:

> **Any Godot value that arrives as `nil` is an ordinary Verse failure.** A mirrored method whose
> Godot signature can return `nil` is `<decides>`; one that cannot, is not.

`gen_verse_api.py` can decide `<decides>`-ness per method from `extension_api.json`'s return type
rather than from a hand list, which is what makes this a rule rather than a convention.

---

## 4. R-NODE-6: what name does Godot see, and what happens when two collide?

The preferred shape was: a script's method keeps its Verse spelling, and Godot's own virtuals are
mapped by a table generated from `extension_api.json` rather than by a naming rule. It needs a
collision story before it can be adopted. Here is the proposed one.

The generated mapping is mechanical — strip the leading `_`, PascalCase the rest — and it
reproduces today's three hardcoded names exactly (`_ready`→`Ready`, `_process`→`Process`,
`_physics_process`→`PhysicsProcess`), which is the evidence that it is the right rule rather than
a newly invented one. What makes it a *table* rather than a *rule* is that only names
`extension_api.json` marks `"is_virtual": true` on the node's own class chain are in it.

**Five collisions, and what each does.**

1. **A script declares a method whose Verse name is a mapped virtual's counterpart.**
   *Rule: it is that virtual.* The table is authoritative and `<override>` is not required for it,
   because after this change `object` no longer declares `Ready`/`Process`/`PhysicsProcess` for a
   script to override. A script that meant a plain method and got a virtual is told so: the
   analysis pass reports it at the member, with the Godot virtual it will be bound to.

2. **Two Verse overloads map to one Godot name.** Verse permits `Fire()` and `Fire(N:int)`; Godot
   has no overloading. *Rule: dispatch by argument count.* `_get_script_method_list` reports one
   entry per arity, `has_method` is true if any arity exists, and `call` picks the overload whose
   parameter count matches. Two overloads of the *same* arity are a diagnostic at the second one,
   and only the first is reachable — Godot cannot express the choice.

3. **A script overrides a method the mirrored Godot class already declares** — `GetParent` on a
   `node`. Verse accepts it; it does nothing, because Godot dispatches `get_parent` through
   ClassDB and never consults the script. The ABI header already records this under
   `vh_complete_item::IsOverridable` ("overriding one changes nothing about what Godot
   dispatches"). *Rule: diagnostic at the member.* This is the single most likely way for an author
   to write code that compiles and silently does not run.

4. **A script method and an `@export` member share a name.** Verse forbids two definitions of one
   name in a class, so this cannot arise. No rule needed; recorded so the absence is deliberate.

5. **A Verse name that is not a legal Godot method name.** Under the verbatim rule there is no
   transform to break, and Verse identifiers are a subset of what `StringName` accepts. Decorated
   names (`Process(:float)`) never leave the host. No rule needed.

**What GDScript sees**, for the record: `mover.Fire()`, `mover.TakeDamage(3)`, `mover.Greeting` —
PascalCase, as `demo/main.gd` already reads properties today. This is the C# convention in Godot
rather than the GDScript one, and it is chosen for consistency with the property path that already
exists rather than as a preference about casing.

---

## 5. What is decided by this document, and what is not

**Settled by evidence, not preference:**

- `[variant]variant` is not available (§2) — a compiler check, not a judgement.
- A self-describing or recursive wire buys generality that the type set does not require (§1),
  because Godot's only recursive types are its reference types.
- The wire and the Godot side already carry the full `Variant` set (`verse_value.cpp`); the gap
  R-TYPE-1 names is on the Verse side alone.

- `variant` becomes a fixed-width tuple of scalars (§1a Q1, Q2) — marshals, hashable, and
  allocation-free. This is settled and is the foundation of everything below.
- A native class' `BeginDestroy` is **not** a release hook for Verse-created values (§1a Q3).
  The `<override>` question is settled too: the virtual mapping is generated per class from
  `extension_api.json`'s `is_virtual` methods — 1413 of them across 106 classes — and Verse's own
  redeclaration rules then supply the `<override>` requirement and its error for free (§4).
- Method names cross verbatim; snake_case is undone only when generating *engine* names into Verse.

- **D-1 is settled by the spike**: reference types are handles, released through a native class'
  `BeginDestroy`. The mechanism is measured, not assumed, and it needs no `VWeakCellMap`.
- **`variant` is a native struct, not a tuple.** Spiked after the wide-tuple measurement: a struct
  used in a native function must itself be `<native>`, which costs a C++ shadow — the same pattern
  `object` already uses — and buys named fields on both sides. A packer then names only the lane it
  fills instead of spelling all twenty-three.
- **The math types are ours, all sixteen, nested and data-only.** `/UnrealEngine.com/Temporary/
  SpatialMath` is already a host dependency and supplies two of the sixteen with operators, but its
  `vector3`, `transform` and `rotation` live in files named `_Deprecated`, and fourteen of the
  sixteen have no counterpart at all. Two types with operators beside fourteen without is a worse
  surface than sixteen consistent ones; the operators come in Phase 2 from Godot's own builtin
  method data, for all of them.
- **Nullability is a property of the type.** `extension_api.json` records no nullability at all —
  8980 return values carry only `type` and `meta` — so the choice was a rule or a hand list. A scan
  of Godot's doc XML found five value-typed returns documented as nullable out of 5304, four of
  them false positives, and the one real case editor-only. The rule wins.

So ABI v2 is:

```
variant<native> := struct:        # a native struct: VNI marshals one given a C++ shadow
    Tag:int                       # Godot's Variant::Type
    Ref:int                       # instance id, or an id in the GDExtension's reference table
    I0..I3:int                    # bool, int, Vector2i/3i/4i, Rect2i
    F0..F15:float                 # every math struct up to Projection
    Text:string                   # String, StringName, NodePath
```

Hashable, so `[variant]variant` is a legal Verse type. Allocation-free for every value type.
Reference types — `Object`, `Array`, `Dictionary`, `Callable`, `Signal`, and the ten packed arrays
— ride in `Ref`, with mirrored Verse classes over them and generated typed accessors, because a
script cannot spell a `variant` and so cannot unpack one itself. No nesting on the wire, ever.

**Phase 1 is built.** What it cost, and what it turned up, is in the roadmap's exit criteria.
