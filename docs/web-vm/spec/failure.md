# Failure, transactions and runtime errors

Status: reviewed by the lead 2026-09-24. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/facts.md`, `docs/web-vm/spec/calls.md`,
`docs/web-vm/spec/godot-natives.md` §7; UE `Engine/Source/Runtime/CoreUObject/{Public,Private}/VerseVM`
(the interpreter, failure and fast-failure contexts, the transaction and trail header, rest values,
placeholders, frames, the VM entry, the runtime-error table and its raising and formatting, the
hang-detection thresholds, the task and semaphore header, the mutable array), the AutoRTFM public
header (commit, abort and completion handler ordering),
`Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/VVMCodeGenerator.cpp` (failure-context
selection, `if`, `or`, `not`, `option`, `for`, assignment), `Engine/Plugins/Verse/Verse` (`Err`),
`Engine/Plugins/Solaris/Source/Solaris/Private/SolarisModule.cpp` (runtime-error text), the
`VerseTestScriptCmd` test sources `Failure` and `Bytecode/ControlFlow`; this repository's
`host/Private/VerseHost.cpp`, `HostRuntime.cpp`, `GodotBindings.cpp` and `include/verse_host_abi.h`.
Probes: `tests/verse_probe/vm_failure_probe.verse`, `tests/verse_probe/vm_failure_reject.verse`.

This file says what a failure context is, what counts as failure, what happens when one fails, what
state the VM must be able to undo and what it must not touch, how nested transactions combine, what a
runtime error does and what the host sees, what effect specifiers change in compiled code, and how a
native's outside-world effects are kept consistent with rollback.

What happens when a failure context finishes while some of its ops are still waiting on unbound
values is `unification.md` §9–§10; this file covers the case where nothing waits. Unification into
a destination, and which comparison answers fail, is `unification.md` §3. Frames and calls are
`calls.md`'s; tasks, cancellation and `defer` are `tasks.md`'s; individual natives are `natives.md`'s
and `godot-natives.md`'s. Op and operand names are `ops.json`'s.

Probe claims cite `vm_failure_probe.verse` and the method; run it with

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        <repo>/tests/verse_probe/vm_failure_probe.verse --class vm_failure_probe

The probe driver calls each public zero-argument method as its own VM entry. `Print` is this
repository's native and defers its output to commit (§11), so a line printed inside a failed context
never appears — that absence is one of the readings.

## 1. The model

A **failure context** is a region of execution that either **succeeds**, keeping what it did, or
**fails**, leaving no trace of what it did on anything the program can still reach. Verse's `if`
condition, `or`'s left side, `not`'s operand, `option{...}`'s body, a `for`'s generators and filters,
and the body of a `<decides>` function called from one of them are all executed inside one.

There are two kinds:

| Kind | Opened by | Undo | Used when |
| --- | --- | --- | --- |
| **full** | `BeginFailureContext`, closed by `EndFailureContext` | a transaction: everything §6 lists is recorded and restored on failure | the region may write, may call a `<decides>` function (other than the few the compiler inlines), or may suspend (§10) |
| **fast** | no op; the region's fast-fail ops share a `LeniencyIndicator` register and an `OnFailure` label, and `EndFastFailureContext` closes it | none | the region can only fail through a comparison, an index, a cast or a query, and writes nothing |

Full failure contexts nest **dynamically**: a context opened while another is current is its child,
whichever procedure opened it. A call does not open or close a context (`calls.md` §5.6): the body of
a callee runs in whatever context was current at the call, so a failure in the callee fails the
caller's context.

Every VM entry — a host call into Verse, a task resumption from the host's tick, a callback — starts
with a **root** full context around everything it runs. The root's transaction commits when the
entry returns normally.

## 2. What failure is

An op **fails** in these situations. "Fails" always means the current full failure context fails
(§4), except for fast-fail ops, which jump to their own `OnFailure` (§5).

| Situation | Ops |
| --- | --- |
| a query of the empty option or `false` | `Query`, `QueryFastFail`. The compiler's explicit "fail here" is a `Query` (or `QueryFastFail`) of the constant `false`: it closes `not` when its operand succeeded, and a non-collecting `for` that found nothing |
| a relation that does not hold | `Lt`, `Lte`, `Gt`, `Gte`, `Neq` and their `…FastFail` forms, `EqFastFail` (§5.2 gives the tests) |
| a unification whose destination already holds a value not equal to the new one | any op with a `unify_def` operand, `Move` and `MoveTrailed` in particular (`unification.md` §3.1); `MoveNonComparable` also on an *undecidable* comparison |
| division by zero | `Div` of two integers, or with a rational operand, when the divisor is zero. Two floats divide totally (`values.md`) |
| an index or lookup that finds nothing | `Call` whose callee is an array (index not an integer in 0 … 2³²−1, or not below the length), a map (key absent) or a type (the value is not of that type); `ArrayIndexFastFail`, `TypeCastFastFail`; `CallSet` on a mutable array with such an index (`calls.md` §10) |
| a native answering *fail* | `Call`, `CallWithSelf` on a native `<decides>` function (`calls.md` §4.3, `natives.md`) |
| a resumed task whose resume value does not unify with its resume slot | `tasks.md` |

`CanFastAppendToArrayFastFail` and `CreateField` also jump to an `OnFailure` label, through the same
fast mechanism, but neither is a failure of the program: the first selects the slow path of `+=` on a
`var` array, the second skips a field initializer that was already run (`objects.md`).

**What is not failure**: an unbound operand (the op parks, `unification.md` §6), a runtime error (§9)
and a VM invariant violation (§9.4). None of them is caught by a failure context.

## 3. Full failure contexts

### 3.1 `BeginFailureContext`

Operands: `OnFailure` (`jump`), `Id` (`const`, `failure_context_id`).

Opens a new full context, child of the current one, and makes it current. The context records:

| Recorded | Used for |
| --- | --- |
| its parent (the context that was current) | becomes current again when this one ends |
| the current frame | where failure resumes: frames entered since are abandoned |
| `OnFailure` | the op failure resumes at, in that frame |
| the current effect token | restored on failure (`unification.md` §8.4) |
| the current batch of pending reference writes | the batch a branch run leniently uses (`unification.md` §9.3); `tasks.md` |
| the task | a context belongs to one task |

and it starts the context's **transaction** (§7) — at once when the effect token is concrete, which
is always the case in stage 1, or when the token becomes concrete otherwise (`unification.md` §8.4).

`Id` identifies the context statically within its procedure: a `BeginFailureContext` and the
`EndFailureContext` that closes it carry the same `Id`. The interpreter needs no `Id` at run time,
because contexts pair dynamically. The compiler emits `BeginFailureContext` more than once with the
same `Id` to **re-enter** a context — a collecting `for` does this after each iteration's body (§8.4)
— and each execution opens a new context.

**Control equivalence.** The compiler guarantees that, if a context does not fail, execution reaches
exactly one `EndFailureContext` for it before anything that would leave the region (no jump escapes a
full context from inside it). The interpreter may rely on this and need not check it.

### 3.2 `EndFailureContext`

Operands: `Done` (`jump`), `Id` (`const`).

When the context has no outstanding parked work (always, in stage 1):

1. The context's transaction commits into its parent's (§7).
2. The parent becomes current.
3. Execution continues with **the next op** — the first op of the `then` branch. `Done` is not used.

`Done` is where the `then` branch ends (for an `if`, the end of the whole `if`). It is used only when
the context ends with parked work outstanding (`unification.md` §9.2). `EndFailureContext` is never
executed by a context that has already failed; reaching one would be an invariant violation.

## 4. What happens on failure

When an op fails and the current context is a full context **F** that has not yet reached its
`EndFailureContext` (always, in stage 1):

1. **Pending children first.** Any child context of F that ended leniently and is still undecided
   (`unification.md` §9) is aborted before F, deepest first and, among siblings, the most recent
   first. In stage 1 there are none.
2. **F's transaction aborts** (§7): every record in its undo log is undone, so each recorded slot
   holds again the value it held when F began; F's compensation actions run; F's queue of deferred
   native effects is discarded.
3. **F and every context below it are marked failed.** Any parked op that belongs to one of them is
   discarded when it would have been woken (`unification.md` §6.3).
4. **Control goes to F's `OnFailure` label, in the frame F recorded.** Frames the failing op was
   running in, if it was inside a callee, are simply abandoned — their remaining ops never run and
   they never `Return`.
5. **The effect token becomes the one F recorded** at its beginning.
6. **F's parent becomes current.** The failure stops here: the code at `OnFailure` (an `else`
   branch, `or`'s right side, the empty-option result of `option{}`, the success path of `not`) runs
   in the parent.

Registers are **not** restored, except those §6.2 records. The compiler never reads, after a
failure, a register that was written inside the failed context, except through the recorded ones.

If F is the **root** context of the VM entry — a `<decides>` function the host called directly
declined — the entry ends: everything the entry did is undone as in step 2, and the host is told the
function failed (`VH_ERR_FAILED`). Measured: `DeclinesAtTop` writes a field, prints, then fails; the
call answers status 8 (`VH_ERR_FAILED`), nothing is printed, and the next entry,
`DeclinesAtTopAfter`, reads the field unchanged (`Survivor = 0`).

When F has already reached its `EndFailureContext` leniently, steps 1–3 and 5 are the same, and the
else-branch runs as `unification.md` §9.3 says instead of step 4.

## 5. Fast failure contexts

### 5.1 Shape

The compiler allocates a fresh register for the context's `LeniencyIndicator` and a label for its
failure path. Inside, every op that can fail is one of the fast-fail ops of §5.2, all naming that
indicator and that label. After the region comes `EndFastFailureContext`. A fast context never
contains a full one; a full context may contain fast ones (the compiler compiles comparisons inside a
full context with the ordinary ops, `Lt` rather than `LtFastFail`, and only nested regions that
qualify get fast contexts).

When nothing parks (always, in stage 1):

- A fast-fail op whose test passes unifies its result into `Dest` (if it has one) and execution
  continues with the next op.
- A fast-fail op whose test fails **jumps to its `OnFailure` label**. Nothing else happens: there is
  no transaction, nothing is undone, the current full context is unaffected. If the indicator holds a
  fast failure context record (only possible when something parked, `unification.md` §10), the record
  is marked failed.
- `EndFastFailureContext` finds its `LeniencyIndicator` fresh and falls through to the next op (the
  `then` branch). Its `OnDone` label and `OuterLeniencyIndicator` are used only under leniency
  (`unification.md` §10.3). Reading the indicator here must not turn it into a placeholder.

**Why nothing needs undoing.** The compiler uses a fast context only for regions that contain no
write, no call to a function with `<decides>`, `<writes>` or `<suspends>` in its effects (apart from
the operators §5.2 inlines), no `set`, no `var` creation, no `defer`, and no construction of a class
whose constructor has those effects (§10). So the only state a fast region changes is registers, and
the compiler never reads those after the region fails.

### 5.2 The fast-fail ops

| Op | Test (the op waits first on the operands `unification.md` §6.1 names) | `Dest` on success |
| --- | --- | --- |
| `LtFastFail`, `LteFastFail`, `GtFastFail`, `GteFastFail` | the relation between `Lhs` and `Rhs`: two integers, two floats (as `values.md` §3.3: NaN is unordered against every other value, and `NaN <= NaN` and `NaN >= NaN` succeed), or a rational with an integer or rational (the integer taken as a rational). Other kinds are a VM invariant violation | `Lhs` |
| `EqFastFail` | equality (`values.md`) answers *equal*; any other answer fails | `Lhs` |
| `NeqFastFail` | equality answers *not equal*; any other answer fails | `Lhs` |
| `ArrayIndexFastFail` | `Array` is an array or a mutable array, and `Index` is an integer in 0 … 2³²−1 below its length. The value `false` (which is also the empty option, `format.md` §3) as `Array` fails. Any other kind is an invariant violation | the element |
| `TypeCastFastFail` | `Type` is a type value that admits `Value` (`objects.md`, `values.md`) | `Value` |
| `QueryFastFail` | `Source` is not `false`. An option gives its contents | the option's contents |
| `CanFastAppendToArrayFastFail` | `Ref` is not a `var` with a domain function (an absent `Ref` passes), and `MaybeMutableArray` is a mutable array | none |

The ordinary ops `Lt`, `Lte`, `Gt`, `Gte`, `Neq` and `Query` apply the same tests and give the same
`Dest`; on failure they fail the current full context instead of jumping. `Neq`'s handling of nested
placeholders is `unification.md` §6.1's.

## 6. The undo log

Each full context's transaction keeps an **undo log**: for each slot written while the context is
current, what the slot held before. Undoing restores every recorded slot to the value it held **when
the context began**. An implementation may record every write and replay the log in reverse, or
record only the first write per slot and replay in any order; the result is the same.

The records of §6.2 — registers, return slots, placeholder links — and the array-immutability
change of §6.1 are sometimes called the *trail*. They obey exactly the same rules as the heap records,
with the one exception noted in §6.4, so one log serves for both (design §7.2).

### 6.1 Heap state that is recorded and undone

Measured rows cite `vm_failure_probe`; each writes inside a condition that then fails and reads the
state afterwards.

| Write | By | What is recorded | Measured |
| --- | --- | --- | --- |
| a Verse `var`'s content (local or field) | `RefSet`, `RefSetLive`; `SetField`/`SetFieldLive` and `CallSet` when the slot holds a `var` | the old content | local `var`: `VarOnFailureAndSuccess` (0 after failure, 2 after success); class `var` field: `HeapWrites` (0); a field written through a callee: `FailureAcrossCall` (0) |
| a plain field of a struct held in a `var` | `SetField`, `SetFieldLive` | the old field value | not reachable: a struct cannot declare a `var` member (error 3607) and a non-`var` member cannot be `set` (error 3509); `vm_failure_reject.verse` |
| an element of a mutable array | `CallSet`, `CallSetLive` | the old element | `HeapWrites` (element 1 after a failed `set A[0] = 9`) |
| appending to a mutable array | `ArrayAdd` with `bTransactional` true; `FastAppendToArray` | the old length (the appended elements go with it) | `HeapWrites` (length 3 after a failed `+= array{4, 5}`) |
| inserting a new key into a mutable map | `CallSet` | that the key was absent: undo removes it, restores the count and the insertion order | `HeapWrites` (length 1 after a failed insert) |
| replacing a mutable map entry's value | `CallSet` | the old value | `HeapWrites` (10 after a failed replace) |
| turning a mutable array into an immutable one in place | `InPlaceMakeImmutable` | that it was mutable: undo makes it a mutable array again | source, unprobed (the collecting `for`, §8.4) |
| task state, semaphore counts, awaiter lists, the batch of pending reference writes | the task and batch ops, and `var` writes that signal awaiters | the old state | `tasks.md` |

**Not recorded**, by the compiler's guarantee: `ArrayAdd` with `bTransactional` false. The compiler
emits it only when the array was created in the same transaction as every append to it (a collecting
`for` whose body cannot suspend), so a failure that would undo an append also makes the whole array
unreachable. Recording it anyway is conformant. An implementation may generalize: a write to a cell
allocated since the current transaction began need not be recorded.

**Melt and Freeze change nothing.** `Melt` and `Freeze` build a mutable or immutable **copy**; the
original is untouched, and a new cell needs no undo. (`Freeze` of an accessor calls its getter, which
is a call like any other.) `FreezeIfAccessor` returns non-accessors unchanged.

**A container's representation is not state.** An implementation may change how a mutable array or
map stores its elements (for example, from a packed integer buffer to general values) without
recording it, as long as the visible contents are restored.

### 6.2 Registers, return slots and placeholders

| Write | By | What is recorded |
| --- | --- | --- |
| making a register fresh | `Reset` | the register's old content. `ResetNonTrailed` is not recorded (`unification.md` §4) |
| storing into a fresh register | `MoveTrailed` | that it was fresh. A `MoveTrailed` that unifies into a non-fresh register writes nothing |
| a `Return`'s stores into the return slot and the return effect-token slot | `ReturnTrailed` | their old contents (`calls.md` §5.5) |
| the result and switch registers written by a finishing task | `EndTask` `Write`, `Switch` | their old contents (`tasks.md`) |
| the resume slot a resumed task's value is unified into | task resumption | its old content (`tasks.md`) |
| a link from one placeholder to another | unification of two unbound placeholders (`unification.md` §2.4) | that it was unbound, with its waiters |

### 6.3 Not recorded, never undone

| Not undone | Why this is correct |
| --- | --- |
| allocation of new cells (arrays, maps, objects, options, refs, scopes, functions, placeholders, frames) | a new cell that the failure made unreachable is garbage |
| register writes by `Move` and by every op's `unify_def` destination, `ResetNonTrailed` | the compiler never reads them after the context fails (`unification.md` §4) |
| binding a placeholder to a concrete value | `unification.md` §2.3 |
| initialization of an object under construction (`CreateField`, `UnifyField`, `InitializeVar`, and `NewRef` creating a `var`) | the object and the `var` are new cells |
| the interpreter's position, the effect token | reset by §4 steps 4–5, not by the log |
| effects a native performed immediately | a native that performs an outside effect immediately registers a compensation instead (§11) |
| profiling events (`BeginProfileBlock`, `EndProfileBlock`) | reported as they happen |

### 6.4 The trail under leniency

In Epic's VM a context's trail records pass to its parent at every `EndFailureContext`, including one
reached leniently whose outcome is not yet known, and when each woken op finishes. In stage 1 this is
the same as passing them on at commit. Under leniency it means a context that fails after its
`EndFailureContext` does not undo those records. This file's model undoes them; `unification.md` §12
question 3 tracks whether the difference is observable.

## 7. Nested transactions

Each full context owns a transaction with three parts: its undo log (§6), its **deferred-effect
queue** and its **compensation list** (§11). The root context of a VM entry owns one too.

| Event | Undo log | Deferred-effect queue | Compensation list |
| --- | --- | --- | --- |
| **commit** of a child into its parent (`EndFailureContext`, §3.2) | appended to the parent's; for a slot both record, the parent's older value is the one that counts | appended to the parent's, order kept | appended to the parent's, order kept |
| **abort** of a context (§4) | replayed: every recorded slot restored to its value at the context's beginning | discarded | run, most recently registered first, after the log has been replayed |
| **commit of the root** (the VM entry returns normally) | discarded | **run, in the order the effects were queued** | discarded |
| **abort of the root** (the entry's function declined, §4; or a runtime error, §9) | replayed | discarded | run, most recent first |

Because a child's records join its parent's on commit, aborting a parent also undoes everything its
committed children did (`Nesting`: an inner context that succeeds inside an outer one that fails
leaves `Y = 0`; an inner one that fails inside an outer one that succeeds leaves `X = 1`, the outer
context's own write).

When a context ends leniently and is still undecided, it has not committed, and it is aborted before
its parent if the parent fails (§4 step 1). Only one path of undecided contexts can have an active
transaction at a time: a new transaction does not start until the effect token says every earlier
one has resolved (`unification.md` §8.4).

**Effects become visible at the end of the entry.** Deferred native effects do not run when a child
context commits, only when the root does. In a probe that prints before, inside a failed condition,
inside a succeeding condition and after, the output is `before`, `inside a condition that succeeds`,
`after`, in that order, and the failed condition's line never appears (`DeferredEffects`).

## 8. Worked examples

Op sequences below are illustrative: register names are invented, housekeeping `Reset`s and the
`ResetNonTrailed`s the compiler's post-pass leaves are omitted, and ops not relevant to failure are
elided. The rows give the ops in execution order.

### 8.1 A condition writes a `var`, then fails

`var X:int = 0` then `if (set X = 1, X > 5) {}` (`VarOnFailureAndSuccess`). The condition writes, so
it gets a full context. Inside a full context the comparison is the ordinary `Gt`.

| # | Op | Effect | Undo log of F |
| --- | --- | --- | --- |
| 1 | `BeginFailureContext(OnFailure: L_end)` | opens F, records frame, `L_end`, token; starts F's transaction | empty |
| 2 | `RefCallDomain(r3 ← rX, 1)` | the `var` has no domain function: `r3` = 1 | empty |
| 3 | `Melt(r4 ← r3)` | 1 | empty |
| 4 | `RefSet(rX, r4)` | the `var` now holds 1 | `var X` held 0 |
| 5 | `RefGet(r5 ← rX)`, `Freeze(r6 ← r5)` | `r6` = 1 | unchanged |
| 6 | `Gt(r7 ← r6, 5)` | 1 > 5 is false: **fails** | — |
| — | failure of F | the log is replayed: `var X` holds 0. Control goes to `L_end` in the recorded frame; the token is restored; F's parent is current | discarded |
| 7 | `EndFailureContext(Done: L_end)` | never executed | |
| L_end | … | `X` reads 0 | |

With `X > 0` instead, op 6 succeeds, op 7 commits F into its parent (the parent's log now holds
"`var X` held 0"), and execution continues into the (empty) `then` branch: `X` reads 1.

### 8.2 A condition with no writes

`X := 5` then `if (X > 0) { … }`. No write and no `<decides>` call: a fast context.

| # | Op | Effect |
| --- | --- | --- |
| 1 | `GtFastFail(r2, r3 ← rX, 0, OnFailure: L_else)` | `r3` is the leniency indicator, fresh. 5 > 0: `r2` = 5, continue |
| 2 | `EndFastFailureContext(r4, r3, OnDone: L_end)` | `r3` still fresh: continue into the `then` branch |
| … | then-branch, `Jump(L_end)` | |
| L_else | else-branch | reached directly by op 1's jump if the test had failed; nothing to undo |

### 8.3 Nested contexts and a failure across a call

`if (set Y = 1, Inner := option{set Y = 2}, Y > 5) {}` (`Nesting`), and `if (Bump[-1]) {}` where
`Bump` does `set Counter += 1` then `N > 0` (`FailureAcrossCall`).

| Step | Context | Log |
| --- | --- | --- |
| `set Y = 1` | F (the `if`) | F: `Y` held 0 |
| `option{` opens G | G, child of F | |
| `set Y = 2` | G | G: `Y` held 1 |
| `}` G's `EndFailureContext` | G commits into F | F: `Y` held 0; `Y` held 1 (the older value, 0, is what counts) |
| `Y > 5` fails | F fails | replay: `Y` = 0 |

For `Bump[-1]`: the `if` opens F; `Call` enters `Bump`'s frame, still in F; `RefSet` on `Counter`
records "`Counter` held 0" in F; `Gt(-1, 0)` fails; F fails: `Counter` is restored, `Bump`'s frame is
abandoned without returning, and control continues at the `if`'s `OnFailure` in the caller's frame.
Measured: `Counter` is 0 after the failed call and 1 after `Bump[1]`.

### 8.4 A `for` loop ends by failing

A collecting `for (I := 0..2) { I * I }` compiles to a loop whose **normal exit is a failure**:

| Phase | What runs |
| --- | --- |
| before | `NewMutableArray` for the result, outside every loop context |
| each iteration | a full context L is (re-)entered with `BeginFailureContext(OnFailure: L_exit)`, reusing the `Id` of the first one; inside it the next index is computed (in a nested context of its own) and tested with `Lte` against the bound; L ends with `EndFailureContext`; the body (`Mul`) and `ArrayAdd(bTransactional: false)` run **after** L, outside it |
| exit | the `Lte` that finds the index past the bound fails L; L's log holds nothing the program reads afterwards, and the result array was never written inside L; control goes to `L_exit` |
| after | `InPlaceMakeImmutable` turns the result into an immutable array (recorded, §6.1) and `MoveTrailed` stores it in the `for`'s result register (recorded, §6.2) |

A filter behaves the same way: `for (E : A, E <> 20) { E }` fails the filter's context for the
element 20 and goes on to the next element (`FailureForms`: 2 of 3 kept).

### 8.5 A runtime error

`set Survivor = 1`, `Print(…)`, `set Survivor = 2`, `Err("probe runtime error")`, in one entry
(`RuntimeErrorWrites`).

| Step | State |
| --- | --- |
| the two `set`s | the root's log: `Survivor` held 0 |
| `Print` | queued in the root's deferred-effect queue |
| `Err` | a runtime error (§9): every transaction of the entry aborts; `Survivor` is restored to 0; the queued `Print` is discarded; the host receives the error; the entry answers `VH_ERR_RUNTIME` (status 6) |
| next entry (`RuntimeErrorAfter`) | reads `Survivor = 0`; nothing was printed by the failed entry |

## 9. Runtime errors

### 9.1 What raises one

| Condition | Diagnostic name | Message text | Reachable from a Godot script |
| --- | --- | --- | --- |
| the native `Err(Message)` is called (`/Verse.org/Verse`, `Err<native>(Message:string)<computes><predicts>:false`) | `ErrorRequested` | `User Message: '<Message>'` — the message verbatim between single quotes, also when empty | yes (`RuntimeErrorWrites`, `RuntimeErrorEmptyMessage`) |
| a single VM entry has been running too long (§9.6) | `ErrRuntime_ComputationLimitExceeded` | the diagnostic's own description (below) | yes |
| a `NewRef` runs while a module's top-level data initializes (a module-scoped `var`) | `ErrRuntime_UnimplementedGlobalVariable` | `Can't create a var at module scope.` | no: a module-scoped `var` that is not a `weak_map` is compile error 3502 (`vm_failure_reject.verse`) |
| an `InitializeVar` with `bCheckIfVariableAllocationIsAllowed` true runs while a module's top-level data initializes | `ErrRuntime_UnimplementedGlobalVariable` | `Can't allocate mutable var field <Name> while initializing module.` where `<Name>` is the op's `Name` string | no: constructing an object with a `var` field in module data is compile error 3512 (`vm_failure_reject.verse`) |
| the `Err` op executes | `ErrRuntime_Internal` | the diagnostic's own description | no: never emitted |
| calling the missing-procedure function | `ErrRuntime_InvalidFunctionCall` | `calls.md` §10.1 | no |
| a native raises | as the native says | `natives.md`, `godot-natives.md` §14; e.g. printing an integer beyond 64 bits (`facts.md` §4) | yes |

Diagnostics of Epic's VM that concern engine objects, native references, persistence or AutoRTFM
itself (`ErrRuntime_InvalidRef`, `ErrRuntime_MemoryLimitExceeded` for engine-object allocation,
`ErrRuntime_WeakMapInvalidKey`, `ErrRuntime_TransactionAbortedByLanguage`) have no counterpart in
this VM.

The full diagnostic table this project's hosts render from (name, then description):

| Name | Description |
| --- | --- |
| `ErrRuntime_Internal` | `An internal runtime error occurred. There is no other information available.` |
| `ErrRuntime_NativeInternal` | `An internal runtime error occurred in native code that was called from Verse. There is no other information available.` |
| `ErrRuntime_GeneratedNativeInternal` | `An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available.` |
| `ErrRuntime_ComputationLimitExceeded` | `The runtime terminated prematurely because Verse code took too long to execute within a single server tick. Try offloading heavy computation to async contexts.` |
| `ErrRuntime_IntegerOverflow` | `Integer overflow encountered.` |
| `ErrRuntime_IntegerBoundsExceeded` | `A value does not fall inside the representable range of a Verse integer.` |
| `ErrRuntime_InvalidArrayLength` | `Invalid array length.` |
| `ErrRuntime_InvalidStringLength` | `Invalid string length.` |
| `ErrRuntime_InvalidFunctionCall` | `Attempted to call an invalid function.` |
| `ErrRuntime_UnimplementedGlobalVariable` | `Allocating a global var is not yet implemented.` |
| `ErrorRequested` | `A runtime error was explicitly raised from user code.` |

(Natives may use others; `natives.md` lists the ones each native raises.)

### 9.2 What the host receives

The message line is

    <Name>: <Description> (<Message>)

and, when the message text is empty, `<Name>: <Description>` with no parentheses. A diagnostic raised
without its own message text uses its description as the message, so the description appears twice.
Measured: `ErrorRequested: A runtime error was explicitly raised from user code. (User Message: 'probe
runtime error')`, and with `Err("")`, `… (User Message: '')` (`RuntimeErrorEmptyMessage`).

The call stack is a list of frames, innermost first:

| Frame | Path | Function | Line |
| --- | --- | --- | --- |
| a native the error was raised in | `[native]` | the native's decorated name, e.g. `(/Verse.org/Verse:)Err(:[]char)` | 0 |
| each Verse frame, then its callers | the procedure's file (`format.md` §5) | the procedure's name, e.g. `(/user@localhost/vm_failure_probe:)RaiseInside` | the source line of the op the frame is at: for the innermost Verse frame, the op that raised or called the raising native; for each caller, its call op (`format.md` §5 locations) |

Measured (`RuntimeErrorInCondition`): `[native] Err`, then `RaiseInside` at line 123 (the `Err`
call), then `RuntimeErrorInCondition` at line 117 (the `if`'s call). A native frame appears for each
native on the path, the raising one innermost, including a native that called back into Verse
(§9.3); frames continue across task boundaries into the task that started the current one
(`tasks.md`).

The host gets the message and frames through its runtime-error callback (`vh_runtime_error`), and
the entry point answers `VH_ERR_RUNTIME`. In the UE editor host the frame list also carries a bogus
first frame parsed from a header line (§12, question 3); the VM must not reproduce it.

### 9.3 What a runtime error does

1. **Everything since the VM entry is rolled back.** The entry here is the outermost one on the
   native stack: if Verse called a native which called back into Verse, and the inner run raises, the
   outer run's work is rolled back too. Every transaction of the entry aborts as in §7 — innermost
   first, compensations run, every deferred native effect discarded, every undo log replayed — and
   every context is marked failed.
2. **It cannot be caught.** No failure context, `or`, `option{}` or `not` stops it: in
   `RuntimeErrorInCondition` neither the `then` nor the `else` branch runs.
3. The error is reported to the host (§9.2).
4. The host terminates the raising script instance's task scope, cancelling its suspended work
   (`tasks.md`; the UE host does this after the report). Other instances are unaffected, and the next
   entry runs normally (`RuntimeErrorNextEntry`).

A `<decides>` function the host calls directly that raises answers `VH_ERR_RUNTIME`, not
`VH_ERR_FAILED` (`RaiseInside`, status 6).

### 9.4 VM invariant violations

Bytecode the compiler never emits — an unsupported operand kind for an op, an *undecidable*
unification outside `MoveNonComparable`, a `CallWithSelf` on a function that has a receiver, an
unbound operand where the op cannot wait, the other rows of `calls.md` §10 — makes Epic's VM abort the
process. This VM must not: it treats every invariant violation as a runtime error, rolled back and
reported as §9.3 says, with

| | |
| --- | --- |
| diagnostic | `ErrRuntime_Internal` |
| message text | `VM invariant violated: <what> at <OpName> in <procedure name>, <file>:<line>` |

This is **this project's decision**, not Epic's behaviour; a differential fixture cannot produce one.
The lead may change the wording.

### 9.5 The stage-1 park error

Design §7.1 stage 1 makes a runtime park fatal. It is reported the same way as §9.4, with message
text `Stage-1 interpreter cannot wait: <OpName> in <procedure name>, <file>:<line> needs a value that
is not yet known`, and it increments a park counter the host can read. Also this project's decision.

### 9.6 The computation watchdog

Epic's VM raises `ErrRuntime_ComputationLimitExceeded` when one top-level VM entry has run for longer
than a threshold: 9 seconds by default, checked every 3 seconds, so the error arrives between about 9
and 12 seconds in. The check is suspended while a debugger is attached. The error is raised at the
next op boundary after the check fires, so it is rolled back and reported like any other. This VM
should check elapsed time at op boundaries (an op count between clock reads is fine) against the same
9-second threshold, measured per top-level entry, and not while the debugger holds the VM stopped.
The exact moment is not something a differential test can pin down.

## 10. Effect specifiers and compiled code

No op carries an effect specifier, and the VM never checks effects at run time: a `<transacts>`
function and a `<computes>` function made of the same ops behave identically. Effects matter only
through what the compiler emits:

| In source | Changes in emitted code |
| --- | --- |
| a condition (of `if`, `or`'s left side, `not`, `option{}`, a `for`'s generators and filters) that contains a call whose callee's effects include `decides`, `writes` or `suspends` — except the operators the compiler inlines as fast-fail ops (`=`, `<>`, `<`, `<=`, `>`, `>=` on ints and floats, `?` on logic and options, array indexing, a fallible type cast, a `for` over a range or an array) | the region gets a **full** context (`BeginFailureContext` … `EndFailureContext`) instead of a fast one |
| a condition that contains `set`, an assignment operator, a `var` creation, `defer`, or constructs a class whose constructor's effects include `decides`, `writes` or `suspends` | a full context |
| any other condition | a fast context: fast-fail ops and `EndFastFailureContext` |
| `=` inside a full context | two `Move`s into one register (`unification.md` §3.3); in a fast context, `EqFastFail` |
| a call to a `<suspends>` function | the `Call` has `bCalleeYields` true (`calls.md` §4.4) |
| the body of a `<suspends>` function | its returns are `ReturnTrailed` (`calls.md` §5.5) |
| a collecting `for` whose body is `<suspends>` | its `ArrayAdd` has `bTransactional` true (§6.1) |
| `set A += B` on a `var` array, result unused, `B` without `writes` | the `CanFastAppendToArrayFastFail` / `FastAppendToArray` fast path, with `RefSet` as the slow path |
| a `<native>` function | a wrapper procedure that `CallWithSelf`s the native procedure (`calls.md` §4.2) |

`<transacts>` matters only because it includes `writes`; `<reads>`, `<computes>`, `<converges>`,
`<allocates>`, `<predicts>` and `<no_rollback>` change no emitted op. `<decides>` on a definition
changes nothing in the function's own body, which runs in its caller's context; it changes how call
sites are compiled. `<no_rollback>` code cannot appear inside a failure context at all, because the
compiler refuses it there; the VM does not enforce it.

## 11. Native side effects

A native's effects inside the VM — reading and writing Verse values it is given — need nothing
special: if it writes a VM slot, that write is recorded in the current transaction's undo log like any
op's (§6). The question is effects on the world outside the VM, which no undo log can reach. A native
has three options, and must use one:

| Pattern | When | Protocol |
| --- | --- | --- |
| **defer** | the effect returns nothing the caller needs now (a Godot property write, `Print`) | queue the effect on the current transaction's deferred-effect queue. It runs when the VM entry's root transaction commits, in queue order (§7), after the undo logs are discarded and before control returns to the host. If any enclosing transaction aborts first, it is discarded unrun |
| **compensate** | the effect must happen now because the native returns something that depends on it (connecting a signal, minting a Godot object) | perform it immediately and register a compensation on the current transaction's compensation list. On abort of that transaction — or of an ancestor after a commit merged the list upward — the compensation runs, after the undo log is replayed, most recent first. On root commit it is discarded |
| **immediate** | the effect is meant to be visible at once and accepted as not undoable | perform it immediately. `godot-natives.md` §7 names the only two: signal emission and container writes |

Consequences a reimplementation must preserve:

- A Godot read made after a deferred Godot write in the same VM entry sees the value from before the
  write (`godot-natives.md` §7).
- A runtime error anywhere in an entry discards every deferred effect of that entry, including lines
  printed before the error (`facts.md`, "a raise erases the whole call's `Print`s"; §8.5 here).
- A `<decides>` entry that declines discards its deferred effects too (§4).
- Deferred effects from separate VM entries run at the end of their own entries, so across entries
  they are in entry order.

Which natives use which pattern is `godot-natives.md` §7 and §8.

## 12. Open questions

1. **Lenient trail merge.** §6.4: Epic's VM merges trail records into the parent before a lenient
   context is decided. Only relevant to stage 2; `unification.md` §12 question 3.
2. **The runtime error's frames across a native callback.** §9.3 says a raise inside a nested entry
   rolls back the outer entry. The frames the host then receives should run through the native frame
   into the outer entry's Verse frames; the UE host renders them that way (source, unprobed). A
   fixture needs a native that calls back into Verse synchronously (a Godot signal handler emitted
   from Verse), which only the integration layer can drive.
3. **The UE editor host's bogus frame.** The UE host binds a text formatter that puts the call stack
   after the message; Solaris binds its own afterwards, which adds a blank line and a
   `Callstack follows:` header. The host's splitter then turns that header into a frame with path
   `Callstack` and function `follows:` (every runtime error in `vm_failure_probe`'s transcript starts
   its frames with it). In a cooked runtime host, Solaris's formatter returns the message alone, so an
   exported game's runtime errors may carry **no** frames at all (source, unprobed). Both are defects
   in this repository's host, not VM behaviour; the differential harness should compare the message
   line and treat frames as advisory until they are fixed.
4. **The watchdog's clock.** §9.6 follows Epic's thresholds. Whether a web build, where a long
   computation also freezes the page, wants a shorter limit is a design decision.
5. **Task and batch state rows of §6.1** are stated from source and delegated to `tasks.md`; no
   fixture here writes task state inside a failing context (a `spawn` inside a condition was not
   tried).

### 12.1 The lead's answers

- **Q2 is implemented as written** (T3.5): a raise in a nested entry rolls back the outer entry,
  its frames run inner, then `[native]`, then outer, and it is reported to the host once, by the
  outermost entry.
- **Q3 is fixed** in the UE host (T5.7): the provider is bound after Solaris starts, so frames are
  real in the editor host and present in a cooked one.
- **A nested entry that declines** answers `VH_ERR_FAILED` to the native that entered it and undoes
  only its own writes; the outer entry continues. The native decides what the decline means.
- **Within a runtime error's rollback**, each transaction replays its undo log and then runs its
  compensations, innermost transaction first, so a compensation in an inner context still sees the
  outer context's writes in place (§7, §9.3).
