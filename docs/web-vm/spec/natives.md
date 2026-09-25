# `natives.md`: every native a Godot game's program reaches, except the Godot ones

Status: reviewed by the lead 2026-09-24. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/facts.md`,
`docs/web-vm/spec/godot-natives.md` (headings and §8.1, §8.25), `CLAUDE.md`,
`host/VerseHost.Build.cs`, `host/Private/HostScript.cpp` (package setup), `host/Private/HostCook.cpp`
(package enumeration), `host/Private/VerseHost.cpp` (runtime-error reporting); in the Unreal
checkout at `203d764`: every `.native.verse` of the Verse, VerseNative, VersePredicts,
VerseSpatialMath, EpicGamesEngineRestricted and VerseSimulationMetadata modules, the `Build.cs` files
of the modules the host links, the generated native-binding files for the runtime host
(`Intermediate/Build/Win64/verse_host_runtime/Inc/*/VNI`), the Verse plugin's native
implementations (math, strings, errors, time, random, events, easing, diagnostics, messages,
classifiable subsets), VerseVM's intrinsics, native-procedure, task, runtime-error, float-printing
and native-conversion sources, the VerseVM code generator's native-procedure and parameter
compilation, uLang's decorated-name construction, and Epic's own `VerseTestScriptCmd` tests for
events, localization, intrinsics and float printing. Probes: `tests/verse_probe/vm_natives_probe.verse`,
`tests/verse_probe/vm_natives_edges_probe.verse`, `tests/verse_probe/vm_natives_reach_reject.verse`
(and `vm_facts_probe.verse` through `facts.md`).

## 1. Scope

This file is the contract for every native procedure a program compiled by this project can reach
**other than** the 39 natives of `/Godot.org/Godot` (`host/Verse/Godot.native.verse`), which are
`godot-natives.md`. It covers:

- which packages a program contains and which of their natives a Godot script can reach (§2);
- the calling convention every native obeys, the Godot ones included (§3);
- the intrinsics of the built-in package (§4);
- each reached native of `/Verse.org` (§5–§8);
- what is named but out of contract, with the reason (§9).

"Outcome" below always means one of the five a native call can have:

| Outcome | Meaning to the calling op |
| --- | --- |
| **Return** *v* | the call succeeds and *v* is unified into the call's destination (`unification.md`) |
| **Fail** | the call fails; the innermost failure context fails (`failure.md`). Only a `<decides>` native may answer it |
| **Block** *p* | an input was an unbound placeholder *p*; the call parks on it (`unification.md`; a runtime error in the interpreter's stage 1, design §7.1) |
| **Yield** | the current task suspends; its resumption later supplies the call's value (`tasks.md`). Only a `<suspends>` native may answer it |
| **Error** | a runtime error was raised; the text is given per native and formatted by §3.10 (`failure.md` for the unwinding) |

`Print` and `Sleep` are **not** here: the only `Print` and the only `Sleep` in this program are
`/Godot.org/Godot`'s, `godot-natives.md` §8.1 and §8.25/§10. `/Verse.org/Verse`'s own `Print`, with
its `?Duration` and `?Color` parameters, and the `/Verse.org/Colors` module are not in the program
at all (§9.1).

## 2. What a program contains

### 2.1 The package set

A program is the project's script package plus every package it depends on, and a script package
depends on every other package in the project (`HostScript.cpp`, `AddScriptPackage`). The Verse
packages present are exactly those of the Unreal modules the host binary links that declare Verse
code, plus the built-in intrinsics package the VM itself supplies. Measured: the compiler's own
diagnostic, when asked for a segment that does not exist under `/Verse.org`, lists what does —
*"Possible segments are Verse, Native, Temporary, Persona, Concurrency, Diagnostics, Predicts,
Random, Simulation, SpatialMath"* (`vm_natives_reach_reject.verse`, first diagnostic).

| Verse root | Comes from | Natives | Verdict |
| --- | --- | --- | --- |
| `/Verse.org/Verse` (package `$BuiltIn`) | the VM's intrinsics | 14 | 10 reached, 4 unreachable (§4) |
| `/Verse.org/Verse`, `/Verse.org/Verse/Easing` | the Verse plugin | 50 functions, 9 methods | reached (§5, §7) |
| `/Verse.org/Random` | the Verse plugin | 2 | reached (§6) |
| `/Verse.org/Concurrency` (`task`, `awaitable`) | VerseNative | 10 methods of `task` | reached (§8) |
| `/Verse.org/Native` | VerseNative | none (attributes only) | — |
| `/Verse.org/Persona` | VerseNative | 5 | named, out of contract (§9.3) |
| `/Verse.org/Diagnostics` | VerseNative | none (one attribute) | — |
| `/Verse.org/Predicts` | VersePredicts | 31 | named, out of contract (§9.3) |
| `/Verse.org/SpatialMath` | VerseSpatialMath | 16 | named, out of contract (§9.2) |
| `/Verse.org/Temporary` | EpicGamesEngineRestricted | none (attributes only) | — |
| `/Verse.org/Simulation` | VerseSimulationMetadata | none (attributes only) | — |
| `/Godot.org/Godot` | this project | 39 | `godot-natives.md` |

`/Verse.org/Simulation` here is the **metadata** module only: `Sleep`, `session`, `player` and the
rest of Epic's simulation library are a different module the host does not link.

### 2.2 Why `epic_internal` natives are reached

Every script package is created with the `InternalUser` scope (`HostScript.cpp`,
`AddScriptPackage`; the bindings and attribute packages likewise), and that scope may reach
`<epic_internal>` definitions. So `ToString(:char32)`, `Warn`, `task.Cancel`, the `task` phase
queries, `subscribable_event_intrnl`, `CanCallerAccessEpicInternal`, the message constructors and
the `Predicts` natives are all callable from an ordinary Godot script. Measured:
`CanCallerAccessEpicInternal()` compiles and answers `true` from a script method
(`vm_natives_probe.verse`, `G01_EpicInternal`); `ToString(0u00e9)`, `Warn`, every `task` phase query
and `task.Cancel` compile and run (`B01_Strings`, `B03_Warn`, `F02_QueryTask`, `F06_Cancel`).
"Reached" in this file therefore means "a script can name it, or code the compiler generates for a
script calls it", and not merely "it is `<public>`".

## 3. The native calling convention

### 3.1 The native procedure

A native is a `native procedure` cell (`format.md` §4, kind 13) with two fields the interpreter
uses:

- its **name**, the function's decorated name (§3.2), which is what a call stack shows for the
  native's frame (§3.10);
- its **positional count**: the number of parameters the native receives, which is the number of
  parameter *definitions* in the function's signature after flattening nested tuples, named
  parameters included (§3.5).

A native procedure is reached through a `function` cell whose procedure is it (for a call site that
calls the native directly) or through a bytecode wrapper procedure (§3.5). Either way the program
contains the `native procedure` cell once, as a definition of its package.

### 3.2 Binding: which native a cell is

**The decorated name** of a function is built from three parts, in order:

1. A qualifier `(`*scope*`:)` where *scope* is the Verse path of the scope in which the function's
   *root* definition was declared — for an override, the path of the class or interface that first
   declared the member (`signalable.Signal` is qualified `/Verse.org/Verse/signalable` even on
   `event`). **The qualifier is omitted for a `<suspends>` function**: `event.Await` is plain `Await`.
2. The name as written, with operators spelled `operator'+'` and extension methods
   `operator'.Name'`.
3. If the function has at least one parameter, `(` then each parameter type as `:`*type*,
   comma-separated, then ` where ` and the type variables if any, then `)`. `string` is spelled
   `[]char`; a nominal type is spelled with its own qualifier, e.g. `:(/Verse.org/Verse:)message`;
   a function type `:t->void`.

**The binding key** of a native is `(`*enclosing*`/`*decorated*`:)Native`, where *enclosing* is the
Verse path of the scope that directly contains the definition — the module for a module function,
the class for a method — and *decorated* is the decorated name above. Examples:
`(/Verse.org/Verse/(/Verse.org/Verse:)Sqrt(:float):)Native`,
`(/Verse.org/Verse/event/(/Verse.org/Verse/signalable:)Signal(:payload):)Native`,
`(/Verse.org/Verse/event/Await:)Native`. It is the path under which the native procedure is
recorded among its package's definitions (`format.md` §7), so the loader finds each native's key by
finding the definition that holds the cell. **Keys are unique across the program; names are not**:
`event.Await` and `task.Await` are both named `Await`. The loader must therefore bind by key, not by
the cell's name. (Source, unprobed; the `.vbc` dump of T2.1 confirms the spelling.)

Every key the interpreter must bind is listed in the tables of §4–§8. A key is matched as a byte
string; the interpreter never constructs one.

**An unbound native.** In Epic's VM a native procedure whose implementation was never bound is
called through a null pointer: the process dies with no message (`CLAUDE.md`, "The cooked path").
There is no reference text to match. **The lead's rule (§11.1 Q6, and `modules.md` §6):** a load
never refuses a file for an unbound native, because a `.vbc` carries every native of every reachable
package. Every native procedure whose key the interpreter has no implementation for — in §9's list
or not — binds to a stand-in that, when called, raises the runtime error `ErrRuntime_NativeInternal`
with the message `The native function <binding key> is not implemented by this runtime.` — our text,
not Epic's.

### 3.3 `Self`

Every native receives a `Self` value beside its positional arguments:

- a **module-level** native (every native of §4, §5.1–§5.9, §5.11 and §6, and the module
  functions of §5.10) receives the `false` value as `Self`;
- a **method** (the methods of §5.10, and every native of §7 and §8) receives the object the method
  was loaded from — the `classifiable_subset_var`, the `event`, the `task`, the
  `event_subscription`;
- when a wrapper (§3.5) stands in front of the native, the wrapper passes its own `Self` through,
  so the rule above holds either way.

If `Self` is an unbound placeholder the call Blocks on it before the native runs.

### 3.4 Argument adaptation

A call op supplies *n* positional arguments; the native wants *k* (its positional count). Exactly
one of these applies:

| Case | What the native receives |
| --- | --- |
| *n* = *k* | the arguments, in order |
| *n* = 1, *k* ≠ 1 | the one argument is a tuple of exactly *k* elements; the native receives its elements, in order |
| *n* ≠ 1, *k* = 1 | the *n* arguments, boxed into one tuple (an immutable array) |
| anything else | cannot occur in a program the compiler produced |

This is the same rule a bytecode procedure's parameters follow (`calls.md`). A call op never
passes named arguments to a native procedure: a native with named parameters always has a wrapper.

### 3.5 The bytecode wrapper at the public name

The compiler puts a **wrapper procedure** in front of a native whenever the native cannot take the
call's arguments as they arrive. It does so when any of these holds:

- the function's positional parameter count differs from its flattened parameter count — a nested
  tuple parameter, or an extension method with no call arguments (whose parameters are the
  receiver and the empty tuple);
- the function has named parameters;
- compiling the parameters produced any code (destructuring, or default values of named
  parameters).

The wrapper is an ordinary bytecode procedure, recorded under the key
`(`*enclosing*`/`*decorated*`:)Procedure`, whose body destructures its parameters exactly as any
procedure's would (`calls.md`), applies defaults to absent named parameters, then calls the native
procedure with `CallWithSelf` — callee the native procedure as a constant, `Self` the wrapper's own
`Self` register, the flattened parameters in declaration order as positional arguments, no named
arguments, `bCalleeYields` set exactly when the function is `<suspends>` — and returns the result.
Named parameters reach the native **positionally**, in their declaration position, already
defaulted.

The interpreter needs no knowledge of which natives have wrappers: a wrapper is bytecode in the
program, and the native procedure behind it is called with exactly its positional count. None of
the natives this file specifies in §4–§8 has a wrapper (each is a flat parameter list with no named
parameters); `/Verse.org/SpatialMath`'s zero-argument extension methods would (§9.2).

### 3.6 What a native requires of its inputs

- **Concreteness.** A native reads an argument only once it is concrete. The intrinsics of §4 check
  every argument before doing anything; every other native checks each argument as it converts it,
  left to right, so the first unbound one is the one it Blocks on. Elements of an array or map
  argument are checked as they are converted.
- **Integers must fit 64 bits.** Every `int` parameter of a §5–§8 native (not the §4 intrinsics,
  which take any integer) is converted to a signed 64-bit integer. An argument outside
  [−2^63, 2^63−1] is an **Error**, raised before the native's body runs: diagnostic
  `ErrRuntime_GeneratedNativeInternal`, message `Value exceeds the range of a 64 bit integer.`
  Measured: `Mod[2^63, 3]` (`vm_natives_probe.verse`, `A10_ModBig`); `ToString(2^63)` (`facts.md`
  §4). An `int` *result* of such a native always fits, so results are never refused.
- **Strings** arrive as the byte sequence of a `[]char`; no UTF-8 validation is performed.

### 3.7 The effect token

A native **never consumes or replaces the effect token**: the token after the call is the token
before it (Epic's VM treats a native that changed it as a fatal internal error, so no program does).
What differs between natives is whether they **wait** for it. Most natives require the incoming
token to be concrete and Block on it if it is not; the natives declared `@vm_no_effect_token`
(Sqrt, Sin, Cos, Tan, ArcSin, ArcCos, both ArcTan, Sinh, Cosh, Tanh, ArSinh, ArCosh, ArTanh, Pow,
Exp, Ln, Lerp) and the §4 intrinsics and §8 `task` natives do not look at it. The tables below say
which (`token`: **waits** or **—**). What the token is and when it is unbound is `unification.md`;
in stage 1 of the interpreter it is always concrete at a native call.

### 3.8 Transactions

A native runs inside the caller's transaction (`failure.md`). **Every change a native makes to
Verse-visible state — objects it allocates and fields it sets, an event's subscription table, a
`classifiable_subset_var`'s contents — must be undone if an enclosing failure context fails**, like
any op's writes; the interpreter records them in its undo log. The natives that make no such
changes (every float and integer function) need no record. Two natives deliberately step outside:
`GetRandomInt` and `GetRandomFloat` advance the generator whether or not the transaction commits,
which is not observable (§6).

`Err`, `Warn` and the natives that raise do so as described in §3.10; a raise rolls back the
transaction the way `failure.md` says, including the deferred Godot writes of the same call — which
is why a method that raises shows none of its own `Print`s (`facts.md`, "Adjacent finding").

### 3.9 What a native can see

Beyond `Self` and its arguments, a native has the execution context of the call:

- the **current task**, which a `<suspends>` native registers as a waiter and then answers
  **Yield** for; it can also attach a cleanup that runs if that task is cancelled or unwinds while
  waiting, which is how a cancelled awaiter stops being a waiter (§7.1, §8.2);
- the **innermost failure context and transaction**, which its writes are recorded against (§3.8);
- the **call stack**, as frames: each bytecode frame's procedure and line, and native frames by
  name. `CanCallerAccessEpicInternal_Impl` inspects it (§5.9) and a runtime error renders it (§3.10).

### 3.10 A runtime error raised by a native

A raise carries a **diagnostic** and a **message**. What the embedder's runtime-error callback
receives as the error's text is

> *DiagnosticName*`: `*Description*` (`*message*`)`

or, when the message is empty, *DiagnosticName*`: `*Description* alone. The diagnostics used by this
file, with their descriptions verbatim:

| Diagnostic | Description |
| --- | --- |
| `ErrRuntime_Internal` | `An internal runtime error occurred. There is no other information available.` |
| `ErrRuntime_NativeInternal` | `An internal runtime error occurred in native code that was called from Verse. There is no other information available.` |
| `ErrRuntime_GeneratedNativeInternal` | `An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available.` |
| `ErrRuntime_IntegerOverflow` | `Integer overflow encountered.` |
| `ErrRuntime_IntegerBoundsExceeded` | `A value does not fall inside the representable range of a Verse integer.` |
| `ErrRuntime_MemoryLimitExceeded` | `Exceeded memory limit(s).` |
| `ErrRuntime_InvalidFunctionCall` | `Attempted to call an invalid function.` |
| `ErrRuntime_InvalidArrayLength` | `Invalid array length.` |
| `ErrorRequested` | `A runtime error was explicitly raised from user code.` |
| `WarningRequested` | `A runtime warning was explicitly raised from user code.` |

A raise whose message is the diagnostic's own description repeats it, e.g.
`ErrRuntime_IntegerOverflow: Integer overflow encountered. (Integer overflow encountered.)`
(measured, `A09_QuotientOverflow`).

In the call stack handed with the error, the raising native is the **first frame**, with the
native's name (§3.1) as the procedure and no source location; the bytecode frame that called it
follows with its file and line. Measured: `(/Verse.org/Verse:)Floor(:float)` then
`(/user@localhost/vm_natives_probe:)A08_FloorHuge` at the probe's line 69. How frames are rendered
and delivered is `failure.md` and `godot-natives.md` §1.

## 4. The built-in intrinsics

The VM supplies a package named `$BuiltIn` whose root path is `/Verse.org/Verse` (`format.md` §4,
cell kind 3: the loader supplies it). The compiler emits calls to these for the operations listed,
passing the `function` cell as a constant callee. `Self` is `false`. None waits for the effect token;
every one requires all of its arguments concrete before doing anything (Block otherwise).

| Name (as a call stack shows it) | Binding key | *k* | Emitted for | Reached |
| --- | --- | --- | --- | --- |
| `(/Verse.org/Verse:)Abs` | `(/Verse.org/Verse/(/Verse.org/Verse:)Abs:)Native` | 1 | `Abs(:int)`, `Abs(:float)` | yes |
| `(/Verse.org/Verse:)Ceil` | `(/Verse.org/Verse/(/Verse.org/Verse:)Ceil:)Native` | 1 | `Ceil` of a `rational` | yes |
| `(/Verse.org/Verse:)Floor` | `(/Verse.org/Verse/(/Verse.org/Verse:)Floor:)Native` | 1 | `Floor` of a `rational` | yes |
| `(/Verse.org/Verse:)BitAnd` | `(/Verse.org/Verse/(/Verse.org/Verse:)BitAnd:)Native` | 2 | `BitAnd(:int,:int)` | yes |
| `(/Verse.org/Verse:)BitOr` | `(/Verse.org/Verse/(/Verse.org/Verse:)BitOr:)Native` | 2 | `BitOr(:int,:int)` | yes |
| `(/Verse.org/Verse:)BitXor` | `(/Verse.org/Verse/(/Verse.org/Verse:)BitXor:)Native` | 2 | `BitXor(:int,:int)` | yes |
| `(/Verse.org/Verse:)BitNot` | `(/Verse.org/Verse/(/Verse.org/Verse:)BitNot:)Native` | 1 | `BitNot(:int)` | yes |
| `(/Verse.org/Verse:)ConcatenateMaps` | `(/Verse.org/Verse/(/Verse.org/Verse:)ConcatenateMaps:)Native` | 2 | `ConcatenateMaps(:[k]v,:[k]v)` | yes |
| `(/Verse.org/Verse:)weak_map` | `(/Verse.org/Verse/(/Verse.org/Verse:)weak_map:)Native` | 1 | the type expression `weak_map(k, v)` | yes, if a script writes that type |
| `(/Verse.org/Verse:)MissingProcedure` | `(/Verse.org/Verse/(/Verse.org/Verse:)MissingProcedure:)Native` | 0 | a function slot of a native class never given a value | no (§9.4) |
| `(/Verse.org/Verse:)FitsInPlayerMap` | `(/Verse.org/Verse/(/Verse.org/Verse:)FitsInPlayerMap:)Native` | 1 | persistence checks | no (§9.4) |
| `(/Verse.org/Verse:)MakePersistentMap` | `(/Verse.org/Verse/(/Verse.org/Verse:)MakePersistentMap:)Native` | 5 | a module-scoped `var` `weak_map(player, t)` | no (§9.4) |
| `(/Verse.org/Verse:)MakeSessionVar` | `(/Verse.org/Verse/(/Verse.org/Verse:)MakeSessionVar:)Native` | 1 | a module-scoped `var` `weak_map(session, t)` | no (§9.4) |
| `(/Verse.org/Verse:)NotifyPersistentMapMutation` | `(/Verse.org/Verse/(/Verse.org/Verse:)NotifyPersistentMapMutation:)Native` | 3 | writes to a persistent map | no (§9.4) |

Behaviour of the reached ten. All answer **Return**; none can Fail or raise.

- **Abs.** An integer argument: its exact absolute value (an arbitrary-precision integer; `Abs` of
  −2^63 is 2^63). A float argument: the argument with its sign bit cleared (C `fabs`); NaN stays NaN.
  Measured: `Abs(-5) = 5`, `Abs(-2.5) = 2.5`, `Abs(-(2^63))` exceeds 2^63−1 (`A04_IntNatives`).
- **Ceil, Floor.** An integer argument is returned unchanged. A `rational` argument (the result of
  `int / int`) answers the exact ceiling or floor of numerator ÷ denominator as an integer, of any
  size. Measured: `Floor(7/2) = 3`, `Floor(-7/2) = -4`, `Ceil(-7/2) = -3`, `Ceil(7/2) = 4`, and the
  floor of 2^63/3 (`A04_IntNatives`, edges `A04_BigBits`). These are **not** the float
  `Floor[]`/`Ceil[]` of §5.2, which are different natives.
- **BitAnd, BitOr, BitXor, BitNot.** Bitwise operations on arbitrary-precision integers under
  infinite two's-complement semantics: a negative integer behaves as an infinite run of 1 bits to
  the left. `BitNot(x) = −x − 1`. Measured: `BitAnd(12, 10) = 8`, `BitOr(12, 10) = 14`,
  `BitXor(-1, 5) = -6`, `BitNot(0) = -1`; `BitAnd(-1, 2^63) = 2^63`, `BitNot(2^63) + 2^63 = -1`,
  `BitXor(2^63, 2^63) = 0` (`A04_IntNatives`, edges `A04_BigBits`).
- **ConcatenateMaps.** A new immutable map built from the first map's entries in order, then the
  second's. A key present in both keeps the **second** map's value and takes the **second** map's
  position: the rule for building a map from a pair sequence with a repeated key is that the last
  pair wins both value and position (`values.md`). Measured:
  `ConcatenateMaps(map{"0"=>0,"1"=>1}, map{"1"=>10,"0"=>11,"2"=>12})` iterates `1=>10, 0=>11, 2=>12`
  (`D01_Maps`).
- **weak_map.** The argument is a two-element array (key type, value type); the result is the
  weak-map type value for them (`values.md`, `objects.md` for what a type value is). (Source,
  unprobed.)

## 5. `/Verse.org/Verse`

The Verse plugin's natives. Package `Verse/Verse`. Every function here is module-level (`Self` is
`false`) except the methods listed in §5.10, and none has a wrapper. In the tables, *key* abbreviates the binding key as
`V:`*decorated*, meaning `(/Verse.org/Verse/`*decorated*`:)Native`, where *decorated* always begins
`(/Verse.org/Verse:)`.

### 5.1 Float functions

```
Sqrt<native><public>(X:float)<predicts><reads>:float
Sin<native><public>(X:float)<reads><predicts>:float
Cos<native><public>(X:float)<reads><predicts>:float
Tan<native><public>(X:float)<reads><predicts>:float
ArcSin<native><public>(X:float)<reads><predicts>:float
ArcCos<native><public>(X:float)<reads><predicts>:float
ArcTan<native><public>(X:float)<reads><predicts>:float
ArcTan<native><public>(Y:float, X:float)<reads><predicts>:float
Sinh<native><public>(X:float)<reads><predicts>:float
Cosh<native><public>(X:float)<reads><predicts>:float
Tanh<native><public>(X:float)<reads><predicts>:float
ArSinh<native><public>(X:float)<reads><predicts>:float
ArCosh<native><public>(X:float)<reads><predicts>:float
ArTanh<native><public>(X:float)<reads><predicts>:float
Pow<native><public>(A:float, B:float)<reads><predicts>:float
Exp<native><public>(X:float)<reads><predicts>:float
Ln<native><public>(X:float)<reads><predicts>:float
Lerp<native><public>(From:float, To:float, Parameter:float)<predicts><reads>:float
```

All: token **—**, outcome always **Return**, no raise, no writes. Each is computed in IEEE 754
binary64, round-to-nearest-even, with no fused multiply-add and no reassociation. "+0.0" below
means the input is first replaced by *x* + 0.0, which turns −0.0 into +0.0 and changes nothing
else.

| Function | *decorated* | Result |
| --- | --- | --- |
| Sqrt | `Sqrt(:float)` | C `sqrt` of (*X* + 0.0) |
| Sin | `Sin(:float)` | C `sin`(*X*) |
| Cos | `Cos(:float)` | C `cos`(*X*) |
| Tan | `Tan(:float)` | C `tan` of (*X* + 0.0) |
| ArcSin | `ArcSin(:float)` | C `asin`(*c*), where *c* = −1 if *X* < −1, else *X* if *X* < 1, else 1 |
| ArcCos | `ArcCos(:float)` | C `acos`(*c*), *c* as for ArcSin |
| ArcTan (one) | `ArcTan(:float)` | C `atan`(*X*) |
| ArcTan (two) | `ArcTan(:float,:float)` | +0.0 if *X* = 0 and *Y* = 0 (either sign), else C `atan2`(*Y*, *X*) |
| Sinh | `Sinh(:float)` | C `sinh`(*X*) |
| Cosh | `Cosh(:float)` | C `cosh`(*X*) |
| Tanh | `Tanh(:float)` | C `tanh`(*X*) |
| ArSinh | `ArSinh(:float)` | C `asinh`(*X*) |
| ArCosh | `ArCosh(:float)` | C `acosh`(*X*) |
| ArTanh | `ArTanh(:float)` | C `atanh`(*X*) |
| Pow | `Pow(:float,:float)` | C `pow`(*A*, *B*) |
| Exp | `Exp(:float)` | C `exp`(*X*) |
| Ln | `Ln(:float)` | C `log` of (*X* + 0.0) |
| Lerp | `Lerp(:float,:float,:float)` | (*From* × (1 − *Parameter*)) + (*To* × *Parameter*), each product rounded, then the sum |

Consequences worth a test, all measured (`vm_natives_probe.verse` `A01`–`A03`, edges `A01`,
`A02c`):

| Input | Result |
| --- | --- |
| `Sqrt(-1.0)`, `Sqrt(NaN)` | NaN |
| `Sqrt(Inf)` | Inf |
| `Sin(Inf)`, `Cos(Inf)`, `Tan(Inf)` | NaN |
| `ArcSin(2.0)`, `ArcSin(-2.0)` | ±1.570796… (clamped, **not** NaN) |
| `ArcCos(2.0)` | 0 |
| `ArcSin(NaN)` | **1.570796…** — NaN clamps to 1 because both comparisons fail |
| `ArcCos(NaN)` | **0** |
| `ArcTan(Inf)` | 1.570796… |
| `ArcTan(0.0, 0.0)` | 0 (C would give 0 or ±π by sign) |
| `ArcTan(0.0, -1.0)` | 3.141593… |
| `ArcTan(NaN, 0.0)` | NaN |
| `ArSinh(Inf)`, `ArTanh(1.0)` | Inf |
| `ArCosh(0.5)`, `ArTanh(2.0)` | NaN |
| `Exp(1000.0)` | Inf; `Exp(-1000.0)` 0 |
| `Ln(0.0)` | −Inf; `Ln(-1.0)` NaN |
| `Pow(0.0, 0.0)` | 1; `Pow(-8.0, 1/3)` NaN; `Pow(-2.0, 3.0)` −8 |
| `Lerp(0.0, 1.0, 2.0)` | 2 (extrapolates) |
| `Lerp(0.0, Inf, 0.0)` | NaN (0 × Inf); `Lerp(1.0, 2.0, NaN)` NaN |

**Negative zero never reaches these natives.** A negative zero computed at run time
(`0.0 * -X` with *X* obtained from a native, so the compiler cannot fold it) arrives as +0.0:
`ArcTan` of it against −1 answers +π, and `Sin`, `Sqrt`, `Tan`, `Abs` of it are all positive
(edges `A01_NegativeZero`). Why is `values.md`'s business; the +0.0 normalisations in the table
are therefore unobservable but must still be performed, in case a route to −0.0 exists that the
probes did not find.

**Bit-for-bit.** `sqrt` is exactly rounded by IEEE 754, so it is portable. The others are the
**Microsoft UCRT** implementations in Epic's Windows runtime, which are not guaranteed correctly
rounded; a different C library (Emscripten's, a Linux one) may differ in the last place for some
inputs. §11 records this; a differential fixture that compares through `ToString` (six decimals)
will not see a one-ulp difference.

Non-native float functions of the same module are bytecode in the program and need no native:
`Min`, `Max` and `Clamp` on floats, `Log` (which is `Ln(X) / Ln(B)`), `Sgn`, `IsFinite`,
`IsAlmostZero`, `IsAlmostEqual`, and the constant `PiFloat`.

### 5.2 Float to integer

```
Ceil<native><public>(Val:float)<reads><predicts><decides>:int
Floor<native><public>(Val:float)<reads><predicts><decides>:int
Round<native><public>(Val:float)<reads><predicts><decides>:int
Int<native><public>(Val:float)<reads><predicts><decides>:int
```

Token **waits**. *decorated*: `Ceil(:float)`, `Floor(:float)`, `Round(:float)`, `Int(:float)`.

1. If *Val* is not finite (NaN, +Inf, −Inf): **Fail**.
2. Otherwise *r* is: Ceil → C `ceil`(*Val*); Floor → C `floor`(*Val*); Int → C `trunc`(*Val*);
   Round → *Val* rounded to the nearest integer, ties to even (C `rint` under the default rounding
   mode).
3. **Ceil, Floor, Int:** if *r* ≥ 2^63 or *r* < −2^63, **Error** `ErrRuntime_IntegerBoundsExceeded`
   with the message
   `The value `*R*` cannot be converted to an integer because it does not fall inside the representable range of a Verse integer.`
   where *R* is *r* printed as C `%f` (fixed notation, six fraction digits). Otherwise **Return**
   *r* as an integer.
4. **Round** never raises. It answers: *r* when −2^63 ≤ *r* < 2^63; **−2^63** when *r* = 2^63
   exactly; and **0** when *r* > 2^63 or *r* < −2^63. This is what Epic's Windows runtime does
   (the 64-bit conversion saturates to −2^63 at exactly 2^63 and answers 0 beyond), and it is
   measured, not designed: reproduce it.

Measured (`facts.md` §5; `vm_natives_probe.verse` `A05`–`A08`; edges `A02`, `A02b`, `A03`):

| Input | Result |
| --- | --- |
| `Round[2.5]`, `Round[-2.5]`, `Round[0.5]`, `Round[-0.5]`, `Round[3.5]` | 2, −2, 0, 0, 4 |
| `Floor[-0.5]`, `Floor[0.5]`, `Ceil[-0.5]`, `Ceil[0.5]` | −1, 0, 0, 1 |
| `Int[-2.7]` | −2 |
| `Floor[Inf]`, `Round[NaN]` | fail |
| `Round[9.2e18]` | 9200000000000000000 |
| `Round[2^63]`, `Round[-2^63]` | −9223372036854775808, −9223372036854775808 |
| `Round[9.3e18]`, `Round[±1.2e19]`, `Round[1e19]`, `Round[-1e30]` | 0 |
| `Int[-2^63]` | −9223372036854775808 |
| `Floor[1e19]` | Error: `ErrRuntime_IntegerBoundsExceeded: A value does not fall inside the representable range of a Verse integer. (The value 10000000000000000000.000000 cannot be converted to an integer because it does not fall inside the representable range of a Verse integer.)` |
| `Ceil[2^63]` | Error, same form, value `9223372036854775808.000000` |

### 5.3 Integer functions

```
Clamp<native><public>(Val:int, A:int, B:int)<computes><predicts>:int
Quotient<native><public>(X:int, Y:int)<computes><predicts><decides>:int
Mod<native><public>(X:int, Y:int)<computes><predicts><decides>:int
```

Token **waits** for all three. Every argument must fit 64 bits (§3.6).

- **Clamp** (`Clamp(:int,:int,:int)`): with *Lo* = min(*A*, *B*) and *Hi* = max(*A*, *B*),
  **Return** max(min(*Val*, *Hi*), *Lo*) — the median of the three. Measured: `Clamp(5, 10, 0) = 5`,
  `Clamp(-5, 10, 0) = 0`, `Clamp(15, 10, 0) = 10` (`A04_IntNatives`). The float `Clamp` is bytecode.
- **Quotient** (`Quotient(:int,:int)`) and **Mod** (`Mod(:int,:int)`): Euclidean division.
  1. *Y* = 0: **Fail**.
  2. *X* = −2^63 and *Y* = −1: **Error** `ErrRuntime_IntegerOverflow` with the message
     `Integer overflow encountered.` — for **both** natives, although the Euclidean remainder
     would be 0 (measured for Quotient, `A09_QuotientOverflow`; Mod from source).
  3. Otherwise let *q* and *m* be C's truncating `/` and `%`. If *m* < 0: Mod answers *m* + |*Y*|
     and Quotient answers *q* − 1 when *Y* > 0, *q* + 1 when *Y* < 0. Else Mod answers *m* and
     Quotient *q*. So 0 ≤ Mod < |*Y*| and Quotient × *Y* + Mod = *X*.

  Measured (`facts.md` §2): `Mod[7,2]=1, Mod[-7,2]=1, Mod[7,-2]=1, Mod[-7,-2]=1`;
  `Quotient[7,2]=3, Quotient[-7,2]=-4, Quotient[7,-2]=-3, Quotient[-7,-2]=4`; both fail for *Y* = 0.

`Min`, `Max`, `Sgn` on integers are bytecode.

### 5.4 Strings

```
ToString<native><public>(Val:int)<computes><predicts>:string
ToString<native><public>(Val:float)<predicts><reads>:string
ToString<native><public>(Character:char)<computes><predicts>:string
ToString<native><epic_internal>(Character:char32)<computes><predicts>:string
Join<native><public>(Strings:[]string, Separator:string)<computes><predicts>:string
```

Token **waits** for all five. String interpolation of an `int` or `float` calls the same natives
(`facts.md` §3, §4).

| *decorated* | Result |
| --- | --- |
| `ToString(:int)` | the decimal digits, a leading `-` for a negative value, no grouping, no `+`. Argument outside 64 bits: the §3.6 Error |
| `ToString(:float)` | NaN → `NaN`; +Inf → `Inf`; −Inf → `-Inf`; otherwise C `%f`: an optional `-`, the integer part in full, `.`, exactly six fraction digits. The digits are those of the **exact** binary value rounded to six places, a tie (exact) going to the even digit |
| `ToString(:char)` | a one-byte string holding that code unit unchanged, even when it is not valid UTF-8 on its own |
| `ToString(:char32)` | the UTF-8 encoding of the code point (1 to 4 bytes) |
| `Join(:[][]char,:[]char)` | the elements concatenated with *Separator* between consecutive ones; an empty array gives the empty string; one element gives that element |

Measured (`facts.md` §3; `B01_Strings`; edges `A02c`, `A05`):

| Call | Result |
| --- | --- |
| `ToString(1.0e20)` | `100000000000000000000.000000` |
| `ToString(0.0078125)`, `ToString(0.0234375)` | `0.007812`, `0.023438` (exact ties, to even) |
| `ToString(0.0000005)` | `0.000000` (the double is just below the tie) |
| `ToString(-0.0000001)`, `ToString(-1.5)` | `-0.000000`, `-1.500000` |
| `ToString('a')`; `ToString(0o80)` | `a`; a one-byte string |
| `ToString(0u00e9)`, `ToString(0u1f600)`, `ToString(0u10ffff)` | 2, 4 and 4 bytes |
| `Join(array{"a","b","c"}, ", ")`, `Join(array{}, "x")`, `Join(array{"a"}, "-")` | `a, b, c`, empty, `a` |

Epic's own test corpus also prints `1e100` and the largest double in full (source, unprobed): the
integer part is never abbreviated, which is why a correct implementation needs an exact decimal
expansion rather than a shortest-round-trip printer.

**Join's limit.** If the result would exceed 2^31 − 1 bytes, **Error** `ErrRuntime_InvalidArrayLength`
with the message `Invalid array length.` (source, unprobed; the interpreter may impose a lower
limit on wasm32 with the same error).

`ToString(:string)` is bytecode (it returns its argument).

### 5.5 `Err` and `Warn`

```
Err<public><native>(Message:string)<computes><predicts>:false
Warn<epic_internal><native>(Message:string)<computes><predicts>:void
```

Token **waits** for both. *decorated*: `Err(:[]char)`, `Warn(:[]char)`.

- **Err** never returns: **Error** `ErrorRequested` with the message `User Message: '`*Message*`'`,
  *Message* inserted byte for byte with no escaping. Measured text:
  `ErrorRequested: A runtime error was explicitly raised from user code. (User Message: 'boom')`, and
  for `Err("")`, `... (User Message: '')` (`B02_Err`, `B04_ErrEmpty`).
- **Warn** answers **Return** `false` (the value every `void` native returns) and execution
  continues (`B03_Warn` printed its next line). It is a **warning**, not an error: nothing reaches
  the embedder's runtime-error callback and nothing is rolled back. Epic's runtime writes
  `WarningRequested: A runtime warning was explicitly raised from user code. (User Message: '`*Message*`')`
  and a truncated stack to its own log only. The interpreter may log the same line to stderr; it
  must not report it as an error.

### 5.6 `GetSecondsSinceEpoch`

```
GetSecondsSinceEpoch<public><native>()<reads>:float
```

Token **waits**. *decorated*: `GetSecondsSinceEpoch`. **Return** Unix time in seconds (UTC, leap
seconds ignored) as a float, with a fractional part.

The value is taken from a clock sample Epic's runtime refreshes once per engine frame. **This host
runs no engine frame, so the sample is taken once when the runtime starts and never changes**:
two calls in different ticks answered the same `1790305622.904000` (`C01_Epoch`, `C02_EpochAgain`).
The sample has millisecond resolution on Windows. The interpreter must reproduce the observable
contract: one sample taken at `vh_init`, the same value for every call for the life of the process
(§11 asks whether that is what we want).

### 5.7 `ToDiagnostic`

```
ToDiagnostic<public><native>(Value:any)<predicts><reads>:diagnostic
```

Token **waits**. *decorated*: `ToDiagnostic(:any)`. **Return** a new `diagnostic` object whose
`String` field is the VM's diagnostic rendering of *Value*. `diagnostic.String` is `<epic_internal>`
and so readable by a script (§2.2). Measured (`I01_Diagnostic`):

| *Value* | `String` |
| --- | --- |
| `42` | `42` |
| `1.5` | `1.5` (not `ToString`'s `1.500000`) |
| `"s"` | `"s"` with the quotes |
| `array{1, 2}` | `(1, 2)` |
| `true` | `true` |
| `option{1}` | `option{1}` |

The general rendering belongs with value printing in `values.md`; §11 lists what is unmeasured. An
object of a class implementing `diagnosable` renders as its `GetDiagnostic()` result's `String`.

### 5.8 Messages

```
Localize<public><native>(Message:message)<reads>: string
Join<native><public>(Messages:[]message, Separator:message)<transacts>:message
MakeMessageInternal<native><epic_internal>(K:string, D:string, S:[string]localizable_value)<converges>: message
MakeMessageLiteral<native><epic_internal>(K:string, D:string)<converges>: message
```

A `message` is an object with three fields: `Key` (string), `DefaultText` (string) and
`Substitutions` (a map from name to `localizable_value`, whose subclasses `localizable_string`,
`localizable_int`, `localizable_float`, `localizable_message` each hold one `Value`). A script
produces one by declaring `Name<localizes>:message = "..."` or a `<localizes>` function; the
compiler turns that into a call to `MakeMessageLiteral` (no interpolations) or `MakeMessageInternal`
(with them, building the substitution map through the bytecode helpers `MakeLocalizableValue`).
Such `<localizes>` functions carry the default effects, so they cannot be called from `<transacts>`
code (measured: the probe had to drop `<transacts>` from `H01`/`H02`).

| *decorated* | Token | Result |
| --- | --- | --- |
| `MakeMessageLiteral(:[]char,:[]char)` | waits | a new `message` with `Key` *K*, `DefaultText` *D*, no substitutions |
| `MakeMessageInternal(:[]char,:[]char,:[[]char](/Verse.org/Verse:)localizable_value)` | waits | a new `message` with the three fields as given |
| `Join(:[](/Verse.org/Verse:)message,:(/Verse.org/Verse:)message)` | waits | none → a new message with empty key and empty default text; one → **that same object**; *n* ≥ 2 → a new message with empty key, default text `{0}{s}{1}{s}…{s}{`*n*−1`}`, and substitutions `"0"`…`"`*n*−1`"` → the messages and `"s"` → *Separator* |
| `Localize(:(/Verse.org/Verse:)message)` | waits | the string below |

**Localize.** This program has no translation tables, so the key never changes the text. If the
message has no substitutions, the result is `DefaultText` unchanged. Otherwise `DefaultText` is a
format string in which `{`*name*`}` is replaced by the rendering of substitution *name*: a string
as itself, a message by its own `Localize`, an integer and a float by the formatter's number
rendering. Measured (`H01_Localize`, `H02_JoinMessages`, edges `A06_LocalizeFloats`):

| Message | `Localize` |
| --- | --- |
| `Hello<localizes>:message = "Hello"` | `Hello` |
| `"n={N}"` with 1234567, −5 | `n=1,234,567`, `n=-5` |
| `"f={F}"` with 1234.5678 | `f=1,234.568` |
| `"{F}"` with 0.5, 2.0, −1234.5, 0.0001, 0.0005 | `0.5`, `2`, `-1,234.5`, `0`, `0` |
| `"{F}"` with 1.0e20, NaN, Inf | `,100,000,000,000,000,000,000.0`, `NaN`, `inf` |
| `"s={S}"` with `"x"` | `s=x` |
| `Join(array{Hello, Named("y"), Count(3)}, Hello)` | `HelloHellos=yHellon=3` |
| `Join(array{}, Hello)` | empty |

Numbers render with `,` grouping every three digits, at most three fraction digits with trailing
zeros removed, and the two oddities in the last-but-one row. The formatter's full grammar (escapes
with a backtick, argument modifiers, which are disabled for untranslated text) and its exact float
rounding are not settled here; §11. The non-native overloads `Localize(:string)`,
`Localize(:int)`, `Localize(:float)` are bytecode (`int` and `float` go through `ToString`, so they
do **not** group).

### 5.9 Type and caller queries

```
GetCastableFinalSuperClass<epic_internal><native>(base_type:type, Instance:base_type)<reads><decides> : castable_subtype(base_type)
GetCastableFinalSuperClassFromType<epic_internal><native>(base_type:type, sub_type:subtype(base_type))<reads><decides> : castable_subtype(base_type)
(InInstance:t where t:castable_subtype(any)).IsOfType<native><internal>(query_type:castable_subtype(any))<reads><decides>:void
CanCallerAccessEpicInternal_Impl<epic_internal><native>()<transacts>:logic
```

All wait for the token. (Source, unprobed, except `CanCallerAccessEpicInternal`.)

- **GetCastableFinalSuperClassFromType** (`GetCastableFinalSuperClassFromType(:base_type,:sub_type where base_type,sub_type)`):
  walk from *sub_type* up its superclass chain. At each class *C* with parent *P*: if *P* is
  *base_type*, or *base_type* is an interface that *C* lists **directly** among its interfaces, stop;
  **Return** *C* if *C* carries both the `ExplicitlyCastable` and `FinalSuper` class flags
  (`ops.json` `ClassFlags`), else **Fail**. If the chain ends without stopping, or *sub_type* is
  *base_type* itself, **Fail**. Either argument not a class type: **Fail**.
- **GetCastableFinalSuperClass** (`GetCastableFinalSuperClass(:base_type,:base_type where base_type)`):
  **Fail** if *Instance* is not a class instance (a struct, a primitive); otherwise the above with
  *Instance*'s class as *sub_type*.
- **IsOfType** (`operator'.IsOfType'(:t,:query_type where t,query_type)`): **Return** if
  *InInstance*'s dynamic type is a subtype of *query_type*, **Fail** otherwise.
- **CanCallerAccessEpicInternal_Impl** (`CanCallerAccessEpicInternal_Impl`): reached only through
  the bytecode function `CanCallerAccessEpicInternal`, which calls it. It looks three frames up the
  call stack from itself — past `CanCallerAccessEpicInternal`, past the function that called that,
  to *that* function's caller — and **Returns** `true` if that frame is native code (including the
  embedder's entry into the VM) or a bytecode procedure whose "can access `epic_internal`" flag is
  set (`format.md` §5, procedure flag bit 0), else `false`; `false` also if there is no such frame.
  Measured: called from a script method the embedder invoked, `true` (`G01_EpicInternal`).

### 5.10 `classifiable_subset`

`@experimental`, which this project's packages allow. (Source, unprobed.) A `classifiable_subset(t)`
object holds `Elements`, a map from a fresh `classifiable_subset_key(t)` object (a `<unique>`
class, so compared by identity) to an element, in insertion order. A `classifiable_subset_var(t)`
holds one current `classifiable_subset(t)`. Every native that "makes a new set" allocates a new
object; none mutates an existing set's `Elements`.

| *decorated* | Token | Result |
| --- | --- | --- |
| `MakeClassifiableSubset(:[]t where t)` | waits | new set, one new key per element, in order |
| `operator'+'(:(/Verse.org/Verse:)classifiable_subset(t),:(/Verse.org/Verse:)classifiable_subset(t) where t)` | waits | new set: the left set's entries then the right's; a key in both keeps its left position and takes the right value |
| `operator'.FilterByType'(:(/Verse.org/Verse:)classifiable_subset(t),:element_type where t,k,element_type)` | waits | new set: the entries whose element is of *element_type* (as `IsOfType`), keys kept |
| `MakeClassifiableSubsetVar(:[]t where t)` | waits | new var holding `MakeClassifiableSubset` of the elements |
| `operator'.Add'(:(/Verse.org/Verse:)classifiable_subset_var(t),:t where t)` | waits | new key; the var now holds a new set = old entries + (key → element); **Return** the key |
| `operator'.Remove'(:(/Verse.org/Verse:)classifiable_subset_var(t),:(/Verse.org/Verse:)classifiable_subset_key(t) where t)` | waits | **Fail** if the key is not in the current set; else the var holds a new set without it |

Methods (key `(/Verse.org/Verse/`*class*`/`*decorated*`:)Native`):

| Class | *decorated* | Result |
| --- | --- | --- |
| `classifiable_subset_var` | `(/Verse.org/Verse/classifiable_subset_var:)Read` | the current set |
| `classifiable_subset_var` | `(/Verse.org/Verse/classifiable_subset_var:)Write(:(/Verse.org/Verse:)classifiable_subset(element_type))` | replace the current set |
| `classifiable_subset` | `(/Verse.org/Verse/diagnosable:)GetDiagnostic` | a `diagnostic`; text below |
| `classifiable_subset_key` | `(/Verse.org/Verse/diagnosable:)GetDiagnostic` | a `diagnostic`; text below |

The two `GetDiagnostic` texts embed Unreal object paths (`<full name>{{elem}{elem}…}` and the key's
full name), which no other runtime can reproduce; they are implementation-defined for the
interpreter (§11). Every write here is transactional (§3.8).

### 5.11 `Easing`

```
Easing<public> := module:
    CubicBezierInterpInternal<internal><native>(Value:float, X1:float, Y1:float, X2:float, Y2:float)<reads>:float
    CubicBezier<public><native>(X1:float, Y1:float, X2:float, Y2:float)<converges>:type{_(:float)<reads>:float}
```

Keys under `/Verse.org/Verse/Easing`, *decorated* `(/Verse.org/Verse/Easing:)CubicBezierInterpInternal(:float,:float,:float,:float,:float)`
and `(/Verse.org/Verse/Easing:)CubicBezier(:float,:float,:float,:float)`. Both wait for the token.
`@experimental`. `Linear`, `Ease`, `EaseIn`, `EaseOut`, `EaseInOut` are bytecode calling the first.

**CubicBezierInterpInternal**(*T*, *X0*, *Y0*, *X1*, *Y1*) — the CSS cubic-bézier easing with
control points (0,0), (*X0*,*Y0*), (*X1*,*Y1*), (1,1), evaluated at progress *T*. **Return**:

1. *T* unchanged, unless 0 ≤ *X0* ≤ 1 and 0 ≤ *X1* ≤ 1 (Verse's float ordering, `values.md`: a NaN
   fails every comparison here).
2. *T* unchanged if *X0* = *Y0* and *X1* = *Y1* (Verse's float equality, reflexive for NaN).
3. Otherwise, in binary64 with every operation rounded as written:
   - *a* = *X0* − 0, *b* = *X1* − *X0*, *c* = 1 − *X1*, *d* = *b* − *a*;
     *c3* = *c* − *b* − *d* (left to right), *c2* = 3 × *d*, *c1* = 3 × *a*, *c0* = 0 − *T*.
   - Newton iteration for the root of *c3*·*r*³ + *c2*·*r*² + *c1*·*r* + *c0* on [0, 1]: start
     *r* = *T*, *step* = 1. While fewer than 10 iterations have run and |*step*| > *ε*, where *ε* is
     the single-precision number nearest 0.0001 widened to binary64 (9.99999974737875e-05):
     *N* = ((*c3*·*r* + *c2*)·*r* + *c1*)·*r* + *c0*; *D* = ((3·*c3*)·*r* + (2·*c2*))·*r* + *c1*;
     *r′* = clamp(*r* − *N* ÷ *D*, 0, 1), where the division treats a zero *D* as +0.0 and
     clamp(*x*, 0, 1) = max(min(*x*, 1), 0) with min(*p*, *q*) = *p* if *p* < *q* else *q* and
     max(*p*, *q*) = *p* if *q* < *p* else *q* (so a NaN becomes 1);
     *step* = *r′* − *r*; *r* = *r′*.
   - With lerp(*p*, *q*, *s*) = *p* + *s*·(*q* − *p*): *e* = lerp(0, *Y0*, *r*),
     *f* = lerp(*Y0*, *Y1*, *r*), *g* = lerp(*Y1*, 1, *r*), *h* = lerp(*e*, *f*, *r*),
     *i* = lerp(*f*, *g*, *r*); the result is lerp(*h*, *i*, *r*).

**CubicBezier**(*X1*, *Y1*, *X2*, *Y2*): **Return** a function value: a new instance of the class
`Easing.cubic_bezier_capture_internal` with its fields `X1`, `Y1`, `X2`, `Y2` set to the arguments,
and that instance's method `Evaluate` bound to it (method key
`(/Verse.org/Verse/Easing/cubic_bezier_capture_internal:)Evaluate(:float)`, which is bytecode
calling `CubicBezierInterpInternal(Value, X1, Y1, X2, Y2)`).

Measured (`J01_Easing`): `Linear(0.3) = 0.300000`, `Ease(0.25) = 0.408511`,
`EaseIn(0.5) = 0.315357`, `EaseOut(0.5) = 0.684643`, `EaseInOut(0.25) = 0.129162`,
`CubicBezier(0.42, 0.0, 0.58, 1.0)(0.25) = 0.129162`, `CubicBezier(2.0, 0.0, 0.5, 1.0)(0.3) = 0.300000`.

## 6. `/Verse.org/Random`

```
GetRandomFloat<native><public>(Low:float, High:float)<transacts>:float
GetRandomInt<native><public>(Low:int, High:int)<transacts>:int
```

Keys `(/Verse.org/Random/(/Verse.org/Random:)GetRandomFloat(:float,:float):)Native` and
`(/Verse.org/Random/(/Verse.org/Random:)GetRandomInt(:int,:int):)Native`. Both wait for the token;
integer arguments must fit 64 bits. `Shuffle` is bytecode over `GetRandomInt`.

**The generator is not seeded by anything a program controls and is not reproducible across
runs**: Epic's runtime seeds it from operating-system entropy at first use. No test can observe
the sequence; what is observable is only the
contract below, and the interpreter may use any cryptographically secure or ordinary uniform
source (§11). Each draw is a 64-bit unsigned value *u*. The generator's advance is **not** undone
by a failing transaction (measured harmless: a failing `if` around a draw kept nothing,
`C04_RandomRollback`).

**GetRandomInt**(*Low*, *High*): if equal, **Return** *Low* without drawing. Otherwise swap so
*Low* < *High*; *R* = *High* − *Low* as an unsigned 64-bit value; *mask* = all bits from the
highest set bit of *R* down; draw *u* repeatedly until (*u* AND *mask*) ≤ *R*; **Return**
*Low* + (*u* AND *mask*). Uniform over [*Low*, *High*] inclusive. Measured: `GetRandomInt(5, 5) = 5`,
`GetRandomInt(3, 1)` ∈ [1, 3], and 300 draws of `(0, 2)` hit all three values (`C03_Random`,
edges `A07_RandomSpread`).

**GetRandomFloat**(*Low*, *High*): if *Low* = *High* (IEEE equality), **Return** *Low*. If
*Low* > *High*, swap. Draw *u*; *t* = (*u* shifted right 11) × 2^−53, so 0 ≤ *t* < 1; *v* =
((1 − *t*) × *Low*) + (*t* × *High*); **Return** max(min(*v*, *High*), *Low*) with min/max as in
§5.11 (the first operand is kept only when the comparison succeeds). The comparisons make the NaN
and infinite cases exact, and they are measured (`C03_Random`):

| Call | Result |
| --- | --- |
| `GetRandomFloat(2.0, 2.0)` | 2 |
| `GetRandomFloat(-Inf, Inf)` | +Inf (every *v* is NaN, and the clamp answers the upper bound) |
| `GetRandomFloat(0.0, NaN)` | 0 |
| `GetRandomFloat(NaN, 1.0)` | NaN |

## 7. `event(t)` and its relatives

```
event<native><public>(t:type) := class(event_base_intrnl, signalable(t), awaitable(t)):
    Await<native><override>()<suspends>:t
    Signal<native><override>(Val:t):void
subscribable_event_intrnl<native><epic_internal>(t:type) := class(event(t), listenable(t)):
    Subscribe<native><override>(Callback(:t):void)<transacts>:cancelable
    Signal<native><override>(Val:t)<predicts>:void
event_subscription<native><internal> := class(cancelable):
    Cancel<override><native>()<transacts>:void
```

| Class | Method | Binding key | *k* | Token | Outcomes |
| --- | --- | --- | --- | --- | --- |
| `event` | `Await` | `(/Verse.org/Verse/event/Await:)Native` | 0 | waits | Yield (or Return on resumption, §7.1) |
| `event` | `Signal` | `(/Verse.org/Verse/event/(/Verse.org/Verse/signalable:)Signal(:payload):)Native` | 1 | waits | Return |
| `subscribable_event_intrnl` | `Subscribe` | `(/Verse.org/Verse/subscribable_event_intrnl/(/Verse.org/Verse/subscribable:)Subscribe(:t->void):)Native` | 1 | waits | Return |
| `subscribable_event_intrnl` | `Signal` | `(/Verse.org/Verse/subscribable_event_intrnl/(/Verse.org/Verse/signalable:)Signal(:payload):)Native` | 1 | waits | Return |
| `event_subscription` | `Cancel` | `(/Verse.org/Verse/event_subscription/(/Verse.org/Verse/cancelable:)Cancel:)Native` | 0 | waits | Return |

`subscribable_event_intrnl` inherits `event`'s `Await`. `event()` is `event(tuple())`, whose
payload is the empty tuple. `Self` is the event (or subscription). Neither `Signal` may be called
from a failure context (its effects include `no_rollback`), so a `Signal` is never rolled back.

**State.** Every `event` object carries, beyond its Verse fields, two hidden lists that start
empty: the **awaiters** (tasks, first-in first-out) and the **subscriptions** (callback, owning
task scope, subscription object). `godot-natives.md` §8.17–§8.18 adds entries to the same
subscription list for an `@export_signal` event, which is why plain `event.Signal` runs
subscriptions too.

### 7.1 `Await`

Appends the current task to the awaiters, attaches a cleanup that removes it again if the task is
cancelled or unwinds before it is resumed, and answers **Yield**. When a `Signal` resumes the task,
the call's value is that `Signal`'s *Val*. If the event is never signalled, or is collected, the
task never resumes — no error. Measured in Epic's tests (source) and here: three waiters spawned in
one call, resumed in spawn order by one `Signal` (`E01_StartWaiters`, `E02_Signal`).

### 7.2 `Signal`

1. Take the whole awaiter list as one **batch** and leave the list empty, so a task that awaits the
   event while this `Signal` is running — including a resumed task that awaits again — is not
   resumed by it.
2. Resume the batch's tasks one at a time in FIFO order. Each runs synchronously, inside this call,
   until it next suspends or completes; then the next. A batch member cancelled before its turn is
   skipped. A `Signal` of the same event made from inside a resumed task runs to completion
   immediately with the awaiters registered since step 1 (its own batch), then the outer batch
   continues.
3. Then call every subscription's callback with *Val* (§7.3).
4. **Return**. Measured: all three waiters printed before `Signal` returned; a second `Signal` with
   no waiters is a no-op (`E02_Signal`).

### 7.3 Subscriptions

**Subscribe**(*Callback*): creates a new `event_subscription` object bound to this event and to the
**task scope of the current script instance** (`tasks.md`, R-ASYNC-4), records the callback, and
**Returns** the subscription. The recording is transactional (§3.8): a `Subscribe` inside a failure
context that fails leaves no subscription, which is what Epic's own tests assert (source). When the
owning task scope is cleaned up (its instance is released) the subscription is removed and the
subscription object reset.

**Calling the callbacks** (step 3 of §7.2): the set of subscriptions is snapshotted when the step
starts; each is called with *Val* unless it has been cancelled since, or its owning task scope has
been terminated, in which case it is skipped. Each callback runs in its own transaction under its
owner's task scope. **The order is not the subscription order: Epic's runtime visits the
subscriptions in a fresh uniformly random permutation on every `Signal`** (measured: five
subscriptions ran as `2 1 4 5 3` and then `2 5 3 1 4`, `E03_SubscribeOrder`). Any order is
conforming for the interpreter; a fixture must not depend on it.

**Cancel** (on the subscription): if it is still bound to an event, remove it from that event's
subscriptions (transactionally) and reset it; a second `Cancel`, or a `Cancel` after its scope was
cleaned up, does nothing (measured, `E03_SubscribeOrder`). **Return**.

## 8. `task(t)`

```
task<native><public>(t:type) := class<abstract><final>(awaitable(t)):
    Active<native><epic_internal>()<transacts><decides>:void
    Completed<native><epic_internal>()<transacts><decides>:void
    Canceling<native><epic_internal>()<transacts><decides>:void
    Canceled<native><epic_internal>()<transacts><decides>:void
    Unsettled<native><epic_internal>()<transacts><decides>:void
    Settled<native><epic_internal>()<transacts><decides>:void
    Uninterrupted<native><epic_internal>()<transacts><decides>:void
    Interrupted<native><epic_internal>()<transacts><decides>:void
    Await<native><override>()<suspends>:t
    Cancel<native><epic_internal>()<suspends>:void
```

Package: VerseNative; module `/Verse.org/Concurrency`. `Self` is the task — the value `spawn`
answers (`tasks.md`). None of the ten waits for the token. *k* is 0 for all.

| Method | Binding key |
| --- | --- |
| `Active` … `Interrupted` | `(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)`*Name*`:)Native` |
| `Await` | `(/Verse.org/Concurrency/task/Await:)Native` |
| `Cancel` | `(/Verse.org/Concurrency/task/Cancel:)Native` |

### 8.1 Phase queries

Each **Returns** `false` (the `void` value) when its condition holds and **Fails** otherwise.
`tasks.md` owns the phases; in its terms a task is *running normally*, *cancel requested* (a cancel
has been asked for and the task has not yet reached a point where it can unwind), *cancelling*
(its children are being cancelled or it is unwinding and running `defer`s), or *cancelled*; and
separately it may be *completed* (it has a result).

| Query | Succeeds when |
| --- | --- |
| `Active` | not completed, and running normally or cancel requested |
| `Completed` | completed |
| `Canceling` | cancelling (after the request, before cancelled) |
| `Canceled` | cancelled |
| `Unsettled` | not cancelled and not completed |
| `Settled` | cancelled or completed |
| `Uninterrupted` | running normally (completed or not) |
| `Interrupted` | anything but running normally |

Measured (`F02`, `F04`, `F06`): a task suspended in `Await` answers Active, Unsettled,
Uninterrupted; after it completes, Completed, Settled, Uninterrupted; after `Cancel` returns in
the canceller, Canceled, Interrupted, Settled.

### 8.2 `Await`

If the task has completed, **Return** its result at once (measured: `105`, `F05_AwaitCompleted`).
Otherwise append the current task to the task's awaiters (first-in first-out), attach a cleanup
that removes it if the awaiting task is cancelled, and **Yield**; completion resumes the awaiters
in order with the result. The awaiting task is not a child of the awaited one: cancelling either
does not cancel the other, and an awaiter of a task that is cancelled is never resumed.

### 8.3 `Cancel`

If the task has already completed or been cancelled: **Return** at once. Otherwise request
cancellation (`tasks.md`): if the task can unwind now, its children are cancelled newest first,
it unwinds running its `defer` blocks, and `Cancel` **Returns**; if it cannot yet (it or one of its
descendants is running — for instance the caller is cancelling itself or an ancestor), the caller
is appended to the task's cancel-waiters, a cleanup is attached as for `Await`, and `Cancel`
**Yields** until the cancellation finishes. A caller outside a task (none in this program) fires
and forgets. Everything else about cancellation is `tasks.md`.

## 9. Named but not in contract

### 9.1 Not in the program at all

| Name | Where it would come from | Evidence |
| --- | --- | --- |
| `/Verse.org/Verse`'s `Print(Message, ?Duration, ?Color)` and its `message`/`diagnostic` overloads | the VersePrint plugin, not linked | `Print("x", ?Duration := 1.0)` is refused: the only `Print` takes a `[]char` (`vm_natives_reach_reject.verse`, `C02`) |
| `/Verse.org/Colors`: `color`, `NamedColors`, `MakeColorFromHex`, … | the VerseColors plugin, not linked | `using { /Verse.org/Colors }` is refused (`C01`) |
| `/Verse.org/Simulation`'s `Sleep`, `session`, `player`, `GetSession` | the VerseSimulation plugin, not linked | a bare `Sleep(0.0)` resolves to `/Godot.org/Godot`'s (`C04` compiles) |

So there is no default `Color` argument initialising a colour module, and no `Print` other than the
Godot one (`godot-natives.md` §8.1).

### 9.2 `/Verse.org/SpatialMath`

A script can name it (`IdentityRotation()` compiles, `C03`; its `vector3` collides with the Godot
mirror's and must be qualified). Its natives are `MakeRotationRadians`,
`MakeRotationFromYawPitchRollDegrees`, `MakeRotationFromEulerRadians`, `IdentityRotation`,
`Distance` and `AngularDistanceRadians` on rotations, two `operator'*'`, and the rotation methods
`GetYawPitchRollDegrees`, `GetEulerRadians`, `GetAxis`, `GetAngleRadians`, `Invert`, `IsFinite`,
plus `MakeShortestRotationBetween` and `Slerp`. They operate on `rotation`, an opaque native struct
whose representation is Epic's C++ quaternion, and `format.md` v1 has no cell kind for a native
struct. **No script in this repository names the module.** Out of contract for this phase: the
loader binds these keys to the §3.2 stub, and a program using one fails with that stub's error at
the first call. Specifying them means specifying Epic's quaternion arithmetic bit for bit; that is
a later task if a game needs it.

### 9.3 `/Verse.org/Predicts` and `/Verse.org/Persona`

Both are `<epic_internal>` modules a script package can reach (§2.2), with no meaning in a Godot
game: the 31 `Predicts…` natives drive Epic's client-prediction networking (debug modes, object
registration, replication tests), and `Persona.ToJson`, `FromJson` (two each) and `Cast` convert
between Verse types and JSON schemas for Epic's persona tooling. No code the compiler generates for
a script calls them. Out of contract: bound to the §3.2 stub.

### 9.4 Unreachable intrinsics

- **FitsInPlayerMap, MakePersistentMap, NotifyPersistentMapMutation, MakeSessionVar** are emitted
  only for persistent or session-scoped module `var`s, whose key types (`player`, `session`) live
  in the unlinked simulation library. The interpreter binds them to the stub.
- **MissingProcedure** stands in for a function-typed field of a native class that was never given
  a value; no class in this program has one. If called it raises `ErrRuntime_InvalidFunctionCall`
  with the message `Attempted to call an uninitialized function.`, i.e.
  `ErrRuntime_InvalidFunctionCall: Attempted to call an invalid function. (Attempted to call an uninitialized function.)`
  (source, unprobed); the interpreter should implement that text rather than the stub, since the
  writer may still serialise the cell.

## 10. Library code that is not native

The program also contains the bytecode of the non-native functions in the same files. They need no
native and are listed so that nobody writes one: `Min`, `Max`, float `Clamp`, `Log`, `Sgn`,
`IsFinite`, `IsAlmostZero`, `IsAlmostEqual`, `PiFloat`; `ToString(:string)`; the array helpers
(`Concatenate`, `Slice`, `Insert`, `RemoveElement`, `RemoveFirstElement`, `RemoveAllElements`,
`ReplaceElement`, `ReplaceFirstElement`, `ReplaceAllElements`, `ReplaceAll`, `Remove`, `Find`,
`Last`); `Shuffle`; `MakeSuccess`, `MakeError` and the `result` classes; the `diagnostic` operators;
`Localize` of a string, int or float and `MakeLocalizableValue`; `CanCallerAccessEpicInternal`;
`Easing.Linear`, `Ease`, `EaseIn`, `EaseOut`, `EaseInOut` and `cubic_bezier_capture_internal.Evaluate`;
the `classifiable_subset` `Contains*` helpers; and the `listenable`, `subscribable`,
`awaitable`, `signalable`, `cancelable`, `disposable`, `enableable`, `invalidatable`, `showable`
interfaces, none of which has a native member.

## 11. Open questions

1. **libm.** §5.1 names C functions and says Epic's results are the Windows UCRT's. Must the
   interpreter be bit-identical to UCRT for `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`,
   `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `pow`, `exp`, `log`? If yes, it needs a
   UCRT-compatible implementation on wasm, which does not exist off the shelf. The recommendation is
   no: use the platform's libm, compare through `ToString` in fixtures, and record any fixture that
   differs as a known divergence.
2. **`GetSecondsSinceEpoch` is frozen** in this host (§5.6) because the engine's per-frame refresh
   never runs. Reproduce the freeze (conformance) or refresh per `vh_tick` (what the declaration's
   documentation implies)? The spec says freeze; the lead should decide, and `CLAUDE.md` should
   record the UE host's behaviour either way.
3. **Localize's number formatting.** Integer grouping and "at most three fraction digits" are
   measured; the float rounding rule (0.0005 renders `0`), `inf` in lower case, and the leading
   comma and `.0` of 1.0e20 are measured quirks without a rule. Full conformance means
   reimplementing the relevant part of Unreal's text formatter. Proposal: implement grouping, the
   three-digit float rendering with the quirks in §5.8 as special cases, and `{name}` substitution;
   treat the backtick escape and argument modifiers as unsupported until a fixture needs them.
4. **`ToDiagnostic`'s general rendering** (objects, maps, tuples, structs, floats in general) is
   not measured beyond §5.7. It should be written once, in `values.md`, beside the other value
   printer.
5. **`ToString(:char32)` of an unpaired surrogate** — whether a `char32` can hold one at all, and
   what bytes result — is not settled.
6. **Unbound-native text.** §3.2's load-time refusal and stub error are our design, since Epic's
   runtime has no text for the case. The lead should confirm the wording.
7. **The binding key needs the definitions table.** `format.md`'s `native procedure` cell carries
   one name; §3.2 shows the *name* is ambiguous and the *key* is what binds. The key is recoverable
   from the package definitions (`format.md` §7) provided every native procedure appears there,
   which the source says it does. T2.1's dump should confirm, or the cell should carry the key as
   a second string.
8. **Writes by natives and native-field references.** Epic's runtime invalidates outstanding
   references into native struct fields whenever a writing native runs. No reached native above
   holds such a reference, but `godot-natives.md`'s `variant` fields are native struct fields;
   whether any Godot native's behaviour depends on this belongs to that file and `objects.md`.

### 11.1 The lead's decisions

- **Q1: not bit-identical.** Use the platform's libm. Fixtures compare results through `ToString`'s six
  decimals; a fixture that still differs is recorded as a known divergence, not fixed.
- **Q2: freeze.** `GetSecondsSinceEpoch` answers the time taken once at `vh_init`, matching the
  reference host.
- **Q3: the proposal stands** — grouping, the three-digit rendering with §5.8's quirks as special
  cases, `{name}` substitution; the rest is unsupported until a fixture needs it.
- **Q6: the wording stands** — `The native function <binding key> is not implemented by this
  runtime.`
- **Q7: the cell carries the key.** `format.md`'s `native procedure` now has the binding key as its
  first field, so the loader never needs the definitions table to bind.
