# Phase 5 — Concurrency

**Status:** Plan · 2026-09-13 · **not started.** Written before the work, like
`phase-4.5-design.md` and unlike every other design in this repo. §2's spikes have **not** run, so
§3 onward is written against assumptions that the spikes exist to break.

**Prerequisite: Phase 4.5 is complete.** By decision. `<suspends>` is a second effect axis and it
lands on top of the one 4.5 settles; the alternative — designing both at once — was considered and
rejected. If 4.5 has not run, stop here.

**Companion to:** `spec.md` §7 (R-ASYNC-1 … R-ASYNC-8), §5.3's R-SIG-5, `roadmap.md` "Phase 5",
`dodge-the-creeps.md` wall 3, `phase-4-gaps.md` G9, and §14's **OQ-6**, **OQ-13** and **OQ-16**.

---

## 0. How to read this

**§1 is the decisions, and they are already made.** They came out of an interview, each against
prior art read in Godot's and Unreal's sources rather than recalled, and the citations are inline.
Do not relitigate them; if a spike contradicts one, record that in §14 and raise it.

**§2 is the spikes, and nothing in §3 onward should be trusted until they have run.** Phase 4's
design was written after its spikes for exactly this reason — one of them retired the design the
phase would otherwise have been built around. All four here were chosen because a different answer
changes what gets built rather than merely how.

§14 is deliberately empty and is the most valuable section in the document once the work is done.

---

## 1. Decisions already made

