# Ops: the per-op reference

Status: reviewed by the lead 2026-09-24. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/facts.md`, `docs/web-vm/tasks.md`,
every other file of `docs/web-vm/spec/` (values, unification, failure, calls, objects, tasks,
modules, natives, godot-natives, sidecar); UE `Engine/Source/Runtime/CoreUObject/Private/VerseVM/`
(the interpreter's arithmetic, comparison, container, reference, freeze/melt, profile, module and
fast-fail ops), `Engine/Source/Runtime/CoreUObject/Public/VerseVM/VVMRef.h` and
`Inline/VVMValueInline.h`, `Engine/Source/Runtime/CorePreciseFP/Public/VerseVM/VVMFloat.h`,
`Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/VVMCodeGenerator.cpp` (`set`, `set live`,
`.Length`, profile blocks, value domains, module data), and Epic's `LiveVariables.versetest`.
Probes: `tests/verse_probe/vm_ops_probe.verse`, `tests/verse_probe/vm_ops_live_probe.verse`.

This file is an **index** over the other spec files. For every op the compiler emits it gives the
operands, what the op does in a few sentences in those files' vocabulary, how it fails, raises or
suspends, and which section owns the detail. Where no other file specifies an op, this file does,
in full; §3 collects those rules. Waiting on unbound operands is `unification.md` §6.1's table and is
referenced, not restated.

Probe citations read `ops:Method` for `vm_ops_probe.verse` and `live:Method` for
`vm_ops_live_probe.verse`. Run either with

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        <repo>/tests/verse_probe/vm_ops_probe.verse --class vm_ops_probe

A claim read in source and not run is marked **(source, unprobed)**.

## 0. Reading order for an implementer

The ops in the order that gets a program running soonest, grouped by the task in
`docs/web-vm/tasks.md` that builds them. Each group assumes the ones above it.

| Stage | Task | Ops | What runs afterwards |
| --- | --- | --- | --- |
| 1. A method that prints | T3.3 → T3.4 | `Reset`, `ResetNonTrailed`, `Move`, `Call` (function and native callees only), `CallWithSelf` (native procedure callee), `Return`, `NewArray`, `Add` (arrays: string concatenation) | a zero-argument method that interpolates a string and calls `Print` |
| 2. Arithmetic and values | T3.4 | `Add`, `Sub`, `Mul`, `Div`, `Neg`, `Length`, `NewOption`, `NewMap`, `MapKey`, `MapValue`, `NewMutableArray`, `ArrayAdd`, `InPlaceMakeImmutable`, `MoveTrailed`, `ReturnTrailed` | expressions, tuples, maps, collecting `for` |
| 3. Control flow and fast failure | T3.4 | `Jump`, `JumpIfInitialized`, the nine `…FastFail` ops, `EndFastFailureContext`, `Query` | `if` with a pure condition, `for` with filters, named parameters with defaults |
| 4. Calls and closures | T3.4 | `Call` (array, map, type callees), `CallWithSelf` (function callee), `NewScope`, `NewFunction`, `LoadParentScope`, `LoadCapture`, `BeginProfileBlock`, `EndProfileBlock` | local functions, nested tuple parameters, `profile` |
| 5. Variables | T3.4, recording in T3.5 | `NewRef`, `RefGet`, `RefSet`, `RefCallDomain`, `Melt`, `Freeze`, `FreezeIfAccessor`, `LengthWithEffects`, `CallSet`, `CanFastAppendToArrayFastFail`, `FastAppendToArray` | `var` locals, `set`, `set X += …`, `set A[I] = …` |
| 6. Full failure | T3.5 | `BeginFailureContext`, `EndFailureContext`, `Lt`, `Lte`, `Gt`, `Gte`, `Neq`, `Div`'s failure; the undo records of every write in §3.3 | `if` whose condition writes or calls a `<decides>` function; `<decides>` entries; runtime-error rollback |
| 7. Objects and classes | T3.6 | `NewObject`, `CreateField`, `UnifyField`, `InitializeVar`, `UnifyNativeObject`, `UnwrapNativeConstructorWrapper`, `LoadField`, `LoadFieldFromSuper`, `LoadConstructor`, `JumpIfDefaultSubObject`, `SetField`, `TypeCastFastFail` on a class, `NewClass`, `BindNativeClass`, `ConstructNativeDefaultObject`, `BeginModule`, `EndModule`, `EndModuleData` | script classes, construction, blocks, fields, casts |
| 8. Unification | T3.7 | `MoveNonComparable`; unification into non-fresh destinations everywhere | `=` in a full context, definition binding |
| 9. Tasks | T4.1 | `SelfTask`, `BeginTask`, `CallTask`, `EndTask`, `Yield`, `NewSemaphore`, `WaitSemaphore`, `Switch` | `spawn`, `branch`, `sync`, `race`, `rush`, `return` out of an arm |
| 10. Cancellation | T4.2 | `ResumeUnwind`, and the unwind edges | `defer`, cancellation |
| 11. Waiting on state | T4.3 | `BeginAwait`, `AwaitSuccess`, `EndAwait`, `BeginBatch`, `EndBatch`, `RefSetLive`, `CallSetLive`, `SetFieldLive`; §3.1 and §3.2 | `await`, `batch`, `set live` |

Never needed: the ten inline-cache ops (§14); `Mod`, `MutableAdd`, `NewMutableArrayWithCapacity`
(refused at load, §13); the three union ops (refused at load, §9.10); `LoadImport` and
`NewPersistentOrSessionWeakMapRef` (unreachable from a Godot script, §10, §8.3).

## 1. Conventions

### 1.1 How each entry reads

**Operands** lists `ops.json`'s operands in its order as *Name* (role, kind). "Absent in practice"
marks a `value` operand the compiler leaves absent although `ops.json` does not mark it optional; an
absent operand reads as uninitialized (`calls.md` §2.3, `format.md` §5.1). **Cache operands**
(`cache: true` in `ops.json`) are not in the file (`format.md` §5.1); they are listed so a reader
generated from `ops.json` knows to skip them.

**Waits** points at `unification.md` §6.1, which names the operands an op parks on and in which
order. In a stage-1 interpreter every park is the stage-1 park error (`failure.md` §9.5).

**Outcome** uses the vocabulary of the other files: an op **continues** (execution goes on at the
next op), **jumps**, **fails** (the current full failure context fails, `failure.md` §4; a
fast-fail op instead jumps to its `OnFailure`, `failure.md` §5), **raises** a runtime error
(`failure.md` §9), or **suspends** the task (`tasks.md` §4.2).

**Owner** is the section that holds the detail.

### 1.2 Results are unified

Every `unify_def` operand receives its value by unification (`unification.md` §3), never by a plain
store. In emitted code nearly every destination is fresh, so the unification is a store; the two
patterns that rely on a non-fresh destination are `unification.md` §3.3's. "Into `Dest`" below
always means "unified into `Dest`".

### 1.3 Outside the contract

An operand of a kind the op does not define — which the compiler's type checking prevents — makes
the reference abort the process. The interpreter reports it as a **VM invariant violation**
(`failure.md` §9.4): a runtime error with diagnostic `ErrRuntime_Internal`, rolled back like any
other. Each entry says what is outside its contract; `values.md` §15 lists the value-level cases.

### 1.4 Name collision with the host callbacks

`NewRef`, `RefGet` and `RefSet` are also the names of `vh_godot_api` callbacks that
`godot-natives.md` §8.30–§8.46 drives (Godot containers). **The ops of this file act on Verse
variables and have nothing to do with those callbacks.** A Verse op never calls the host except
through a native.

### 1.5 Operands absent in practice

| Op | Operand | When |
| --- | --- | --- |
| `Move` | `Source` | the escape scope's result and switch registers are set to uninitialized by a `Move` from an absent operand (`tasks.md` §6.2) |
| `NewFunction` | `Self`, `ParentScope` | compiler-generated helper procedures, including the live-binding loop (`calls.md` §7.2, §3.2 here) |
| `BeginTask` | `Parent` | `spawn` (`tasks.md` §6.3) |
| `CallTask` | `Parent` | `spawn` (`tasks.md` §6.3) |
| `EndTask` | `Which` | every task body that is not an arm of an escape scope (`tasks.md` §6.3) |
| `EndTask` | `Value` | only in compiler helpers for persistent maps, which a Godot script cannot reach (source, unprobed) |

`CanFastAppendToArrayFastFail`'s `Ref` and the first `RefSet` of a `set live` declaration are given
a **constant** holding uninitialized rather than an absent operand; both read the same.

## 2. What compiled `set` looks like

Several ops only make sense as a sequence. These shapes are what the compiler emits; the
interpreter needs no knowledge of them beyond the ops, but a reader of a dump will meet them
(source, and consistent with every probe here):

| Source | Ops, in order |
| --- | --- |
| `var X:t = E` | `Reset R`, `NewRef R`, *E*, `RefCallDomain D ← R, E`, `Melt M ← D`, `RefSet R, M` |
| `set X = E` (`X` a `var`) | *E*, `RefCallDomain D ← X, E`, `Melt M ← D`, `RefSet X, M` |
| `set X += E` (and the other assignment operators) | *E*, `RefGet G ← X`, `Freeze F ← G`, `Add N ← F, E`, then as `set X = N` |
| reading `X` | `RefGet G ← X`, `Freeze V ← G` |
| `X.Length` for a `var` array or map | `FreezeIfAccessor A ← X`, `LengthWithEffects L ← A` |
| `set A[K] = E` (`A` a `var`) | *E*, `Melt M ← E`, `CallSet C, K, M` where `C` is the `var`'s melted content |
| `set A += B` (fast path, result unused) | *B*, `RefGet G ← A`, `CanFastAppendToArrayFastFail (A, G)`, `EndFastFailureContext`, `FastAppendToArray G, B`, `RefSet A, G`; on the slow path `Freeze`, `Add`, `RefCallDomain`, `Melt`, `RefSet` |
| `set O.F = E` on a class `var` member | `LoadField R ← O, F` (the member's variable), then as `set X = E` on `R` |
| `set live X = E` | §3.2 |

A `set M[K] = V` on a map is `<decides>` in the language — the compiler refuses it outside a failure
context (error 3512, *"This map access can fail"*, met while writing `vm_ops_probe.verse`) — although
`CallSet` on a mutable map never fails (§9).

## 3. Rules this file specifies

Three behaviours no other spec file states. They are shared by several ops, so they are stated once
here and referenced from each op.

### 3.1 Await registration of element and field reads

`tasks.md` §5.7 says that while a task has an await point, every read of a `var` registers the task
with that variable, and a write to it resumes the task. The same holds one level deeper: while the
current task has an await point,

- reading an element of a **mutable array** (`Call` with a mutable-array callee,
  `ArrayIndexFastFail`),
- reading a value of a **mutable map** (`Call` with a mutable-map callee), and
- reading a non-constant field of a **VM-level object** — a struct value, or any object not held as
  a native object (`LoadField`)

registers the task with **that slot**. The implementation may do this by replacing the slot's
content with a hidden variable that holds the same value and has the task registered, which is what
the reference does; the replacement is not a transaction write and is never undone. A slot that
already holds such a hidden variable answers the variable's content and registers the task again.
Everything that reads a slot reads through a hidden variable transparently; freezing a value reads
through every hidden variable it contains.

A **write** to such a slot — `CallSet` on the element or map entry, `SetField` on the field — is a
write to the hidden variable, and follows `RefSet` (§8.3): the live-binding rule of §3.2, the
content replaced and recorded, and every registered task resumed (or batched, `tasks.md` §5.7).

Measured (`live:A01_AwaitElement`–`A03_WriteElement`): a task spawned on
`await{Elems[0] > 2}` for a `var Elems:[]int` resumes, synchronously, inside `set Elems[0] = 3` —
a write that changes the element through `CallSet` and never writes the variable `Elems` itself.

No other observable effect exists: without an await point nothing is registered and nothing changes.
An interpreter that implements `await` must implement this, or an `await` over an element or a
struct field never wakes. `objects.md` §18 Q5 asked whether the reference's "transparent reference"
in a slot is reachable: it is exactly this, and it is reachable through `await`.

`LengthWithEffects` reading a `var` does not register the task (source, unprobed), so
`await{X.Length > 2}` wakes only through the variable's other reads in the condition (§17 Q3).

### 3.2 Live bindings

`set live X = E` and `var live X:t = E` make `X` follow `E`: `X` is set now, and again whenever a
`var` that `E` read is written. Both compile in this project's script package and run
(`live:L01_Bind`, `L02_WriteSource`: after `set live Dst = Src`, `set Src = 5` makes `Dst` 5 within
the same call). They are reachable from a Godot script, which `tasks.md` §5.5 and `unification.md`
§5 did not expect (§15 items 5 and 6).

What the compiler emits for one (source; each op is specified in its own entry): a closure computing
`E`; a task body that loops — `await` on the closure's value, `Freeze` of the target's current
value, a comparison by `MoveNonComparable` in a full failure context that goes round again when the
two are equal and leaves the loop when they differ, then a `CallTask` of the loop body again (as a
child of the binding task below) and the live write; then, in an inline task started for the
binding — the **binding task** — a `SelfTask`, a `CallTask` of the loop with the binding task as
`Parent`, and a `Yield` that nothing resumes; then, back in the enclosing code, a first ordinary
evaluation of `E` and the live write of its value. The live write is `RefSetLive` for a `var`,
`CallSetLive` for an element or map entry, `SetFieldLive` for a struct field; its `Task` operand is
always the binding task, so cancelling the binding task (and with it the loops, its children) is
what ends the binding. `var live X:t = E` compiles the same way (source; Epic's own tests use it,
this project's probe used `set live`).

The rule the three `…Live` ops and the ordinary writes share:

- Every variable (and every hidden variable of §3.1) has at most one **live task**, initially none.
- A **live write** to a variable with `Task` = *T*: if the variable has a live task other than *T*,
  that task is cancelled first (`tasks.md` §7.1, as a native cancel: nothing waits); then *T*
  becomes the variable's live task; then the write proceeds as the ordinary write does.
- An **ordinary write** to a variable (`RefSet`, and `CallSet`/`SetField` through a hidden
  variable) cancels the variable's live task, if it has one, before writing. So a plain `set`
  of a live-bound `var` ends the binding (`live:L03_OverwriteBound`, `L04_WriteSourceAgain`:
  after `set Dst = 100`, a later `set Src = 7` leaves `Dst` at 100).
- If cancelling the live task raises, the write raises.
- A live write to an **element** that is not held in a hidden variable records nothing: the element
  has no live task. So an ordinary write of the element does not end the binding
  (`live:L07_OverwriteBoundElement`, `L08_WriteSourceAgainForElement`: after
  `set live Arr[1] = Src`, `set Arr[1] = 100`, a later `set Src = 11` makes `Arr[1]` 11 again).

Whether the loop task's re-evaluation, cancellation and failure-context use obey anything beyond
`tasks.md` is not separately specified: they are ordinary tasks, awaits and failure contexts.

### 3.3 Which writes are recorded

For `failure.md` §6: `RefSet`, `RefSetLive`, `CallSet`, `CallSetLive`, `SetField`, `SetFieldLive`,
`FastAppendToArray`, `ArrayAdd` with `bTransactional` true, `InPlaceMakeImmutable`, `Reset`,
`MoveTrailed` into a fresh register, `ReturnTrailed`, `EndTask`'s `Write`/`Switch`, and a
placeholder link. Changing a variable's live task (§3.2) is task state and follows `tasks.md` §2.
The hidden-variable replacement of §3.1 is not recorded.

## 4. Arithmetic

### Add

**Operands:** `Dest` (unify_def, register), `LeftSource` (use, value), `RightSource` (use, value).

The first matching row decides:

| Left | Right | Into `Dest` |
| --- | --- | --- |
| integer | integer | the exact sum (`values.md` §2.2) |
| float | float | the IEEE sum (`values.md` §3.1) |
| a rational on either side, the other a rational or an integer | | the exact sum in lowest terms (`values.md` §2.4) |
| array or mutable array | array or mutable array | a new immutable array, left's elements then right's (`values.md` §6.3) |
| `false` | anything | `RightSource` unchanged |
| anything | `false` | `LeftSource` unchanged |

Integer arithmetic never wraps: `(2^63 − 1) + 1 − 1` equals 2^63 − 1, `2147483647 + 1` prints
`2147483648`, and a value computed through the heap range comes back to an ordinary small integer —
`A[(2^63 − 1 + 1) − (2^63 − 1)]` indexes element 1 and a map keyed by `1` is found by
`(2^63 + 1) − 2^63` (`ops:P01_IntBoundaries`). `false + false` is `false` and `"" + ""` has
Length 0 (`ops:P08_Concat`).

**Waits:** `unification.md` §6.1. **Outcome:** continues; never fails, never raises. An int with a
float, a rational with a float, and any other pairing is outside the contract (the compiler refuses
int + float, `values.md` §2.2). **Owner:** `values.md` §2.2, §2.4, §3.1, §5.4, §6.3.

### Sub

**Operands:** `Dest` (unify_def, register), `LeftSource`, `RightSource` (use, value).

Integer − integer exactly; float − float by IEEE; a rational with a rational or integer, exactly in
lowest terms. Nothing else — arrays included — is defined. `−(2^63) − 1 + 1` prints
`-9223372036854775808` (`ops:P01_IntBoundaries`).

**Waits:** `unification.md` §6.1. **Outcome:** continues; never fails or raises. **Owner:** `values.md` §2.2, §2.4,
§3.1.

### Mul

**Operands:** `Dest` (unify_def, register), `LeftSource`, `RightSource` (use, value).

| Left | Right | Into `Dest` |
| --- | --- | --- |
| integer | integer | the exact product |
| integer | float | the integer converted to the nearest binary64 (ties to even; too large → ±Inf), times the float |
| float | integer | the float times the converted integer |
| float | float | the IEEE product |
| a rational on either side, the other a rational or an integer | | the exact product in lowest terms |

Measured: `(−2^63) × −1` equals 2^63; `(2^63−1)²` is greater than 2^63−1 and divides back exactly;
`(−2^63) × 1.0` prints `-9223372036854775808.000000`, `(2^63−1) × 1.0` prints
`9223372036854775808.000000`, `0 × −1.5` prints `0.000000` (`ops:P01_IntBoundaries`). A rational
with a float is outside the contract.

**Waits:** `unification.md` §6.1. **Outcome:** continues; never fails or raises. **Owner:** `values.md` §2.2, §2.7,
§3.1.

### Div

**Operands:** `Dest` (unify_def, register), `LeftSource`, `RightSource` (use, value).

| Left | Right | Outcome |
| --- | --- | --- |
| integer | integer 0 | **fails** |
| integer | non-zero integer | a rational in lowest terms, positive denominator — never an integer, even when exact |
| float | float | the IEEE quotient, with a divisor of −0 treated as +0; never fails |
| a rational on either side, the other a rational or an integer | zero | **fails** |
| a rational on either side, the other a rational or an integer | non-zero | the exact quotient |

`Floor((2^63−1)² / (2^63−1))` is 2^63−1 (`ops:P01_IntBoundaries`). Int with float in either order
is outside the contract (the compiler refuses it).

**Waits:** `unification.md` §6.1. **Outcome:** continues or fails; never raises. **Owner:** `values.md` §2.3, §2.4,
§3.1; `failure.md` §2.

### Neg

**Operands:** `Dest` (unify_def, register), `Source` (use, value).

Integer: the exact negation (`−(−2^63)` exceeds 2^63−1, `ops:P01_IntBoundaries`). Float: the sign
flipped, so `Neg` of `0.0` is −0 (unobservable, `values.md` §3.2). Rational: the numerator negated.
Anything else is outside the contract.

**Waits:** `unification.md` §6.1. **Outcome:** continues; never fails or raises. **Owner:** `values.md` §2.2, §3.1,
§3.2.

## 5. Comparisons in a full failure context

These five are what a comparison compiles to inside a **full** failure context (`failure.md` §10);
the same comparison in a fast context is the `…FastFail` form (§6). The two families give identical
answers, including every NaN case (`ops:P02_FullContext` against `ops:P03_FastForms`).

**The result of a successful comparison is its left operand**, into `Dest`: `(3 < 5)` gives 3,
`(4 <> 5)` gives 4, `(2.5 >= 2.5)` gives `2.500000` in a full context as in a fast one
(`ops:P02_FullContext`; `values.md` §12).

### Lt

**Operands:** `Dest` (unify_def, register), `LeftSource`, `RightSource` (use, value).

Succeeds iff `LeftSource < RightSource`: two integers exactly at any size; two floats by Verse's
ordering, under which every NaN comparison with `<` fails (`values.md` §3.3); a rational with a
rational or an integer, exactly. Any other pair is outside the contract (the compiler refuses
ordering on chars, strings, rationals and everything else, `values.md` §12). On success
`LeftSource` goes into `Dest`.

**Waits:** `unification.md` §6.1. **Outcome:** continues or fails. **Owner:** `values.md` §3.3, §12.

### Lte

**Operands:** as `Lt`.

Succeeds iff `LeftSource <= RightSource`, with the same kinds as `Lt`. For floats this is Verse's
`<=`, which **succeeds for NaN against NaN** and fails for NaN against anything else — not IEEE
(`values.md` §3.3; `ops:P02_FullContext`: NaN ≤ NaN succeeds, NaN ≤ 1.0 fails). `−0.0 <= 0.0`
succeeds.

**Waits:** `unification.md` §6.1. **Outcome:** continues or fails. **Owner:** `values.md` §3.3, §12.

### Gt

**Operands:** as `Lt`. Succeeds iff `RightSource < LeftSource` under `Lt`'s rules; NaN on either side
fails. **Waits:** `unification.md` §6.1. **Outcome:** continues or fails. **Owner:** `values.md` §3.3, §12.

### Gte

**Operands:** as `Lt`. Succeeds iff `RightSource <= LeftSource` under `Lte`'s rules, so NaN ≥ NaN
succeeds and `1.0 >= NaN` fails (`ops:P02_FullContext`). **Waits:** `unification.md` §6.1. **Outcome:** continues or
fails. **Owner:** `values.md` §3.3, §12.

### Neq

**Operands:** `Dest` (unify_def, register), `LeftSource`, `RightSource` (use, value).

Compares by `values.md` §11 equality; succeeds with `LeftSource` into `Dest` iff the answer is
**not equal**. Equal, undecidable and error all fail (an error has already been raised by the
comparison). Through `comparable`: `1 <> 1.0` succeeds, `array{1, 2} <> array{1, 2, 3}` succeeds,
`array{1, 2} <> array{1, 2}` fails (`ops:P02_FullContext`). A nested unbound placeholder makes
`Neq` wait only when no decidable difference exists (`unification.md` §6.1).

Note that `=` has no ordinary op: in a full context it is two `Move`s into one register
(`unification.md` §3.3), and `1 = 1.0` through `comparable` fails there too
(`ops:P02_FullContext`).

**Outcome:** continues or fails. **Owner:** `values.md` §11.3; `unification.md` §6.1.

## 6. Fast-fail ops

All nine share one shape (`failure.md` §5): `LeniencyIndicator` is the context's indicator register,
normally fresh; `OnFailure` is where a failed test jumps. On success the result goes into `Dest` (if
the op has one) and execution continues; on failure the op **jumps** to `OnFailure` and nothing else
happens — no transaction, no failure of the full context. If the indicator holds a fast failure
context record (only after a park), the record is marked failed (`unification.md` §10.2). None of
them raises, except that an equality whose comparison raised (a struct field read, `values.md`
§11.1) has already raised by the time `EqFastFail` or `NeqFastFail` jumps. **Waits:** `unification.md` §6.1 (top-level operands only). **Parking:**
`unification.md` §10.

### LtFastFail

**Operands:** `Dest` (unify_def, register), `LeniencyIndicator` (unify_def, register), `Lhs`, `Rhs`
(use, value), `OnFailure` (jump, label).

`Lt`'s test; `Lhs` into `Dest` on success. **Owner:** `failure.md` §5.2; `values.md` §12.

### LteFastFail

**Operands:** as `LtFastFail`. `Lte`'s test, NaN ≤ NaN included (`ops:P03_FastForms`); `Lhs` into
`Dest`. **Owner:** `failure.md` §5.2 (but see §15 item 1); `values.md` §3.3, §12.

### GtFastFail

**Operands:** as `LtFastFail`. `Gt`'s test; `Lhs` into `Dest`. **Owner:** `failure.md` §5.2.

### GteFastFail

**Operands:** as `LtFastFail`. `Gte`'s test; `Lhs` into `Dest`. **Owner:** `failure.md` §5.2.

### EqFastFail

**Operands:** as `LtFastFail`.

Succeeds iff `values.md` §11 equality answers **equal**; not equal, undecidable and error all jump
to `OnFailure`. `Lhs` into `Dest`. A nested unbound placeholder counts as equal and stays unbound
(`unification.md` §12 Q6). Measured beyond `values.md`: a `char` never equals a `char32` of the same
code point (`'a'` against `0u0061`); an integer equals a rational with denominator 1 in either order
(`2 = 4/2`, `4/2 = 2`); `option{1}` does not equal `option{1.0}` (`ops:P03_FastForms`).

**Owner:** `failure.md` §5.2; `values.md` §11.

### NeqFastFail

**Operands:** as `LtFastFail`. Succeeds iff equality answers **not equal**; `Lhs` into `Dest`
(`'a' <> 0u0061` succeeds, `ops:P03_FastForms`). **Owner:** `failure.md` §5.2; `values.md` §11.3.

### ArrayIndexFastFail

**Operands:** `Dest` (unify_def, register), `LeniencyIndicator` (unify_def, register), `Array`,
`Index` (use, value), `OnFailure` (jump, label).

| `Array` | Succeeds when | Into `Dest` |
| --- | --- | --- |
| array or mutable array | `Index` is an integer in [0, 2^32) below the length | the element (for a mutable array, as currently stored, with §3.1's registration) |
| `false` | never | — |
| anything else | outside the contract | |

A negative index, one at or beyond the length, and any integer of 2^32 or more jump — a heap-int
index included, but an index *computed through* the heap range that ends small is an ordinary index
(`ops:P01_IntBoundaries`: `A[4294967296]` fails, `A[4294967296 − 4294967295]` is element 1).

**Owner:** `failure.md` §5.2; `values.md` §6.2.

### TypeCastFastFail

**Operands:** `Dest` (unify_def, register), `LeniencyIndicator` (unify_def, register), `Type`,
`Value` (use, value), `OnFailure` (jump, label).

Succeeds with `Value` unchanged into `Dest` when `Type` admits it; a cast never converts. By the
kind of the `Type` cell:

| `Type` | Admits |
| --- | --- |
| `class` of kind class or interface | an object whose class is `Type` or inherits it transitively (`objects.md` §13) |
| `int type` | an integer, or a rational with denominator 1, within the bounds; an uninitialized bound is unbounded (`objects.md` §13) |
| `float type` | a float *v* with lower ≤ *v* and (upper is NaN, or *v* ≤ upper); NaN only when lower is −Inf and upper is NaN (`values.md` §13). Measured with `type{_X:float where 0.0 <= _X, _X <= 1.0}`: 0.5, 0.0, −0.0 and 1.0 pass; 2.0, −1.0, NaN and Inf jump (`ops:P07_FloatCast`) |
| `simple type` `any` | everything |
| any other type cell — `tuple type`, `map type`, `array type`, `option type`, `pointer type`, another simple type, a struct class, `false`, `true`, `option` | outside the contract |
| a value that is not a type | outside the contract |

The same test in a full context is `Call` with the type as callee (§11).

**Owner:** `objects.md` §13; `values.md` §13.

### QueryFastFail

**Operands:** `Dest` (unify_def, register), `LeniencyIndicator` (unify_def, register), `Source`
(use, value), `OnFailure` (jump, label).

`Query`'s test (§6 `Query` below): jumps on `false`; otherwise the option's content into `Dest`. The
compiler's explicit "fail here" is a `QueryFastFail` of the constant `false`.

**Owner:** `values.md` §9; `failure.md` §2.

### CanFastAppendToArrayFastFail

**Operands:** `LeniencyIndicator` (unify_def, register), `Ref` (use, value), `MaybeMutableArray`
(use, value), `OnFailure` (jump, label). No `Dest`.

The guard of `set A += B`'s fast path (§2). Passes iff `MaybeMutableArray` is a mutable array
**and** `Ref` is not a `var` that has a domain function. `Ref` is the `var` itself for a variable,
or the uninitialized constant for an element or struct field; an accessor reference or a native
reference as `Ref` also passes (source, unprobed; §15 item 9). Jumping selects the slow path, which
is indistinguishable to a script.

**Owner:** `values.md` §6.4.

### Query

(The ordinary form; listed here beside its fast-fail twin.)

**Operands:** `Dest` (unify_def, register), `Source` (use, value).

| `Source` | Outcome |
| --- | --- |
| `false` | **fails** |
| `true` | succeeds; `false` (its content) into `Dest` |
| any other option | succeeds; its content into `Dest` |
| anything else | outside the contract |

`option{3}?` in a full context gives 3; `false?` fails (`ops:P02_FullContext`).

**Decision (`values.md` §16 Q5):** the reference answers a native object by succeeding and leaving
`Dest` holding uninitialized. The compiler emits `Query` only for the `?` of a `logic` or an option
and for its explicit "fail here" on the constant `false`, so a native object never reaches it; the
interpreter treats it as outside the contract, like every other non-option. **Waits:** `unification.md` §6.1.
**Owner:** `values.md` §9.

## 7. Failure contexts

### EndFastFailureContext

**Operands:** `OuterLeniencyIndicator` (unify_def, register), `LeniencyIndicator` (use, value),
`OnDone` (jump, label).

Nothing parked (the indicator is fresh, or its record has no outstanding ops): continues into the
then-branch; `OnDone` and `OuterLeniencyIndicator` unused. **Reading the indicator here must not
turn it into a placeholder.** Otherwise the undecided path of `unification.md` §10.3. Never parks.

**Owner:** `failure.md` §5.1; `unification.md` §10.3.

### BeginFailureContext

**Operands:** `OnFailure` (jump, label), `Id` (const, failure_context_id).

Opens a full failure context, child of the current one, recording frame, `OnFailure`, effect token,
batch and task, and starts its transaction. `Id` has no run-time use; a re-executed
`BeginFailureContext` with the same `Id` opens a new context. **Outcome:** continues.
**Owner:** `failure.md` §3.1; `unification.md` §8.4.

### EndFailureContext

**Operands:** `Done` (jump, label), `Id` (const, failure_context_id).

With nothing outstanding: commits the context's transaction into its parent, makes the parent
current, and continues at the next op (`Done` unused). With work outstanding: `unification.md` §9.2.
**Owner:** `failure.md` §3.2, §7.

## 8. Moves, control flow and variables

### 8.1 Moves and resets

### Move

**Operands:** `Dest` (unify_def, register), `Source` (use, value; absent in practice, §1.5).

Unifies `Source` into `Dest` (`unification.md` §3): a store into a fresh register, a comparison
into a filled one. Not equal **fails**; undecidable is a VM invariant violation; an error during
the comparison raises. Never parks. **Owner:** `unification.md` §3, §5.

### MoveTrailed

**Operands:** as `Move`. As `Move`; a store into a fresh `Dest` is recorded, so failure makes `Dest`
fresh again. **Owner:** `unification.md` §5; `failure.md` §6.2.

### MoveNonComparable

**Operands:** as `Move`. As `MoveTrailed`'s unification, except that **undecidable fails** instead
of being an invariant violation. Emitted by the live-binding loop (§3.2), which makes it reachable
from a Godot script. **Owner:** `unification.md` §5; `values.md` §11.3.

### Reset

**Operands:** `Dest` (clobber_def, register), `LiveRange` (const, live_range).

Makes `Dest` fresh, recording its old content. `LiveRange` has no run-time effect.
**Owner:** `unification.md` §4; `failure.md` §6.2.

### ResetNonTrailed

**Operands:** as `Reset`. As `Reset`, not recorded (recording it anyway is conformant).
**Owner:** `unification.md` §4.

### 8.2 Jumps

### Jump

**Operands:** `JumpOffset` (jump, label). Continues at `JumpOffset`. **Owner:** this file.

### JumpIfInitialized

**Operands:** `Source` (use, value), `JumpOffset` (jump, label).

Jumps when `Source` is **not** uninitialized; otherwise continues. A fresh register jumps (reading it
yields a placeholder, which is not uninitialized). Used for named-parameter defaults, the init-super
step of a constructor, and an escape scope's "already decided" test. Never parks.
**Owner:** `calls.md` §2.2, §5.4; `objects.md` §7.2; `tasks.md` §6.3.

### Switch

**Operands:** `Which` (use, value), `JumpOffsets` (jump, label, variadic).

Continues at `JumpOffsets[Which]`. `Which` must be a concrete integer *i* with 0 ≤ *i* < the number
of labels; anything else — unbound, not an integer, out of range — is a VM invariant violation
(never parks). The compiler emits it after an escape scope with `Which` the switch register an
arm's `EndTask` wrote: 0 continue, 1 return. **Owner:** `tasks.md` §6.2, §6.8; this file.

### 8.3 Variables

A **variable** is the cell a `var` is: a single content slot, an optional **domain** function, the
awaiting tasks registered with it (`tasks.md` §5.7) and a live task (§3.2). Two other things may
stand where a variable is expected: an **accessor reference** (`objects.md` §16) and a **native
reference** (a `var` whose storage is a native field, `objects.md` §9.2).

### NewRef

**Operands:** `Dest` (unify_def, register), `Domain` (use, value, optional).

A new variable into `Dest`, with `Domain` as its domain function when present and not uninitialized.
Its content is fresh; the compiler always writes it (`RefSet`) before reading it. During module top
level it instead **raises** `ErrRuntime_UnimplementedGlobalVariable` with the message
`Can't create a var at module scope.` (unreachable: compile error 3502). Never parks.
**Owner:** `modules.md` §4; `failure.md` §9.1; this file.

