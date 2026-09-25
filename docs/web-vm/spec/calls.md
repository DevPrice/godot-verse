# Calls, frames and closures

Status: reviewed by the lead 2026-09-24. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`; UE `Engine/Source/Runtime/CoreUObject/{Public,Private}/VerseVM`
(the interpreter, frames, functions, scopes, procedures, intrinsics, runtime-error table),
`Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/VVMCodeGenerator.cpp` (parameter and closure
emission), `Engine/Source/Programs/UnrealBuildTool/System/VerseVMBytecodeGenerator.cs`. Probes:
`tests/verse_probe/vm_calls_probe.verse`.

This file says what a call does in the abstract machine: what a frame holds, how the `Call` and
`CallWithSelf` ops find and enter their callee, how arguments meet parameters, how a result comes
back, and how closures reach the variables they capture. Unification, parking and the effect token
are `unification.md`'s; failure contexts, the trail and runtime-error rendering are `failure.md`'s;
tasks and suspension are `tasks.md`'s; individual natives are `natives.md`'s; objects, fields and
archetypes are `objects.md`'s. Op and operand names are `ops.json`'s; cell kinds are `format.md`'s.

Probe claims cite `vm_calls_probe.verse` and the method that shows them; run it with

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        <repo>/tests/verse_probe/vm_calls_probe.verse --class vm_calls_probe

## 1. Procedures

A **procedure** (`procedure` cell, `format.md` §5) is compiled code. What a call needs from it:

| Field | Meaning for a call |
| --- | --- |
| register count | the size of the register file every frame of this procedure gets |
| positional parameters (P) | how many registers, starting at register 2, receive positional arguments |
| named parameters | a list of (interned name, register) pairs; the registers are anywhere in the file, not necessarily after the positional ones |
| constants | the constant pool that `value` operands with the constant tag read |
| ops, unwind edges, locations | what runs; see `ops.md`, `tasks.md`, `failure.md` |

A procedure always has at least `2 + P + N` registers, where N is the number of named parameters.

P is decided by the compiler and read from the file; the interpreter never derives it. For
orientation only: `F(X:tuple(int, int))` has one positional parameter, `F(A:int, B:int)` has two,
and a function whose only parameter is named has none.

A **native procedure** (`native procedure` cell) has a decorated name and a positional-parameter
count and no code; the loader binds it to an implementation (`natives.md`, `modules.md` §6). It has
no named parameters: whenever a native function declares named or nested parameters, the compiler
wraps it in an ordinary procedure that does the parameter work and then calls the native with
`CallWithSelf` (§4.2). So a native is only ever given a flat positional list.

## 2. Frames

A **frame** is one activation of one procedure. It is heap data, never native stack: a
Verse-to-Verse call must not recurse on the interpreter's own C++ stack, because a suspended task is
its chain of frames (`tasks.md`) and because recursion depth is bounded only by memory (§9).

A frame holds:

| Part | Meaning |
| --- | --- |
| procedure | the procedure being run |
| registers | `register count` slots, each holding a value (§2.2) |
| caller | the caller's frame and the op index to continue at, or none for an entry frame |
| return slot | where the result goes: a register of the caller's frame, or a slot the host owns, or nothing |
| return effect-token slot | where the effect token is delivered on return (`unification.md`) |

### 2.1 Fixed registers

| Register | Holds on entry |
| --- | --- |
| 0 | **Self**: the receiver the callee was entered with (§4) |
| 1 | **Scope**: the function's parent scope (§7), or the `false` value when it has none |
| 2 … 2+P−1 | the positional parameters, in order |
| the registers the named-parameter list names | each named parameter's argument, or *uninitialized* (§5.3) |
| every other register | *fresh* |

What `Self` is for each kind of callee:

| Callee | Register 0 |
| --- | --- |
| a module-level function | the `false` value — "no receiver", which counts as a resolved receiver |
| a method loaded from an object (§6) | that object |
| a closure made by `NewFunction` | the `Self` operand `NewFunction` was given (§7.2) |
| any function entered with `CallWithSelf` | the op's `Self` operand |

### 2.2 Fresh, uninitialized and placeholders

Three different things can be "not a value yet", and the interpreter must keep them apart:

- **Fresh.** A register nobody has written since the frame was made or since a `Reset` /
  `ResetNonTrailed` op cleared it. It stands for a new, unbound logic variable. Reading a fresh
  register produces an unbound placeholder and leaves that placeholder in the register, so a second
  read sees the same one. Writing a fresh register binds it without any comparison. Whether a
  placeholder is actually allocated before the first read is the implementation's business; the
  behaviour is as if it were. `unification.md` owns placeholders.
- **Uninitialized.** A distinct VM value (`format.md` §3, value tag 0) that is not a Verse value,
  not `false` and not a placeholder. The VM uses it for "no argument was supplied" (§5.3), for "no
  receiver bound yet" in a function cell (§6), and as the value an absent operand reads as (§2.3).
  `JumpIfInitialized` jumps exactly when its operand is **not** this value.
- **Placeholder.** A bound or unbound logic variable (`unification.md`).

Two consequences an implementer can get wrong:

- `JumpIfInitialized` on a fresh register jumps, because reading it yields a placeholder, which is
  not uninitialized.
- A register holding uninitialized is not fresh. The compiler always emits `Reset` on such a
  register before it writes a default into it (§5.4); an interpreter must not treat uninitialized
  as "free to overwrite".

### 2.3 An absent value operand

A `value` operand may be *absent*: neither a register nor a constant. It reads as uninitialized.
The compiler produces absent operands in places `ops.json` does not mark optional — `NewFunction`'s
`Self` and `ParentScope` in compiler-generated helper procedures are the ones this file depends on,
and `BeginTask`'s `Parent` is another. `format.md` §5.1 encodes it as `0`. The writer's cooks also
found `CallTask`'s `Parent`, `EndTask`'s `Which` and, twice in `tests/integration`, `Move`'s
`Source` absent, so a reader must accept an absent `value` operand in any op.

## 3. Argument adaptation

In Verse every function takes one argument, which is a tuple when there are several. The VM avoids
building that tuple where it can, so a call site's positional argument count A and the callee's
positional parameter count P need not agree. They are reconciled like this, first matching row
wins:

| Case | What the callee's parameter registers receive |
| --- | --- |
| A = P | argument *i* into parameter *i* |
| A = 1, P ≠ 1 | the one argument is a tuple (an array value) of exactly P elements, and element *i* goes into parameter *i* |
| P = 1, A ≠ 1 | a new immutable tuple of the A arguments, in order, goes into the one parameter; with A = 0 that is the empty tuple |
| anything else | a VM invariant violation (§10) |

If the A = 1 row applies and the tuple's length is not P, that is also a VM invariant violation. A
compiled program never produces either; the rows exist because both directions are real
(`vm_calls_probe` `TupleBoxing`: `TakesTuple(3, 5)` packs two arguments into one tuple parameter and
answers 35; `TakesTwo(U)` with `U := (7, 8)` unpacks one tuple into two parameters and answers 78).

Arguments are passed as they are: an argument may be an unbound placeholder, and the callee
receives the placeholder (`unification.md`). Only the callee value, and `CallWithSelf`'s `Self`, must
be concrete (§4).

The same adaptation applies wherever the VM starts a procedure or native with a list of values: the
`Call` family, `CallWithSelf`, a host entry call (§8), and a native being handed its arguments.

### 3.1 Nested tuple parameters

A parameter written as a tuple pattern, `Nested(A:int, (X:int, Y:int))`, is one positional
parameter. The callee's own code takes it apart: for each unnamed element it emits a `Call` whose
callee is the parameter register and whose single argument is the element index as an integer
constant, which is an index into a tuple (§4.1). So nothing about nesting reaches the calling
convention (`NestedParameters`: `Nested(1, (2, 3))` answers 123, and `Nested(1, P)` with a tuple
variable answers 145).

A tuple pattern with exactly one unnamed element and one or more named ones,
`NestedNamed(A:int, (B:int, ?C:int = 1))`, is flattened by the compiler: the unnamed element
becomes an ordinary positional parameter and the named ones join the procedure's named-parameter
list. `NestedNamed(1, 2)` answers 121 and `NestedNamed(1, (4, ?C := 5))` answers 145.

## 4. `Call` and `CallWithSelf`

### 4.1 `Call`

Operands: `Dest`, `Callee`, `Arguments` (variadic values), `NamedArguments` (variadic interned
names), `NamedArgumentValues` (variadic values, parallel to the names), `bCalleeYields`.

The op first reads `Callee`. If it is an unbound placeholder, the op parks on it
(`unification.md`; in stage 1 of design §7.1 that is the runtime park error). Then, by the kind of
the callee:

| Callee | What happens |
| --- | --- |
| a `function` whose procedure is a `procedure` | a new frame is entered (§5) with Self = the function's own receiver field and Scope = its parent scope; the result arrives later through `Return` (§5.5) |
| a `function` whose procedure is a `native procedure` | the native is called with Self = the function's receiver field and the positional arguments adapted to its parameter count (§3); named arguments are ignored. The outcome is handled per §4.3 |
| an immutable array (including a tuple) | **indexing.** Exactly one argument is required. It must be concrete (park otherwise). If it is a non-negative integer that fits in 32 bits and is less than the length, the element is unified into `Dest`; otherwise the op **fails** — negative, too large, a big integer, or not an integer at all (`IndexAndLookup`: `A[3]` and `A[-1]` both fail on a three-element array) |
| a mutable array | the same, with the element read as it is currently stored |
| an immutable or mutable map | **lookup.** Exactly one argument, required concrete. If the map has an equal key (equality as `values.md`), its value is unified into `Dest`; otherwise the op **fails** (`M["three"]` fails) |
| a type | **cast.** Exactly one argument, required concrete. If the type admits the value, the value itself is unified into `Dest`; otherwise the op fails. Which types admit what is `values.md`/`objects.md` |
| a union variant tag | exactly one argument, required concrete; a new union value of that variant carrying the argument is unified into `Dest` (union cells are provisional in `format.md`) |
| anything else | a VM invariant violation (§10). This includes a bare `native procedure` cell, which only `CallWithSelf` accepts |

Indexing and lookup never raise an error for a missing element; they fail, and the enclosing
failure context handles it (`failure.md`). A weak map whose key is not a valid weak-map key is the
one lookup that raises instead; it is not reachable from a Godot script.

For the non-function callees the named-argument lists are empty and `bCalleeYields` is false in
every compiled program.

### 4.2 `CallWithSelf`

Operands: those of `Call` plus `Self`, between `Callee` and `Arguments`.

It enters a callee with an explicitly supplied receiver. Both `Callee` and `Self` must be concrete;
the op parks on whichever is not, `Callee` first. Then:

| Callee | What happens |
| --- | --- |
| a `function` with **no** receiver bound (its receiver field is uninitialized) | as `Call` with a `function`, except register 0 is the `Self` operand |
| a `function` whose receiver is already bound — an object **or** `false` | a VM invariant violation (§10) |
| a bare `native procedure` | the native is called directly with the `Self` operand as its receiver and the positional arguments adapted to its parameter count (§3). This is how a native's wrapper procedure (§1) hands its flattened arguments to the native |
| anything else | a VM invariant violation (§10) |

The compiler uses `CallWithSelf` for constructors and class bodies being run on a new object
(`objects.md`) and for native wrappers.

### 4.3 What a native callee's outcome does at the call site

A native answers one of five outcomes (`natives.md` defines which natives answer which). Before the
native runs, its receiver must be concrete; if it is an unbound placeholder the op parks on it.

| Outcome | At the call site |
| --- | --- |
| **return** a value | the value is unified into `Dest` (§5.6), and execution continues with the next op |
| **fail** | the call op fails; the current failure context takes over (`failure.md`) |
| **block** on a placeholder | the call op parks on that placeholder and runs again from the start when it is bound (`unification.md`); stage 1 treats it as the runtime park error |
| **yield** | the task suspends at this op, and `Dest` becomes the task's resume slot: when the task is resumed, the resume value is unified into `Dest` and execution continues with the next op (`tasks.md`). A native may only yield at a call whose `bCalleeYields` is true (§4.4) |
| **error** | a runtime error, already raised by the native, propagates as `failure.md` describes |

A native never consumes or replaces the effect token (`unification.md`).

### 4.4 `bCalleeYields`

`bCalleeYields` is true on exactly the calls whose callee may suspend the calling task — calls to
`<suspends>` functions, and the wrapper's `CallWithSelf` to a `<suspends>` native. It means:

- **This call may suspend the task.** A native that answers *yield* at a call whose flag is false is
  a VM invariant violation (§10). A Verse callee that suspends does so from its own ops (`tasks.md`),
  which carry their own may-yield property; the flag on the caller's `Call` is not consulted for it.
- **It is a potential unwind point.** When a task is cancelled while suspended somewhere beneath
  this call, unwinding passes through this op; the procedure's unwind edges (`format.md` §5) already
  record which landing op covers it. The interpreter reads the edges; it does not derive anything
  from the flag for this purpose.

The flag carries no other meaning and does not change how the callee is entered.

## 5. Entering and leaving a procedure

### 5.1 Entry

Entering a `procedure` for a `Call` or `CallWithSelf`:

1. Make a frame of the procedure's register count, every register fresh.
2. Record the caller frame, the op index after the call, and the caller's `Dest` register as the
   return slot.
3. Set register 0 and register 1 (§2.1).
4. Adapt the positional arguments into registers 2 … 2+P−1 (§3).
5. Fill the named parameters (§5.2).
6. Continue at the callee's op 0, in the same task and the same failure context, with the same
   effect token (`unification.md`).

Steps 3 to 5 read argument values that the call op read from the caller's registers before the new
frame existed; nothing the callee does can change what it was passed.

### 5.2 Named arguments

For each entry of the callee's named-parameter list, in list order: look through the call's
`NamedArguments` for the same interned name — by identity of the interned string, which the loader
guarantees is the same as equality of contents (`format.md` §2.2). If one is found, the value at the
same position in `NamedArgumentValues` goes into that parameter's register; the first match wins.
If none is found, the register receives **uninitialized**.

A named argument whose name matches no parameter is ignored. The compiler never produces one, nor a
duplicate. A native callee ignores named arguments entirely (§4.1).

The name is the parameter's source name without decoration, the same string at the call site and in
the callee's list (`NamedAndDefaults`: `TwoNamed(?Y := 5, ?X := 6)` answers 65, so order at the call
site does not matter).

### 5.3 What the callee does with an unsupplied named parameter

Nothing at entry: the register simply holds uninitialized. Defaults are computed by the callee's own
code (§5.4).

### 5.4 Defaults

For a named parameter that declares a default, the callee's code, in parameter declaration order,
does: `JumpIfInitialized` on the parameter register past the default; otherwise `Reset` the
register, compute the default expression, and `Move` it into the register. A default may use any
parameter declared before it (`DefaultFromEarlier(3)` answers 306 with `?B:int = A * 2`).

Consequently a default's side effects happen **inside the callee, after entry and after every
argument expression at the call site has been evaluated**, and only for defaults that were not
supplied (`EvaluationOrder`, second group: `a`, then `argument C`, then `default B`; `default C` is
never evaluated because `C` was supplied).

A named parameter with no default and no argument cannot happen in a compiled program.

### 5.5 `Return` and `ReturnTrailed`

Operand: `Value`.

1. The current effect token is unified into the frame's return effect-token slot
   (`unification.md`).
2. `Value` is read.
3. The frame is left. If it has a caller, execution continues in the caller frame at the recorded op
   index. If it has none, it was an entry frame (§8) and this run of the interpreter ends.
4. The value is unified into the return slot (§5.6).

`ReturnTrailed` is `Return` except that the writes of steps 1 and 4 are recorded in the current
failure context's trail, so that the failure of an enclosing context undoes them (`failure.md`). The
compiler uses it for the bodies of `<suspends>` functions and some compiler-generated procedures;
the interpreter does not need to know why, only to record the writes.

### 5.6 How the result reaches `Dest`

Every result — a `Return`, a native's *return*, an index or lookup — is **unified** into `Dest`, not
stored (`unification.md`). `Dest` is normally fresh, so the unification simply binds it. If `Dest`
already holds a value and the result is not equal to it, the call **fails** in the caller, in the
caller's current failure context. A frame boundary does not open or close a failure context: after
`Return`, failure is judged in whatever context was current when the call was made.

There is no tail call: every call makes a frame and every `Return` leaves one.

## 6. Methods and receivers

A class's methods are stored in its archetype (`objects.md`) as `function` cells whose receiver is
**uninitialized** and whose parent scope is the class scope (§7.4). Such a function is *unbound*.

Loading a field whose stored value is an unbound function (`LoadField`, `objects.md`) does not
return the stored function. It returns a **new** function with the same procedure, the same parent
scope, and the loaded-from object as its receiver. Every load makes a new one; functions are not
comparable in Verse, so the identity is not observable. A field that holds a function which already
has a receiver — a module function stored in a field of function type, whose receiver is `false` —
is returned unchanged.

So the receiver of a function cell has three states, which `format.md`'s `function` cell must keep
apart: uninitialized (a method awaiting a receiver), `false` (takes no receiver, e.g. a module
function), or an object.

A bound method is an ordinary value: it can be stored and called later with `Call`, and runs with
the object it was loaded from (`MethodValues`: `F := D.Describe` then `F()` runs the override on
`D`). Dispatch happens at the load: which override runs is decided by which function the object's
class stores under that name, not by the static type of the expression (`B:calls_base = D` then
`B.Describe()` still runs `calls_derived`'s override).

## 7. Scopes and closures

### 7.1 Scopes

A **scope** (`scope` cell) is an immutable parent-scope reference (possibly none) plus a fixed list
of captured values. A function's parent scope is how its code reaches values defined outside it:
locals of an enclosing function, and for methods, the class's archetype. A procedure only ever sees
its own scope, in register 1, and walks up from it.

| Op | Operands | Behaviour |
| --- | --- | --- |
| `NewScope` | `Dest`, `ParentScope`, `Captures` (variadic) | makes a new scope. Its parent is `ParentScope` if that value is a scope and none otherwise (the `false` value is the usual "none"). Its captures are the `Captures` operands' values, in order, read as they are — unbound placeholders included. The scope is unified into `Dest` |
| `NewFunction` | `Dest`, `Procedure`, `Self`, `ParentScope` | makes a new `function` cell. `Procedure` must be concrete (park otherwise) and is a procedure. `ParentScope` must be concrete (park otherwise); if it is a scope it becomes the function's parent scope, and otherwise the function has none. `Self` is taken as read, without requiring it concrete; an absent `Self` (§2.3) reads as uninitialized, which makes the function unbound (§6). The function is unified into `Dest` |
| `LoadParentScope` | `Dest`, `Scope` | `Scope` must be concrete (park otherwise) and a scope with a parent; its parent is unified into `Dest`. A scope with no parent is a VM invariant violation (§10) |
| `LoadCapture` | `Dest`, `Scope`, `Index` (a constant integer) | `Scope` must be concrete (park otherwise); its capture at `Index` is unified into `Dest`. `Index` is within range in every compiled program |

### 7.2 What the compiler builds with them

A function defined inside another function's body is a closure (`LocalFunction`: a local
`AddBase(X:int):int = X + Base` reads the enclosing `Base` and can be passed as a value). For each
one, the enclosing procedure:

- chooses a parent scope: the constant `false` when the nested function captures nothing at any
  depth; its own register 1 unchanged when the nested function captures nothing itself but something
  further out does; otherwise the result of `NewScope` with its own register 1 as parent and the
  captured values as captures;
- then runs `NewFunction` with that scope and, as `Self`, its own register 0 — so a closure inside a
  method carries the method's receiver.

Compiler-generated bodies — the arms of `spawn`, `race`, `sync`, `branch`, live `set` loops — are
closures made the same way (`tasks.md`). Some of their helper procedures are made with an absent
`Self` and an absent `ParentScope` (§2.3), which yields an unbound function with no scope.

### 7.3 Reaching a captured value

To read a variable captured *n* scope levels out, the code starts from register 1, applies
`LoadParentScope` once for each intervening level whose scope was actually allocated (a level that
captured nothing and reused its parent's scope costs no step), and then `LoadCapture` with the
variable's index in that scope. The indices and the number of steps are compiled in; the interpreter
does only what the ops say.

### 7.4 The class scope and `(super:)`

When a class is created (`NewClass`, `objects.md`), its constructor body, its blocks procedure and
every method in its archetype are wrapped as unbound functions whose parent scope is one **class
scope**: a scope with no parent and exactly one capture, the class's own archetype. In a linked
program these function and scope cells already exist; the loader reads them.

`LoadFieldFromSuper` (`Dest`, `Scope`, `Self`, `Name`) is how `(super:)Method` is compiled:

1. `Scope` must be concrete (park otherwise). It is the calling procedure's register 1 — or a scope
   derived from it inside a closure.
2. `Self` must be concrete (park otherwise) and must be an object of a class — not a struct. Anything
   else is a VM invariant violation.
3. From `Scope`, follow parents to the root scope. The root must have exactly one capture, which is
   the archetype of the class the calling method was **defined in**; that archetype and its class
   must be concrete (park otherwise).
4. Walk that class's archetype chain towards its ancestors (`objects.md` defines the chain),
   **skipping the defining class itself**, and take the first ancestor whose archetype has an entry
   named `Name` holding an unbound function.
5. Bind that function to `Self` exactly as in §6 and unify the result into `Dest`. The call is then
   an ordinary `Call` on it.

Because the start point is the defining class, not the object's class, `(super:)` always means the
parent of the class whose source contains it, whatever the runtime class of `Self`; and because the
walk takes the first ancestor that has the method, a level that does not override is skipped
(`SuperSkipsALevel`: `calls_leaf`'s `(super:)Describe()` reaches `calls_derived`'s override, since
`calls_mid` in between declares none, and that one's `(super:)` reaches `calls_base`). If no
ancestor has the name, it is a VM invariant violation; the compiler guarantees one does.

## 8. Entry calls from the host

When the embedder calls into Verse — `vh_instance_call` and the other execution entry points — it
enters a `function` the same way a `Call` does, with three differences:

- The frame has no caller and its return slot belongs to the host. `Return` in it ends the run
  (§5.5 step 3) and the host reads the slot.
- If the function's procedure is a native, the native is invoked directly with the function's
  receiver and the adapted arguments (§3); its outcome is the host's to handle.
- Arguments, including named ones, come from the host rather than from registers; they are adapted
  and matched exactly as §3 and §5.2 say.

Starting a task body (`CallTask`, `spawn`) is `tasks.md`'s. It passes positional arguments only and
always in the count the procedure declares.

## 9. Recursion

There is **no recursion depth limit** and no stack-overflow error. `vm_calls_probe`
`RecursionVeryDeep` recurses two million levels through an ordinary non-tail-recursive function and
answers 2000000 with no diagnostic; the frame chain lives on the heap and is bounded only by memory.
An interpreter must therefore neither recurse natively per Verse call nor impose a depth cap of its
own. What running out of memory looks like is not specified here (§12).

## 10. Errors a call can produce

| Situation | What happens |
| --- | --- |
| index out of range, non-integer or negative index, missing map key, cast refused | the op **fails** (§4.1) |
| a callee, `Self`, receiver, procedure or scope is an unbound placeholder where §4–§7 require it concrete | the op **parks** (`unification.md`) |
| a native answers *error* | runtime error (§4.3) |
| calling the missing-procedure function | runtime error, below |
| an argument-count mismatch §3 cannot reconcile; a callee kind the op does not accept; `CallWithSelf` on a function with a receiver; `LoadParentScope` on a root scope; `LoadFieldFromSuper` finding nothing; a native yielding where `bCalleeYields` is false | a **VM invariant violation**: bytecode no compiler emits. Epic's VM aborts the process. The interpreter must stop the run and report the op index and source line; how that reaches the host is `failure.md`'s. None of these is a Verse runtime error, and none has user-visible text to match |

### 10.1 The missing-procedure function

The built-in package (`format.md` §4, cell kind 3) defines one function that stands in for "a
function slot that was never given one":

| | |
| --- | --- |
| definition key in the built-in package | `(/Verse.org/Verse/(/Verse.org/Verse:)MissingProcedure:)Native` |
| its procedure | a native procedure named `(/Verse.org/Verse:)MissingProcedure`, zero positional parameters |
| its receiver | `false` |

Calling it by any route raises a runtime error with diagnostic `ErrRuntime_InvalidFunctionCall`
(an unrecoverable-class diagnostic whose table description is `Attempted to call an invalid
function.`) and the message text

    Attempted to call an uninitialized function.

Rendered by the rule in `failure.md`, the first line the host receives is
`ErrRuntime_InvalidFunctionCall: Attempted to call an invalid function. (Attempted to call an
uninitialized function.)` (source, unprobed). It is the same function cell wherever it appears, so
an implementation may test for it by identity.

Nothing a Godot script compiles to reaches it: Epic's engine produces it only when converting
Blueprint-era function properties that were never assigned. It is specified because the built-in
package the loader supplies must contain it, and a `.vbc` that references it must resolve.

## 11. Evaluation order

- The interpreter executes ops in stream order; a call op reads all its operands when it executes
  and before the callee does anything. Argument *expressions* are evaluated by the ops the compiler
  placed before the call, so their order is the op order and the interpreter must not reorder
  anything.
- As compiled, positional argument expressions run left to right, then named argument expressions
  in the order they are **written at the call site**, not the callee's declaration order
  (`EvaluationOrder`: `a`, `b`, `c`; and `argument Y` before `argument X` for
  `TwoNamed(?Y := …, ?X := …)`).
- Defaults run afterwards, inside the callee, in parameter declaration order (§5.4).
- Nested tuple parameters are taken apart inside the callee, in declaration order, interleaved with
  default computation in the same order (§3.1).

## 12. Open questions

1. **Encoding an absent value operand.** `format.md` §5.1 encodes a `value` operand as
   `index << 1` (register) or `index << 1 | 1` (constant), which leaves no spelling for the absent
   operand of §2.3. The writer (T2.1) meets it in at least `NewFunction` (`Self`, `ParentScope`) and
   `BeginTask` (`Parent`), none of which `ops.json` marks optional. Suggested fix, the lead's to
   decide: reserve `uv 0` for absent and shift both tags up by one.
2. **The `function` cell's receiver.** `format.md` §4 says the receiver is "uninitialized for none",
   which collapses two states §6 needs apart: uninitialized (unbound method) and `false` (no
   receiver). The writer must write `false` as the `false` cell and only an unbound receiver as value
   tag 0.
3. **Out of memory.** Deep recursion is bounded by memory only (§9). What the UE runtime host does
   when a frame allocation cannot be satisfied was not measured, and a wasm32 heap is far smaller
   than the machine this probe ran on. A differential fixture should not recurse deeper than the
   interpreter can hold; what the interpreter reports when it cannot is a design decision.
4. **The computation watchdog.** Epic's VM runs a wall-clock watchdog that raises
   `ErrRuntime_ComputationLimitExceeded` when one top-level run takes too long. It is not a call rule
   and is not specified here; whether the interpreter implements one belongs with `failure.md` or
   `tasks.md`.
5. **Extension-method call shape.** `5.Twice()` works (`FunctionValues`), and an extension method
   is a module-level function whose parameters are the receiver followed by the call's own argument
   tuple. Nothing about it is special at the VM level as far as the source shows; a fixture that
   passes arguments to an extension method would confirm the argument count the writer sees.
