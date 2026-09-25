# Tasks, structured concurrency and cancellation

Status: reviewed by the lead 2026-09-24; `ops.md` §15.1 overrides this file where they disagree. Room: dirty. Sources read: VerseVM's task, task-group,
interpreter, bytecode-emitter, bytecode-analysis, ref, enter-VM and runtime-error sources under
`Engine/Source/Runtime/CoreUObject/{Public,Private}/VerseVM`; the code generator under
`Engine/Plugins/VerseVM/Source/VerseVMCodeGen`; the native-coroutine header (`VVMCoroutine.h`);
`Engine/Plugins/Verse/Verse/Source/Verse/{Private/VerseEvent.cpp,Public/VerseEvent.h,Verse/Verse/Event.native.verse}`;
`Engine/Plugins/Solaris/Source/VerseNative/{Private/VerseContentScope.cpp,Verse/Concurrency/Task.native.verse}`;
this repo's `host/Private/{GodotBindings.cpp,HostEventLoop.cpp,HostScript.cpp,HostRuntime.cpp}`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/spec/calls.md`.
Probes: `tests/verse_probe/vm_tasks_probe.verse`, `vm_tasks_probe2.verse`, `vm_tasks_probe3.verse`,
`vm_tasks_subscribe_probe.verse`, `vm_tasks_reject.verse`, and the existing `sleep_probe.verse`.

This file says what a task is, what the twelve task ops (and `ResumeUnwind`) do to tasks, how
`spawn`, `branch`, `sync`, `race` and `rush` are built from them, and what cancellation, `defer`,
termination, `task(t)` and `event(t)` do — all as observable state and ordering. The interpreter is
single-threaded and a Verse-to-Verse call never recurses on the native stack (`calls.md` §9), so
"concurrency" here is only *interleaving*: exactly one task executes at any moment, and every rule
below is a rule about which one executes next.

Values, unification and the effect token are `values.md` and `unification.md`; failure contexts and
the undo log are `failure.md`; frames, calls and closures are `calls.md`; individual natives —
including the decorated names of `task(t)`'s and `event(t)`'s members, and `Sleep` — are
`natives.md`; op operands are `ops.md`. Each is referred to, not restated.

**How to read the probe citations.** `verse_probe` compiles a fixture, then calls each
zero-argument, non-suspending method of the named class in declaration order, running one host
tick (`vh_tick`) between calls. A citation such as `vm_tasks_probe` `B3_*` names the steps of one
group; the transcript it produced is reproduced in the table next to the claim. Every table in this
file was observed, not predicted. `Print` output is delivered when the transaction that printed
commits (`godot-natives.md`), which preserves the order of prints within a committed run.

## 1. Vocabulary

| Term | Meaning |
| --- | --- |
| **task** | a heap cell (a `task(t)` value to Verse) holding one thread of Verse execution: where it is suspended, whom it returns control to, its relationships and its outcome (§2) |
| **current task** | the task whose ops are executing. There is always exactly one while the interpreter runs |
| **running** | a task is *running* while it is on the **chain**: it is the current task, or it started or resumed a task that is (directly or through further tasks) the current one, and has not been given control back yet. A running task can be neither resumed nor unwound; it can only get control back by the tasks above it suspending or finishing (§4) |
| **suspended** | not running. A suspended task has a resume point and waits for something to resume it |
| **resume point** | (op index, frame) where a suspended task continues, plus a **resume slot**: the register (if any) a resume value is unified into |
| **yield-to point** | (task, frame, op index) that receives control when this task next suspends or finishes. Set when the task is started or resumed; cleared when it is used. An empty yield-to point means "return to whoever called into the interpreter" (§4.4) |
| **suspension point** | an op at which the current task may stop running: `WaitSemaphore` when it blocks, a call whose callee answers *yield* (`calls.md` §4.3), `Yield`, and `EndTask` when it must wait (§5.4). These are also the only places a pending cancellation takes effect (§7.2) |
| **root task** | a task with no parent. Only root tasks belong to a task group |
| **task group** | the set of root tasks started while a given content scope was active (§8.1) |
| **native defer hooks** | callbacks a native attaches to a task, run when the task is next resumed *or* when it starts to unwind *or* when it is terminated (§2, §11) |
| **on-finish hooks** | callbacks a native attaches to a task, run once when the task finishes, whether it completes, is cancelled or is terminated (§2) |

## 2. The state of a task

| Field | Contents | Initially |
| --- | --- | --- |
| phase | one of **Active**, **CancelRequested**, **CancelStarted**, **CancelUnwind**, **Canceled**, ordered in that sequence. It only ever moves forward | Active |
| running | boolean (§1) | true — a task is created running |
| parent | a task or none | from the creating op's `Parent` operand |
| children | an ordered list, oldest first; the *newest child* is the last. A task joins its parent's list when created and leaves it when it finishes (§5.4 step 6) | empty |
| result | a value, or none. A task is **completed** exactly when it has one; only normal completion sets it (§5.4). A cancelled task never has one | none |
| awaiters | ordered list of tasks suspended in this task's `Await` (§9), oldest first | empty |
| cancelers | ordered list of tasks suspended in this task's `Cancel` or waiting for this child to finish cancelling (§7), oldest first | empty |
| resume point | op index, frame, resume slot (§1) | unset |
| yield-to point | task, frame, op index (§1) | from the creating op |
| root frame | the first frame of the task: for `BeginTask` the frame it shares with its creator, for `CallTask` the new frame | — |
| native defer hooks | a stack, newest on top; run newest first; each runs once and is removed | empty |
| on-finish hooks | a queue, run oldest first; each runs once and is removed | empty |
| group | the task group it belongs to, or none (root tasks only) | §5.2, §5.3 |
| await state | for `await` only (§5.7): an *initializing* flag and an *await point* op index | clear |
| membership links | a task suspended in some other task's `Await` or `Cancel` is on exactly one awaiters or cancelers list at a time | — |

Every field write the interpreter makes while a transaction is open goes into the undo log
(design §7.2), with two exceptions called out where they occur: joining and leaving a task group,
and termination (§8.2), which are not undone.

## 3. Phases, and what Verse can ask

| Phase | Meaning | Reached by |
| --- | --- | --- |
| Active | running or suspended normally | creation |
| CancelRequested | someone asked to cancel it while it was running; it keeps executing until its next suspension point | §7.1 |
| CancelStarted | it has stopped, and is cancelling its children, newest first | §7.2 |
| CancelUnwind | its children are gone and it is executing `defer` bodies on its way to `EndTask` | §7.3 |
| Canceled | finished by cancellation (or termination, §8.2) | `EndTask` §5.4, termination |

The phase queries on `task(t)` (`natives.md` has their decorated names; each is
`<transacts><decides>` and succeeds with no value or fails):

| Query | Succeeds when |
| --- | --- |
| `Active[]` | phase is before CancelStarted **and** the task has no result |
| `Completed[]` | the task has a result |
| `Canceling[]` | phase is CancelStarted or CancelUnwind |
| `Canceled[]` | phase is Canceled |
| `Unsettled[]` | phase is before Canceled and the task has no result |
| `Settled[]` | phase is Canceled or the task has a result |
| `Uninterrupted[]` | phase is Active (a completed task included) |
| `Interrupted[]` | phase is not Active |