| # | decision | why, and the prior art it rests on |
| --- | --- | --- |
| D1 | **Scopes are two-tier: one `verse::FContentScope` per `vh_instance`, plus one per script class** for tasks started with no instance on the stack | GDScript registers every suspended coroutine on the **script** and *additionally* on the instance when there is one — `_script->pending_func_states.add(…)` then `if (p_instance) p_instance->pending_func_states.add(…)` (`gdscript_vm.cpp:2631-2636`). A `static func` that awaits has `state.instance = nullptr` and is owned by the script alone. So an instance-less task is **not refused**, it is owned one tier up |
| D2 | **A raise stops only the raising call.** Its transaction aborts, its Godot writes are dropped, the error is reported, and the next call runs | Today it halts every script until the next `vh_tick`, on the conservative reading that others should not run against a half-rolled-back scene. That reading was only ever about the scene, and a sibling's writes were already committed. Replaces R-DIAG-3's project-wide halt; `GHaltedUntilTick` and its `TickScripts` resume go away |
| D3 | **Resumption is event-driven, not pumped.** A task awaiting a Godot signal resumes **inside `emit_signal`**, in connection order | GDScript has no scheduler: `GDScriptFunctionState::_signal_callback` is an ordinary `Callable` connected to the awaited signal and it calls `resume()` synchronously (`gdscript_function.cpp:257-286`). C# is the same shape via `SignalAwaiter`. Epic's own native `Sleep` does it this way too — `Call.Suspend(Ctx)` up front, `TStrongVerseCall::Return(Ctx)` from inside the engine callback (`Simulation.cpp:82-150`) |
| D4 | **`vh_tick` keeps only work with no Godot event behind it** — a `spawn`ed body not yet yielded to anything, `Sleep` resumptions, Verse-internal scheduler jobs, reaping | Falls out of D3. It also gives R-ASYNC-6's budget a coherent meaning: it governs that queue, and a resumption inside an emission is unbudgeted, exactly as GDScript's resume is |
| D5 | **The Verse-facing surface is `Await()` on `godot_signal(t)`, `Await()` on a `signal_ref`, and a native `Sleep`.** No `AwaitNextFrame`, no `AwaitPhysicsFrame`, no task handle | Every Godot signal is already a mirrored accessor, so one primitive reaches all of them: `Timer.Timeout.Await()`, `GetTree().ProcessFrame.Await()`, `GetTree().CreateTimer(1.0).Timeout.Await()`. The frame wrappers would add spelling, not reach |
| D6 | **`Sleep` is a real native on the host's own real-time clock** (`FPlatformTime::Seconds()`), not `SceneTreeTimer` | It has to work where there is no SceneTree — `host_smoke`, the probe, a `@tool` script. The cost is stated rather than avoided: it ignores `Engine.time_scale` and it counts wall-clock under `--fixed-fps`. See D7 |
| D7 | **Game code uses timer awaits; `Sleep` is for eventless and host-side work** | The yardstick runs `--fixed-fps 60` headless precisely because "a `Timer` counts real seconds while the loop runs flat out". A real-time `Sleep` is wrong in exactly that run. Wall 3's two waits are both mirrored accessors, so `Await()` alone closes it with no clock question in it |
| D8 | **Tasks ignore pause.** `SceneTree.paused` and `process_mode` do nothing to a Verse task | Matches Godot: a GDScript coroutine is not paused either, its *source* is — a `Timer` stops emitting, so the await stalls. `SceneTreeTimer` even defaults `process_always = true`. One rule, no new state, and the author steers it by choosing what to await. Note D6's consequence: a real-time `Sleep` keeps counting in a paused game, which is consistent with this and worth one line in the manual |
| D9 | **Free cancels; leaving the tree does not.** Plus scene change | `GDScriptInstance::~GDScriptInstance` clears `pending_func_states` (`gdscript.cpp:2069-2073`) — the instance dying is the trigger, not the node leaving the tree. Pooling and re-parenting remove and re-add nodes constantly; Dodge the Creeps' mobs are that shape. **R-ASYNC-5's wording is amended by this phase**, with the reason recorded |
| D10 | **A rebuild cancels every suspended task and reports a count** | Both engines cancel. GDScript: `GDScriptCompiler` calls `cancel_pending_functions(true)` (`gdscript_compiler.cpp:2721`), which warns *"Canceling suspended execution of \"X\" due to a script reload."* (`gdscript.cpp:1516-1529`). C# does not even try — `reload_assemblies` is editor-only, hard-reload only, and skips scripts with non-collectible instances (`csharp_script.cpp:625+`); a pending `await` dies with the ALC. We report a **count**, not a per-method name, because reading a decorated name back off a suspended continuation may not be free |
| D11 | **OQ-16 closes: the task is the owner.** An awaiting continuation is anchored to its task, which is anchored to its scope | R-ASYNC-5 and OQ-16 become one mechanism: free the node → the scope goes → the task cancels → the connection drops. Unbound callbacks *outside* a task stay refused, as Phase 4a decided |
| D12 | **ABI v6, a major bump, is acceptable** | Pre-1.0, no compatibility obligation, and `run_tests.py --build` already rebuilds the test binaries. Design the right ABI and bump it |
| D13 | **Exit is DtC wall 3 down plus `docs/by-hand-checklist.md` run** — the checklist at the *end*, covering all three phases' entries in one windowed session | Same gate shape Phase 4a used: the port is rewritten in place and the diff is the measurement |
| D14 | **A void virtual may be `<suspends>`; a value-returning one may not**, and the refusal is at the member's line | GDScript allows `func _ready(): await …` — the engine gets a `GDScriptFunctionState` back and ignores it, and wall 3's `show_game_over` is written in that shape. But `_Get`, `_HasPoint` and `_CanDropData` must answer a value and a suspended call has none. Refuse it where the author wrote it, never where it fires — the shape G1–G4 gave signals |
| D15 | **Each resumption opens its own nested `AutoRTFM::Transact`** | Under D3 a task resumes inside `emit_signal`, usually inside a Godot callback inside another method's transaction. Without nesting, a raise in the task aborts *that* method's transaction and drops its writes — D2's blast radius back again, in miniature. Nesting makes D2's rule literally true for tasks as well as calls |
| D16 | **Tasks run in the editor only for `@tool` scripts**, exactly as `_Process` does | Matches GDScript. It also keeps a non-tool script's task away from the scene the author is editing, which is the half of R-DIAG-3 Phase 3 could not fix |
| D17 | **`signal_ref.Await()` answers `[]variant`**, unpacked with the existing `As*` readers | Consistent with how `Callv` already answers a foreign method and how the container accessors already work. `As<Type>[V]` is a `<decides>` reader the author already uses, arity is whatever the emitter sent, and the failure lands at the unpack where it can be seen |
| D18 | **The pump resumes in FIFO order** | R-ASYNC-3 asks for deterministic, and `GEnqueuedAsyncJobs` is already a `TQueue`. Stated rather than left emergent; no other order has a claim |