### NewPersistentOrSessionWeakMapRef

**Operands:** `Dest` (unify_def, register).

A new variable with no domain into `Dest`, **without** the module-top-level check. Emitted only in
a module procedure for a module-scoped `var` of `weak_map(player, …)` or `weak_map(session, …)`,
which a Godot project cannot declare (`modules.md` §5), and module procedures never run in the
interpreter (`modules.md` §2). Implement trivially. **Owner:** this file.

### RefGet

**Operands:** `Dest` (unify_def, register), `Ref` (use, value).

| `Ref` | Into `Dest` |
| --- | --- |
| a variable | its content — for a value with structure, the melted value the variable holds. If the current task has an await point, the task is registered with the variable (`tasks.md` §5.7) |
| a native reference | the reference itself |
| an accessor reference | the reference itself |
| anything else | outside the contract |

The last two rows pass through; the `Freeze` or `FreezeIfAccessor` that follows reads them.
Effectful: it waits for the effect token before anything else. **Waits:** `unification.md` §6.1.
**Outcome:** continues. **Owner:** `values.md` §7; `unification.md` §8.3; this file.

### RefSet

**Operands:** `Ref` (use, value), `Value` (use, value). No `Dest`.

| `Ref` | Outcome |
| --- | --- |
| a variable | waits for the effect token; cancels the variable's live task (§3.2); **replaces** the content with `Value` as is (no unification, no melting — the compiler melts first), recording the old content; then resumes every task registered with the variable, synchronously and before `RefSet` completes (`tasks.md` §5.7; their relative order is `tasks.md` §14 Q5), or, inside a batch, adds the variable to it |
| a native reference | waits for the effect token; stores `Value` into the native field, converting it (`objects.md` §9.2), which can **raise** |
| an accessor reference | calls the setter with the reference's path and `Value` (`objects.md` §16); the outcome is the setter call's |
| anything else | outside the contract |