Two consequences: a task whose cancellation was requested while it was running answers both
`Active[]` and `Interrupted[]` until it reaches a suspension point; and a cancelled task answers
`Canceled[]` and `Settled[]` but never `Completed[]` (`vm_tasks_probe` `F2_Cancel`,
`vm_tasks_probe3` `B3_Phase`).

## 4. How control moves

### 4.1 The chain

Control moves between tasks only at the four places below. There is no scheduler queue in the VM
itself: whoever makes a task runnable runs it, synchronously, until it stops, and then takes control
back. Deferred work (a `Sleep` that is due, a Godot signal) re-enters from the embedder (§11).

| Event | Control goes to | The yield-to point of the task that now runs |
| --- | --- | --- |
| **start** (`BeginTask`, `CallTask`) | the new task, at its first op | the creator, at the op after the body (`BeginTask`'s `OnYield`) or after the `CallTask` |
| **suspend** (§4.2) | the suspending task's yield-to point, which is then cleared | unchanged |
| **finish** (`EndTask`, §5.4) | the tasks it resumes, then its yield-to point | set by §5.4 step 8 |
| **resume by a native** (§11) | the resumed task, at its resume point | empty: when it next stops, control returns to the native |

### 4.2 Suspending

When an op suspends the current task T (§1's suspension points):

1. T stops running. Its resume slot becomes the op's destination register if the op has one (the
   call's `Dest`; `WaitSemaphore`, `Yield` and `EndTask` have none).
2. If T's phase is CancelRequested, T's cancellation begins here (§7.2): phase becomes
   CancelStarted and T tries to cancel all its children. If that succeeds, T becomes running again and
   begins unwinding (§7.3) from the position it was suspending at, instead of suspending. If a child
   could not be cancelled yet, T stays suspended; its last child will resume it (§5.4 step 3d).
3. T's resume point becomes (the op to continue at, the current frame). The op to continue at is the
   op after the suspending op, except for `Yield` (its `ResumeOffset`) and a waiting `EndTask` (the
   `EndTask` itself).
4. Control passes to T's yield-to point, which is cleared. If it was empty, the interpreter returns to
   whatever called it with the outcome *yield* (§4.4).
5. The task U that now has control is running. If U's phase is CancelStarted, U retries cancelling
   its children: on success U begins unwinding (§7.3) from the op it was about to continue at; if a
   child still cannot be cancelled, U suspends in turn (back to step 3 with U). Otherwise U simply
   continues at its yield-to op.

### 4.3 Resuming, and the order of several resumptions

A task is resumed by:

- `EndTask` of a task it awaited or was cancelling, or of its last child during cancellation, or
  through a semaphore (§5.4);
- a native completing a suspended call (§11);
- a write to a variable its `await` read (§5.7).

When one event resumes several tasks, they run **one at a time, each until it suspends or finishes,
in this order**: the finishing task's awaiters or cancelers, oldest first; then its *signaled task*
(the semaphore waiter, or the parent waiting for its last child); then control returns to the
finishing task's own yield-to point. `vm_tasks_probe` `T2_Signal` shows two awaiters in arrival
order before the signaler continues:

| # | Printed | Why |
| --- | --- | --- |
| 1 | `T2 signal` | |
| 2 | `  t body end` | the event resumes the body; it completes |
| 3 | `  t w1 got 6` | first awaiter, run to completion |
| 4 | `  t w2 got 6` | second awaiter |
| 5 | `T2 signal returned` | control back to the body's yield-to point, then out of `Signal` |

A resumption that finds the task in any phase other than Active does nothing (the task is being or
has been cancelled; it cannot leave the point where it is waiting).

### 4.4 Entering from the embedder

Every entry into the VM (`calls.md` §8) runs inside a fresh **entry task**: a root task, not in any
task group, with an empty yield-to point. Non-suspending code simply runs in it and returns. A
`spawn` inside it creates a root task (§6.4) that runs until it suspends and then gives control back
to the entry task at the op after the `spawn`. If the entered function itself suspends — the host
calling a `<suspends>` method directly — the entry returns with the outcome *yield* and no value,
which `vh_instance_call` reports as success with no result (source, unprobed).

## 5. The task ops

`ops.json` gives the operand lists; `format.md` §5.1 the encodings. `Parent` of `BeginTask` and
`CallTask`, and `Which` of `EndTask`, are **absent** in much compiled code (`calls.md` §2.3) and read
as uninitialized.

### 5.1 `SelfTask` (`Dest`)

Unify the current task into `Dest`.

### 5.2 `BeginTask` (`Dest`, `Parent`, `bAddToTaskGroup`, `OnYield`)

Starts an **inline** task: its body is the ops that follow this one, in the *same* frame.

1. Read `Parent`: a task, or absent (none).
2. Create a task: phase Active, running, the given parent (it becomes that parent's newest child),
   root frame = the current frame, yield-to point = (the current task, the current frame, `OnYield`).
3. If `bAddToTaskGroup` is true and the task has no parent, it joins the task group of the active
   content scope (§8.1), if there is one. A task with a parent never joins a group.
4. The new task becomes the current task. `Dest` is unified with it — in the shared frame, so the
   creator sees it too.
5. Execution continues with the next op, as the new task.

Because the frame is shared, the creator and every inline task it starts address the same registers;
the compiler gives each its own. When the new task suspends or finishes, the creator continues at
`OnYield` in that frame.

### 5.3 `CallTask` (`Dest`, `Parent`, `Callee`, `Arguments`…)

Starts a task whose body is a function.

1. Read `Parent` as in §5.2. `Callee` must be a `function` whose procedure is a bytecode procedure
   (a native here is a VM invariant violation, `calls.md` §10).
2. Make the callee's frame as a call would (`calls.md` §5.1) but with **no caller frame and no
   return slot**, binding `Arguments` positionally; there are never named arguments. The callee's
   procedure is a task body and ends in `EndTask` (§6.1), so it never executes `Return`.
3. Create a task: phase Active, running, the given parent, root frame = the new frame, yield-to point
   = (the current task, the current frame, the op after this one). It joins the active task group if
   it has no parent (there is no flag; this always applies).
4. Unify the new task into `Dest` in the *creator's* frame.
5. The new task becomes the current task and execution continues at the callee's op 0.

### 5.4 `EndTask` (`Write`?, `Switch`?, `Value`, `Which`, `Signal`?)

Finishes the current task T. Every task body ends in one, and cancellation always ends in one
(§7.3), so `EndTask` is also the last suspension point a task has. T must be running (otherwise a VM
invariant violation). In order:

1. If T's phase is CancelRequested, it becomes CancelStarted — reaching `EndTask` counts as reaching
   a suspension point, so a task cancelled while running that then simply finishes its body still
   ends **cancelled** (§7.5).
2. **If T's phase is Active** (normal completion):
   1. Cancel T's remaining children, newest first (§7.2). If one cannot be cancelled yet because it
      is running, T joins that child's cancelers list (the newest remaining child's) with a native
      defer hook that takes it off again, and suspends *at this `EndTask`* (resume slot none); when
      resumed it starts again at step 1.
   2. T's result becomes `Value`.
   3. **Write, first wins.** If `Write` is present and the `Write` register holds *uninitialized*
      (`calls.md` §2.2 — the compiler puts that there on purpose; a fresh register does not count),
      store the result into it, and then, if `Switch` is present, store `Which` into the `Switch`
      register (which must also hold uninitialized, else a VM invariant violation). If `Write` already
      holds a value, neither register is touched. Both stores are trailed (`failure.md`).
   4. **Signal.** If `Signal` is present it is a semaphore (§5.6): add 1 to its count. If the count
      is now exactly 0, the semaphore's waiting task becomes T's *signaled task* and the waiting slot
      is cleared.
   5. T's *resume list* is its awaiters list, which is taken and emptied.
3. **Otherwise** (T's phase is CancelStarted or CancelUnwind — T is being cancelled):
   1. Cancel T's remaining children as in 2.1 (children created while unwinding are cancelled here
      synchronously). If one cannot be cancelled yet, T suspends at this `EndTask` *without* joining
      any list; its last child resumes it (3.4 below, from the child's side).
   2. T's phase becomes Canceled. T gets no result; the value handed to its cancelers is `false`.
   3. T's resume list is its cancelers list, taken and emptied. Its awaiters are **left where they
      are and never resumed** (§9).
   4. If T has a parent whose phase is CancelStarted and whose newest child is T, that parent is T's
      signaled task. (A parent cancelling its children is implicitly waiting on the newest one.)
   `Write`, `Switch`, `Value` and `Signal` are ignored on this path.
4. Run T's on-finish hooks, oldest first.
5. T stops running and leaves its task group, if it is in one.
6. T is removed from its parent's children list.
7. T's resume point becomes *finished*: anything that later resumes T does nothing at all. Control
   passes to T's yield-to point, which is cleared.
8. Arrange the resumptions so that they run in §4.3's order: each task on T's resume list whose phase
   is Active, oldest first (one whose phase is not Active is skipped — resuming a cancelling task is a
   no-op); then T's signaled task, if there is one and it is not running. Each resumed task gets as
   its yield-to point the one that runs after it, the last getting T's original yield-to point. As
   each awaiter or canceler is arranged, its native defer hooks run and the resume value (T's result,
   or `false` for cancelers) is unified into its resume slot; this happens for all of them, newest
   first, before any of them runs. A failed unification here is a VM invariant violation.
9. The first task of the arrangement (or, if there is none, the task at T's yield-to point) gets
   control. If it is running with phase CancelStarted, §4.2 step 5 applies to it. If there is no task
   at all (T had an empty yield-to point and resumed nothing), the interpreter returns *yield* to its
   caller.

### 5.5 `Yield` (`ResumeOffset`)

Suspend the current task (§4.2) with resume point `ResumeOffset` and no resume slot. Nothing in the
task machinery resumes a task parked by `Yield`; only something that knows about it does — `await`'s
variable writes (§5.7) — or cancellation. The compiler emits it for `await`, `when`, `upon` and live
variables, none of which a Godot script normally writes (§5.7).

### 5.6 `NewSemaphore` (`Dest`) and `WaitSemaphore` (`Source`, `Count`)

A semaphore is a cell with an integer count, starting at 0, and one slot for a waiting task.
`NewSemaphore` unifies a new one into `Dest`.

`WaitSemaphore` subtracts the constant `Count` from `Source`'s count. If the result is negative, the
current task is recorded in the semaphore's waiting slot (which must be empty, else a VM invariant
violation) and suspends (§4.2); otherwise execution continues. The only thing that adds to a count is
`EndTask`'s `Signal` (§5.4 step 2.4), one at a time; the task that brings the count back to exactly
0 wakes the waiter. So `WaitSemaphore S, n` after *k* signals have already arrived waits for the
remaining *n − k*, and does not suspend at all if *k ≥ n*.

A semaphore's waiter that is cancelled is not removed from the slot. If a later signal brings the
count to 0, the "wake" resumes a task whose resume point is finished, which does nothing (§5.4
step 7). The compiled forms never let this happen, but an interpreter must not fault on it.

### 5.7 `BeginAwait`, `AwaitSuccess`, `EndAwait`; `BeginBatch`, `EndBatch`

These implement `await{Condition}` and `batch{…}`. They are rare in Godot scripts — `await` is an
unfinished Verse feature — but this project's script package may use them: both compile
(`vm_tasks_reject.verse` items 7 and 8 produce no glitch) and both run (`vm_tasks_probe2` `F*`). All
five also take part in the effect-token protocol (`ops.json` marks them as capturing the return
effect token; `unification.md` owns that).

`await{C}` compiles to: `BeginAwait`; then, at a labelled *await point* that is also an unwind
point, `C` inside a failure context followed by `AwaitSuccess`; on success, continue past the
construct; on failure, `EndAwait` and `Yield` back to the await point.

| Op | Effect on the current task |
| --- | --- |
| `BeginAwait` | sets *initializing*, and records the op after it as the task's await point (both must have been clear — else a VM invariant violation) |
| `AwaitSuccess` | if *initializing* is set, **fails** (the current failure context's failure path is taken); otherwise clears the await point and continues |
| `EndAwait` | clears *initializing* and the await point (the await point must have been set) |

While a task has an await point, every read of a mutable variable (a `var` reference) registers the
task with that variable. A write to a variable resumes, synchronously and before the writing op
completes, each registered task that is still suspended exactly at its await point, with the resume
point reset to the await point and the await point re-armed; the order among several such tasks is
not specified here (source, unprobed). A registration lapses once the task is anywhere else.

Observable consequences (`vm_tasks_probe2` `F1`–`F7`):

| Step | Printed |
| --- | --- |
| `F1` spawn `await{Counter > 2}` with Counter 0 | `  aw start, Counter = 0` — then it waits |
| `F2` `set Counter = 1` | nothing from the awaiter (re-evaluated, still false, waits again) |
| `F3` `set Counter = 3` | `  aw resumed, Counter = 3` printed **before** `F3 end`: the write resumes it synchronously |
| `F6` spawn `await{Counter > 0}` with Counter 12 | `  aw3 start, Counter = 12` — it waits although the condition already holds, because the first evaluation always fails (`AwaitSuccess` while initializing) |
| `F7` `set Counter = 12` | `  aw3 resumed, Counter = 12`: a write of the same value still resumes it |

`BeginBatch` requires a concrete effect token (`unification.md`) and opens a batch; batches nest.
While any batch is open, writes that would resume awaiting tasks are recorded instead. `EndBatch`
(also requiring a concrete effect token) closes one level; when the outermost closes, every task
registered with any variable written during the batch is resumed once (a task registered with
several of them is resumed once), and the record is cleared. A runtime error raised by such a
resumption makes `EndBatch` raise. `vm_tasks_probe2` `F5`: inside `batch`, `set Counter = 11`, a
print, `set Counter = 12` print `  f5 inside batch after set` and then `  aw2 resumed, Counter = 12`
— one resumption, after the batch.

### 5.8 `ResumeUnwind`

Continue unwinding the current task (§7.3) from the op after this one: the landing search of §7.3
starts again at this op's position in the current frame. The task's phase is already CancelUnwind;
its native defer hooks, already run, are normally empty and are run again if not. It is emitted only
at the end of a `defer` body's cancellation path (§7.4).

## 6. How the structured forms compile

Every form below is built from two shapes. The tables list the ops that matter to tasks, in emission
order; ordinary ops (moves, jumps, the arm's own code) are elided.

### 6.1 The task body

Whatever runs as a task — an arm, a `branch` body, a `spawn` body, a whole `CallTask` procedure — is
compiled as:

| Op | Note |
| --- | --- |
| *(unwind region opens, landing = the `EndTask` below)* | covers the whole body and the position of the `EndTask` itself |
| body | may contain `defer`s, which open nested regions (§7.4) |
| `EndTask` `Write`, `Switch`, `Value` = the body's value (`false` for a void body), `Which`, `Signal` | the operands are the construct's (tables below) |

So a task that is cancelled anywhere in its body, after running its `defer`s, arrives at its own
`EndTask` (§7.3).

### 6.2 The escape scope

`sync`, `race` and `rush` wrap their arms in an *escape scope*, which is what lets an arm `return`
out of the enclosing function (§6.8):

| Op | Note |
| --- | --- |
| `NewSemaphore` E | E is signalled once when the whole construct is done |
| `Move` T ← absent, `Move` K ← absent | T receives the construct's value, K which way to continue; both now hold *uninitialized*, so the first `EndTask` that writes them wins |
| the construct's body (below) | |
| `WaitSemaphore` E, 1 | the enclosing task waits here for the construct |
| `Switch` K → [continue, return] | only if some arm contains a `return` |
| on *return*: the function's return path with the value in T (running any `defer`s on the way) | |
| on *continue*: the construct's value is T (for `race`, `rush`) | |

`Which` constants are 0 for *continue* and 1 for *return*.

### 6.3 Per construct

| Construct | Ops, in order | Each arm's `EndTask` | The wrapper's `EndTask` |
| --- | --- | --- | --- |
| `spawn{B}` | `BeginTask` (`Parent` absent, `bAddToTaskGroup` true) + body B, **or** `CallTask` (`Parent` absent) of a closure whose procedure is the task body | `Write`, `Switch`, `Which`, `Signal` all absent | — |
| `branch{B}` | `SelfTask` P; `BeginTask` (`Parent` P, `bAddToTaskGroup` true) + body, or `CallTask` (`Parent` P) | all absent | — |
| `race{A1 … An}` | escape scope; `SelfTask` P0; `BeginTask` W (parent P0) { `SelfTask` P1; `NewSemaphore` S; for each arm: `JumpIfInitialized` T → *wait*, `BeginTask` (parent P1) + arm }; *wait*: `WaitSemaphore` S, 1; W's `EndTask` | `Write` T, `Switch` K, `Which` 0 (or 1 on `return`), `Signal` S | W: `Write` absent, `Signal` E |
| `sync{A1 … An}` | escape scope; `SelfTask` P0; `BeginTask` W (parent P0, `Write` T …) { `SelfTask` P1; `NewSemaphore` S; `NewSemaphore` R; for each arm i: `JumpIfInitialized` T → *wait*, `Move` Ri ← absent, `BeginTask` (parent P1) + arm }; *wait*: `WaitSemaphore` R, n — or, if an arm can `return`, `BeginTask` (parent P1) { `WaitSemaphore` R, n; `EndTask` `Signal` S } then `WaitSemaphore` S, 1; W's `EndTask`; after the scope, `NewArray` of R1…Rn | `Write` Ri, `Signal` R (on `return`: `Write` T, `Switch` K, `Which` 1, `Signal` S) | W: `Write` T, `Switch` K, `Which` 0, `Signal` E |
| `rush{A1 … An}` | escape scope; `SelfTask` P; for each arm: `BeginTask` (parent P) + arm — **no** `JumpIfInitialized`, **no** wrapper task | `Write` T, `Switch` K, `Which` 0 (or 1), `Signal` E | — |

What the shapes imply, each confirmed by a probe:

| Consequence | Evidence |
| --- | --- |
| Every arm and body starts synchronously, in source order, and runs until it first suspends before the next starts or the creator continues | all tables below |
| `race`: an arm that finishes without suspending ends the race before later arms **start** (they never run at all) | `B1` |
| `race`: when an arm finishes, the wrapper W finishes and cancels the remaining arms, newest first, before the code after the race runs | `B2`, `B3` |
| `race`: two arms woken by the same event produce one winner; the second is cancelled before it runs (it was taken off the event's list by its cancellation) | `vm_tasks_probe3` `C2` |
| `sync`: every arm starts, even after one finishes immediately; its value is the array of arm values in source order | `C1`, `C2` |
| `rush`: every arm starts even when the first finishes immediately (no `JumpIfInitialized`); the first to finish gives the value and the enclosing task continues; the others are **children of the enclosing task** and keep running — until that task itself finishes, whose `EndTask` cancels them | `D*`, `D5` |
| `branch`: the body is a child of the enclosing task; it outlives nothing — the enclosing task's `EndTask` cancels it | `E*`, `E4`/`E5` |
| `spawn`: a root task, in the active task group, cancelled by nothing structural; a `spawn` inside a race arm survives the arm | `vm_tasks_probe2` `C*` |

### 6.4 `spawn` and `branch` (`vm_tasks_probe` `A*`, `E*`)

| # | Printed | Why |
| --- | --- | --- |
| 1 | `A1 before spawn` | |
| 2 | `  A body start` | the spawned body runs at once… |
| 3 | `A1 after spawn` | …until it suspends in `Await`; control returns to the op after the spawn |
| 4 | `A2 before spawn` | (next step) |
| 5 | `  A2 body runs to end` | a body that never suspends finishes inside the `spawn` |
| 6 | `A2 after spawn` | |
| 7 | `A3 before signal` | (next step) |
| 8 | `  A body resumed` | `Signal` resumes it synchronously |
| 9 | `A3 after signal` | |

`branch`, driver `E begin` / branch start / `E after branch` / await:

| Step | Printed |
| --- | --- |
| `E1_Start` | `E1 start`, `  E begin`, `  e-branch start`, `  E after branch`, `E1 after spawn` |
| `E2_SignalBranch` | `E2 signal branch`, `  e-branch end`, `  e-branch defer`, `E2 signal returned` |
| `E3_SignalDriver` | `E3 signal driver`, `  E driver end`, `E3 signal returned` |
| `E4_BranchOutlived` | `E4 start`, `  ec-branch start`, `  EC after branch`, `E4 after spawn` |
| `E5_EndDriverFirst` | `E5 signal driver`, `  EC driver end`, `  ec-branch defer` (the driver's `EndTask` cancels the branch), `E5 signal returned` |

### 6.5 `race` (`vm_tasks_probe` `B*`)

`Arm(Name, Ev)` prints `start`, awaits `Ev`, prints `end`; `Imm(Name, V)` prints `runs to end`;
both print from a `defer`.

| Step | Printed, in order |
| --- | --- |
| `B1` race{Imm arm1; Arm arm2} | `B1 start`, `  b1-arm1 runs to end`, `  b1-arm1 defer`, `  B1 race result 10`, `B1 after spawn` — arm2 never starts |
| `B2` race{Arm arm1; Imm arm2} | `B2 start`, `  b2-arm1 start`, `  b2-arm2 runs to end`, `  b2-arm2 defer`, `  b2-arm1 defer`, `  B2 race result 20`, `B2 after spawn` |
| `B3_Start` race of three Arms | `B3 start`, `  b3-arm1 start`, `  b3-arm2 start`, `  b3-arm3 start`, `B3 after spawn` |
| `B3_SignalMiddle` | `B3 signal arm2`, `  b3-arm2 end`, `  b3-arm2 defer`, `  b3-arm3 defer`, `  b3-arm1 defer`, `  B3 race result 32`, `B3 signal returned` |

### 6.6 `race` around `sync` (`vm_tasks_probe` `N*`)

`race{ block{ sync{Arm s1; Arm s2}; … }; block{ Arm r2 } }`:

| Step | Printed, in order |
| --- | --- |
| `N1_Start` | `N1 start`, `  N begin`, `  n-r1 begin`, `  n-s1 start`, `  n-s2 start`, `  n-r2 begin`, `  n-r2 start`, `N1 after spawn` |
| `N2_SignalS1` | `N2 signal s1`, `  n-s1 end`, `  n-s1 defer`, `N2 signal returned` — the sync still waits |
| `N3_SignalS2` | `N3 signal s2`, `  n-s2 end`, `  n-s2 defer`, `  n-r1 sync done 1 2`, `  n-r2 defer`, `  N end 3`, `N3 signal returned` |

### 6.7 `sync` and `rush` (`vm_tasks_probe` `C*`, `D*`)

| Step | Printed, in order |
| --- | --- |
| `C1` sync{Imm 1; Arm c-arm2; Imm 3} | `C1 start`, `  c-arm1 runs to end`, `  c-arm1 defer`, `  c-arm2 start`, `  c-arm3 runs to end`, `  c-arm3 defer`, `C1 after spawn` |
| `C2_Signal` | `C2 signal`, `  c-arm2 end`, `  c-arm2 defer`, `  C result 1 2 3`, `C2 signal returned` |
| `C3` two arms on one event, then `C4` signals it | `C4 signal`, `  c3-arm1 end`, `  c3-arm1 defer`, `  c3-arm2 end`, `  c3-arm2 defer`, `  C3 result 4 4`, `C4 signal returned` |
| `D1` rush{Arm d1; Arm d2}, driver then awaits another event | `D1 start`, `  d-arm1 start`, `  d-arm2 start`, `D1 after spawn` |
| `D2_SignalArm1` | `D2 signal arm1`, `  d-arm1 end`, `  d-arm1 defer`, `  D rush result 41`, `D2 signal returned` |
| `D3_SignalArm2` | `D3 signal arm2`, `  d-arm2 end`, `  d-arm2 defer`, `D3 signal returned` — the loser ran to completion |
| `D4_Release` | `D4 release driver`, `  D driver end`, `D4 signal returned` |
| `D5` rush{Imm 1; Arm 2; Arm 3}, driver ends right after | `D5 start`, `  de-arm1 runs to end`, `  de-arm1 defer`, `  de-arm2 start`, `  de-arm3 start`, `  DE rush result 1`, `  de-arm3 defer`, `  de-arm2 defer`, `D5 after spawn` |

### 6.8 `return` out of an arm

`return` inside an arm finishes the arm through its own `EndTask` with `Which` = 1 (the arm's own
`defer`s run first, on the way), which records the returned value in T and wakes the construct;
the construct then finishes, cancelling what it cancels; the escape scope's `Switch` takes the
return path, running the function's `defer`s; the function returns the value.

| Case | Printed after the winning arm is signalled |
| --- | --- |
| `race` (`vm_tasks_probe` `B4_Signal`) | `  b4-arm1 returns 7 from the function`, `  b4-arm1 defer`, `  b4-arm2 defer`, `  B4 fn defer`, `  B4 fn returned 7`, `B4 signal returned` |
| `sync` (`vm_tasks_probe2` `A2_Signal`) | `  sr-arm1 returns 5`, `  sr-arm1 defer`, `  sr-arm2 defer`, `  sr fn defer`, `  sr returned 5`, `A2 signal returned` — the other arm is cancelled |
| `rush` (`vm_tasks_probe2` `B2_Signal`, `B3_SignalLoser`) | `  rr-arm1 returns 6`, `  rr fn defer`, `  rr returned 6`, `B2 signal returned` — the other arm is **not** cancelled; a later signal prints `  rr-arm2 end`, `  rr-arm2 defer` |

## 7. Cancellation

### 7.1 Who cancels, and asking

A task is asked to cancel by:

- `task(t).Cancel()` from Verse (§9) — `<epic_internal>`, which this project's script package may
  call;
- its parent's `EndTask`, for every child still unfinished (§5.4 steps 2.1 and 3.1) — which is how
  `race`, `sync`, `rush` and `branch` cancel;
- a cancelling parent, cancelling its children (§7.2);
- a native (fire-and-forget: nothing waits for it).

Asking a task X to cancel, from task C (or from a native, C = none):

1. If X is Canceled or completed, nothing happens; `Cancel()` returns at once (`vm_tasks_probe`
   `T4_CancelCompleted`; `Q2_Cancel`'s second canceler).
2. If X's phase is Active, it becomes CancelRequested.
3. If X is **running**, or already CancelStarted (waiting on its own children), X cannot be unwound
   now. C joins X's cancelers list (with a native defer hook that takes it off again) and suspends
   in `Cancel` until X finishes (§7.5).
4. Otherwise X is suspended: X becomes CancelStarted and cancels its own children (§7.2). If that
   succeeds, X is **unwound now, synchronously** (§7.3): its `defer`s run inside the `Cancel` call,
   and `Cancel()` returns after X's `EndTask` (`F2_Cancel` below). If it does not, C waits as in 3.

### 7.2 At the next suspension point: cancelling children

A task that is CancelRequested keeps executing — including starting new children — until it reaches
a suspension point (§4.2 step 2, or `EndTask` step 1). Then it becomes CancelStarted and cancels its
children **newest first**. For each, while the task has a newest child:

- ask the child to cancel (§7.1 steps 2–4, but the parent does not join a cancelers list); while this
  happens the parent is marked running, so nothing the child does can resume it;
- if the child can be unwound now, it is unwound to completion (§7.3) before the next, older child is
  looked at — so one child's entire `defer` output precedes the next child's;
- if a child cannot (it is running, or waiting on its own children), stop: the parent stays suspended
  (or suspends) and the child resumes it, as its signaled task, when it finishes (§5.4 step 3.4).

Grandchildren are cancelled before their parent's own unwinding, newest first at each level
(`vm_tasks_probe` `Q2_Cancel`: a `sync` of three arms inside a spawned task, cancelled):

| # | Printed | Why |
| --- | --- | --- |
| 1 | `  q c1 cancels` | |
| 2 | `  q-kid3 defer` | the sync's arms, newest first… |
| 3 | `  q-kid2 defer` | |
| 4 | `  q-kid1 defer` | |
| 5 | `  q parent defer` | …then the cancelled task's own `defer` |
| 6 | `  q c1 cancel returned` | `Cancel` returns after the target is Canceled |
| 7 | `  q c2 cancels` | a second canceler, same step |
| 8 | `  q c2 cancel returned` | the target is already Canceled: immediate |

### 7.3 Unwinding

When a task T has no children left and is CancelStarted, it unwinds, from the position P at which it
is suspended (for a task being cancelled by someone else: its resume point; for a task that reached a
suspension point itself: the position it was suspending at, as in §4.2 step 3):

1. T becomes running and its phase CancelUnwind.
2. T's native defer hooks run, newest first. (This is what takes a task off an event's awaiters, a
   task's awaiters or cancelers list, and so on — §9, §10.)
3. **Find the landing.** Starting in the frame of P with position p = P's op index, and walking out
   through callers: in a frame whose procedure has an unwind edge covering p, continue at that edge's
   landing op in that frame; otherwise move to the caller frame, with p = the op after the call. The
   walk always ends at a task body's own edge (§6.1); failing to find one is a VM invariant violation.
   Frames passed over are abandoned — nothing returns from them.
4. Execute from the landing. It is always either a `defer` body's cancellation path (§7.4) or the
   task's `EndTask`, which, in phase CancelUnwind, finishes the task as Canceled (§5.4 step 3).

**Which op index an edge covers.** The compiler forms a region from the ops emitted inside it,
indices *a* through *b*. A position p (the op at which execution would continue: the resume point,
or the op after a call in an outer frame, or the op after `ResumeUnwind`) is covered exactly when
**a < p ≤ b + 1** — equivalently, when the op *before* p, at index p − 1, lies in *a…b*. Read
`format.md` §5's "an op is covered when `first <= index <= last`" with index = p − 1. The difference
matters at two places: a task waiting in `EndTask` resumes at the `EndTask` itself, which is the op
just after its body region (p = b + 1, covered); and `Yield` resumes at its `ResumeOffset`, not at
the op after it. Edges are recorded only for regions that contain an op at which a task can suspend,
so a region without one never lands anything.

### 7.4 `defer`

A `defer:` block opens an unwind region from the statement after it to the end of its enclosing
block. It runs:

- on normal exit from that block, at the block's end;
- on `return` or `break` passing out of the block;
- on cancellation passing through the region (§7.3);

each exactly once, and **innermost first**: later `defer`s in a block before earlier ones, an inner
block's before its enclosing block's, a callee frame's before its caller's, and all of a task's
before its `EndTask`. On the cancellation path, a `defer` body ends by jumping to the cancellation
path of the next enclosing `defer` in the same procedure — or to the task body's `EndTask`, when the
procedure is itself the task body and there is no such `defer` — or, if the procedure has neither, by
`ResumeUnwind` (§5.8), which continues the landing search outward from there. A `defer` body may not suspend (compiler refusal, §12), so
unwinding never suspends: once a task is CancelUnwind it runs to its `EndTask` without stopping.

`vm_tasks_probe` `G1` (normal exit) and `F2_Cancel` (cancellation across two frames):

| `G1_NormalDefers` | `F2_Cancel` (FOuter: `defer`, calls FInner: two `defer`s, awaits) |
| --- | --- |
| `  g in block` | `  f cancel begin` |
| `  g inner-block defer` | `  f inner defer 2` |
| `  g body end` | `  f inner defer 1` |
| `  g d2` | `  f outer defer` |
| `  g d1` | `  f cancel returned`, then `Canceled[]` and `Settled[]` succeed, `Completed[]` fails |

### 7.5 Cancelling a task that is running

If the target is on the chain when it is asked, it is marked CancelRequested and **carries on**
until its next suspension point; the canceler waits in `Cancel`. Two cases, both probed:

**The canceler is the target's descendant** (`vm_tasks_probe` `H2_Signal`: a parent resumed by an
event starts a `branch` whose body cancels the parent). The parent continues after the branch,
reaches `Await`, cancels its children there — the branch, still waiting in `Cancel`, is unwound — and
then unwinds itself. The child's `Cancel` never returns:

| # | Printed |
| --- | --- |
| 1 | `H2 signal` |
| 2 | `  h parent resumed` |
| 3 | `  h child cancels parent` |
| 4 | `  h parent continues after branch` |
| 5 | `  h child defer` |
| 6 | `  h parent defer` |
| 7 | `H2 signal returned` |

**The canceler is not a descendant** (`vm_tasks_probe3` `D2_Signal`: the target signals an event
whose awaiter cancels the target). The target continues after `Signal` until it next suspends, then
unwinds; its `EndTask` resumes the canceler, whose `Cancel` returns:

| # | Printed |
| --- | --- |
| 1 | `D2 signal` |
| 2 | `  t target signals the watcher's event` |
| 3 | `  t canceller resumed, cancelling its resumer` |
| 4 | `  t target continues after Signal` |
| 5 | `  t target defer` |
| 6 | `  t canceller: cancel returned` |
| 7 | `D2 signal returned` |

**A target that finishes its body without suspending again still ends Canceled** (§5.4 step 1).
`vm_tasks_probe3` `B2_Signal`: `  q parent resumed`, `  q child cancels the parent`,
`  q parent body ends without suspending`, `  q parent defer` (its own `defer`, on normal exit from
the body), `  q child defer` (the child, cancelled by the parent's `EndTask`); the task's awaiter is
never resumed, and a later `B3_Phase` reads `Canceled[]`.

**A parent that finishes while a child is running** waits in `EndTask` (§5.4 step 2.1) and is resumed
by that child's `EndTask` when the child has finished cancelling. `vm_tasks_probe3` `A2_Go`: a
`branch` child signals the event its parent awaits; the parent finishes its body while the child —
which resumed it — is still on the chain:

| # | Printed | Why |
| --- | --- | --- |
| 1 | `A2 go` | |
| 2 | `  p child signals the parent's event` | |
| 3 | `  p parent body ends` | the parent runs to its `EndTask`… |
| 4 | `  p parent defer` | …its own `defer` on normal exit; `EndTask` cannot cancel the running child, so it waits |
| 5 | `  p child after signal` | the child (now CancelRequested) continues |
| 6 | `  p child defer` | at its next `Await` it unwinds |
| 7 | `  p-watcher got 7` | the child's `EndTask` resumes the parent's `EndTask`, which completes with 7 and resumes its awaiter |
| 8 | `A2 signal returned` | |

### 7.6 Summary of what cancellation does to the task's relations

| Relation | Outcome |
| --- | --- |
| children | cancelled newest first, each completely, before the task's own `defer`s |
| native defer hooks | run at the start of unwinding, newest first |
| `defer` bodies | run, innermost first (§7.4) |
| on-finish hooks | run at `EndTask` |
| awaiters (`Await`) | **never resumed** — they stay suspended until they are themselves cancelled or terminated (`F3`, `F4` below) |
| cancelers (`Cancel`) | resumed, oldest first, with `false` |
| parent | resumed if it is cancelling and this was its newest child |
| task group | left at `EndTask` |
| result | none; `Completed[]` stays false |

`vm_tasks_probe` `F3_AwaitCanceled` prints `  f awaiting a canceled task` and nothing more, then or
ever; `F4` signalling the cancelled task's old event prints only its own two lines.

## 8. Content scopes, termination and runtime errors

### 8.1 Content scopes as the embedder sees them

A **content scope** is an embedder-owned object with a *terminated* flag and a task group. At most one
is *active* at a time, and the embedder decides which. The VM uses it in four places:

| Where | What the VM does with the active scope |
| --- | --- |
| entering the VM | if the active scope is terminated, the entry runs **nothing** and reports that nothing ran |
| `BeginTask` with `bAddToTaskGroup`, `CallTask` | a new root task joins the active scope's group |
| a native suspending a task (§11), an event `Await` (§10), a subscription | the scope active at that moment is captured with the suspension |
| completing that suspension later | the captured scope is made active for the resumption; if it has been terminated meanwhile, the resumption is **skipped** entirely |

In this project (`CLAUDE.md`, R-ASYNC-4) the embedder keeps **one content scope per script
instance**, active for every call into that instance, and one *project scope* for everything else
(`Main`, the tick's queued jobs, field access). A terminated scope is never revived: the embedder
replaces it with a fresh one at the next entry into that instance, so a raise costs exactly that
instance's suspended work. The instance's scope is terminated when the instance is released.

### 8.2 Terminate

Terminating a scope terminates every root task in its group, in no particular order. Terminating a
task, which must be a root task (anything else is a VM invariant violation):

1. It leaves its group.
2. Its phase becomes Canceled.
3. Its native defer hooks run, newest first; then its on-finish hooks, oldest first.
4. Its resume point and yield-to point are cleared: nothing can resume it.
5. Its children are terminated the same way, newest first (each after its parent's hooks, depth
   first).

None of this is transactional and none of it can be undone. When a transaction is open it happens
when that transaction commits.

### 8.3 Terminate versus cancel

| | Cancel | Terminate |
| --- | --- | --- |
| caused by | `Cancel()`, a parent's `EndTask`, a cancelling parent | the embedder terminating a content scope: a runtime error, or an instance being released |
| applies to | any task | root tasks, and their descendants through them |
| transactional | yes | no |
| `defer` bodies | run | **not run** |
| native defer hooks, on-finish hooks | run | run |
| children | cancelled newest first, each unwound | terminated newest first, nothing unwound |
| cancelers | resumed | never resumed |
| awaiters | never resumed | never resumed |
| parent | may be signalled | not involved |
| final phase | Canceled | Canceled |

### 8.4 Runtime errors

A raise rolls back every open transaction of the current VM entry (`failure.md`) and then terminates
the **active** content scope — the raising instance's own scope, or, when a suspended task is being
resumed, the scope captured when it suspended (§8.1). Everything that entry was doing stops: the run
does not return to the code that started it.

`vm_tasks_probe` `R*` (all in one instance, so one scope):

| Step | Printed | |
| --- | --- | --- |
| `R1_Start` | `R1 start`, `  r sleeper waiting`, `R1 end` | a task suspends on an event |
| `R2_Raise` | *(nothing of its own)* — the host reports the runtime error and `vh_instance_call` answers `VH_ERR_RUNTIME` | the `Print` before `Err` was in the rolled-back transaction; the sleeper is terminated: its `defer` does **not** print |
| `R3_Signal` | `R3 signal`, `R3 signal returned` | the terminated sleeper is not resumed |
| `R4_Start` | `R4 start`, `R4 end` | (a new scope) a task suspends on another event |
| `R5_Signal` | `R5 signal`, then the runtime error; `  r raiser resumed, raising`, `  r raiser defer` and `R5 signal returned` are **not** printed, and the call answers `VH_ERR_RUNTIME` | a raise inside a task resumed *by* `Signal` abandons the signaler's run too |

Why `R5 signal` survives when `R2 about to raise` does not is `failure.md`'s question (§14).

## 9. `task(t)` from Verse

A `spawn` expression's value is the `task(t)` it created (`Dest` of `BeginTask` / `CallTask`).

**`Await()<suspends>:t`.** If the task has a result, return it at once (`vm_tasks_probe` `T3`:
`  t w3 awaiting`, `  t w3 got 6` in the same step). Otherwise the caller joins the task's awaiters
list, with a native defer hook that takes it off again, and suspends; the task's normal completion
resumes it with the result (§5.4 step 8). If the task is cancelled or terminated, the caller is never
resumed (§7.6). Awaiting does not make the awaited task a child: cancelling the awaiter cancels
nothing but the awaiter, whose defer hook removes it from the list.

**`Cancel()<suspends>:void`** (`<epic_internal>`). §7.1. Returns once the target is Canceled (or at
once if it is already settled), except in the one case where the canceler is itself cancelled first
(§7.5).

**Phase queries.** §3.

## 10. `event(t)`

`event(t)` is the Verse library's native class (`natives.md` has its names). Its ordering is:

**`Await()<suspends>:t`** appends the calling task (with the content scope then active) to the
event's pending list, attaches a native defer hook that removes it from the pending list and from any
signal in progress, and suspends.

**`Signal(Val:t):void`**:

1. If the pending list is non-empty, **snapshot** it: take the whole list and leave the pending list
   empty. Tasks that call `Await` from now on — including a task this very signal resumes and that
   awaits again — are pending for the *next* signal (`vm_tasks_probe2` `D2`: two looping awaiters each
   print `got 1` exactly once).
2. While the snapshot is non-empty: take its oldest entry and complete that suspension with `Val`
   (§11): the task runs, synchronously, until it suspends or finishes; only then is the next one
   taken. An entry whose task was cancelled in the meantime has already been removed by its defer
   hook and is skipped (`D5`, below).
3. Then the subscribers, if any (§10.1).

A nested `Signal` of the same event from inside a resumed task snapshots only the pending list as it
is at that moment — which does not include the outer signal's remaining entries (`vm_tasks_probe2`
`D3`: `  n1 got 1, signalling again`, `  n1 nested signal returned`, `  n2 got 1`, `  n3 got 1`: the
nested signal resumed nobody).

`vm_tasks_probe2` `D5_CancelDuringSignal` (awaiters c1, c2, c3 in that order; c1 cancels c2):

| # | Printed |
| --- | --- |
| 1 | `D5 signal` |
| 2 | `  c1 got 9, cancelling c2` |
| 3 | `  c2 defer` |
| 4 | `  c1 cancel returned` |
| 5 | `  c3 end` |
| 6 | `  c3 defer` |
| 7 | `D5 signal returned` |

A dropped event does not cancel its awaiters; they simply never resume (source, unprobed).

### 10.1 Subscribers

Only the Verse library's `subscribable_event_intrnl(t)` (`<epic_internal>`) takes subscribers, and
in a script that imports `/Godot.org/Godot` — every Godot script — its `Subscribe` is ambiguous with
the Godot package's own `event(t).Subscribe` extension (glitch 3518) and is reachable only through a
`listenable(t)`-typed reference (`vm_tasks_subscribe_probe.verse`). The bridge's `signal(t)` does not
use this mechanism (`godot-natives.md`).

After the awaiters, `Signal` calls each subscriber callback once, with `Val`:

- in a **random order, freshly shuffled on every `Signal`** — a uniform permutation drawn from a
  process-wide generator seeded from the clock at startup, so the order differs between runs and
  between two signals of one run (`vm_tasks_subscribe_probe`: run 1 printed 3 1 5 4 6 2 then
  5 3 4 1 2 6; run 2 printed 3 5 1 2 6 4 then 3 2 6 1 5 4);
- each with the content scope that was active when it subscribed made active, skipped if that scope
  was terminated;
- each inside its own nested transaction.

Awaiters always come before subscribers (`  awaiter got 1` precedes every `sub` line). An
implementation may use any permutation, deterministic or not; a conformance test must not depend on
the order.

## 11. Natives that suspend

`calls.md` §4.3 lists a native's five outcomes. **Yield** is the one that concerns tasks. A native
that wants to suspend the calling task:

1. captures the task and the active content scope, so that it can complete the call later;
2. may attach native defer hooks to the task (for example, to deregister itself if the task is
   cancelled);
3. answers *yield*. The interpreter suspends the task at the call (§4.2), with the call's `Dest` as
   the resume slot.

Later, **completing** the call with a value (a `void` native completes with `false`):

- If the captured content scope was terminated, nothing happens.
- If the task is **running** — the native is completing during its own call, before it has answered
  — the task's native defer hooks run and the value is simply the call's result (the native answers
  *return*, not *yield*).
- If the task's phase is not Active, nothing happens — **not even its native defer hooks**. A task
  that was cancelled while a native held it is already past this point.
- Otherwise the task is resumed **synchronously, on the completer's native stack**: it becomes
  running, its native defer hooks run (newest first), the value is unified into the resume slot (a
  mismatch fails the innermost failure context, which compiled code never arranges), and it executes
  from its resume point until it suspends or finishes — and so does everything it resumes (§4.3) —
  after which the completion returns to the native that asked. A runtime error during this run
  propagates to the completer (§8.4).

This is the one place the interpreter nests a run inside a native: a Verse-to-Verse call never does,
but `Signal`, a synchronous `Cancel`, a variable write that wakes an `await`, and every host
resumption each run the resumed tasks to their next stop before returning. An implementation that
instead queued the resumption would reorder every table in this file.

### 11.1 `Sleep` and cancellation

`Sleep` is `natives.md`'s; what matters here:

- `Sleep(S)` with `S` < 0 does **not** suspend and is therefore not a suspension point
  (`vm_tasks_probe` `S2`: `  s2 before Sleep(-1.0)`, `  s2 after Sleep(-1.0)` inside the spawn).
- `Sleep(S)` with `S` ≥ 0 always suspends, even for 0, and is resumed by the embedder's tick
  (`sleep_probe.verse` `StartSleep`/`ReportWoke`).
- It attaches no native defer hook. Cancelling a sleeping task unwinds it at once (§7.1 step 4); the
  pending wake stays queued until its deadline and then does nothing, because the task is no longer
  Active (§11). It remains counted in `vh_tick_stats`' sleeping total until then. `vm_tasks_probe`
  `S1`: `  s sleeping`, `  s sleeper defer`, `  s cancel returned`, and nothing on the following tick.
- A sleeping task whose scope is terminated is never resumed; its wake is skipped (§8.1).

## 12. Tasks and failure contexts

The compiler keeps suspension out of failure contexts, so the interpreter never has to suspend a
task that has an open failure context of its own (`vm_tasks_reject.verse`, all refused):

| Written | Glitch |
| --- | --- |
| a `<suspends>` call in an `if` condition | 3512: the call has effects not allowed by its context: `suspends`, `no_rollback` |
| `race` in an `if` condition | 3512: *This 'race' macro has the 'suspends' effect, which is not allowed by its context.* (and 3512 on each arm's call) |
| `<suspends><decides>` on one function | 3656: *The suspends and decides effects are mutually exclusive and may not be used together.* |
| a `<suspends>` call inside a `defer` body | 3512: the call *has the 'suspends' effect, which is not allowed by its context.* |
| `branch` in a non-`<suspends>` function | 3512: *This 'branch' macro has the 'suspends' effect, which is not allowed by its context.* |
| `spawn` of a wide (`no_rollback`) body inside a failure context | 3512 on the spawned call's `no_rollback` |

What **is** allowed is `spawn` of a narrowed body — `<suspends><transacts>` — inside a failure
context. Such a body can never actually suspend (everything that suspends carries `no_rollback`), so
it runs to completion inside the `spawn`, and if the failure context then fails, the task and all its
effects are undone with everything else. `vm_tasks_probe2` `E1`: a failing `if` that spawned a body
incrementing a counter prints `  e1 else, FailSpawned = 0`; a succeeding one prints
`  e1 succeeding if, FailSpawned = 1`.

`Cancel`, `Await` and `Signal` all carry `no_rollback` or `suspends` and so never occur inside a
failure context either. The consequence for the interpreter: task-state writes need the undo log
(design §7.2) only for the narrowed-`spawn` case above and for trailed register writes (§5.4 step
2.3).

## 13. Invariant violations

These are malformed programs, not Verse runtime errors; `calls.md` §10 says what an interpreter does
with one.

| Condition | Where |
| --- | --- |
| `EndTask` executed by a task that is not running | §5.4 |
| `Switch` register not holding uninitialized when `EndTask` writes it | §5.4 step 2.3 |
| `WaitSemaphore` on a semaphore whose waiting slot is taken | §5.6 |
| `CallTask` of a native procedure | §5.3 |
| unwinding finds no covering edge in any frame | §7.3 |
| `BeginAwait` with an await already open; `EndAwait` with none | §5.7 |
| terminating a task that has a parent | §8.2 |
| a resume value that does not unify with an awaiter's or canceler's resume slot | §5.4 step 8 |

## 14. Open questions

1. **`format.md` §5's coverage sentence.** §7.3 defines coverage on the resume position p and tests
   p − 1 against the recorded range. `format.md` should say which index it means, and the writer
   (T2.1) must record ranges so that the test is exact at both ends; a writer that records the
   region's end offset as `last` would be off by one.
2. **What a raise rolls back of the signaler.** In `R5_Signal`, `R5 signal` was printed although it
   came before the raise in the same VM entry, while in `R2_Raise` the print before `Err` was lost.
   Whether calling a `no_rollback` native (here `event.Signal`) commits what came before it is
   `failure.md`'s to settle; the order of what *is* printed is as §8.4 says.
3. **Which scope a raise in a resumed task terminates, across instances.** §8.1 says the scope
   captured at suspension (source). The probe had signaler and awaiter in one instance; a
   two-instance probe needs `tests/integration`, since `verse_probe` instantiates one object.
4. **`spawn` as `BeginTask` or `CallTask`.** Which one the compiler picks for a given `spawn` or
   `branch` (it depends on the capture scope the analyser attaches) was not determined. Both must be
   supported; their observable behaviour is the same.
5. **Order of several `await` resumptions** from one variable write, and of `EndBatch`'s
   resumptions, was not probed (§5.7).
6. **A `<suspends>` method called directly by the host** runs in the entry task, which is in no
   task group, so a raise does not terminate what it left suspended (§4.4, source, unprobed). Whether
   the bridge ever does this for a Godot-called method, and whether the interpreter should put that
   task in the instance's group instead, is a question for `godot-natives.md` and the lead.
7. **Reclaiming unreachable suspended tasks.** A task group holds its root tasks weakly, so a
   suspended task nothing else references (awaiting an event that was itself dropped) can be
   collected, silently, with no hooks run (source, unprobed). A precise collector that keeps every
   suspended task alive would differ only in memory, never in output.