### 1.1 What D3 costs, stated plainly

A resumption inside an emission runs arbitrary Verse **while the outer `vh_instance_call`'s
`AutoRTFM::Transact` is still open**, re-entrantly. That is not a hypothetical — it is the normal
case, because the emission is usually reached from inside a Verse method that Godot called. It is
S-4's whole subject, and it is the single most likely thing to retire part of this design.

**D15 is the answer to the half of it that is a policy rather than a measurement.** A nested
transaction per resumption is what keeps one task's raise from rolling back the method that happened
to emit the signal. Whether AutoRTFM's nesting behaves that way *here* is a measurement, and S-4
takes it in the same sitting.

---

## 2. Spikes — run all four before writing anything

### S-1 — does a task already resume across `vh_tick`?

**Why first.** `vh_instance_call` already handles `FOpResult::Yield` with the comment "A `<suspends>`
method started a task instead of completing… the task runs on under `vh_tick`"
(`HostScript.cpp:5537-5541`), and `PumpEventLoop` already drains `GEnqueuedAsyncJobs` with the budget
(`HostEventLoop.cpp`). **No test has ever called a `<suspends>` script method and checked it
resumed.** R-ASYNC-1 may be substantially free, or that comment may be describing dead code.

**How.** `tests/host_smoke`, no Godot: a fixture class with a `<suspends>` method that sets a member,
yields, and sets it again. Instantiate, call it, assert the first write happened and the second did
not, drive `vh_tick` in a loop, assert the second write happened. Then the same with `spawn`, `race`
and `sync` bodies.

**What each answer changes.** Free → R-ASYNC-1 is a test, not a feature, and the phase is §4–§8 only.
Not free → §3 has to build a scheduler and the phase roughly doubles. **This spike also builds the
tick-loop harness §11 needs**, so it pays for itself either way.

### S-2 — can `FContentScope`s be per-instance?

**Why.** D1's feasibility, and the phase's first item.

**How.** Host-side. Questions, in order: can several scopes coexist; is `FContentScopeGuard` a stack
or a single active slot (`GetActiveScope()` suggests one active at a time, so every
`vh_instance_call` would push and pop); what does that cost per `_Process` call on a project with
many scripted nodes; and does the scope's `UObject` outer have to be rooted per instance the way
`UPlaceholderObjectForContentScope::MakeRooted()` does today, which would put a GC object per node.

**What each answer changes.** If the guard does not nest, every entry point needs a
push/call/pop discipline and the cost is measurable — take the number with `tools/build_bench.py`
the way `phase-2-design.md` §3.1 took its numbers. If per-instance scopes are infeasible, D1 falls
and R-ASYNC-4 needs a different mechanism entirely, which is a design-retiring answer.

### S-3 — can a native `<suspends>` be declared through VNI in our package?

**Why.** R-SIG-5's and `Sleep`'s feasibility. Epic's shape is
`FVerseResult Sleep(TVerseCall<void> Call, double Seconds)` with `Call.Suspend(ExecContext)`,
a `TStrongVerseCall<void>` captured into the engine callback calling `Call.Return(ExecContext)`, and
`Call.Defer(ExecContext, …)` to unregister on cancellation (`Simulation.cpp:82-150`). Whether our
VNI bindings in `host/Verse` can carry that signature shape is not known.

**How.** Declare `VhSleep<native>(Seconds:float)<suspends>:void` in `Godot.native.verse`, implement
it in `GodotBindings.cpp` over `FPlatformTime::Seconds()` and the pump, build the host, and run
`tests/verse_probe` — remembering that **a host build passing is not enough to know a `.verse` file
compiles**, because VNI compiles `host/Verse` at build time against one package set and the runtime
compiler re-reads the same files against another.

**Also ask, in the same sitting:** does `Call.Return` work from inside a Godot signal callback that
is already in `AutoRTFM::Open`? That is D3's mechanism and it is cheaper to ask here than to discover
in §4.

**What each answer changes.** Refused → `Await` cannot be a native suspends and R-SIG-5 needs
another shape (or is blocked, which is a spec-status answer rather than a workaround).

