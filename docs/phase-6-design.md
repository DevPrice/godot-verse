# Phase 6 — Debugging and profiling

**Status:** Draft 2 · 2026-09-14 · **built.** This document was written *before* the work, the way
Phase 4.5's and Phase 5's were, so **§13 is the part to read**: it carries the six spikes' answers,
the places §1 and §9 turned out wrong, and what is load-bearing and not obvious. Where §13 and
anything above it disagree, §13 is the record.

**Prerequisite: Phase 5 is complete** — built 2026-09-13, ABI v6; the editor-performance and
editor-last-mile work has since taken the ABI to **8.0**.

**Companion to:** `spec.md` §11 (R-DIAG-1 … R-DIAG-7), §14's **OQ-9** and **OQ-13**,
`roadmap.md` "Phase 6", `phase-5-design.md` §14 (which reshaped OQ-13 twice and is why §5 reads
the way it does), and `by-hand-findings.md` (which is where §10's by-hand record goes).

---

## 0. How to read this

**§1 is the decisions.** D1–D19 came out of one interview held against prior art read directly in
Godot's and Unreal's sources — every row cites the file it rests on. Do not relitigate them; if a
spike contradicts one, record it in §13 and raise it.

**§2 ran before the work.** Six spikes, one of which (**S-1**) could have blocked the entire phase.
Five have answers, in §13.1; the sixth (**S-6**) is the editor session, which is a windowed check
and is what this phase still owes.

**§3 is what the two engines actually offer**, as found rather than as remembered. It is the
factual base under everything else, and three of its findings contradict what `spec.md` currently
says about this phase — see §7.1.

**§13 is written**, as §14 was for Phase 5 and §11 for Phase 4.5. Where §13 and anything above it
disagree, §13 is the record.

---

## 1. Decisions

| # | decision | why, and the prior art it rests on |
| --- | --- | --- |
| D1 | **Build on `Verse::FDebugger`, not on `Verse::SocketDebugger`** | `VVMDebugger.h` declares a four-method interface — `Notify`, `AddLocation`, `AddTask`, `HasConnectedClient` — that `SetDebugger()` installs. The socket debugger is one *implementation* of it (`VVMSocketDebugger.cpp`, 479 lines) and is unusable here for a structural reason: it owns a thread and a socket and parks the mutator on its own condition variable, whereas Godot needs the stopped thread itself to run the debug loop (D2). What we want is the interface underneath it, and a 479-line worked example of how to drive it |
| D2 | **The host asks Godot; Godot blocks on the calling thread; Godot re-enters the host through new reads** | Forced, not chosen. `RemoteDebugger::debug()` (`core/debugger/remote_debugger.cpp:400-470`) loops on the thread that called it, servicing messages and calling `script_lang->debug_get_stack_level_*` from inside that loop. There is no shape where the host parks on its own primitive and Godot still runs. So `Notify` calls out through two new `vh_godot_api` callbacks, and the GDExtension calls back in through new `vh_debug_*` reads **while that outward call is still on the stack** |
| D3 | **The breakpoint list is Godot's alone. The host never holds one** | `EngineDebugger` is a bound singleton with `is_breakpoint(line, source)`, `is_skipping_breakpoints()`, `set_lines_left`, `set_depth`, `line_poll` — the whole of GDScript's break decision is eleven lines at `modules/gdscript/gdscript_vm.cpp:3925-3948`, and every call in it is available to a GDExtension. Duplicating the list in the host would mean keeping it in sync with an editor that can toggle one mid-run, for nothing |
| D4 | **The host dedups by location before it asks** | `Notify` fires per *bytecode op*, not per line. Asking Godot once per op would cross the ABI millions of times a second. `VSocketDebugger::UpdatePrevLocation` (`VVMSocketDebugger.cpp:294-306`) is the exact filter — same frame, same file, same location means "already asked" — and it is what makes per-op `Notify` affordable at all |
| D5 | **Stepping follows the interpreter, with no task filter — Epic's behaviour** | The SocketDebugger's `StopIfNext`/`StopIfStepOut` test only frame ancestry (`IsProperAncestorOf`), never which `VTask` is running. Matching it means a step can land in a different task than the one you stopped in. That is a real difference from GDScript, whose coroutines each carry their own stack; it is recorded in §4.6 as a known shape rather than papered over, because diverging from the only Verse debugger that exists is a worse trade than the surprise |
| D6 | **Attach whenever Godot's debugger is active. Measure it. Add a breakpoint mirror only if the number says so** | The game process cannot ask Godot "are there any breakpoints?" — `EngineDebugger` exposes only `is_breakpoint(line, source)`, a query. So the choice is attach-always-when-active (unconditionally correct, unmeasured cost) or a polled mirror that sweeps `is_breakpoint` over the lines Verse has locations for and attaches only when one exists. Build the correct one, take the number in **S-2**, and add the mirror only against a measurement. The number is recorded either way |
| D7 | **A local arrives as a real Variant when the bridge carries its type, and as `VValue::ToString` otherwise** | `Debugger::ForEachStackFrame` hands back `FRegisters` — named `VValue`s, plus `Self` — and a Verse local can be a tuple, an option, a map or a class instance, none of which `vh_value` describes. One rule: marshal what already crosses, so an `int`, a `float`, a `string`, a `logic`, a Godot object and a container are typed and inspectable; render the rest with `VValue::ToString(Context, EValueStringFormat, Depth)` (`VVMValue.h:263`), which is readable and honest about not being inspectable |
| D8 | **`_debug_get_stack_level_instance` returns `nullptr`, permanently.** `Self` is delivered through `_debug_get_stack_level_members` | **Landmine.** Godot calls `inst->get_owner()` on whatever that virtual returns (`remote_debugger.cpp:507-509`), i.e. it calls a C++ virtual on a `ScriptInstance*`. Our instances are raw `GDExtensionScriptInstanceInfo3` vtables; the `ScriptInstanceExtension` wrapper that *is* a `ScriptInstance` is built by Godot core (`script_language_extension.h:785`) and its address never reaches the GDExtension. Returning a `vh_instance*` or a `VerseScriptInstance*` is a type-confused virtual call and a crash |
| D9 | **No expression evaluation. R-DIAG-4 is amended, not failed** | Three independent reasons, and any one is sufficient. (1) Epic's own Verse DAP client handles exactly `setBreakpoints, continue, pause, next, stepIn, stepOut, threads, stackTrace, scopes, variables, disconnect` (`VVMClient.cpp:96-156`) — there is no `evaluate` and no `setVariable` anywhere in the Verse tooling to build on. (2) Godot never asks us: its `evaluate` command bails at `remote_debugger.cpp:553-556` when `debug_get_stack_level_instance` is null, which D8 makes permanent. (3) Evaluating would mean compiling an expression against a stopped frame's scope and running it in a VM paused mid-op. Revisit if and when Verse's own tooling grows the feature |
| D10 | **`_debug_get_globals` answers empty, and that is truthful** | A Verse module-level definition is a constant, not a mutable global a debugger would watch change. Empty is the right answer, and it is recorded in §7's table as *honestly empty* rather than as a stub |
| D11 | **OQ-13's answer is "nothing is bounded, and here is what actually needed fixing."** Godot already throttles the error; the bridge's own stack printing is what does not | `RemoteDebugger` drops errors past `network/limits/debugger/max_errors_per_second` and characters past `max_chars_per_second` (`remote_debugger.cpp:140,153,310-312`), and says so once. So the every-frame raise is already handled for the *error*. What is not is `VerseRuntime::on_runtime_error`'s stack tail (`src/verse_runtime.cpp:1253-1266`): one `UtilityFunctions::print` per Verse frame, unthrottled, sixty times a second, which eats the char budget and takes every other script's output down with it. Rate-limiting that is the whole of the work, and it is a real defect rather than a restatement |
| D12 | **One policy in both processes. R-DIAG-3's `@tool` clause is amended and closed** | Phase 5's per-instance scopes already mean an editor-time raise costs that instance's suspended work and nothing else's, and no Godot write survives a failed transaction. The clause was written from a fear in Phase 3's world, where one raise stopped every script in the process. Apply D11's rate limit in the editor exactly as in the game, then restate the requirement as what is true: the error is reported, not swallowed, and the scene the author is editing is unharmed |
| D13 | **The profiler is boundary instrumentation plus Verse's own `profile{}` blocks. No sampler** | Godot's `ProfilingInfo` wants `call_count`/`total_time`/`self_time` per function and GDScript fills it by instrumenting every function entry and exit. Verse offers no per-call hook. What the bridge knows *exactly* is every crossing it makes, so those rows are true — and `FVerseProfilingDelegates::OnEndProfilingEvent` (`VVMProfilingLibrary.h`) gives an author an opt-in way to get an inner row with an exact count and an exact time by writing `profile("tag"){…}`. `FSamplingProfiler` is rejected: it cannot produce a call count, so every row it contributed would carry a fabricated one |
| D14 | **A profiler row's signature is GDScript's three-part shape**: `res://scripts/player.verse::12::player._Process` | `gdscript_compiler.cpp:2489-2504` builds `path::start_line::Class.func`. The editor's profiler splits on it, so matching the shape is what makes a Verse row read like every other row rather than like a foreign string |
| D15 | **Profiling is off until Godot turns it on, and off costs one relaxed load** | `_profiling_start`/`_profiling_stop` are the switch. With it off, a boundary crossing pays a predictable branch and nothing else; `profile{}` costs a delegate that is not bound |
| D16 | **Every declared virtual is audited, and the audit is §7's table** | The roadmap asks for it, and §7.1 shows why it is not cosmetic: `spec.md` currently says the `_debug_*` stubs "must either be implemented or removed", and for four of them **removal is not available** — godot-cpp binds them with `GDVIRTUAL..._REQUIRED` and Godot errors at the call site when a required virtual is unbound, which is exactly what the comment at `src/verse_script_language.h:127` already says |
| D17 | **ABI v8.1 — a minor bump** | Everything added is additive under the policy at the top of `include/verse_host_abi.h`: a new enumerator (`VH_ERR_STOPPED`, if S-3 asks for it), new callbacks **appended** to `vh_godot_api` behind its `StructSize`, and new entry points. No layout changes and no existing field changes meaning. A host built at 8.1 must therefore still work against a consumer built at 8.0 — which means never breaking when the new callbacks are null |
| D18 | **OQ-9 closes as *not needed*** | The roadmap makes it conditional: the DAP framing question is only worth answering if R-DIAG-4 turns out blocked. S-1 decides. If R-DIAG-4 is reachable, the row closes with that as its written answer rather than staying open forever |
| D19 | **R-DIAG-7 is not in this phase** | Verse `Print` reaching the output panel without the script's identity attached is real and small, but it is output plumbing rather than debugging, and folding it in makes the phase's exit ambiguous. It stays at **part** with a pointer to this decision |

---

## 2. Spikes — these ran before §4 was trusted

Phase 4 and Phase 5 both ran their spikes before their design was believed, and both found the
design wrong in ways no amount of reading would have caught. Same rule here, and it held: S-4 found
a shape nobody had guessed at and S-3 relaxed a rule that would otherwise have shipped conservative.

Each spike says **how to run it**, **what a positive answer changes**, and **what a negative answer
costs**, as written before the work. **The answers are in §13.1**; five of the six have one.

### S-1 · Does `Notify` fire for project scripts, and what is in `VProcedure::FilePath`? — **blocking**

**Why it can block the phase.** Every stop path in Epic's own debugger begins with
`if (FilePath->Num() == 0) { return; }` (`VVMSocketDebugger.cpp:137-141`, and again in each of the
four `StopIf*`). A procedure with no file path is invisible to a debugger. This project's scripts
are not loose files on a data source — they are `FHostSourceSnippet`s built in memory and handed to
`FindOrAddModule(...).AddSnippet(...)` (`host/Private/HostScript.cpp:912-919`), with the snippet's
path taken from `vh_source_file::PathUtf8`, which the ABI documents as "Absolute path to the file
on disk" (`include/verse_host_abi.h:552-555`). **Whether that snippet path survives into the
compiled `VProcedure` is not known**, and the answer decides the phase.

**How to run it.** A throwaway `FDebugger` in the host — the shape
`memory/ask-the-verse-compiler.md` describes, and the same one `tests/verse_probe` uses for
language questions. Implement the four methods; make `HasConnectedClient()` return true; have
`Notify` print the first ~20 distinct `(ToString(*Frame.Procedure->FilePath), Procedure->GetLocation(PC)->Line)`
pairs to stderr and then detach. Install it with `SetDebugger` + `FContext::AttachedDebugger()`
before running `tests/host_smoke`'s `hello.verse`, and again against `dodge-the-creeps/scripts`.

**Positive** (a non-empty path): if it is the absolute disk path, §4.2's mapping is one
`ProjectSettings::localize_path` call, because that is exactly what the consumer passed in. Note
that Epic's own mapping runs it through `FPaths::FindCorrectCase` + `NormalizeFilename`
(`VVMBreakpoint.cpp:44-49`), so **case and separators are not guaranteed to match what Godot holds**
— compare case-insensitively with normalised separators, the way `HostScript.cpp`'s existing
`GetSnippetPath` comparisons already do (`ESearchCase::IgnoreCase`, e.g. line 5771).

**Negative** (empty, or a synthetic path): R-DIAG-4 through Godot's debugger is blocked, **OQ-9
reopens** as the roadmap says it would, and the phase is restructured around R-DIAG-5 and R-DIAG-3
with R-DIAG-6 investigated in place of R-DIAG-4. Do not start §4 until this is known.

### S-2 · What does an attached debugger cost?

**How to run it.** With S-1's spike still installed and `HasConnectedClient()` true, but `Notify`
doing nothing but the `UpdatePrevLocation` dedup and returning: `dodge-the-creeps` headless for 60 s
at `--fixed-fps 60`, and `tools/build_bench.py`'s per-call figure. Compare against the same runs
with no debugger installed.

**What it changes.** D6's second half. Under ~10% the always-attach path ships alone and the
breakpoint mirror is never written. Over it, §4.3's mirror becomes required and the number is what
justifies the extra machinery.

Two facts to carry into the measurement. First, `FContextImpl::CheckForHandshake` is already a
relaxed load and a compare on the fast path (`VVMContextImpl.h:205-217`); what attaching costs is
that every op now takes the *slow* path into `HandleHandshakeSlowpath` and a virtual call. Second,
**the computation watchdog is disabled while a debugger is attached** (`VVMContextImpl.cpp:897`,
`!HasDebugger()`), which is load-bearing for §4 in its own right — sitting on a breakpoint for a
minute must not trip `ErrRuntime_ComputationLimitExceeded`.

### S-3 · Is re-entering the VM from inside `Notify` safe?

**Why it matters.** While stopped, Godot's debug loop keeps servicing the editor — including the
remote scene tree, which reads node properties. A Verse property read is `vh_instance_get_field`,
i.e. entry into a VM stopped mid-op inside `Notify`, on the same thread, inside `AutoRTFM::Open`,
inside a live transaction.

**How to run it.** From the spike's `Notify`, on the first stop, call `vh_instance_call` on a
*different* instance and then `vh_instance_get_field` on the stopped one. Watch for: the
`EnterVerse` scope handling in `HostScript.cpp` (never `Context.EnterVM` directly — CLAUDE.md), an
`ensure` from `VVMEnterVMInline.h`, and whether the outer frame resumes correctly afterwards.

**Positive:** the while-stopped rule is "reads keep working", the remote inspector stays live while
paused, and no new status code is needed.
**Negative:** every entry point that would run Verse answers a new `VH_ERR_STOPPED` — `vh_tick`,
`vh_instance_call`, `vh_instance_get_field`/`set_field`, `vh_callback_invoke`,
`vh_check_project_*`, `vh_instantiate`, `vh_run_main` — and only the `vh_debug_*` reads are legal.
The GDExtension turns that into "no value" for the inspector. This is the group-membership rule
CLAUDE.md already states for entry points; a fourth group is not being created, `vh_debug_*` is
simply the one that is legal in this state.

### S-4 · Does every statement have a location?

`Procedure->GetLocation(PC)` returns `const FLocation*` and Epic's code treats null as "not a stop
site". If locations are sparse — attached to some ops and not others — a breakpoint on a line that
carries no op never fires, which reads to an author as "breakpoints don't work".

**How to run it.** Extend S-1's spike to collect the full set of distinct lines reported for one
known script, and diff it against the statement lines in the file.

**What a sparse answer costs.** §4.2 gains nearest-following-line matching: a breakpoint asked for
at line N arms at the first line ≥ N that the procedure actually reports, and the GDExtension tells
Godot where it really stopped. That is what most debuggers do and Godot's script editor draws the
stop marker wherever it is told.

### S-5 · Does the compiler accept `profile{}` in a user package?

`profile` is in the compiler's reserved symbols (`src/verse_keywords.h:116`) and the VM implements
the opcode — `FVerseProfilingDelegates::RaiseEndProfilingEvent` is called from
`VVMInterpreter.cpp:4452` with a `FProfileLocus` carrying `SnippetPath` and `BeginRow`. Reserved is
not the same as available to a `/user@localhost` package at `Version::LatestUnstable`.

**How to run it.** `tests/verse_probe`, which exists for exactly this and answered all twelve of
Phase 5 §2's questions. One fixture with `profile("tag"){ … }` in a method body; bind
`OnEndProfilingEvent` in the probe and print what arrives.

**Negative:** the profiler is boundary-only and D13's second half is dropped, with the refusal's
text recorded — the way `tests/verse_probe/async_reject.verse` records refusals.

### S-6 · The editor half — by hand

Not automatable (see §10). One editor session, run after §4 builds:

1. Open a `.verse` script in Godot's script editor and click the breakpoint gutter. `ScriptTextEditor`
   is language-agnostic on paper — `_breakpoint_toggled` sends `edited_res->get_path()` and the row
   (`editor/script/script_text_editor.cpp:1214-1217`) — confirm it in fact.
2. Run the project. The editor passes the current list as `--breakpoints` at launch
   (`editor/run/editor_run.cpp:128-142`) *and* sends each one again on connect
   (`editor/debugger/editor_debugger_node.cpp:452-453`); confirm both paths arm.
3. Confirm the Debugger panel populates: stack frames, the locals list, the members list.
4. Step in, step over, step out, continue; toggle *Skip Breakpoints*.
5. Toggle a breakpoint **while the game is running** and confirm it arms.

Record it in `by-hand-findings.md` as a numbered entry, the way B1–B13 are.

---

## 3. What the two engines actually offer

Read out of the sources rather than remembered. This is the base under §4–§6.

### 3.1 Verse's side — `Verse::FDebugger`

`Engine/Source/Runtime/CoreUObject/Public/VerseVM/VVMDebugger.h`:

- **`struct FDebugger`** — four pure virtuals: `Notify(FRunningContext, const FOp&, VFrame&, VTask&)`,
  `AddLocation(FAllocationContext, VUniqueString& FilePath, const FLocation&)`,
  `AddTask(FAccessContext, VTask&)`, `HasConnectedClient()`.
- **`SetDebugger(FDebugger*)` / `GetDebugger()`** — a single global, plain pointer, store-store
  fenced (`VVMDebugger.cpp:15-24`).
- **`Debugger::ForEachStackFrame(Context, PC, Frame, Task, NativeFrame, Callback)`** — walks the
  stack innermost-first, handing each callback a `const FLocation*` (null for a native frame) and a
  `Debugger::FFrame` carrying the procedure's `Name`, its `FilePath`, and `FRegisters`: an array of
  `(VUniqueString name, VValue value)`. `Self` is prepended when the frame has one and it is not
  `GlobalFalse()`. A register outside its live range at the current bytecode offset comes back
  uninitialised, deliberately — that is how "not yet in scope" is expressed.
  The `FNativeFrame*` comes from `Context.NativeFrame()`, captured at the moment of the stop
  (`VVMClient.cpp:242`).
- **`FContext::AttachedDebugger()` / `DetachedDebugger()`** — public statics
  (`VVMContextImpl.h:515-516`) that set `HasDebuggerBit` on every live context. The socket debugger
  calls them from `SetClientSocket`/`NullifyClientSocket` (`VVMClient.cpp:314,320`). Without them,
  the bit is only sampled when a context is claimed (`VVMContextImpl.cpp:171-177`), so a debugger
  installed mid-run would not take effect.
- **Where `Notify` is called from:** `FInterpreter::HandleHandshakeSlowpath`
  (`VVMInterpreter.cpp:4716-4719`), which the per-op `CheckForHandshake` reaches whenever the
  context's state is anything but plain `HasAccessBit`. So with a debugger attached, `Notify` fires
  **on every bytecode op**. The interpreter is `AUTORTFM_DISABLE` (`VVMInterpreter.cpp:450`), which
  is why a handshake — annotated `AutoRTFM::UnreachableIfClosed` — is legal there at all.

### 3.2 Godot's side — `EngineDebugger`

A bound core singleton, in `extension_api.json` and generated into godot-cpp
(`godot-cpp/gen/include/godot_cpp/classes/engine_debugger.hpp`). Everything needed is public:
`is_active`, `script_debug(ScriptLanguage*, can_continue, is_error_breakpoint)`,
`is_breakpoint(line, source)`, `is_skipping_breakpoints`, `set_lines_left`, `get_lines_left`,
`set_depth`, `get_depth`, `line_poll`, `insert_breakpoint`, `remove_breakpoint`, `clear_breakpoints`.

GDScript's entire break decision, for reference — `modules/gdscript/gdscript_vm.cpp:3925-3948`:
`lines_left > 0` and `depth <= 0` decrements; `lines_left <= 0` breaks; `is_breakpoint(line, source)`
breaks; then `line_poll()` unconditionally. Depth is pushed and popped around calls in
`GDScriptLanguage::enter_function`/`exit_function` (`modules/gdscript/gdscript.h:468-530`), guarded
by `lines_left > 0 && depth >= 0`.

`script_debug` reaches `RemoteDebugger::debug()` (`core/debugger/remote_debugger.cpp:400`), which
blocks on the calling thread, sends `debug_enter`, and loops servicing `step`/`next`/`out`/
`continue`/`get_stack_dump`/`get_stack_frame_vars`/`evaluate`/… — each dispatching into
`ScriptLanguage::debug_get_*`.

### 3.3 The three things this contradicts in `spec.md`

1. **"must either be implemented or removed" is only half available.** `_debug_get_stack_level_locals`,
   `_debug_get_stack_level_members`, `_debug_get_stack_level_instance`, `_debug_get_globals`,
   `_debug_get_current_stack_info`, `_profiling_get_accumulated_data` and
   `_profiling_get_frame_data` are bound with `GDVIRTUAL..._REQUIRED`
   (`core/object/script_language_extension.h:595,615,636,643,666,734,742`), and Godot errors at the
   call site when a required virtual is unbound. The rest — `debug_get_error`,
   `debug_get_stack_level_count/line/function/source`, `debug_parse_stack_level_expression` (`:664`)
   and `profiling_start/stop/set_save_native_calls` — are `EXBIND`, which is silent. §7's table
   carries the column, and **exactly one of the debug rows can be honestly deleted**.
2. **Expression evaluation cannot be reached** even if it were built — see D9(2).
3. **R-DIAG-3's `@tool` clause and OQ-13 are both narrower than written**, because Phase 5 changed
   what a raise costs and Godot already throttles the error. See D11 and D12.

---

## 4. Stage 1 — R-DIAG-4, the debugger

### 4.1 The shape

```
  Verse interpreter (host, game thread)
    └─ CheckForHandshake → HandleHandshakeSlowpath → FGodotDebugger::Notify(Context, PC, Frame, Task)
         ├─ dedup by (Frame, FilePath, Location)            [D4]
         ├─ Godot.DebugShouldBreak(path, line) ─────────────────────┐
         │                                                          │  GDExtension
         │                                                          ├─ lines_left / depth / is_breakpoint
         │                                                          │  (mirrors gdscript_vm.cpp:3925-3948)
         │  ◄───────────────────────────────────────────────────────┘
         └─ if breaking:
              stash (Context, PC, Frame, Task, Context.NativeFrame(), reason)
              Godot.DebugBreak(reason) ─────────────────────────────┐
                                                                    │  GDExtension
                                                                    ├─ EngineDebugger::script_debug(this, …)
                                                                    │    └─ BLOCKS, servicing the editor
                                                                    │         └─ _debug_get_stack_level_* 
                                                                    │              └─ vh_debug_* ──► host
                                                                    │                   (reads the stash)
              ◄─────────────────────────────────────────────────────┘  returns when the user continues
              clear the stash; return; the interpreter runs on
```

The stash is the whole trick. `Notify`'s four arguments are only valid for the duration of the
call, so the host keeps them in a file-static for exactly as long as `DebugBreak` is on the stack
and clears them on the way out. Every `vh_debug_*` read is defined **only** while a stash is
present and answers `VH_ERR_STATE` otherwise.

### 4.2 The break decision

The host reduces a stop to `(path, line)` and asks. Everything else is the consumer's.

**Host side**, in `Notify`, in this order — each step is Epic's, cited:

1. `Frame.Procedure->FilePath`; return if `Num() == 0` (`VVMSocketDebugger.cpp:137-141`).
2. `Procedure.GetLocation(PC)`; return if null.
3. `UpdatePrevLocation(Context, Frame, FilePath, Location)`; return if unchanged
   (`VVMSocketDebugger.cpp:294-306`). **This is D4 and it is not optional.**
4. `Godot.DebugShouldBreak(path, line)`.
5. If true: stash, `Godot.DebugBreak(reason)`, clear.

**Consumer side**, in `VerseScriptLanguage`, transliterating `gdscript_vm.cpp:3925-3948`:

```cpp
// The order is GDScript's: a pending step wins, then a breakpoint, then the poll runs whatever
// the answer was. Diverging here makes stepping and breakpoints disagree about which one fires.
bool do_break = false;
ScriptDebugger via EngineDebugger *dbg = EngineDebugger::get_singleton();
if (dbg->get_lines_left() > 0) {
    if (dbg->get_depth() <= 0) { dbg->set_lines_left(dbg->get_lines_left() - 1); }
    if (dbg->get_lines_left() <= 0) { do_break = true; }
}
if (dbg->is_breakpoint(line, res_path)) { do_break = true; }
dbg->line_poll();
```

`res_path` is `ProjectSettings::localize_path(path)` — S-1 says whether the host's path needs
case-normalising first, and S-4 says whether `line` needs nearest-following matching.

`is_skipping_breakpoints` needs no handling here: `RemoteDebugger::debug` checks it itself
(`remote_debugger.cpp:402-404`) and returns without stopping. Calling it ourselves would only
duplicate the check.

**Depth.** GDScript pushes and pops depth around calls so that *step over* does not descend. The
bridge has no equivalent hook — it does not see a Verse call, only an op. The honest translation is
that `get_depth()` stays at whatever Godot set it to, and *step over* therefore behaves as *step in*
for a call made inside one Verse function. **S-4's answer may improve this**: if `VFrame::CallerFrame`
ancestry is available in `Notify` — it is, that is exactly what `IsProperAncestorOf` uses
(`VVMSocketDebugger.cpp:287-293`) — then step-over can be implemented the way Epic does it, by
comparing the current frame against the frame we last stopped in, and depth can be left alone
entirely. **Prefer Epic's ancestry test over Godot's depth counter**; it is the mechanism that fits
what `Notify` is given.

### 4.3 Attach

`vh_debug_set_enabled(vh_bool)` installs or removes the `FGodotDebugger` and calls
`FContext::AttachedDebugger()` / `DetachedDebugger()`. `HasConnectedClient()` answers the same flag.

The consumer calls it from `VerseScriptLanguage::_frame` (`src/verse_script_language.cpp:2588`),
which is already the per-frame hook and already the place `poll_check` runs:

- attach when `EngineDebugger::get_singleton()->is_active()` and we are not attached;
- detach when it stops being active.

If **S-2** says the cost matters, the mirror goes here too: a timer (not per frame) sweeping
`is_breakpoint(line, path)` over the lines each loaded `.verse` reports, attaching only when the set
is non-empty. A breakpoint then arms within one poll interval, which is the trade the number has to
justify.

### 4.4 The stack, the locals and the members

`vh_debug_stack_count` / `vh_debug_stack_frame` / `vh_debug_stack_values` all run
`Debugger::ForEachStackFrame` over the stash and index into the result.

- **`_debug_get_stack_level_count`** — frames walked. Native frames are included (Epic emits them
  with a name and no location), which is what makes a stop inside a mirrored method legible.
- **`_debug_get_stack_level_line`** — `FLocation::Line`, or 0 for a native frame.
- **`_debug_get_stack_level_function`** — `Procedure->Name`.
- **`_debug_get_stack_level_source`** — the localised `res://` path, empty for a native frame.
- **`_debug_get_stack_level_locals`** — returns `{"locals": PackedStringArray, "values": Array}`;
  the extension wrapper splits it (`script_language_extension.h:596-613`). Every register from
  `FRegisters` **except** `Self`, D7's rule applied to each value. A register outside its live range
  is reported with a distinct rendering rather than dropped — the name is in scope in the source and
  its absence from the list would read as a bug.
- **`_debug_get_stack_level_members`** — `Self`'s fields, plus `Self` itself. This is where the
  script instance's state appears, and per D8 it is the *only* place it appears.
- **`_debug_get_stack_level_instance`** — `nullptr`. D8.
- **`_debug_get_globals`** — empty. D10.
- **`_debug_get_error`** — the stash's reason string: `"Breakpoint"`, `"Step"`, or whatever D11's
  error path supplies.

`p_max_subitems` and `p_max_depth` map onto `VValue::ToString`'s `RecursionDepth`; a marshalled
Variant needs no truncation because Godot truncates it itself.

### 4.5 While stopped

**S-3 decides.** Two rules, both writable now:

- *If nested entry is safe:* only `vh_compile_project` and `vh_check_project_*` are refused (a build
  while stopped is incoherent for a different reason — it would publish a generation under a frame
  that belongs to the old one). Everything else works and the remote inspector stays live.
- *If it is not:* every entry point that runs Verse answers `VH_ERR_STOPPED`, and the GDExtension
  answers "no value" upward. This is the conservative default; write §9's enumerator either way and
  delete it if S-3 comes back positive.

Independently of S-3: **`vh_tick` must not run while stopped.** Resuming a `Sleep`d task inside a
stopped VM is not something any of this is designed for, and `_frame` is reachable from Godot's own
debug loop through `flush_output`.

### 4.6 What stepping does across a suspension — the known shape

D5 takes Epic's behaviour, and it has a consequence worth stating where an author will find it.

Verse tasks are not OS threads and Godot's debugger has no concept of them. A task that suspends in
`Await` or `Sleep` resumes frames later, possibly inside a different Godot callback. With no task
filter:

- stepping over a line that suspends hands the step to whatever the interpreter reaches next, which
  may be an entirely different script's `_Process`;
- a second instance of the same script reaching the same line can take a step you asked for in the
  first.

This matches the only Verse debugger that exists and diverges from GDScript, whose coroutines each
carry their own stack. It goes in the manual (R-QUAL-8, Phase 8) and in `spec.md` beside R-DIAG-4,
as a *known shape* in the sense R-DIAG-2's entry already uses that phrase.

---

## 5. Stage 2 — R-DIAG-3 and OQ-13

Smallest stage, and it needs no ABI.

**What is actually wrong** (D11): `VerseRuntime::on_runtime_error` pushes one error and then prints
the whole Verse stack as separate `UtilityFunctions::print` lines
(`src/verse_runtime.cpp:1253-1266`). Godot throttles errors and characters
(`remote_debugger.cpp:140,153,310-312`) but those printed lines are *output*, and a script raising
at 60 Hz with a six-frame stack emits ~360 lines a second into a shared char budget — so the
runaway script silences every other script's output, which is the opposite of the intent.

**What to build.**

1. A rate limit on the bridge's own stack printing, keyed on the raise site — the innermost located
   frame's `(path, line)` plus the message. First occurrence prints in full. Repeats inside the
   window print nothing; when the window closes, one line says how many were suppressed. Godot's
   own `n_errors_dropped` message is the model (`remote_debugger.cpp:310-312`) and its wording is
   worth echoing so the two read alike.
2. The same code path in the editor process, which closes D12's half of R-DIAG-3.
3. **Nothing bounds the script.** That is OQ-13's answer and it goes in the row as an answer, with
   the three reasons: Godot does not bound GDScript either; Phase 5's per-instance scopes already
   confine the cost to the raising node; and disabling an instance is a policy an author cannot see
   coming and cannot undo without a reload.
4. **The `spawn` runaway gets a number, not a limit.** Phase 5 opened it — a `spawn` in `_Process`
   makes sixty tasks a second on one instance. `vh_tick_stats` already carries `JobsPending` and
   `Sleeping`, and `verse/queued_jobs` is already a custom monitor
   (`src/verse_runtime.cpp:962-964`). Add the per-instance task count to the same place, so the
   runaway is *visible* in the profiler where the author is already looking, and say in the OQ-13
   row that observability was chosen over a cap.

**What closes R-DIAG-3.** Its "missing" paragraph is rewritten to say what is true after this: the
`@tool` clause is met by Phase 5's per-instance scopes plus this rate limit, and OQ-13 has a written
answer. The requirement moves from **part** to **done**.

---

## 6. Stage 3 — R-DIAG-5, the profiler

### 6.1 What a row is

Two sources, one row shape (D14).

**Boundary rows** — one per script method the bridge calls, named
`res://scripts/player.verse::<decl line>::player._Process`:

| entry point | what it times |
| --- | --- |
| `vh_instance_call` | a virtual, a signal handler dispatched to a method, any script method Godot calls |
| `vh_callback_invoke` | a Verse callback Godot invoked through a `Callable` |
| `vh_tick` | queued work: `Sleep` resumptions, `Main`, anything with no Godot event behind it. One synthetic row, `<verse>::0::vh_tick` |

`call_count` is exact. `total_time` is wall time across the call. `self_time` is `total_time` minus
the time spent in *nested* boundary entries, which is what makes a Verse method that emits a signal
that calls another Verse method attribute correctly — keep a stack of active timings in the host and
subtract each child's total from its parent as it pops.

**`profile{}` rows** — bound from `FVerseProfilingDelegates::OnEndProfilingEvent(UserTag, TimeInMs, Locus)`,
named `<Locus.SnippetPath>::<Locus.BeginRow>::<UserTag>`. Exact counts and exact times, opt-in, and
they nest inside a boundary row without disturbing it because they are a separate accumulator.
**Conditional on S-5.**

### 6.2 The ABI and the switch

`vh_profiling_set_enabled(vh_bool)` and `vh_profiling_read(vh_bool FrameOnly, const vh_profile_row**, int32_t*)`,
answering a host-owned array valid until the next call — the same shape as `vh_class_method_list`.
`FrameOnly` distinguishes Godot's two questions: `_profiling_get_frame_data` wants this frame's
rows and resets them, `_profiling_get_accumulated_data` wants the run's.

The consumer copies into `ScriptLanguageExtensionProfilingInfo`. Note godot-cpp's struct carries
`internal_time` as a fifth field in current Godot (`core/object/script_language.h:317-323`); leave
it zero and say so.

With profiling off, each boundary crossing pays one relaxed load and a predicted branch (D15).
Measure the on and off cost with `tools/build_bench.py` and record both; R-PERF-1 still owes four
numbers and this is not one of them, but an instrument that costs when idle is a defect.

### 6.3 What it does not show

Written into `spec.md` beside R-DIAG-5, not left for a user to discover: **a Verse function called
from another Verse function has no row of its own** unless the author wraps it in `profile{}`.
Verse exposes no per-call hook; the only thing that could produce one is `FDebugger::Notify`, which
fires per op and is why §2's S-2 exists. `FSamplingProfiler` was considered and rejected for
producing rows with no call count (D13).

---

## 7. The declared-virtual audit

Every virtual `VerseScriptLanguage` and the script instance declare, and whether the declaration
tells Godot the truth. The **bind** column is why removal is or is not available:
`GDVIRTUAL..._REQUIRED` errors at the call site when unbound, `EXBIND` is silent.

Done, by reading `src/verse_script_language.h` against `core/object/script_language_extension.h`:
59 declared virtuals, each marked *implemented*, *honestly empty*, or *knowingly incomplete*. The
rows below are the ones where the answer is not simply "implemented"; the audit found **no row
outside the `_debug_*`/`_profiling_*` group where a declaration was already telling Godot something
untrue**, which is the thing it was looking for.

| virtual | bind | today | after Phase 6 |
| --- | --- | --- | --- |
| `_debug_get_error` | EXBIND | empty | **implemented** — `"Breakpoint"` or `"Step"`, from the consumer's own decision rather than from the host (§13.2) |
| `_debug_get_stack_level_count` / `_line` / `_function` / `_source` | EXBIND | 0 / empty | **implemented** — §4.4 |
| `_debug_get_stack_level_locals` / `_members` | **REQUIRED** | empty | **implemented** — §4.4, with `Self` and its *data* members under `_members` |
| `_debug_get_stack_level_instance` | **REQUIRED** | nullptr | **stays nullptr**, permanently — D8 |
| `_debug_get_globals` | **REQUIRED** | empty | **honestly empty** — D10 |
| `_debug_parse_stack_level_expression` | EXBIND | empty | **removed** — D9 |
| `_debug_get_current_stack_info` | **REQUIRED** | empty | **knowingly incomplete, out of scope by decision.** `ScriptBacktrace` uses it to attach a script stack to *any* engine error, not only to a breakpoint. Empty is truthful when no Verse frame is running and untruthful when one is. §4's walk is most of what would close it and still not all: `ForEachStackFrame` needs the PC, frame and task, which exist only inside `Notify` — outside a stop there is no interpreter state to hand it, and the only other route to a Verse stack is the string `RuntimeErrorTextProvider` renders during a raise |
| `_profiling_start` / `_stop` | EXBIND | empty | **implemented** — §6.2 |
| `_profiling_set_save_native_calls` | EXBIND | empty | **honestly empty, and stays one.** Godot's flag asks a language to attribute time spent inside engine calls to the script that made them; the bridge cannot do otherwise — a mirrored call happens inside the Verse method's boundary row and its time is in that row's total whether anyone asks or not |
| `_profiling_get_accumulated_data` / `_get_frame_data` | **REQUIRED** | 0 | **implemented** — §6.2, and see §13.3 on the array's stride |
| `_auto_indent_code` | **REQUIRED** | returns the code unchanged | **honestly empty.** It cannot be omitted, and doing nothing is a real answer: Verse rejects mixed tabs and spaces outright, so a reindenter that guessed wrong would produce a file that does not compile |
| `_get_public_functions` / `_get_public_constants` / `_get_public_annotations` | **REQUIRED** | empty | **honestly empty.** These are GDScript's host-injected globals and `@`-annotations. Verse has neither: its module-level definitions are constants a script reaches by `using`, and its attributes are declared in a package the host adds at runtime and completed through `vh_complete_symbol`'s `VH_COMPLETE_ATTRIBUTES` mode |
| `_get_doc_comment_delimiters` | GDVIRTUAL | empty | **honestly empty.** Verse has no doc-comment form; the convention `verse_doc_comment_above` implements — whatever precedes a definition documents it — is not a delimiter |
| `_thread_enter` / `_thread_exit` | EXBIND | empty | **honestly empty.** Verse is pinned to the thread that called `vh_init` (R-ASYNC-8); there is nothing per-thread to set up, and a call from any other thread is refused with `VH_ERR_THREAD` |
| `_open_in_external_editor` / `_overrides_external_editor` | EXBIND | `ERR_UNAVAILABLE` / false | **implemented** — a consistent pair: Godot never calls the first because the second says no |
| `_validate_path` | EXBIND | empty | **implemented** — empty *is* the answer, meaning "no objection to this path" |
| `_make_function` | EXBIND | builds a stub | **implemented and untestable from here** (G5): it is a `ScriptLanguageExtension` virtual with no ClassDB entry, and `Script.get_language()` is not public, so the editor's own C++ is its only caller |
| *(every other declared virtual)* | — | — | **implemented** — 59 declared, and no row where "adding an override you do not implement changes behaviour" had already happened outside the `_debug_*`/`_profiling_*` group this phase fixed |

Two standing rules the audit applies, both already in CLAUDE.md and both half-true:

- *"omitting one is how you say unsupported"* — true for `EXBIND` rows, **false for REQUIRED ones**,
  where omitting produces an editor error at every call. Where a REQUIRED virtual has no meaningful
  answer, the row is *honestly empty* and this table is where that is recorded.
- *"adding an override you do not implement changes behaviour"* — still true, and the audit's job is
  to find any row where it has already happened.

---

## 8. What this phase deliberately does not build

Recorded so the omissions are visible rather than forgotten.

- **Expression evaluation** (D9). R-DIAG-4 is amended to say so, with the three reasons.
- **R-DIAG-6 / OQ-9** (D18). The roadmap makes it conditional on R-DIAG-4 being blocked. If S-1 is
  positive, OQ-9 closes as *not needed*; if S-1 is negative, this section is wrong and the phase is
  restructured.
- **R-DIAG-7** (D19) — `Print` with the script's identity. Stays at **part**.
- **`_debug_get_current_stack_info`** — §7's table says what it would take.
- **A sampling profiler** (D13), and any per-Verse-function row that is not a `profile{}` block (§6.3).
- **Anything about threads.** Godot's debugger models OS threads; Verse's tasks are not those, and
  R-ASYNC-7 / OQ-6 is not this phase's.

---

## 9. ABI v8.1 — the whole delta

Minor bump (D17). `VH_ABI_VERSION_MINOR` 0 → 1. Both DLLs still must be rebuilt in practice, and
`run_tests.py --build` is how the test binaries follow; the *policy* consequence of it being minor
is that the host must tolerate a consumer that supplies none of this.

**Appended to `vh_godot_api`**, behind its `StructSize`:

```c
/* Phase 6 / R-DIAG-4. Both null in a consumer that does not debug; the host then never breaks.
 * Called from inside the interpreter's handshake, on the vh_init thread, with a Verse op in
 * flight -- so the consumer must do nothing here that re-enters the host except the vh_debug_*
 * reads, and must do that only from inside DebugBreak. */

/* Is (PathUtf8, Line) a place to stop? The consumer owns the breakpoint list and the step state;
 * the host asks once per distinct location, never once per op. */
vh_bool (*DebugShouldBreak)(void* Ctx, const char* PathUtf8, int32_t PathLen, int32_t Line);

/* Stop. Returns when the user continues. The consumer is expected to block here -- Godot's debug
 * loop runs on this thread and calls back through the vh_debug_* reads while it does. */
void (*DebugBreak)(void* Ctx, const char* ReasonUtf8, int32_t ReasonLen);
```

**New entry points:**

```c
int32_t vh_debug_set_enabled(vh_bool Enabled);
int32_t vh_debug_stack_count(int32_t* OutCount);
int32_t vh_debug_stack_frame(int32_t Level, const vh_debug_frame** OutFrame);
int32_t vh_debug_stack_values(int32_t Level, int32_t Kind, const vh_debug_value** OutValues, int32_t* OutCount);
int32_t vh_profiling_set_enabled(vh_bool Enabled);
int32_t vh_profiling_read(vh_bool FrameOnly, const vh_profile_row** OutRows, int32_t* OutCount);
```

`Kind` is `VH_DEBUG_LOCALS` or `VH_DEBUG_MEMBERS`. The three `vh_debug_stack_*` reads answer
`VH_ERR_STATE` when nothing is stopped; they are the **only** entry points legal while stopped
(§4.5), and they join the "answers from what is stashed" group rather than the "waits for an
analysis" group or the "executes Verse" group — CLAUDE.md's rule that there is never a fourth group
holds.

**New structs:**

```c
typedef struct vh_debug_frame {
    int32_t StructSize;
    const char* PathUtf8;   int32_t PathLen;    /* empty for a native frame */
    const char* NameUtf8;   int32_t NameLen;
    int32_t Line;                               /* 0 for a native frame */
} vh_debug_frame;

typedef struct vh_debug_value {
    int32_t StructSize;
    const char* NameUtf8;   int32_t NameLen;
    /* Exactly one is populated. Value when the bridge carries the type, Rendered otherwise --
     * a Verse local can be a tuple, an option, a map or a class instance and vh_value describes
     * none of them (phase-6-design.md D7). */
    const vh_value* Value;
    const char* RenderedUtf8; int32_t RenderedLen;
} vh_debug_value;

typedef struct vh_profile_row {
    int32_t StructSize;
    const char* SignatureUtf8; int32_t SignatureLen;  /* path::line::owner.name */
    int64_t CallCount;
    double TotalSeconds;
    double SelfSeconds;
} vh_profile_row;
```

**New enumerator:** `VH_ERR_STOPPED`, if and only if S-3 says nested entry is unsafe (§4.5).

---

## 10. Tests

**The automated half is `host_smoke` only. The Godot half is by hand.** Decided; the trade is
stated rather than hidden: a regression in the GDExtension's half goes unnoticed until someone runs
the editor.

**`tests/host_smoke`** — one `main`, `Step(name, result)` per case, exits non-zero on failure
(`tests/host_smoke/host_smoke.cpp`). New cases, driving a stub consumer that supplies
`DebugShouldBreak`/`DebugBreak` itself:

1. attach and detach; `Notify` fires only while attached;
2. a breakpoint at a known line of `hello.verse` stops there, once — not once per op (the D4 dedup);
3. the stack has the expected depth and the innermost frame's path, name and line are right;
4. locals carry the expected names, an `int` local arrives as a `vh_value` and a non-marshalling
   local arrives rendered (D7);
5. members carry `Self` and the instance's declared fields;
6. `vh_debug_stack_*` answers `VH_ERR_STATE` when nothing is stopped;
7. whichever of S-3's two rules holds: nested entry works, or every executing entry point answers
   `VH_ERR_STOPPED`;
8. stepping: from a stop, ask for one line and confirm the next stop is where §4.2's rules say;
9. the profiler: with it on, a known number of `vh_instance_call`s produces a row with that exact
   `call_count`, a `total_time` bounded below by a deliberate `Sleep`, and a `self_time` less than
   the total when the call nests;
10. with the profiler off, `vh_profiling_read` answers zero rows.

**By hand**, recorded in `by-hand-findings.md` as numbered entries: S-6's five steps, plus a
profiler session (turn it on in the Debugger panel, run `dodge-the-creeps`, confirm Verse rows
appear beside Godot's own with plausible numbers).

**Recorded and not taken:** Godot's `LocalDebugger` is drivable headless —
`godot --headless --debug --breakpoints res://scripts/x.verse:N` reads `bt`, `lv`, `mv`, `c` from
**stdin** and prints frames, locals and members
(`core/debugger/local_debugger.cpp:120-215`), which `run_tests.py` could pipe and assert on in the
same shape as `tests/coverage_diagnostic`. It is written down here so that if the by-hand check
proves too costly to repeat, the automated route is a known quantity rather than a rediscovery.

---

## 11. Exit

The roadmap's exit: **a breakpoint in Godot's script editor stops a Verse script and shows its
locals.** Met against the ABI, with one item outstanding.

- ~~S-6's by-hand session run~~ — **the one thing still owed.** It is a windowed editor session and
  cannot be automated; `by-hand-findings.md` carries its five steps as an open check.
- ✅ `run_tests.py` green, with the new `host_smoke` cases — a breakpoint stops once rather than
  once per op, the stack's depth, name, path and line, locals, members, stepping, attach/detach,
  and the profiler's counts, self time, signature shape and frame reset.
- ✅ R-DIAG-3 **done**, its `@tool` clause restated as what is true after R-ASYNC-4, and **OQ-13
  closed** with a written answer — nothing is bounded, and the defect it pointed at was the
  bridge's own stack printing.
- ✅ R-DIAG-4 **done**, with its two amendments plus the two shapes the spikes found — a line that
  emits no op carries no location, and a step may not land where it started.
- ✅ R-DIAG-5 **done**, with §6.3's limit written into the requirement.
- ✅ §7's audit table complete: 59 virtuals, and every row that stays empty saying why.
- ✅ **OQ-9 closed** as not needed — S-1 was positive.
- ✅ **§13 written**, and the status line points at it.

---

## 12. Risks

Written before the work; what each came to is beside it.

- ~~**S-1 is a real cliff.**~~ **Did not fire.** The file path survives verbatim, so none of this
  document was void.
- ~~**The attach cost may force the mirror** (S-2).~~ **Did not fire**, on the number that
  mattered: +1.6% at frame level. The mirror was never written.
- ~~**Re-entrancy while stopped** (S-3) is the one place this phase can corrupt rather than merely
  fail.~~ **Did not fire.** Nested entry is safe and the conservative rule was relaxed by
  measurement rather than assumed away.
- ~~**Step-over may be unimplementable in Godot's terms.**~~ **Fired, and was fixable.** The
  ancestry test composes with `lines_left` exactly as proposed, but by itself it stopped on the
  line it stepped from — because a call reports its line twice. One extra rule (§13.2) closed it;
  step-over did not have to degrade to step-in.
- ~~**`profile{}` may be refused by the compiler** (S-5).~~ **Did not fire.**
- **The by-hand test decision stands, and it is now the phase's only open item.** A regression in
  `verse_script_language.cpp`'s debug surface is invisible to `run_tests.py`: the host half is
  covered end to end, and everything from `EngineDebugger` inward is not. §10 records the automated
  route that was not taken (`LocalDebugger` is drivable headless and reads `bt`, `lv`, `mv`, `c`
  from stdin), so the cost of reversing the decision is a known quantity rather than a
  rediscovery.

---

## 13. What building this corrected

Written after the work, and where this and anything above it disagree, this is the record.

### 13.1 The spikes' answers

| # | answer |
| --- | --- |
| **S-1** | **Positive, and unqualified.** `VProcedure::FilePath` is the absolute disk path `vh_compile_project` was handed, **verbatim** — `C:/Users/.../godot-verse\tests\host_smoke\debug_probe.verse`, mixed separators and all. The chain is `VVMCodeGenerator.cpp:6435` → `VVMLocationUtil.h`'s `GetPath`, which converts the snippet path to relative against the Vst package's `_DirPath` and falls back to the snippet path when that is empty — and a `CSourceDataPackage` built by `FindOrAddSourcePackage` has no `_DirPath`, so the fallback is always what is taken. No case normalisation is needed either, because nothing normalises it on the way through. The consumer's whole translation is `replace("\\", "/")` and `ProjectSettings::localize_path`, cached per distinct path. **The phase was not blocked and §4–§7 stood.** |
| **S-2** | **The mirror is not needed.** A one-line method's call goes from 0.27 µs to 2.79 µs with the debugger attached — ten times, which reads alarming — but the frame-level figure is what D6 asked for and it is **+1.6%**: the `dodge-the-creeps` yardstick runs in a median 4.26 s plain and 4.33 s under `--debug`, three runs each, 0 failures either way. Under the 10% threshold, so the always-attach path ships alone and §4.3's polled breakpoint mirror was never written. The per-call figure is in `tools/build_bench.py` as `vh_instance_call (debugging)` so it can be taken again. |
| **S-3** | **Positive: nested entry is safe.** From inside `Notify`, a `vh_instance_call` on a *different* instance and a `vh_instance_get_field` on the stopped one both answered `VH_OK`, and the outer call ran to completion after continuing. So the remote inspector stays live while paused, and **`VH_ERR_STOPPED` is far narrower than §4.5's conservative default**: it is answered by `vh_compile_project` and `vh_check_project_begin` alone — not for re-entrancy but because publishing a generation underneath a frame of the retiring one is incoherent — and `vh_tick` is refused silently, having no status to answer with. |
| **S-4** | **Nearly dense, with one shape that matters.** Every statement line reports a location, and so does a function's *declaration* line. What does not is a trailing bare expression that only reads a register: `Helper`'s `Inner := 21` on line 21 reports, and the `Inner` on line 22 that answers it does not, because it emits no op of its own. So a breakpoint on the last line of such a body never fires. Nearest-following-line matching would not help — there is no following line — so this is recorded as a known shape rather than worked around, and `tests/host_smoke` asserts *both* halves so a compiler change moves the answer visibly. |
| **S-5** | **Positive, both halves.** `profile("tag"): …` compiles in a `/user@localhost` package at `Version::LatestUnstable`, nests, and answers a value (`tests/verse_probe/profile_probe.verse`). At runtime `FVerseProfilingDelegates::OnEndProfilingEvent` delivers the tag, the time and a `FProfileLocus` carrying the snippet path and the begin row, and `tests/host_smoke` asserts a `::smoke_tag` row with a call count of exactly 1. D13's second half stands. |
| **S-6** | **Not run.** The editor half is a windowed session and is the one thing this phase owes; it is recorded in `by-hand-findings.md` as open, with its five steps. |

### 13.2 Where §1 and §9 turned out wrong

- **`DebugBreak` carries no reason, and §4.4's "stash's reason" is gone.** The host was to stash a
  reason string and hand it over; but the *consumer* is the side that decided whether the stop was
  a breakpoint or a step, so the host would have been inventing a word for a decision made on the
  other side of the ABI. `_debug_get_error` answers from `VerseScriptLanguage`'s own state instead,
  and the callback takes `Ctx` alone.
- **`DebugShouldBreak` gained a fifth argument, and §4.2's depth discussion resolves into it.**
  §9 fixed the signature at `(Ctx, Path, PathLen, Line)`, and §4.2 separately argued that Epic's
  frame-ancestry test should be preferred to Godot's depth counter. Those two cannot both hold:
  ancestry is visible only to `Notify`, and the step decision is the consumer's. The resolution is
  `vh_debug_frame_relation` — SAME, DEEPER or OTHER — computed in the host and passed across, with
  the consumer reading Godot's `get_depth()` as *which kind* of step is pending (-1 in, 0 over,
  1 out) rather than as a count it has to maintain. Godot's depth is never written.
- **A step needs one rule neither design nor prior art mentions: it may not land where it started.**
  `Total := Helper()` reports its line **twice** — once before the call and once when the result
  lands — so a step-over from that line stopped on that line again and looked like nothing had
  happened. GDScript is not exposed to this because its line opcode is per source line; a Verse
  location is per op. The consumer therefore remembers the (source, line) it stopped at and
  requires a pending step to differ from it in the same frame. Epic's socket debugger has the same
  gap; it is not visible there because a DAP client re-issues the step.
- **Every name the VM hands back is decorated, and undecorating is not `FindLastChar(')')`.** A
  procedure's `Name` is `(/user@localhost/debug_probe:)Helper` and a shape key for a *method* is
  `(/Godot.org/Godot/object:)Connect(:[]char,:callable)` — whose last `)` belongs to the parameter
  list. The first `:)` is the end of the scope and the only correct split.
- **The members list needed filtering, and the design did not say so.** A `VShape` carries methods
  and accessors alongside data, and the smallest script class inherits ~52 of Godot's own: the
  first working stop reported 55 members for a class with three fields. `VEntry::IsMethod()` and
  `IsAccessor()` are the filter. Godot's members panel means instance state.
- **D7 is narrower in practice than in principle.** "A container is typed and inspectable" does not
  survive contact: a Verse container is a *wrapper class instance* holding a `godot_ref`, and
  `VH_TYPE_REF` is declared but unimplemented on this wire, so a container renders. What crosses
  typed is an `int`, a `float`, a `string`, a `logic` and a Godot object handle.
- **OQ-13's answer moved the work.** §5 already suspected it, and the build confirmed it: the
  question asks what bounds a raising script, and what actually needed writing was a rate limit on
  the bridge's own stack printing. One correction to §5's sketch — the "n dropped" summary cannot
  be emitted by the *next* occurrence, because a script that raises sixty times and then stops has
  no next occurrence. It is flushed from `vh_tick`.

### 13.3 Load-bearing and not obvious

- **`TArray::AddDefaulted_GetRef()` does not zero a POD.** It default-*initializes*, so a
  `vh_debug_value` taken that way came back holding whatever the previous answer had left in the
  lane this one does not fill — and the consumer, seeing a non-null `Value`, dereferenced a
  fragment of a string as a pointer. It was a segfault four calls into the first working stop, and
  it would have been a silent wrong answer if the garbage had happened to be null. Every descriptor
  built in a loop here is now assigned from a zeroed local and `Add`ed.
- **`FDebugger`'s four virtuals are `AUTORTFM_ENABLE` and an override may not narrow that.** The
  implementation is a file-scope `AUTORTFM_DISABLE` function reached through `AutoRTFM::Open` from
  a plain override — which is the shape Epic uses too (an `AUTORTFM_DISABLE` VCell holding an inner
  `FDebugger` that forwards to it), arrived at from the compiler's error rather than from the
  example.
- **The debugger's dedup state must be reachable by the collector.** `PrevFrame`, `PrevFilePath`
  and `LastStoppedFrame` are compared by address and never dereferenced, so raw pointers would
  "work" — until a collected frame was reallocated at the same address, at which point a stop is
  silently skipped. `TGlobalHeapPtr` is the shape for a global that holds heap cells, and it is
  what Epic's write barriers on a VCell are doing.
- **`ScriptLanguageExtensionProfilingInfo` is a stride trap.** Godot's real `ProfilingInfo` has had
  five fields since 4.3; its `GDREGISTER_NATIVE_STRUCT` string still lists four, so godot-cpp's
  generated struct is 32 bytes for an array whose elements are 40. Writing `p_info_array[i]` for
  any `i > 0` corrupts. The stride comes from `Engine::get_version_info()` instead. Anyone adding
  another native-struct out-parameter should check the registration string against the header
  rather than trusting the generated type.
- **`vh_tick_stats`'s `StructSize` check had to stop comparing against `sizeof`.** The wait fields
  were gated on `StatsSize >= sizeof(vh_tick_stats)`, which was right until a field was appended
  after them — at which point every v6.1 consumer would silently have lost them. Each appended
  group is now tested against its own end.
- **The two debuggers are mutually exclusive, and the refusal has to be said once.** `SetDebugger`
  is a single global pointer, so `verse/host/enable_debugger` (Epic's socket debugger, opened at
  `vh_init`) and Godot's own cannot both be attached. `vh_debug_set_enabled` answers `VH_ERR_STATE`
  rather than overwriting, and the consumer warns once rather than every frame.
- **A stopped stack is walked once and turned into bytes immediately.** `Debugger::FFrame` owns its
  registers and dies with the callback, so there is no later moment at which a value could be
  asked for; and `ForEachStackFrame` skips any frame whose procedure has no file path, which is
  every frame of the generated mirror. Native frames survive, with a name and no location.

