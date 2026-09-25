# Unification, placeholders and parking

Status: reviewed by the lead 2026-09-24. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/facts.md`, `docs/web-vm/spec/calls.md`;
UE `Engine/Source/Runtime/CoreUObject/{Public,Private}/VerseVM` (the interpreter, placeholders, rest
values, frames, suspensions, failure and fast-failure contexts, the transaction and trail header,
equality, the array, map and object equality routines, the bytecode emitter header, the bytecode
analysis pass), `Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/VVMCodeGenerator.cpp`,
`Engine/Source/Programs/UnrealBuildTool/System/VerseVMBytecodeGenerator.cs` (the capture flags), and
the `VerseTestScriptCmd` test sources `Placeholder`, `LenientClassConstruction`,
`await-regalloc-transactional-unification-variables`, `return-should-be-trailed` and
`Bytecode/ControlFlow`. Probes: `tests/verse_probe/vm_unification_probe.verse`,
`tests/verse_probe/vm_unification_reject.verse`.

This file says what a logic variable is in the abstract machine, what it means to unify a value into
a destination, which ops can wait on an unbound variable ("park"), what a parked op must remember,
how the effect token keeps effects in program order while ops wait, and how a failure context
finishes when some of its ops are still waiting. The last section says how much of that a stage-1
interpreter (design §7.1) may leave out.

Equality of two concrete values — its four answers and every value kind's rule — is `values.md`'s.
Failure contexts, the undo log and runtime errors are `failure.md`'s. Frames, `Return` and the
return slot are `calls.md`'s. Tasks, suspension of a whole task and resumption are `tasks.md`'s; a
*parked op* here is a different thing from a *suspended task* there. Op and operand names are
`ops.json`'s, operand roles included (`use`, `unify_def`, `clobber_def`, `immediate`, `const`,
`jump`).

Probe claims cite `vm_unification_probe.verse` and the method; run it with

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        <repo>/tests/verse_probe/vm_unification_probe.verse --class vm_unification_probe

## 1. Vocabulary

| Term | Meaning |
| --- | --- |
| **slot** | any place a value rests: a register of a frame, a frame's return slot or return effect-token slot, a field of an object, a capture of a scope |
| **fresh** | a slot nobody has written since it was made or since a `Reset` / `ResetNonTrailed` cleared it (`calls.md` §2.2). It stands for a new unbound logic variable |
| **placeholder** | a heap cell standing for a logic variable. It is *unbound*, *linked* to another placeholder, or *bound* to a concrete value |
| **concrete** | any value that is not a placeholder. `uninitialized` (`calls.md` §2.2) is concrete for this file's purposes: it is a value, not a variable |
| **root** | following a placeholder's links to the end reaches its root: either a bound placeholder (whose value is then *the* value) or an unbound one |
| **waiters** | the parked ops (§6) an unbound root placeholder will wake when it is bound |
| **park** | an op meets an unbound placeholder where it needs a concrete value, and waits instead of completing |
| **leniency** | the whole scheme of parking and later re-execution |

A value operand never *reads* as a bound placeholder: every read follows links, and a bound
placeholder reads as its value. So an op sees either a concrete value or an unbound root placeholder.

## 2. Placeholders

### 2.1 Creation

A placeholder comes into existence only when something reads a **fresh** slot. The read produces a
new unbound placeholder and leaves it in the slot, so a second read sees the same one (`calls.md`
§2.2). An implementation may allocate lazily; the behaviour must be as if every fresh slot held its
own unbound placeholder from the start.

The ways a fresh slot gets read in a compiled program:

| Where | When |
| --- | --- |
| a register read by any operand before anything wrote it | only if the compiled code reads a register ahead of its definition; §11.1 shows the compiler refuses every source spelling that does this in a function body |
| a register whose value a parked op captures (§7) | an op parks and saves its `unify_def` destination: the destination is read, so if fresh it now holds a placeholder the op will later bind |
| a frame copied for lenient completion (§9.2, §10.3) | every fresh register of the original is read, so the copy and the original share each placeholder |
| the effect token (§8) after an effectful op parks | the next token is a fresh unbound value that the parked op will bind |
| a host-owned return slot at a VM entry | an implementation may give it a placeholder for the entry's `Return` to bind, or keep it a plain fresh slot; nothing else reads it, so the two are equivalent (`calls.md` §8) |

A linked program holds no placeholder in any cell or constant (`format.md` §3).

### 2.2 States and following

| State | Holds | Reads as |
| --- | --- | --- |
| unbound | a list of waiters, possibly empty | itself (an unbound root) |
| linked | another placeholder | whatever that one reads as |
| bound | a concrete value | that value |

Following a chain of links is the only traversal. Link identity is not observable, so an
implementation may compress paths.

### 2.3 Binding to a concrete value

Binding an unbound root to a concrete value:

1. The root becomes bound to the value. A value is never bound to a placeholder this way; a
   placeholder-to-placeholder meeting is §2.4.
2. The root's waiters, in the order the root holds them (§6.4), are appended to the **ready queue**.
3. The binding is **not** recorded in the undo log (`failure.md` §6): a later failure of the context
   in which the binding happened does not unbind it. No compiled program can observe this (the
   placeholders a failed context bound are reachable only from code of that context or from parked
   ops that belong to it, which are discarded, `failure.md` §4), so an implementation that does undo
   bindings is also conformant; one that does not is simpler.

### 2.4 Union of two unbound placeholders

When unification meets two unbound roots A (the destination side) and B (the value side), and they
are not the same placeholder, one is linked under the other:

| A's waiters | B's waiters | Result |
| --- | --- | --- |
| none | any | A is linked to B |
| some | none | B is linked to A |
| some | some | B's waiters are appended after A's, and B is linked to A |

The link write **is** recorded in the undo log (`failure.md` §6.2): a failure of the current context
unlinks it. Copying B's waiter list onto A is not recorded; the waiters belong to contexts that the
same failure discards.

## 3. Unifying a value into a slot

Every `unify_def` operand, and every other place this file or another spec file says "unified
into", means this operation. Its inputs are a slot and a value (already followed, so concrete or an
unbound root).

| The slot holds | What happens |
| --- | --- |
| nothing: it is **fresh** | the value is stored. No comparison, no failure, nothing recorded (except by the trailed ops, §5) |
| an unbound placeholder | if the value is concrete, the placeholder is bound to it (§2.3); if the value is also an unbound placeholder, the two are unioned (§2.4) |
| a concrete value | the two are compared by the equality of `values.md`, with placeholders found inside either side being unified as §3.1 says. The slot is not written |

### 3.1 Comparing two concrete values

Equality answers one of four things (`values.md`). For unification:

| Answer | Consequence at the op that unified |
| --- | --- |
| **equal** | success; the op continues |
| **not equal** | the op **fails**: the current failure context takes over (`failure.md` §4) |
| **undecidable** | a VM invariant violation (`failure.md` §9.4) — except for `MoveNonComparable`, where it is a failure like *not equal* |
| **runtime error** | a runtime error; the diagnostic was already raised by whatever equality touched (`failure.md` §9) |

Equality is structural: it walks arrays and tuples element by element, maps pair by pair **in
insertion order**, options into their contents, and structs field by field (`values.md`). When the
walk meets an unbound placeholder on either side, it does not stop and does not count it as a
difference: it binds that placeholder to the other side's value at that position (or unions two
placeholders, §2.4) and carries on. Consequences:

- Binding happens left to right as the walk goes. If a later position then answers *not equal*,
  the bindings already made stay (§2.3).
- A comparison that meets a placeholder never parks; unification never waits.

Measured (`vm_unification_probe` `UnifyConcrete`, `UnifyAggregates`, `DefinitionIsTest`), in a
condition so that *not equal* is observable as the `else` branch:

| Unification | Answer |
| --- | --- |
| `X = 5` with `X := 5` | succeeds |
| `X = 6` with `X := 5` | fails |
| `array{1, 2} = array{1, 2}` | succeeds |
| `array{1, 2} = array{1, 2, 3}` | fails |
| `"ab" = "ab"`, `"ab" = "ac"` | succeeds, fails |
| `option{3} = option{3}`, `option{3} = option{4}` | succeeds, fails |
| `0.0 = -0.0` | succeeds |
| two structs with equal fields, with one field different | succeeds, fails |
| a `<unique>` object with itself, with another instance | succeeds, fails |
| two equal tuples | succeeds |
| `map{1 => "a", 2 => "b"}` with an equal map built the same way | succeeds |
| the same pairs inserted in the other order | **fails** — map equality is order-sensitive |

An object of a class that is not `<unique>` cannot be compared at all: `C1 = C1` is compile error
3509 ("expects a value of type tuple(comparable,comparable)"). That is why *undecidable* is
unreachable from compiled code.

### 3.2 A destination that holds `uninitialized`

A slot holding `uninitialized` is not fresh. Unifying into it compares `uninitialized` with the
value, which is *not equal* for every Verse value, so the op fails (source, unprobed). The compiler
always clears such a register with `Reset` before writing it (`calls.md` §5.4); an interpreter must
not special-case it.

### 3.3 The two places the compiler relies on a non-fresh destination

Most `unify_def` destinations are fresh: the compiler emits a `Reset` when it allocates a register
(`calls.md` §2.2) and writes it once. Two compiled patterns deliberately unify into a register that
already holds a value, and an interpreter that "just stores" breaks both:

| Pattern | Ops | What the second op does |
| --- | --- | --- |
| **`=` in a full failure context** (`A = B` where the enclosing condition needs a full context; `failure.md` §10 says when) | `Move R ← A`, then `Move R ← B` on the same fresh `R` | compares B with A; *not equal* fails the context. In a fast failure context the same source compiles to `EqFastFail` instead (`failure.md` §5.2) |
| **binding a definition** in a package procedure (`modules.md`): a class whose base class or attributes were not yet available at compile time, and module-level functions | `Move R ← (the value just built, e.g. `NewClass`'s `ClassDest`)`, then `Move R ← (the definitions-table entry for that name)` | if the entry is an unbound placeholder, binds it — this is how every earlier reference to the definition, compiled against that placeholder, gets its value. If the entry is already that same cell, it is *equal* by identity. If it is a *different* concrete cell, the comparison is *undecidable* for classes and functions, which is an invariant violation (§12, question 2) |

A data definition `X := E` compiles to `Reset R` then `Move R ← E` into a fresh register: a store,
not a test. A local may not be declared without a value (§11.1), so a script cannot write the
"bind later" form at all; `=` on an already-bound name is always a comparison
(`DefinitionIsTest`: `Y := X` then `X = Y` succeeds).

### 3.4 Where unification happens besides `unify_def` operands

| Site | Slot | Spec |
| --- | --- | --- |
| `Return` / `ReturnTrailed` | the frame's return slot and return effect-token slot | `calls.md` §5.5 |
| `UnifyField` | a field of an object under construction | `objects.md` |
| task resumption | the suspended op's resume slot | `tasks.md` |
| completing a parked op that captured a `unify_def` | the captured placeholder (§7) | this file |
| completing a lenient then/else | the context's done effect token (§9) | this file |

## 4. The `clobber_def` exceptions

Four operands overwrite their register without unifying, whatever it holds:

| Op | Operand | What it writes |
| --- | --- | --- |
| `Reset` | `Dest` | makes `Dest` fresh. The write is recorded in the undo log (`failure.md` §6.2) |
| `ResetNonTrailed` | `Dest` | makes `Dest` fresh, **not** recorded |
| `EndTask` | `Write` (optional) | the finishing task's result, and only when the register currently holds `uninitialized` (first writer wins); recorded (`tasks.md`) |
| `EndTask` | `Switch` (optional) | the `Which` value, under the same condition as `Write`; recorded (`tasks.md`) |

`Reset` and `ResetNonTrailed` are otherwise identical. `ResetNonTrailed` is produced from `Reset` by
a compiler post-pass when the register's earlier contents can never be observed after a failure; the
case it cannot rule out involves a task resuming inside a failure context that previously failed
(`await`, live variables). Recording a `ResetNonTrailed` anyway is always conformant; recording a
`Reset` is required.

`LiveRange` (`Reset`'s `const` operand) is the op-index range over which the register's new
variable lives. It is compile-time information for that post-pass and has no runtime effect.

## 5. The trailed moves

| Op | Is | Difference |
| --- | --- | --- |
| `Move` | unify `Source` into `Dest` (§3) | — |
| `MoveTrailed` | the same | if `Dest` was fresh, the store is recorded in the undo log, so a failure of the current context makes it fresh again. A unification into a non-fresh `Dest` writes nothing to record |
| `MoveNonComparable` | the same | an *undecidable* comparison fails instead of being an invariant violation (§3.1). Emitted only by the live-variable lowering; unreachable from a Godot script |
| `ReturnTrailed` | `Return` | the return-slot and return-token stores are recorded (`calls.md` §5.5) |

None of them parks: `may_park` is false for all four, because unification binds rather than waits.
Where the compiler uses the trailed forms: the result register of an `if` and of `or`, the result of
a collecting `for`, and the returns of `<suspends>` function bodies. An implementation does not need
the reason, only to record the write.

## 6. Parking

### 6.1 Which ops can park

`ops.json` marks every op that has a parked form `may_park: true`. Among ops the compiler emits,
only these have a real condition to wait on; each waits on the operands listed, checked in the
order listed, and parks on the **first** that reads as an unbound placeholder:

| Op | Waits on |
| --- | --- |
| `Add`, `Sub`, `Mul`, `Div`, `Neg`, `Lt`, `Lte`, `Gt`, `Gte` | both sources (`Neg`: its source) |
| `Neq` | nothing up front. Compares the two sources in full; if some position is decidably *not equal*, succeeds without waiting. Otherwise, if the walk met an unbound placeholder, parks on the first one met; otherwise fails |
| `Query` | `Source` |
| `LtFastFail` … `NeqFastFail`, `ArrayIndexFastFail`, `TypeCastFastFail` | the two value operands (`Lhs`/`Rhs`, `Array`/`Index`, `Type`/`Value`), top level only |
| `QueryFastFail` | `Source` |
| `CanFastAppendToArrayFastFail` | `Ref`, `MaybeMutableArray` |
| `FastAppendToArray` | the effect token, `LeftSource`, `RightSource`, then each element of `RightSource` after it is melted |
| `RefGet` | the effect token, then `Ref` |
| `RefSet`, `RefSetLive` | `Ref`, then (for a Verse `var`) the effect token |
| `RefCallDomain` | `Ref` (then it may call the domain function, which is a `Call`) |
| `Freeze` | `Value`, then the effect token |
| `FreezeIfAccessor` | `Value` |
| `Melt` | the result of melting `Value` (so `Value` itself, when it is not deeply mutable) |
| `Length` | `Container` |
| `LengthWithEffects` | the effect token, then `Container` |
| `CallSet`, `CallSetLive` | `Container`, `Index`, then the effect token |
| `ArrayAdd`, `InPlaceMakeImmutable` | `Container` |
| `NewUnionVariant` | `Tag`, `Payload` |
| `GetUnionVariantPayload`, `GetUnionVariantTag` | `Source` |
| `NewMap` | every key, top level |
| `MapKey`, `MapValue` | `Map`, `Index` |
| `NewClass` | every `Inherited`, and the next-archetype link of `Archetype` |
| `BindNativeClass`, `EndModuleData` | `Class`; `Value` (`modules.md`) |
| `NewObject` | `Archetype`, `Class`, and the archetype's next-archetype link |
| `LoadField` | `Object` |
| `LoadFieldFromSuper` | `Scope`, `Self`, and what the scope captures (`calls.md` §7.4) |
| `CreateField` | the effect token, `Token`, `Object` — parking goes through the leniency indicator (§10) |
| `UnifyField`, `InitializeVar` | the effect token, then `Object` |
| `SetField`, `SetFieldLive` | `Object`, then the effect token |
| `UnifyNativeObject`, `UnwrapNativeConstructorWrapper` | `Token` and `Object`; `Object` (`objects.md`) |
| `NewFunction` | `Procedure`, `ParentScope` |
| `LoadParentScope`, `LoadCapture`, `LoadConstructor` | `Scope`; `Scope`; `Class` |
| `Call` | `Callee`. For a callee that is not a function (an array, map, type or tag used as a function), also the single argument. A native callee may itself answer *block* on a placeholder (`calls.md` §4.3) |
| `CallWithSelf` | `Callee`, `Self` |
| `BeginAwait`, `AwaitSuccess`, `EndAwait`, `BeginBatch`, `EndBatch` | the effect token |

`NewArray`, `NewMutableArray`, `NewOption`, `NewScope` and `BeginProfileBlock` are marked
`may_park` but never wait: an array, option or scope may hold an unbound placeholder. An unbound
operand of `EndProfileBlock` is an invariant violation.

Every other emitted op never parks (`may_park` false): the moves (§5), `Reset`, `ResetNonTrailed`,
the jumps and `Switch`, the four failure-context ops, the task and semaphore ops, `Return`,
`ReturnTrailed`, `ResumeUnwind`, `NewRef`, and the module ops `BeginModule`, `EndModule`,
`ConstructNativeDefaultObject`, `JumpIfDefaultSubObject`, `LoadImport`.

### 6.2 What parking means, observably

When an op parks:

1. It has made **no observable change**. The op tests its waited-on operands before doing anything
   (the order in §6.1 is chosen so), with one exception: `FastAppendToArray` may have appended some
   elements before meeting an unbound one, and it removes them again before parking.
2. It becomes a waiter on the unbound root placeholder it met (§6.4), and it is counted as
   outstanding in the current full failure context (§9.1) and, for a fast-fail op or `CreateField`,
   in its fast failure context (§10).
3. Its `unify_def` destinations stay unbound. Any later op that needs one of them parks too.
4. If the op has either effect-token capture flag (§7.2), the effect token from here on is a fresh
   unbound value that the op will bind when it completes (§8). Every later op that needs the token
   parks on it.
5. **Execution continues with the next op.** Nothing jumps; the main line runs on past the parked op.
   A procedure can reach `Return` with its result still unbound, and its caller receives the
   placeholder.

Nothing about a park is reported to the host. A program whose parked ops are never woken simply
leaves them outstanding; a failure context that is still waiting when its task ends never completes.

### 6.3 Re-execution

When a placeholder an op waits on is bound (§2.3), the op is put on the ready queue. The ready queue
is drained **before the next op of the main line is dispatched**, and also before execution resumes
after a failure has been handled. Draining takes ops from the front and runs each until the queue is
empty, including ops that running others added to its back.

Running a woken op:

1. If its failure context (the one it saved, §7) has failed, it is discarded without running.
2. Otherwise the current failure context and task become the ones it saved, and the op runs again
   **from its start**, with its saved operand values instead of registers (§7). For a fast-fail op
   the fast failure context's state is consulted first (§10.2).
3. If it meets another unbound placeholder, it parks again on that one (it is the same waiter, not
   a second one, and it is not counted again).
4. If it completes, its `unify_def` results are unified into the saved destinations, its return
   effect token (if it saved one) is bound, its failure context's outstanding count is decreased
   (§9.3), and control goes back to the drain — **not** to the op after it in the procedure. The
   ops after it already ran (or parked themselves) when the main line passed it.
5. If it fails, its failure context fails (`failure.md` §4). If it raises a runtime error, the
   whole VM entry ends (`failure.md` §9).

A woken `Call` or `CallWithSelf` whose callee is a Verse procedure runs that procedure to its
`Return` as a nested computation, then unifies the result into the saved `Dest` and binds its return
token. A woken op never continues the procedure it was part of.

After the drain, if the drain made the main line's current failure context (or an ancestor) fail,
the main line leaves it exactly as for any failure (`failure.md` §4) instead of dispatching its next
op.

### 6.4 Ordering

- A placeholder keeps its waiters **most recent first**: each op that parks on it goes to the front
  of its list.
- Binding appends the whole list, in that order, to the back of the ready queue (§2.3). A union
  (§2.4) puts the surviving root's own waiters before the absorbed root's.
- The ready queue is first-in, first-out.

So when one binding wakes several ops, the one that parked last runs first. The only ops whose
relative order could be observed are effectful ones, and those are ordered by the effect token
(§8), not by this list.

## 7. What a parked op saves

### 7.1 Always

| Saved | Why |
| --- | --- |
| the op's identity: its procedure and op index | to know what to run, and for its source line if it later raises |
| its full failure context | re-execution happens in it (§6.3); the context counts it (§9.1); if the context fails, the op is discarded |
| its task | re-execution runs in it |
| the **value** of every `use` operand, variadic ones element by element | registers are reused by the compiler without regard to parked ops, so the op must never read its registers again. The value may itself be an unbound placeholder; that is what it waits on |
| the **value** of every `unify_def` operand, reading (and so materializing, §2.1) a fresh one | the op's result goes into that placeholder when it completes, and whoever reads the destination register meanwhile holds the same placeholder |
| every `immediate` and `const` operand | they are part of the op |
| for a fast-fail op or `CreateField`, its `OnFailure` label | kept in the fast failure context record (§10.1) rather than with the op; either place is conformant |

A `jump` operand other than a fast-fail `OnFailure` is never needed by a parked op. Cache operands
(`format.md` §5.1) are not saved.

### 7.2 The capture flags

`ops.json` gives each op up to three capture flags. They name state a parked op must save in
addition to §7.1:

| Flag | Save | Why |
| --- | --- | --- |
| `return_effect_token` | the effect token the op must bind when it completes (the fresh one that replaced the current token when it parked, §8.2) | every later effectful op is waiting on it; the op is the only thing that can release them |
| `incoming_effect_token` | the effect token that was current when the op began | an op with this flag does not necessarily need the token to be concrete before it can proceed: a `Call` of a Verse function passes the token into the callee, and `RefSet` needs it only after `Ref` is known. When it re-executes, it must consume the token it was given, which may still be unbound, and deliver the callee's final token (or its own) into the return token |
| `batched_refs` | the current batch of pending reference writes | a `batch` block groups `var` writes so that awaiters of those vars are signalled once at the end. A re-executed write must join the batch that was open when it was issued. Not reachable from a Godot script (`tasks.md`) |

Ops with only `return_effect_token` (`FastAppendToArray`, `RefGet`, `CreateField`, `UnifyField`, the
`Await` and `Batch` ops) always test the token **first** (§6.1). So whenever they run, every earlier
effect is done, and when re-executed they treat the incoming token as the concrete "done" value; they
need not save it.

## 8. The effect token

### 8.1 What it is

The effect token is one value threaded through execution in program order, like an extra register
that is not in the frame: it is part of the running computation, it passes from caller to callee and
back (`calls.md` §5.1, §5.5), and a failure context remembers it (`failure.md` §3). It is either
**concrete** — a single distinguished "done" value, meaning every effect issued so far has happened —
or an **unbound placeholder**, meaning some earlier effectful op is still parked.

At every VM entry the token is concrete. It must be concrete again when control returns to the host
(an entry that ends with it unbound is an invariant violation in Epic's VM).

### 8.2 How an effectful op uses it

An op is **effectful** when it has either effect-token capture flag. For such an op:

1. If the op needs the token before it can do its effect (§6.1 lists where), and the token is an
   unbound placeholder, the op parks on it.
2. If the op parks for any reason, the current token is replaced by a fresh unbound value, which
   the op saves as its return token (§7.2).
3. When the op completes — now or after re-execution — its return token is bound to the concrete
   token.
4. If the op completes without parking, the token stays as it was.

A `Call` into a Verse procedure does not test the token: the callee starts with the caller's current
token, possibly unbound, and the caller continues with whatever token the callee's `Return`
delivered.

Natives never consume or replace the token (`calls.md` §4.3). Beginning and ending a full failure
context also wait for it (§8.4).

### 8.3 The guarantee

**Effects happen in program order.** An effect is anything an effectful op does that another op or
the host could see: a `var`, field, element or map write, a native call, the start or commit of a
failure context's transaction. If effectful op E1 precedes effectful op E2 in the order the main line
issues them, E2 does not perform its effect until E1 has performed its own, because E2 needs a token
that only E1's completion binds.

**Pure ops are not ordered.** An op with no capture flag (arithmetic, comparisons, container
construction, field loads, scope and function construction, moves) runs as soon as its operands are
concrete, which may be before or after effects that precede it in the source. Since such ops change
nothing observable, their order can only be seen through the values they produce, and those flow
through placeholders.

`RefGet` is effectful even though it only reads: a read of a `var` must not move before a write that
precedes it.

### 8.4 Failure contexts and the token

| Event | Token concrete | Token unbound |
| --- | --- | --- |
| `BeginFailureContext` | the context's transaction starts now; the context remembers the token | the context remembers the unbound token; its transaction starts only when that token is bound; meanwhile the current token becomes a fresh unbound value, bound once the transaction has started |
| `EndFailureContext`, nothing outstanding | the transaction commits now | the commit waits for the token in the same way, and the current token becomes a fresh value bound once the commit has happened |
| `EndFailureContext`, ops outstanding | §9.2 | §9.2 |
| failure of the context | the token is reset to the one the context remembered at its beginning (`failure.md` §4) | the same |

Because the transaction of a context that started while the token was unbound has not started,
nothing inside it can perform an effect early: every effectful op inside needs a token that the
transaction start binds.

## 9. Lenient completion of a full failure context

### 9.1 The outstanding count

Every full failure context counts its outstanding work:

- +1 for each op that parks while the context is current (§6.2), whether it is a full-context op or
  a fast-fail op inside it;
- +1 for each child failure context that has completed leniently (§9.2) and whose then- or
  else-branch has not yet finished running.

It decreases by one when such an op completes (§6.3 step 4) or such a child's branch finishes
(§9.3 step 5).

### 9.2 Reaching `EndFailureContext` with work outstanding

If the count is zero, `EndFailureContext` does what `failure.md` §3.2 says. Otherwise the condition
cannot yet be decided, and:

1. The context records that it reached its end, the op after `EndFailureContext` (the start of the
   then-branch) and the `Done` label (the end of the whole `if`).
2. The context's parent gains one outstanding (§9.1).
3. The context remembers the current effect token as its **before-then token**, and the current token
   becomes the context's **done token**: a fresh unbound value that will be bound when the then- or
   else-branch has finished.
4. The context takes a **copy of the current frame**: same procedure, same register count, each
   register holding what the original holds, with every fresh register materialized first (§2.1) so
   that the copy and the original share its placeholder. The copy has no caller. The branch that
   eventually runs runs on this copy. Values a branch computes reach the main line only through
   those shared placeholders — the `if`'s result register (§5) is one.
5. The main line jumps to `Done` and carries on, in the parent context.

The context's transaction is left open: it is neither committed nor aborted. Nothing else can start a
transaction until it resolves, because the main line now holds the unbound done token and every
transaction start waits for the token (§8.4).

Epic's VM also merges the context's undo-log records of trailed register and placeholder writes into
the parent at this point, as it does on an ordinary commit; see `failure.md` §6.4 and §12.

### 9.3 Resolution

The context resolves when either of these happens first:

**Every outstanding piece completes** (the count reaches zero after §9.2 happened):

1. The context's transaction commits (`failure.md` §7); if the before-then token is still unbound,
   the commit waits for it (§8.4).
2. The then-branch runs as a nested computation: from the recorded then-start, on the frame copy,
   with the parent as its failure context, starting with the before-then token (or the fresh token
   standing for the deferred commit). It stops when execution in the copy reaches an op index at or
   beyond `Done`.
3. The done token is unified with the token the branch finished with.
4. If the branch itself completed without failing, its undo records are merged into the parent's.
5. The parent's outstanding count is decreased.

**One outstanding piece fails:**

1. The context fails (`failure.md` §4 steps 1–3): transaction aborted, undo log replayed, pending
   children aborted first, every context in its subtree marked failed. Their parked ops will be
   discarded when woken.
2. The else-branch runs as a nested computation: from the context's `OnFailure` label, on the frame
   copy, in the parent context, starting with the token the context remembered at its beginning. It
   stops at or beyond `Done`. When the `if` has no else, `OnFailure` is `Done` and nothing runs.
3. The done token is unified with the token the branch finished with.
4. The parent's outstanding count is decreased, unless the parent has itself failed.

If the context fails before it reached `EndFailureContext`, it is an ordinary failure: the main line
has not passed the context, and it jumps to `OnFailure` there (`failure.md` §4).

A branch that transfers control out of the `if` — `return`, `break` — while running as a nested
computation is not supported by Epic's VM (§12, question 4).

## 10. Fast failure contexts under leniency

`failure.md` §5 describes fast failure contexts as they run when nothing parks. This section is what
the `LeniencyIndicator` operands are for.

### 10.1 The leniency indicator

The compiler allocates one register per fast failure context and gives it, as `LeniencyIndicator`,
to every fast-fail op and `CreateField` in that context and to the context's `EndFastFailureContext`.
It starts fresh. It stays fresh unless something in the context parks, in which case it holds a
**fast failure context record**:

| Field | Meaning |
| --- | --- |
| outstanding | how many of its ops are parked, plus inner fast contexts not yet resolved |
| failed | whether one of its ops has failed |
| on-failure | the `OnFailure` label of its ops (all ops of one context share it) |
| incoming token | the effect token current when the record was created; the token its then- and else-branch both start with (no effect may happen inside a fast failure context, so one token serves both) |
| batch | the current batch of pending reference writes (§7.2) |
| then-start, done, frame copy, enclosing full context, parent | filled in by `EndFastFailureContext` (§10.3) |
| done token | a fresh unbound value, bound when its branch has run |

### 10.2 A fast-fail op that parks, fails or succeeds

Parking: if the indicator is fresh, a record is created (outstanding 0, incoming token = current
token, batch = current batch) and unified into the indicator register. Its outstanding count
increases, its on-failure label is set, and the op parks as in §6.2 (it counts in the full context
too).

Failing, on the main line: the op jumps to `OnFailure` (`failure.md` §5.2). If the indicator holds a
record, the record is marked failed; ops of the context that are still parked will be discarded when
woken.

When a parked fast-fail op is woken:

1. If its record is marked failed, it does nothing more (it still counts as completed in the full
   context).
2. It runs; if it parks again, it waits again.
3. If it fails, the record is marked failed, and the context's else-branch runs (§10.3) — provided
   the context's `EndFastFailureContext` has already run; in compiled code a parked op is woken only
   after that (§12, question 5).
4. If it succeeds, the record's outstanding count decreases; if that reaches zero and
   `EndFastFailureContext` has run, the then-branch runs (§10.3).

### 10.3 `EndFastFailureContext`

Operands: `OuterLeniencyIndicator` (`unify_def`), `LeniencyIndicator` (`use`), `OnDone` (`jump`).

- If `LeniencyIndicator` is fresh, or holds a record with outstanding zero, nothing parked or all
  parked ops have completed: execution continues with the next op (the then-branch). `OnDone` is not
  used. Reading the indicator here must not materialize a placeholder in it.
- Otherwise the condition is undecided:
  1. The record takes: then-start = the op after this one; done = `OnDone`; a frame copy as in §9.2
     step 4; the enclosing full failure context = the current one.
  2. The record registers with the enclosing fast context through `OuterLeniencyIndicator`: if that
     register holds a record, that record's outstanding count increases; if it is fresh, a new record
     is created for it (with outstanding 1 and no on-failure label) and unified into it. That record
     becomes this record's parent. When the fast context is not nested in another, the compiler
     passes a register used nowhere else, and the record created there is never looked at again.
  3. The current effect token becomes this record's done token.
  4. Execution jumps to `OnDone`.

When the record resolves (§10.2 steps 3–4), its branch — the then-branch from then-start, or the
else-branch from on-failure — runs as a nested computation on the frame copy, in the recorded
enclosing full context, starting with the record's incoming token, and stops at or beyond `done`. The
done token is then unified with the branch's final token, and the branch's undo records merged into
the enclosing full context's when it did not fail. Then, if the record has a parent that has not
failed, the parent's outstanding count decreases, and if it reaches zero and the parent's
`EndFastFailureContext` has run, the parent's then-branch runs in the same way.

A fast-fail op that fails on the main line while running *inside* such a nested branch, in a fast
context whose `EndFastFailureContext` has already run, runs that context's else-branch and ends the
nested computation.

## 11. What a stage-1 interpreter can omit

Design §7.1 stage 1: a runtime park is a fatal error naming the op and line. This section says why
that should be enough for the programs this project runs, how to check a program statically, and
what must still be built in full.

### 11.1 Why ordinary programs never park

A placeholder is created only by reading a fresh slot (§2.1), and a park needs an unbound one. The
compiler at this commit refuses every source spelling that would make compiled code read a variable
ahead of its definition:

| Spelling | Refusal (measured: `tests/verse_probe/vm_unification_reject.verse`, which compiles nothing on purpose; its header lists the expected lines) |
| --- | --- |
| a local declared without a value, `X:int` in a function body | error 3601, "Data definitions at this scope must be initialized with a value" |
| a field initializer reading another field of the same object, in either order | error 3502, "Accessing instance member `A` from this scope is not yet implemented" |
| a module-level data definition reading one defined after it | error 3502, "Accessing a variable from the initializer of a variable that precedes it in the same snippet … isn't implemented yet" |

And the ordering of construction removes the other classic source: a class's `block` runs only after
every field has its value — defaults, fields the archetype supplies, and fields without defaults alike
(`vm_unification_probe` `ConstructionOrder`: a block reads `First = 3, Second = 4`, and in another
class reads `Given = 11` supplied by `probe_needs{Given := 11}`).

So in function bodies, class construction and module data, every register is written before it is
read, the effect token never becomes unbound (it only does after a park), and nothing parks. The one
remaining place is the package procedure's definition binding (§3.3), where an unbound
definitions-table entry is *bound*, which is not a park; whether any op there reads such an entry
*before* binding it is exactly what the static rule below reports.

### 11.2 The static park-risk rule

The writer reports (design §7.1, task T2.4) which ops could park. The rule, per procedure, is a
forward data-flow analysis of **known** registers — registers certain to hold a concrete value:

1. At procedure entry, register 0 (Self), register 1 (Scope), the positional parameter registers and
   the named parameter registers are known (`calls.md` §2.1). Every other register is unknown.
2. `Reset` and `ResetNonTrailed` make their `Dest` unknown.
3. `Move`, `MoveTrailed`, `MoveNonComparable`: if `Dest` is known before, it stays known and `Source`
   becomes known too (a unification into a concrete destination binds the source). If `Dest` is
   unknown and `Source` is known, `Dest` becomes known. If both are unknown, both stay unknown.
4. Any other op makes each of its `unify_def` and `clobber_def` registers known after it.
5. A constant operand is known. At a join of control-flow paths, a register is known only if it is
   known on every incoming path. Labels reached by failure (`OnFailure`) are joins like any other.
6. **At risk:** any op with `may_park` true that reads an unknown register in one of the operand
   positions §6.1 says it waits on.
7. **Escape:** any op that hands an unknown register's value somewhere this analysis does not follow
   — a `Call`/`CallWithSelf`/`CallTask` argument or named argument, a `Return` value, a value stored
   by `NewArray`, `NewMutableArray`, `NewOption`, `NewMap`, `NewScope`, `NewFunction`, `RefSet`,
   `CallSet`, `SetField`, `UnifyField`, `InitializeVar`, `ArrayAdd`, `EndModuleData` or `EndTask`.

If no procedure in the program has an op at risk or an escape, **no op can park at run time**: by
induction, no unbound placeholder is ever created, because parameters, loaded fields, captures, call
results and container elements can only carry one that some procedure let escape. This holds provided
no native returns or stores an unbound placeholder, which none of the natives a Godot script reaches
does (`natives.md`, `godot-natives.md`), and provided the linked program contains no placeholder
(`format.md` §3).

The report should list, per procedure, each at-risk op and each escape with its op index and source
line, and a total. A non-empty report is the measurement that decides whether stage 2 is needed.

### 11.3 What stage 1 must still implement in full

| Must implement | Because |
| --- | --- |
| unification into every `unify_def` destination as §3 says, including the comparison of two concrete values, *not equal* as failure, and structural comparison | `=` in full contexts (§3.3), definition binding, `Return` into a destination that already holds a value (`calls.md` §5.6) |
| binding an unbound placeholder found in a destination or inside a compared aggregate | the definition-binding pattern if the writer leaves any definitions-table entry unbound (§12, question 2); union of two placeholders can be an invariant violation in stage 1 |
| the `clobber_def` rules of §4 and the trailed writes of §5, recorded in the undo log | failure must restore them exactly as Epic's VM does (`failure.md` §6.2) |
| fast failure contexts on the path where nothing parks: fast-fail ops test, jump to `OnFailure` on failure, leave the indicator fresh; `EndFastFailureContext` falls through | every `if` whose condition has no writes compiles this way (`failure.md` §5) |
| `EndFailureContext` with nothing outstanding | every full failure context |
| the effect token as a value that is always the concrete "done" value, delivered by `Return` into the return effect-token slot | it cannot become unbound without a park; an implementation may represent it as a constant |

| May omit in stage 1 | Replace with |
| --- | --- |
| waiter lists, the ready queue, re-execution, saved operand records, the capture flags | the stage-1 park error: at the moment an op would park, stop the VM entry as `failure.md` §9.5 specifies, naming the op, its procedure and source line, and increment a park counter the host can read |
| fast failure context records and `EndFastFailureContext`'s undecided path | nothing: with no parks, the indicator is always fresh |
| `EndFailureContext`'s lenient path, frame copies, done and before-then tokens | nothing: with no parks, the outstanding count is always zero |
| deferred transaction start and commit (§8.4) | nothing: the token is always concrete |
| a `Neq` or equality walk that parks on a nested placeholder | the park error |

A native answering *block* (`calls.md` §4.3) is a park like any other and gets the same error.

## 12. Open questions

1. **Placeholder bindings are not undone.** §2.3 follows Epic's VM: binding a placeholder to a value
   is not recorded, only links are. The argument that this is unobservable from compiled code is
   source reasoning plus the refusals of §11.1, not a probe; no fixture can create an unbound
   placeholder outside a failure context and bind it inside one. If stage 2 is built, a differential
   fixture exercising leniency inside a failing context should confirm it.
2. **The definition-binding pair in a linked program.** §3.3's second `Move` compares the value just
   built with the definitions-table entry. In a program compiled in process, that entry is an unbound
   placeholder at the time, so the `Move` binds it. `format.md` §3 says a linked program holds no
   placeholder, and §7 says loading runs every package procedure. If the writer resolves the entry to
   the cell the cook produced and the loader then runs a `NewClass` that builds a second class cell,
   the comparison is *undecidable* between two distinct classes and is an invariant violation. The
   writer (T2.1) and `modules.md` need to settle which of three things happens: the entry is written
   unbound (a documented exception to "no placeholders"), the loader does not re-run ops that build
   definition cells, or the writer rewrites the pair.
   **Answered by the lead:** the second. `modules.md` §2 established that a loader runs no
   initialization at all, and `format.md` §7 now says so, so no package procedure runs after load
   and the pair is never compared.
3. **Epic's lenient trail merge.** At a lenient `EndFailureContext` (§9.2) and after every woken op,
   Epic's VM merges trailed register and placeholder records into the parent context before the
   context is decided, so a context that fails later does not undo them (its own trail is empty by
   then). The model in this file and in `failure.md` would undo them. Whether any compiled program can
   observe the difference was not settled; it matters only to stage 2.
4. **Control transfer out of a lenient branch.** A `return` or `break` inside a then- or else-branch
   that runs as a nested computation (§9.3, §10.3) is marked unsupported in Epic's VM: the copy has no
   caller, but its return slot is shared with the original frame's. Not specified; only reachable in
   stage 2.
5. **A parked fast-fail op woken before its `EndFastFailureContext`.** §10.2 step 3 assumes a parked
   op of a fast context is woken only after the context's `EndFastFailureContext` has run. Epic's VM
   makes the same assumption without checking it; a binding made later in the same condition could
   break it. Stage 2 only.
6. **Equality with a nested placeholder in `EqFastFail` and `NeqFastFail`.** These two test only
   their top-level operands for concreteness, then compare without binding: a nested unbound
   placeholder is treated as equal and left unbound (source, unprobed). Unreachable while nothing
   parks.