### S-4 — what happens to a suspended task's deferred writes?

**Why.** `vh_instance_call` wraps `Invoke` in `AutoRTFM::Transact`, and every Godot write defers to
`AutoRTFM::OnCommit`. When the call **yields**, that transaction commits with the task still running.
So: which transaction do the task's *later* Godot writes belong to? Nothing in the repo answers this,
and D3 makes it worse by resuming re-entrantly inside an emission that is itself inside another
transaction.

**How.** Host-side, on top of S-1's harness. A `<suspends>` method that writes a Godot property,
yields, writes another, and a variant that fails after the yield. Assert when each write lands and
what the failure undid. Then the re-entrant case: a method that emits a signal whose handler is an
awaiting task, from inside a transaction.

**Also ask, in the same sitting:** does a nested `AutoRTFM::Transact` around the resumption confine an
abort to the task's own writes, leaving the outer call's intact? That is D15, and it is the thing
that makes D2's rule true for tasks. If nesting does not confine it, D15 falls back to the pumped
resumption point D3 declined.

**What each answer changes.** If a resumption's writes have no transaction, they are un-rollbackable
and that is a rule for §7 and for 4.5's paragraph. If re-entrant resumption is unsafe, D3 needs the
fallback that was offered and declined — a pumped resumption point — and §14 records why.

---

## 3. R-ASYNC-4 — two-tier scopes

**First, because it is a correctness bug rather than a feature.** Today one `verse::FContentScope`
made in `GodotVerse::EnterContentScope` (`HostScript.cpp:577`) serves the whole project, so a raise's
`Terminate()` cancels every script's suspended work and `ResetTerminationState()` replaces the task
group wholesale.

**Build:**

- A scope per `vh_instance`, created at `vh_instantiate`, terminated and released at
  `vh_release_instance`.
- A scope per script class per generation, for tasks started with no instance on the stack (D1).
  Cancelled at rebuild, which is when everything is cancelled anyway (D10).
- Every entry point that runs Verse enters the right scope's guard. `EnterVerse` is the one place
  this goes — it exists precisely so a seventh entry point cannot be added that forgets the revive,
  and the same argument applies to the guard.
- **`ReviveContentScope` goes away.** It exists because one script's first raise ended Verse for the
  process; with D2 a raise no longer terminates a scope that anything else depends on. Read the
  comment above it before deleting it — it records the defect it was written for, and that record
  should move rather than vanish.
- **`GHaltedUntilTick`, `GTasksLostToError` and `VH_ERR_HALTED`'s reason for existing all change.**
  `VH_ERR_HALTED` may still be the right answer for a call into an instance whose scope is
  terminated; it is no longer a project-wide state.

**G9 becomes free here.** `phase-4-gaps.md` G9 says Epic's callback→scope link
(`FVerseEventCallbackList` drops a callback when its scope *terminates*, `VerseEvent.cpp:169-188`) is
a regression to copy onto one process-wide revived scope, and that the real content of G9 is
R-ASYNC-4. Once scopes are per instance, **adopt Epic's rule**: a callback records the scope it was
subscribed in and a terminated scope drops it. That is also D11's mechanism.

**Done when** a raise in one script's task leaves another script's suspended work running, proven by
a `host_smoke` case, and the project-wide cancellation message in `TickScripts` is deleted because
there is nothing project-wide left to say.

---

## 4. R-SIG-5 and R-ASYNC-2 — `Await`

    godot_signal(t).Await<public>()<suspends>:t

One method on the class `GodotApi.native.verse` already declares, so it covers a script's own
declared signals **and** all 489 mirrored engine-signal accessors with no per-signal work:

```verse
    MessageTimer.Timeout.Await()
    GetTree().CreateTimer(1.0).Timeout.Await()
    GetTree().ProcessFrame.Await()
    Hit.Await()                    # the script's own
```

**Mechanism** (D3, pending S-3): `Await` is a native `<suspends>` that connects to the Godot signal
one-shot, `Call.Suspend`s, and `Call.Return`s the payload from inside the emission. `Call.Defer`
disconnects when the task is cancelled — which is what makes a cancelled `race` branch not leave a
connection behind.