A resumed task that raises makes `RefSet` raise. **Waits:** `unification.md` §6.1. **Owner:** `failure.md` §6.1;
`tasks.md` §5.7; `objects.md` §16; this file.

### RefSetLive

**Operands:** `Ref`, `Value`, `Task` (use, value).

`RefSet` on a variable, as a live write by `Task` (§3.2). An accessor or native reference as `Ref`
is outside the contract. **Owner:** §3.2.

### RefCallDomain

**Operands:** `Dest` (unify_def, register), `Ref` (use, value), `Argument` (use, value).

| `Ref` | Outcome |
| --- | --- |
| a variable with a domain function | calls the domain function with `Argument` exactly as `Call` does (a Verse or native callee; its receiver is the function's own), the result into `Dest`; the call may fail, raise or park like any call |
| a variable without one, a native reference, an accessor reference | `Argument` into `Dest` unchanged |
| anything else | outside the contract |

`Argument` need not be concrete. In every compiled Godot script seen, the variable has no domain and
the op is a move (`failure.md` §8.1). A domain appears when a `var`'s declared type position names a
value that is a function rather than a type; no Godot-script spelling of that was found (§17 Q2).
**Waits:** `unification.md` §6.1. **Owner:** this file.

### Freeze

**Operands:** `Dest` (unify_def, register), `Value` (use, value).

| `Value` | Outcome |
| --- | --- |
| an accessor reference | calls the getter (`objects.md` §16): arguments the `accessor` enumerator then one per path step, receiver the reference's object; the call's result into `Dest`, its outcome the call's |
| a native reference | waits for the effect token; the native field's current value, converted to a Verse value, into `Dest` |
| a melted value | waits for the effect token; a new immutable copy (`values.md` §7), reading through hidden variables (§3.1), into `Dest` |
| anything without value structure | waits for the effect token; `Value` itself into `Dest` |

Freezing an already immutable array or map is outside the contract (`values.md` §7). An unbound
placeholder anywhere inside `Value` is an invariant violation. **Waits:** `unification.md` §6.1.
**Owner:** `values.md` §7; `objects.md` §16.

### FreezeIfAccessor

**Operands:** `Dest` (unify_def, register), `Value` (use, value).

An accessor reference: the getter call, as `Freeze`. Anything else: `Value` unchanged into `Dest`,
without waiting for the token. Emitted before `LengthWithEffects` for `.Length` of a `var` (§2).
**Waits:** `unification.md` §6.1. **Owner:** `objects.md` §16.

### Melt

**Operands:** `Dest` (unify_def, register), `Value` (use, value).

A deep mutable copy of anything with value structure, the operand itself otherwise, into `Dest`
(`values.md` §7). Not effectful. **Waits:** `unification.md` §6.1. **Outcome:** continues. **Owner:** `values.md` §7.

## 9. Containers

### Length

**Operands:** `Dest` (unify_def, register), `Container` (use, value).

The element count of an array or mutable array, the entry count of a map or mutable map, 0 for
`false`, into `Dest`. Anything else is outside the contract. **Waits:** `unification.md` §6.1. **Outcome:**
continues. **Owner:** `values.md` §6.2, §8.2.

### LengthWithEffects

**Operands:** `Dest` (unify_def, register), `Container` (use, value).

Waits for the effect token, then: a variable is replaced by its content (without registering an
await, §3.1); a native reference answers the length of the value it refers to; then `Length`'s
rule. Measured through a `var`: an array after `+=` has Length 3, a string after `+=` 4, a map after
two inserts and a replace 2, a `false` array 0 (`ops:P04_Length`). **Waits:** `unification.md` §6.1.
**Owner:** `values.md` §6.2; this file.

### CallSet

**Operands:** `Container` (use, value), `Index` (use, value), `ValueToSet` (use, value). No `Dest`.

`set C[K] = V`. `ValueToSet` is stored as is (the compiler melted it). By `Container`:

| `Container` | Outcome |
| --- | --- |
| a mutable array | waits for the effect token. `Index` not an integer in [0, 2^32), or not below the length: **fails**. Otherwise, if the element is a hidden variable (§3.1), a write to it as `RefSet`; else the element is replaced, recording the old one |
| a mutable map | waits for the effect token. If the entry for a key equal to `Index` holds a hidden variable, a write to it as `RefSet`; otherwise the entry's value is replaced in place (an existing key keeps its position) or a new entry is appended, recorded either way. Never fails |
| an accessor reference | a new accessor reference with `Index` appended to its path, then the setter call as `RefSet` |
| a native reference | waits for the effect token; the indexed element of the native value, then a store as `RefSet` on a native reference |
| anything else | outside the contract |

Measured: on `array{1, 2, 3}`, `set A[−1]`, `set A[3]` and `set A[2^63]` fail and
`set A[2^63 − 2^63 + 2]` writes element 2; on a `[float]string` map, `NaN` and `Inf − Inf` are one
key and `0.0` and `−0.0` are one key, the later write replacing the value (`ops:P05_CallSet`, map
Length 2). **Waits:** `unification.md` §6.1. **Owner:** `failure.md` §6.1; `values.md` §7, §8.1; this file.

### CallSetLive

**Operands:** `Container`, `Index`, `ValueToSet`, `Task` (use, value).

`CallSet` as a live write by `Task` (§3.2): an element or entry held in a hidden variable gets the
live-task rule; any other element or entry is written as `CallSet` and records no live task
(`live:L05`–`L08`). An accessor reference as `Container` is outside the contract.
**Owner:** §3.2.

### NewArray

**Operands:** `Dest` (unify_def, register), `Values` (use, value, variadic).

A new immutable array of the operands in order, unbound placeholders included, into `Dest`. Every
tuple is built this way. Never waits. **Owner:** `values.md` §6.4, §10.

### NewMutableArray

**Operands:** `Dest` (unify_def, register), `Values` (use, value, variadic).

A new mutable array of the operands in order into `Dest`; emitted with no operands to start a
collecting `for`. Never waits. **Owner:** `values.md` §6.4.

### ArrayAdd

**Operands:** `Dest` (unify_def, register), `Container` (use, value), `ValueToAdd` (use, value),
`bTransactional` (const, bool).

`Container` must be a mutable array (else outside the contract); `ValueToAdd` is appended as is,
recorded only when `bTransactional` is true; `Container` itself into `Dest`.
**Waits:** `unification.md` §6.1. **Owner:** `values.md` §6.4; `failure.md` §6.1.

### InPlaceMakeImmutable

**Operands:** `Dest` (unify_def, register), `Container` (use, value).

`Container` must be a mutable array; the same cell becomes immutable (recorded); it goes into
`Dest`. **Waits:** `unification.md` §6.1. **Owner:** `values.md` §6.4; `failure.md` §6.1.

### FastAppendToArray

**Operands:** `LeftSource` (use, value), `RightSource` (use, value). No `Dest`.

Effectful: waits for the effect token. `LeftSource` must be a mutable array and `RightSource` an
array; each element of `RightSource`, melted, is appended to `LeftSource` in order, each append
recorded. If a melted element is an unbound placeholder, the appends made so far are removed and the
op parks. The compiler follows it with a `RefSet` (or `CallSet`, `SetField`) of the same array, which
is what resumes awaiters and ends a live binding (§2). **Waits:** `unification.md` §6.1.
**Owner:** `values.md` §6.4; `failure.md` §6.1.

### NewOption

**Operands:** `Dest` (unify_def, register), `Value` (use, value).

A **new** `option` cell holding `Value` (an unbound placeholder allowed) into `Dest`;
`option{false}` is a fresh cell, not the `true` cell. Never waits. **Owner:** `values.md` §9.

### NewMap

**Operands:** `Dest` (unify_def, register), `Keys` (use, value, variadic), `Values` (use, value,
variadic).

A new immutable map from the pairs `Keys[i] => Values[i]` in order, a repeated key taking the last
value and the last position, into `Dest`. The two lists have equal length (else outside the
contract). **Waits:** `unification.md` §6.1 (each key). **Owner:** `values.md` §8.1.

### MapKey

**Operands:** `Dest` (unify_def, register), `Map` (use, value), `Index` (use, value).

The key at 0-based position `Index` of a map or mutable map into `Dest`. `Index` is a small
in-range integer in emitted code; anything else is outside the contract. **Waits:** `unification.md` §6.1.
**Owner:** `values.md` §8.2.

### MapValue

**Operands:** as `MapKey`. The value at position `Index` into `Dest`. **Waits:** `unification.md` §6.1.
**Owner:** `values.md` §8.2.

### 9.10 Unions

The three union ops have compiler emitters, so `ops.json` marks them emitted, but the `union` macro
exists only behind a compiler setting this project never enables (`objects.md` §15), and
`format.md` version 1 has no union cells. A union op in a file could only name a tag or variant the
file cannot hold. **A reader refuses a procedure containing any of the three**, at load, with a
sentence naming the op and the procedure — the same treatment as §13's ops, for the same reason.
Their behaviour, for when unions are added:

### NewUnionVariant

**Operands:** `Dest` (unify_def, register), `Tag` (use, value), `Payload` (use, value).

Waits for both; a new union variant of `Tag` (a union variant tag) carrying `Payload` into `Dest`.
**Owner:** `objects.md` §15.

### GetUnionVariantPayload

**Operands:** `Dest` (unify_def, register), `Source` (use, value). Waits; `Source`'s payload into
`Dest`. **Owner:** `objects.md` §15.

### GetUnionVariantTag

**Operands:** `Dest` (unify_def, register), `Source` (use, value). Waits; `Source`'s tag cell into
`Dest`. **Owner:** `objects.md` §15.

## 10. Objects, classes and modules

### NewObject

**Operands:** `Dest` (unify_def, register), `Archetype` (use, value), `Class` (use, value).
**Cache operands (not in the file):** `CachedClass`, `EmergentTypeOffset`.

A new object of `Class` with the layout of `objects.md` §4 and no slot created into `Dest`; a struct
class gives a struct value; a native-represented class gives the object marked "under
construction". **Waits:** `unification.md` §6.1. **Owner:** `objects.md` §7.1.

### LoadField

**Operands:** `Dest` (unify_def, register), `Object` (use, value), `Name` (immediate,
cell:VUniqueString). **Cache operands:** `EmergentTypeOffset`, `ICPayload`.

By what `Object` is and what `Name` is in its layout, `objects.md` §6's table: a slot's content (a
`var` member's variable), a method bound to `Object`, an accessor reference, a constant, an
accessor reference extended by `Name`'s unqualified form, a native field. A non-constant field of a
VM-level object gets §3.1's registration. A name absent from the layout is outside the contract.
Reading a native reference whose target is gone raises `ErrRuntime_InvalidRef` (source, unprobed).
**Waits:** `unification.md` §6.1. **Owner:** `objects.md` §6; `calls.md` §6.

### LoadFieldFromSuper

**Operands:** `Dest` (unify_def, register), `Scope` (use, value), `Self` (use, value), `Name`
(immediate, cell:VUniqueString).

`(super:)Name`: from `Scope`'s root the defining class, then the first ancestor with an entry
`Name` holding an unbound function, bound to `Self`, into `Dest`. **Waits:** `unification.md` §6.1.
**Owner:** `calls.md` §7.4.

### CreateField

**Operands:** `LeniencyIndicator` (unify_def, register), `Token` (use, value), `Object` (use,
value), `Name` (immediate, cell:VUniqueString), `OnFailure` (jump, label). **Cache operands:**
`SourceEmergentTypeOffset`, `FieldIndex`, `NextEmergentTypeOffset`.

Opens one field initializer: an uncreated slot or accessor is marked created and execution falls
through; a created slot, a constant or a method **jumps** to `OnFailure` (marking the indicator's
record failed if it holds one). Not a program failure. **Waits:** `unification.md` §6.1 (through the leniency
indicator). **Owner:** `objects.md` §7.4.

### UnifyField

**Operands:** `Object` (use, value), `Name` (immediate, cell:VUniqueString), `Value` (use, value).

Unifies `Value` into the created slot `Name` (binding a placeholder an early read left, `objects.md`
§7.10); a native field converts on store and can **raise**. **Waits:** `unification.md` §6.1.
**Owner:** `objects.md` §7.5.

### InitializeVar

**Operands:** `TokenDest` (unify_def, register), `Token` (use, value), `Object` (use, value), `Name`
(immediate, cell:VUniqueString), `Value` (use, value), `ValueDomain` (use, value, optional),
`bCheckIfVariableAllocationIsAllowed` (const, bool).

A slot gets a new variable holding `Value` (domain `ValueDomain`); an accessor gets a deferred setter
appended to the construction token; the token into `TokenDest`. **Raises**
`ErrRuntime_UnimplementedGlobalVariable` (`Can't allocate mutable var field <Name> while
initializing module.`) at module top level when the flag is set. **Waits:** `unification.md` §6.1.
**Owner:** `objects.md` §7.6; `modules.md` §4.

### SetField

**Operands:** `Object` (use, value), `Name` (immediate, cell:VUniqueString), `Value` (use, value).

Writes field `Name` of `Object`: a slot holding a variable or a hidden variable (§3.1) is written as
`RefSet`; a plain slot is overwritten and recorded; an accessor reference calls the setter; a native
reference sets the field behind it. Emitted for `set S.F = V` on a struct held in a `var`, which the
compiler refuses at this commit, and in `+=` on such a field (§2); unreachable from a Godot script.
**Waits:** `unification.md` §6.1. **Owner:** `objects.md` §7.7.

### SetFieldLive

**Operands:** `Object`, `Name`, `Value`, `Task` (use, value; `Name` immediate).

`SetField` as a live write by `Task` (§3.2). Emitted only for `set live S.F = …`, whose non-live
form is refused, so unreachable. **Owner:** §3.2; `objects.md` §7.7.

### UnifyNativeObject

**Operands:** `Token` (use, value), `Object` (use, value).

Runs the deferred setters in `Token`, in order, then the object's blocks function if its actual
class has kind class. Either can raise. **Waits:** `unification.md` §6.1. **Owner:** `objects.md` §7.8.

### UnwrapNativeConstructorWrapper

**Operands:** `Dest` (unify_def, register), `Object` (use, value).

The finished object into `Dest`: the "under construction" mark dropped, or `Object` unchanged.
**Waits:** `unification.md` §6.1. **Owner:** `objects.md` §7.8.

### LoadConstructor

**Operands:** `Dest` (unify_def, register), `Class` (use, value).

`Class`'s constructor function into `Dest`. **Waits:** `unification.md` §6.1. **Owner:** `objects.md` §7.1.

### JumpIfDefaultSubObject

**Operands:** `Object` (use, value), `OnDefaultSubObject` (jump, label).

Never jumps in the interpreter, which makes no default sub-objects. An unbound `Object` is an
invariant violation (never parks). **Owner:** `objects.md` §8.3, §8.4; `modules.md` §4.

### NewClass

**Operands:** `ClassDest`, `ArchetypeDest`, `ConstructorDest`, `BlocksDest` (unify_def, register);
`Package` (immediate, cell:VPackage); `RelativePath`, `ClassName` (immediate, cell:VArray);
`AttributeIndices` (immediate, value_imm, variadic); `Attributes` (use, value, variadic);
`ImportStruct` (immediate, value_imm, optional); `Inherited` (use, value, variadic); `Archetype`
(immediate, cell:VArchetype); `ConstructorBody`, `Blocks` (immediate, cell:VProcedure, optional);
`bNativeBound` (const, bool); `ClassKind` (const, enum:ClassKind); `Flags` (const,
enum:ClassFlags).

Builds a class, its class-body archetype, constructor and blocks functions, and its class scope;
derives the native-representation and predicts flags. Appears only in package procedures, which do
not run in the interpreter (`modules.md` §2). **Waits:** `unification.md` §6.1. **Owner:** `objects.md` §8.1.

### BindNativeClass

**Operands:** `Class` (use, value), `bImported` (const, bool).

Waits until the class is concrete (and see §15 item 4); afterwards the layout is final. Nothing else.
**Owner:** `objects.md` §8.2; `modules.md` §4.

### ConstructNativeDefaultObject

**Operands:** `Class` (use, value). A no-op in the interpreter. **Owner:** `objects.md` §8.3;
`modules.md` §4.

### LoadImport

**Operands:** `Class` (use, value), `ImportPath` (const, asset_path).

Produced only for `@import_as`, which a Godot script cannot use; executing one is a VM invariant
violation. **Owner:** `modules.md` §4; `objects.md` §8.3.

### BeginModule

**Operands:** `Dest` (unify_def, register), `Package` (use, value), `ModuleName` (immediate,
cell:VUniqueString).

The package's `module` object for `ModuleName` into `Dest`; module top level on. **Owner:**
`modules.md` §4.

### EndModule

**Operands:** `Module` (use, value). Module top level off. **Owner:** `modules.md` §4.

### EndModuleData

**Operands:** `FieldName` (immediate, cell:VUniqueString), `Value` (use, value). Waits for `Value`;
nothing else. **Owner:** `modules.md` §4.

## 11. Calls, closures and profiling

### Call

**Operands:** `Dest` (unify_def, register), `Callee` (use, value), `Arguments` (use, value,
variadic), `NamedArguments` (immediate, cell:VUniqueString, variadic), `NamedArgumentValues` (use,
value, variadic), `bCalleeYields` (const, bool).

By the callee, `calls.md` §4.1: a function (enter its procedure, or call its native with the five
outcomes of `calls.md` §4.3); an array (index, fails when out of range); a map (lookup, fails when
absent); a type (cast, the `TypeCastFastFail` table); a union variant tag (outside version 1). Two
callee kinds `calls.md` §4.1 does not list, both exactly one argument (source, unprobed; §15 item
7):

| Callee | Into `Dest` |
| --- | --- |
| an accessor reference | a new accessor reference with the argument appended to its path; the argument need not be concrete. Reading or writing it is a later `Freeze` or `CallSet` |
| a native reference | a native reference to the indexed element; the argument must be concrete; the outcome (fail on a missing element) is the native container's |

A mutable-array element or mutable-map value read gets §3.1's registration. **Waits:** `unification.md` §6.1.
**Owner:** `calls.md` §3–§5, §10; `natives.md` §3.

### CallWithSelf

**Operands:** as `Call`, with `Self` (use, value) between `Callee` and `Arguments`.

An unbound function entered with `Self` as receiver, or a bare native procedure called with `Self`.
A function that already has a receiver is an invariant violation. **Waits:** `unification.md` §6.1.
**Owner:** `calls.md` §4.2.

### Return

**Operands:** `Value` (use, value). The effect token into the return-token slot, the frame left,
`Value` unified into the return slot (a mismatch fails in the caller's context).
**Owner:** `calls.md` §5.5, §5.6.

### ReturnTrailed

**Operands:** `Value` (use, value). `Return` with both stores recorded. **Owner:** `calls.md` §5.5.

### NewScope

**Operands:** `Dest` (unify_def, register), `ParentScope` (use, value), `Captures` (use, value,
variadic). A new scope into `Dest`. Never waits. **Owner:** `calls.md` §7.1.

### NewFunction

**Operands:** `Dest` (unify_def, register), `Procedure`, `Self`, `ParentScope` (use, value; `Self`
and `ParentScope` absent in practice, §1.5). A new function into `Dest`. **Waits:** `unification.md` §6.1.
**Owner:** `calls.md` §7.1, §7.2.

### LoadParentScope

**Operands:** `Dest` (unify_def, register), `Scope` (use, value). The scope's parent into `Dest`.
**Waits:** `unification.md` §6.1. **Owner:** `calls.md` §7.1.

### LoadCapture

**Operands:** `Dest` (unify_def, register), `Scope` (use, value), `Index` (const, u32). The capture
at `Index` into `Dest`. **Waits:** `unification.md` §6.1. **Owner:** `calls.md` §7.1.

### BeginProfileBlock

**Operands:** `Dest` (unify_def, register).

Opens a `profile("tag"){…}` block. Into `Dest` goes a start-time **integer** that only the matching
`EndProfileBlock` reads; the reference stores a clock reading, and since nothing else sees it, an
interpreter without a profiler may put any integer there (0 will do). A profiler may also report a
begin event. Never waits, never fails. **Owner:** this file.

### EndProfileBlock

**Operands:** `WallTimeStart` (use, value), `UserTag` (use, value), `SnippetPath` (immediate,
cell:VUniqueString), `BeginRow`, `BeginColumn`, `EndRow`, `EndColumn` (use, value).

Closes the block. The compiler places it in a `defer` around the block's body, so it runs once on
normal exit, on `return` or `break` out of the block, and on cancellation. `WallTimeStart` is the
integer `BeginProfileBlock` produced; `UserTag` is the tag string (a `[]char`); the four positions
are 1-based integer constants; `SnippetPath` is the source path, made relative to the package's
directory and given a leading `/`. A profiler reports the elapsed time with the tag and locus; an
interpreter without one **reads the operands and does nothing**. An unbound operand is an
invariant violation. Nothing is recorded or undone (`failure.md` §6.3).

Observable: nothing. `profile("t"): 40 + 2` answers 42, a `return 5` inside one returns 5, and a
loop inside one runs normally (`ops:P06_Profile`, `ProfValue`, `ProfReturn`). **Owner:** this file.

## 12. Tasks

### SelfTask

**Operands:** `Dest` (unify_def, register). The current task into `Dest`. **Owner:** `tasks.md`
§5.1.

### BeginTask

**Operands:** `Dest` (unify_def, register), `Parent` (use, value; absent in practice),
`bAddToTaskGroup` (const, bool), `OnYield` (jump, label). Starts an inline task in the same frame.
**Owner:** `tasks.md` §5.2.

### CallTask

**Operands:** `Dest` (unify_def, register), `Parent` (use, value; absent in practice), `Callee`
(use, value), `Arguments` (use, value, variadic). Starts a task whose body is a function. A native
callee is an invariant violation. **Owner:** `tasks.md` §5.3.

### EndTask

**Operands:** `Write` (clobber_def, register, optional), `Switch` (clobber_def, register, optional),
`Value` (use, value), `Which` (use, value; absent in practice), `Signal` (use, value, optional).
Finishes the current task: first-writer-wins result, semaphore signal, resumptions. May suspend.
**Owner:** `tasks.md` §5.4.

### Yield

**Operands:** `ResumeOffset` (jump, label). Suspends with resume point `ResumeOffset`.
**Owner:** `tasks.md` §5.5.

### NewSemaphore

**Operands:** `Dest` (unify_def, register). A new semaphore (count 0, no waiter) into `Dest`.
**Owner:** `tasks.md` §5.6.

### WaitSemaphore

**Operands:** `Source` (use, value), `Count` (const, i32). Subtracts `Count`; suspends when the
result is negative. **Owner:** `tasks.md` §5.6.

### ResumeUnwind

**Operands:** none. Continues unwinding from this op's position. **Owner:** `tasks.md` §5.8, §7.3.

### BeginAwait

**Operands:** none. Waits for the effect token; sets *initializing* and the await point.
**Owner:** `tasks.md` §5.7.

### AwaitSuccess

**Operands:** none. Waits for the effect token; **fails** while *initializing*, else clears the
await point. **Owner:** `tasks.md` §5.7.

### EndAwait

**Operands:** none. Waits for the effect token; clears both. **Owner:** `tasks.md` §5.7.

### BeginBatch

**Operands:** none. Waits for the effect token; opens a batch level. **Owner:** `tasks.md` §5.7.

### EndBatch

**Operands:** none. Waits for the effect token; closes a level, and at the outermost resumes each
registered task once; raises if a resumption raises. **Owner:** `tasks.md` §5.7.

## 13. Ops the compiler never emits

`ops.json` marks five ops, other than the inline-cache forms, as not emitted at `203d764`. None
appears in a compiled program, so none appears in a `.vbc` the writer produces.

### Mod

**Operands:** `Dest` (unify_def, register), `LeftSource`, `RightSource` (use, value).

**Refuse at load** as a malformed file. It computes a value, and its remainder convention is known
only from source — truncating, unlike the Euclidean library `Mod[]` (`values.md` §16 Q7) — so an
implementation that guessed would be silently wrong if a later engine began emitting it; a refusal
says so instead. The script function `Mod[]` is a native (`natives.md` §5.3) and is unaffected.

### MutableAdd

**Operands:** as `Add`. **Refuse at load**, for `Mod`'s reason: it builds a value (`values.md` §6.4
says what it would be), no compiled program contains it, and its presence means the writer and
reader disagree about the op set.

### NewMutableArrayWithCapacity

**Operands:** `Dest` (unify_def, register), `Size` (use, value). **Refuse at load**, as `MutableAdd`.

### Tracepoint

**Operands:** `Name` (immediate, cell:VUniqueString). **Implement trivially**, as a no-op that
continues. In the reference it only writes a log line naming `Name`; it changes nothing a program
can observe, so implementing it costs nothing and cannot be wrong.

### Err

**Operands:** none. **Implement trivially**: it raises `ErrRuntime_Internal` with the diagnostic's
own description as the message, as `failure.md` §9.1 already specifies. The reference uses the op
only inside its own internal procedures and sentinels; the interpreter may use it the same way.

Refusing an opcode at load is an addition to `format.md` §9's list (§17 item 1).

## 14. Inline-cache ops

Never in a file. The writer serializes the base op each was rewritten from; **a reader refuses one
as a malformed file** (`format.md` §5.1, §9).

| Op | Rewritten from |
| --- | --- |
| `NewObjectICClass` | `NewObject` |
| `LoadFieldICOffset` | `LoadField` |
| `LoadFieldICConstant` | `LoadField` |
| `LoadFieldICFunction` | `LoadField` |
| `LoadFieldICNativeFunction` | `LoadField` |
| `LoadFieldICAccessor` | `LoadField` |
| `CreateFieldICValueObjectConstant` | `CreateField` |
| `CreateFieldICValueObjectField` | `CreateField` |
| `CreateFieldICNativeStruct` | `CreateField` |
| `CreateFieldICUObject` | `CreateField` |

## 15. Inconsistencies

Each names both citations; nothing here was silently resolved.

1. **Float ordering in the fast-fail ops.** `failure.md` §5.2 says the four relational fast-fail
   ops compare floats by "IEEE: every relation with NaN is false". `values.md` §3.3 and `facts.md`
   §1 say `NaN <= NaN` and `NaN >= NaN` succeed. The probe agrees with `values.md` for both the
   fast-fail ops and the ordinary ones (`ops:P02_FullContext`, `ops:P03_FastForms`). `failure.md`
   §5.2 should drop the IEEE parenthesis.
2. **`false` and `""` as map keys.** `values.md` §8.5 says an implementation must treat them as
   different keys; the same file's §16.1 (the lead's Q2 decision) says they are the same key. The
   decision presumably wins; §8.5's sentence still says "must".
3. **An unbound native.** `natives.md` §3.2 says a native with no implementation that is not in its
   §9 list refuses the **file** at load, and §9's stubs raise `ErrRuntime_NativeInternal` with
   `Native <key> is not supported by this runtime.` `modules.md` §6 says such a native must **not**
   refuse the file and binds to a stand-in raising `The native function <decorated name> is not
   implemented by this runtime.` `natives.md` §11.1 Q6 adopts the second wording (with the binding
   key), but §3.2's refusal rule and diagnostic were not revised.
4. **What `BindNativeClass` waits on.** `unification.md` §6.1 and `modules.md` §4 say `Class` alone;
   `objects.md` §8.2 says `Class`, every attribute value, every inherited class and every entry
   type. Matters only under stage-2 leniency, and only in package procedures, which do not run.
5. **Live variables are reachable.** `tasks.md` §5.5 says the ops emitted for live variables are
   not something a Godot script normally writes; `unification.md` §5 says `MoveNonComparable` is
   "unreachable from a Godot script". `set live` compiles and runs in a script package
   (`live:L01_Bind`–`L08`), so `MoveNonComparable`, `RefSetLive` and `CallSetLive` are reachable.
   `phase-7.5-design.md` §4 already says live variables compile.
6. **Awaiting beyond variables.** `tasks.md` §5.7 says a task at an await point registers with
   "every read of a mutable variable (a `var` reference)". Element, map-value and struct-field reads
   register too (§3.1 here, `live:A01`–`A03`).
7. **`Call` on reference callees.** `calls.md` §4.1's table makes any callee other than a function,
   array, map, type or tag a VM invariant violation. The reference also accepts an accessor
   reference and a native reference (§11 `Call` here), which `objects.md` §6 and §16 already imply
   for deeper property paths.
8. **Unwind coverage.** `format.md` §5 defines coverage on "the op the frame is stopped in: the op
   that suspended it … never the op it will resume at". `tasks.md` §7.3 defines it on the resume
   position *p* by testing *p* − 1, and says that for `Yield` *p* is `ResumeOffset`, not the op
   after the `Yield`. For a task suspended in `Yield`, the first tests the `Yield`'s own index and
   the second tests `ResumeOffset − 1`. In compiled `await` both lie inside the same region, so the
   answers probably agree, but the two sentences are different rules; `tasks.md` §14 Q1 already
   asks `format.md` to say which index it means.
9. **`CanFastAppendToArrayFastFail`'s `Ref`.** `values.md` §6.4 says the guard passes only when
   `Ref` is absent or a `var` with no domain, and fails otherwise; `failure.md` §5.2 says it passes
   unless `Ref` is a `var` with a domain. The reference follows `failure.md` (an accessor or native
   reference passes). No compiled code passes either, so the difference is unobservable.
10. **`set M[K] = V` on a map.** `failure.md` §2 lists `CallSet` failing only for a mutable array's
    index. The compiler nonetheless makes a map `set` `<decides>` (error 3512 outside a failure
    context). Not a contradiction in the VM — `CallSet` on a map never fails — but a reader of the
    language may expect one.

### 15.1 The lead's resolutions

Items 1, 2, 3 and 8 are corrected in place. For the rest, this section overrides the file it names,
and every such file's `Status:` line says so.

1. `values.md` and the probes win: `NaN <= NaN` and `NaN >= NaN` succeed in every relational op.
   `failure.md` §5.2 now says so.
2. §16.1 wins: `false`, `""`, an empty array and an empty map are one map key. `values.md` §8.5 now
   says so.
3. The stand-in wins: no load is refused for an unbound native, and the stand-in's text is
   `The native function <binding key> is not implemented by this runtime.` `natives.md` §3.2 now
   says so.
4. `objects.md` §8.2 wins: `BindNativeClass` waits on `Class`, every attribute value, every
   inherited class and every entry type. In a snapshot all are concrete, so it never parks.
5. and 6. This file wins: live variables and element, map-value and field `await` registration
   are reachable and are implemented in T4.3.
7. This file's `Call` wins: accessor and native references are callees.
8. The reference's rule: a frame is covered by testing the op before its resume point. `format.md`
   §5 now says so, and it differs from "the op stopped in" only for `Yield`.
9. `failure.md` wins, as the reference does: `CanFastAppendToArrayFastFail`'s guard passes unless
   `Ref` is a `var` with a domain.
10. Nothing to resolve: `CallSet` on a map never fails.

The six opcodes §13 refuses at load are in `format.md` §9's refusals.

## 16. Gaps this file fills

Ops whose behaviour no other spec file specified, now specified here in full:

- `Switch` (the jump table and its invariants, §8.2);
- `Jump` (trivial, but owned nowhere);
- `NewRef`'s content and domain, and `NewPersistentOrSessionWeakMapRef` (§8.3);
- `RefGet`, `RefSet` in full — the three reference kinds, overwrite rather than unify, awaiter
  resumption, the live-task cancellation (§8.3);
- `RefSetLive`, `CallSetLive`, `SetFieldLive` and the live-binding rule (§3.2);
- `RefCallDomain` (§8.3);
- `Freeze` and `FreezeIfAccessor` on native references, and the token order (§8.3);
- `LengthWithEffects` (§9);
- `CallSet` in full, including accessor and native references and hidden variables (§9);
- `Call` on accessor and native references (§11);
- await registration of element, map-value and field reads (§3.1);
- `BeginProfileBlock` and `EndProfileBlock` (§11);
- the decision on `Query` of a native object (§6);
- the treatment of the five non-emitted ops (§13) and of the union ops in version 1 (§9.10).

## 17. Open questions

1. **`format.md` §9 needs the refusals of §13 and §9.10.** The reader refuses `Mod`, `MutableAdd`,
   `NewMutableArrayWithCapacity` and the three union ops at load; `format.md` §9 lists only the
   inline-cache opcodes. The lead should add them, or decide the reader should implement them
   instead.
2. **Domain functions.** `RefCallDomain` and `InitializeVar`'s `ValueDomain` call a function when a
   `var`'s declared type position holds a function value rather than a type. No Godot-script
   spelling that produces one was found; the ops are specified from source. A fixture that compiles
   one would pin the argument and failure behaviour.
3. **`await{X.Length > N}`.** `LengthWithEffects` reads a `var` without registering the awaiting
   task (source), so a condition whose only read of `X` is `.Length` may never wake on `X`'s writes.
   Not probed.
4. **The order of several resumptions from one element write** (§3.1), like `tasks.md` §14 Q5 for
   variables, was not probed.
5. **The live loop's own cancellation and failure paths** (§3.2) — what a live binding does when its
   source expression fails, or when the task that created it is cancelled — follow from the ops but
   were not probed.