**The payload is `t`**, which the existing machinery already handles in both directions: a struct
payload crosses as one Godot argument per field outbound, and `InstanceCall`'s rule that N arguments
satisfy one struct parameter with N fields brings it back (G21). `Await` returns the same shape
`Subscribe`'s callback receives.

**A foreign signal** — one declared by a GDScript or C# script, or made with `add_user_signal` — has
no mirrored accessor. It crosses today as a `signal_ref` (`VhToSignal`, tag 26), so **give
`signal_ref` an `Await()<suspends>:[]variant` too** (D17). The author unpacks it with the `As*`
readers they already use on `Callv`'s result, arity is whatever the emitter sent, and a wrong
expectation fails at the unpack rather than silently. That is the R-INT-1 half, and it is smaller
than a by-name lookup because the reference lane already exists.

### 4.1 A `<suspends>` virtual

D14. `_Ready`, `_Process` and `_Input` may suspend; `_Get`, `_HasPoint`, `_CanDropData` and every
other value-returning virtual may not, because a suspended call has no value and reporting success
with nothing written is precisely the "did not run" reading as "ran and found nothing" that kept the
Phase 3 defect invisible for a phase.

**Where the refusal goes:** the validation pass that `GetClassSignals` already runs, at the member's
line, with a message naming the virtual and its return type. `vh_export_desc` carries a `Reject` and
`vh_signal_desc` carries one; the virtual descriptor needs the same. Godot is never told about a
virtual the bridge cannot honour — the same rule that keeps it from being told about a signal
nothing can emit.

**Watch for** `_Notification`, which is hand-written on the native root rather than generated, and so
needs the check written twice or the check moved somewhere both paths reach.

**One-shot, not subscribe-then-cancel.** Godot has `CONNECT_ONE_SHOT`; use it, so a task that is
cancelled between the connect and the emission cannot be resumed by a stale connection.

**Done when** `race(A.Await(), B.Await())` leaves exactly zero connections behind, and a Verse task
awaiting a GDScript-declared signal resumes.

---

## 5. `Sleep`

    VhSleep<native>(Seconds:float)<suspends>:void

Host-side, on `FPlatformTime::Seconds()`, resuming from the pump (D4, D6). Deliberately **not**
`SceneTreeTimer`, because it must work with no tree: `host_smoke`, the probe, a `@tool` script.

**Its divergences are documented, not hidden:** it ignores `Engine.time_scale`, it counts wall-clock
under `--fixed-fps`, and it keeps counting in a paused game (which is consistent with D8). The
manual sentence is *"`Sleep` is real time. For game timing, await a `Timer` or
`GetTree().CreateTimer(…).Timeout` — those are the engine's clock and behave the way a Godot author
expects."*

`Sleep(0.0)` should mean "resume at the next pump" — the shape Epic's `Sleep` gives it, and the
cheapest way to yield a frame from host-side code.

---

## 6. R-ASYNC-5 — cancellation

- **A freed instance cancels its tasks.** `vh_release_instance` terminates the instance's scope.
  Prior art: `GDScriptInstance::~GDScriptInstance` (`gdscript.cpp:2069-2073`).
- **Leaving the tree does not** (D9). R-ASYNC-5's wording in `spec.md` is amended in this phase, in
  a commit that says why: pooling and re-parenting remove and re-add nodes constantly, GDScript
  coroutines survive it, and what actually stalls is the source being awaited.
- **A scene change cancels what it unloads**, which falls out of the first rule — the nodes are
  freed. Verify rather than assume; Godot frees a replaced scene on the next idle
  (`_flush_delete_queue`), not synchronously.
- **A rebuild cancels everything, reporting a count** (D10).

---

## 7. R-ASYNC-3 — the ordering, documented

With D3 there is almost nothing to invent, which is the point. The document is a table that cites
Godot's own source:

| you await | you resume | where |
| --- | --- | --- |
| `GetTree().PhysicsFrame` | before that step's `_physics_process` pass | `scene_tree.cpp:649`, then `_process(true)` at `:655` |
| `GetTree().ProcessFrame` | before that frame's `_process` pass | `scene_tree.cpp:713`, then `_process(false)` at `:719` |
| a `SceneTreeTimer` timeout | after `_process`, in `process_timers` | `scene_tree.cpp:729` (idle) / `:660` (physics) |
| any node's signal | inside that `emit_signal`, in connection order | — |
| `Sleep` | at the pump: end of `Main::iteration`, after `RenderingServer::draw` | `main.cpp:5107` |

The one rule we own is the last row, and it exists because nothing in Godot fires it. Within that
row, resumptions are **FIFO** (D18) — `GEnqueuedAsyncJobs` is already a `TQueue` and no other order
has a claim.

### 7.1 The editor

**Tasks run in the editor only for `@tool` scripts** (D16), the same rule `_Process` follows, so the
pump has to know which instances are tool instances. Two details the rule does not cover and the
implementation must:

- **`vh_tick` no-ops while a background analysis is running** (`VerseHost.cpp:272-275`), because
  VerseVM blocks execution for the length of one. So editor-side tasks stall per keystroke and then
  resume in a burst. With D6's real-time `Sleep` that burst can be several seconds' worth at once.
  Decide whether the pump drains it or spreads it, and say which.
- **A `@tool` script's task is still a task**, so D10 cancels it at rebuild like any other — which in
  the editor is every build the author triggers.

**Also record what `ScriptLanguage` does not offer**: there is no per-physics-step hook — `frame()`
is the only one (`script_language.h:332`) and `Main::iteration` calls it last (`main.cpp:5103-5108`).
That is why the pump is where it is, and it is the reason D3 matters.

---

## 8. R-ASYNC-6 — the budget

`verse/runtime/frame_budget_ms` exists and defaults to 4.0 (`verse_script_language.cpp:320-333`),
and `PumpEventLoop` already breaks on it. What is missing is **observable** and **reported**:

- an overrun says so, rate-limited so a consistently over-budget frame does not flood the log;
- the queue depth and time spent are readable — a `Performance` custom monitor is the Godot-native
  way, and it costs little;
- the doc says what the budget governs, which after D4 is the pump only. A resumption inside an
  emission is not budgeted, exactly as GDScript's is not.

---

## 9. ABI v6

Expected changes — confirm against S-2 before committing to them:

- `vh_instantiate` / `vh_release_instance` gain scope lifetime meaning. The release path becomes
  load-bearing rather than advisory and the header should say so.
- An entry point to cancel an instance's tasks without releasing it, if §6 finds a case that needs
  it (scene change may).
- Whatever S-1 finds the pump needs. **Note:** a delta parameter on `vh_tick` was considered for
  `Sleep` and is **not** needed under D6 — `Sleep` is real time. Do not add it speculatively.
- Overrun/queue reporting for §8, which can ride the existing diagnostic callback rather than a new
  entry point.

Bump `VH_ABI_VERSION_MAJOR` to 6, rebuild **both** DLLs, and remember the mismatch surfaces at
`vh_init` rather than at compile time.

---

## 10. What Phase 5 does not do

- **OQ-6 / R-ASYNC-7.** Phase 5 must not foreclose it. R-ASYNC-8's refusal stands. **The scoping
  document is not a deliverable of this phase** — the roadmap's "spawn the threading scoping
  document" means the OQ-6 row gets updated with what this phase settled and what it left, not that
  someone writes `oq-6-threading.md` here.
- **Bounding a runaway task.** A task that yields to the pump every frame and never finishes is
  OQ-13's shape pointed at tasks rather than at raises, and this phase creates it. Record it on the
  OQ-13 row rather than solving it; whatever bounds a script that raises every frame should bound
  this too, and both want per-instance scopes first, which is what this phase builds.
- **OQ-13** — bounding a script that raises every frame. Phase 6, with the rest of R-DIAG-3. D2
  changes its shape (a raise no longer halts the project) and that should be noted in the OQ row.
- **OQ-17** — C# has still never been run against this bridge, and `Await` on a `signal_ref` makes
  the claim bigger without testing it. Record it; do not pretend otherwise.

---

## 11. Tests

Two layers, both existing. `run_tests.py` stays the one command (R-QUAL-3).

**`host_smoke`, a new tick-loop layer** — built by S-1 and kept. No Godot at all: instantiate, call a
`<suspends>` method, drive `vh_tick`, assert resumption. This is where R-ASYNC-1, the two-tier scope
behaviour, `Sleep`, and S-4's transaction findings get pinned, because it is the fastest place and
the one with no scene tree to confuse the question.

**`tests/integration/test_main.gd`** — GDScript can `await`, so a case can emit a signal, await a
frame, and assert the Verse task resumed. R-SIG-5, the foreign-signal `Await`, R-ASYNC-5's free-
cancels-tasks rule, and D10's rebuild cancellation live here. Keep the one-line-per-case shape and
`quit(1)` on failure.

**Not a new project.** `coverage_diagnostic` is separate because its script deliberately does not
compile; a task that hangs is a different failure and the tick-loop layer catches it before the
integration project ever sees it.

`tests/verse_probe` stays the tool for the language questions — S-3 is one, and so is anything of the
form "does `race` over a native `<suspends>` do what we think".

---

## 12. Exit

**Dodge the Creeps wall 3 falls.** `hud.verse`'s game-over sequence goes back to the seven readable
lines the GDScript has:

```gdscript
func show_game_over():
	show_message("Game Over")
	await $MessageTimer.timeout
	$MessageLabel.text = "Dodge the\nCreeps"
	$MessageLabel.show()
	await get_tree().create_timer(1).timeout
	$StartButton.show()
```

The `hud_phase` enum goes, the **second Timer node the port added** goes, and the two timeout
handlers that existed only to read the phase go. Per D7 the port uses the timer awaits, not `Sleep`,
so `--fixed-fps 60` stays and `headless_check.gd`'s checks stay deterministic. **The diff is the
measurement**, and `dodge-the-creeps.md`'s wall table gets its seventh row struck.

**And `docs/by-hand-checklist.md` is run** — one windowed session covering all three phases' entries:
Phase 3's windowed yardstick run and editor session, Phase 4's Node panel, `_make_function`,
`_HasPoint` and `_CanDropData` flows, and whatever Phase 5 adds. Nothing on it can run headless, so
it needs a human at a windowed editor. **Ask before launching the editor** — `tools/run_tests.py`'s
headless Godot is fine unprompted; a window is not.

---

## 13. Risks

- **S-4 is the design-retiring one.** If re-entrant resumption inside an open transaction is unsafe,
  D3 needs the pumped fallback that was offered and declined, and every ordering claim in §7 changes.
  Find out first, not in §4.
- **S-2's cost.** A guard push/pop per `vh_instance_call` is on the hot path — every `_Process` on
  every scripted node, every frame. Measure it with `tools/build_bench.py` rather than assuming, the
  way `phase-2-design.md` §3.1 did.
- **A GC object per instance.** If the scope's outer must be rooted per instance the way
  `UPlaceholderObjectForContentScope::MakeRooted()` roots the one today, a scene with a thousand
  scripted nodes has a thousand rooted UObjects. `HostEventLoop.cpp`'s `TickGC` already watches
  `GetObjectArrayEstimatedAvailable()`; this is the thing that would move that number.
- **D2 is a behaviour change to something that was recently fixed.** R-DIAG-3's project-wide halt was
  written deliberately in Phase 3 and `phase-3-design.md` §11 explains it. Changing it means editing
  that requirement, not quietly contradicting it.
- **A suspended task keeps its instance reachable.** A continuation holds `self`, and `vh_instance`
  is a UObject. A Godot Node is freed explicitly rather than refcounted, so the script instance is
  destroyed underneath a task that still references it — either the task is cancelled first (D9, and
  it must be) or there is a dangling reference, or the reference keeps a dead node's UObject alive
  and `TickGC` sees the count climb. S-2 should look at this while it is already in the scope
  lifetime code.
- **`Await` enlarges R-INT's untested claim.** OQ-17 stands, and D17 makes the untested surface
  bigger by reaching GDScript-declared signals that no fixture exercises.

---

## 14. What building this corrected

*Empty. Fill it in after. `phase-2-design.md` §11 and `phase-3-design.md` §11 are the sections
everyone is told to read first, and they exist only because someone wrote them once the work was
done. Record which of §1's thirteen decisions survived contact, what the spikes retired, and what the
implementation could not do.*
