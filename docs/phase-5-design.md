# Phase 5 — Concurrency

**Status:** Built · 2026-09-13. **§14 is the section to read first** — it was written after the
work, and it is where this document turned out to be wrong. §2 was filled in the same way *before*
the work, from twelve questions put to the Verse compiler through `tests/verse_probe`, which is why
so little of §3 onward needed correcting.

**Prerequisite: Phase 4.5 is complete** — built 2026-09-13.

The short version of §14: all four remaining spikes answered positively and none retired a
decision. **D23 is retired** (a scope per instance is made at `vh_instantiate`, because "lazily at
the first `spawn`" has no hook to hang on) and **D17 is amended** (a foreign signal's payload is a
`godot_array`, not `[]variant`, which has no description on this bridge). S-5 took neither of the
two shapes it offered: `verse::event` is a UObject with a public C++ `Signal`, so the host signals
the event directly and there is no Verse-side callback on the signal path at all.

**Companion to:** `spec.md` §7 (R-ASYNC-1 … R-ASYNC-8), §5.3's R-SIG-5, `roadmap.md` "Phase 5",
`dodge-the-creeps.md` wall 3, `phase-4-gaps.md` G9, and §14's **OQ-6**, **OQ-13** and **OQ-16**.

**Five things 4.5 changed under this document.** Read [`phase-4.5-design.md`](phase-4.5-design.md)
§11 first.

- **The lattice has a third level**: `<computes>` ⊂ `<reads>` ⊂ `<transacts>` ⊂ default. `suspends`
  is a separate axis that sits on top of any of them — except that the one thing a script actually
  wants to await with cannot be narrowed at all (S-0 F5).
- **An archetype instantiation carries the constructing class's own effect.** `variant`, `godot_ref`
  and the container wrappers are `<computes>`; a mirrored class cannot be, because it descends from
  the native `vh_object`.
- **`Verse::Stm::OnRollback` does nothing here.** The mechanism that works from inside
  `AutoRTFM::Open` is `AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>`.
- **A raise halts every script until the next `vh_tick`** (R-ASYNC-4). D2 replaces that.
- **§4.2 is scope 4.5 handed over**: `Object.Connect` has no rollback-safe spelling, and the
  machinery to give it one is the machinery `signal_ref.Await()` needs anyway.

---

## 0. How to read this

**§1 is the decisions.** D1–D18 came out of an interview against prior art read in Godot's and
Unreal's sources. D19–D24 came out of a second interview, after §2 ran. **D14 is retired and D5 is
amended**, and both say so in place. Do not relitigate them; if a later spike contradicts one,
record that in §14 and raise it.

**§2 ran before the work and §14 was written after it.** Between them the rest of the document is
history: read it for why something is shaped the way it is, not for what the code does. Where §3–§12
and §14 disagree, §14 is the record.

**§2.2's four remaining spikes are all answered in §14.1**, S-4 — the one this document twice called
design-retiring — included. It retired nothing.

---

## 1. Decisions

| # | decision | why, and the prior art it rests on |
| --- | --- | --- |
| D1 | **Scopes are two-tier: one `verse::FContentScope` per `vh_instance`, plus one per script class** for tasks started with no instance on the stack | GDScript registers every suspended coroutine on the **script** and *additionally* on the instance when there is one — `_script->pending_func_states.add(…)` then `if (p_instance) p_instance->pending_func_states.add(…)` (`gdscript_vm.cpp:2631-2636`). A `static func` that awaits has `state.instance = nullptr` and is owned by the script alone. So an instance-less task is **not refused**, it is owned one tier up |
| D2 | **A raise stops only the raising call.** Its transaction aborts, its Godot writes are dropped, the error is reported, and the next call runs | Today it halts every script until the next `vh_tick`, on the conservative reading that others should not run against a half-rolled-back scene. That reading was only ever about the scene, and a sibling's writes were already committed. Replaces R-DIAG-3's project-wide halt; `GHaltedUntilTick` and its `TickScripts` resume go away |
| D3 | **Resumption is event-driven, not pumped.** A task awaiting a Godot signal resumes **inside `emit_signal`**, in connection order | GDScript has no scheduler: `GDScriptFunctionState::_signal_callback` is an ordinary `Callable` connected to the awaited signal and it calls `resume()` synchronously (`gdscript_function.cpp:257-286`). C# is the same shape via `SignalAwaiter`. Epic's own native `Sleep` does it this way too (`Simulation.cpp:82-150`). **S-0 F9 measured that this is already true of Verse's own `event`**: a suspended task resumed synchronously inside the call that signalled it |
| D4 | **`vh_tick` keeps only work with no Godot event behind it** — a `spawn`ed body not yet yielded to anything, `Sleep` resumptions, Verse-internal scheduler jobs, reaping | Falls out of D3. It also gives R-ASYNC-6's budget a coherent meaning: it governs that queue, and a resumption inside an emission is unbudgeted, exactly as GDScript's resume is |
| D5 | **The Verse-facing surface is `spawn`, `Await()` on `godot_signal(t)`, `Await()` on a `signal_ref`, and a native `Sleep`.** No `AwaitNextFrame`, no `AwaitPhysicsFrame`, no task handle | **Amended by S-0.** The original decision omitted `spawn`, on the assumption that a `<suspends>` method could be called wherever a script wanted one. It cannot: an awaiting body is always wide (F5), and a wide body is reached with `spawn` from a wide caller (F1, F6). `spawn` is therefore part of the surface an author is taught, not an implementation detail. The rest stands — every Godot signal is a mirrored accessor, so one primitive reaches all of them |
| D6 | **`Sleep` is a real native on the host's own real-time clock** (`FPlatformTime::Seconds()`), not `SceneTreeTimer` | It has to work where there is no SceneTree — `host_smoke`, the probe, a `@tool` script. The cost is stated rather than avoided: it ignores `Engine.time_scale` and it counts wall-clock under `--fixed-fps`. See D7 |
| D7 | **Game code uses timer awaits; `Sleep` is for eventless and host-side work** | The yardstick runs `--fixed-fps 60` headless precisely because "a `Timer` counts real seconds while the loop runs flat out". A real-time `Sleep` is wrong in exactly that run. Wall 3's two waits are both mirrored accessors, so `Await()` alone closes it with no clock question in it |
| D8 | **Tasks ignore pause.** `SceneTree.paused` and `process_mode` do nothing to a Verse task | Matches Godot: a GDScript coroutine is not paused either, its *source* is — a `Timer` stops emitting, so the await stalls. `SceneTreeTimer` even defaults `process_always = true`. One rule, no new state, and the author steers it by choosing what to await. D6's consequence — a real-time `Sleep` keeps counting in a paused game — is consistent with this and worth one line in the manual |
| D9 | **Free cancels; leaving the tree does not.** Plus scene change | `GDScriptInstance::~GDScriptInstance` clears `pending_func_states` (`gdscript.cpp:2069-2073`) — the instance dying is the trigger, not the node leaving the tree. Pooling and re-parenting remove and re-add nodes constantly; Dodge the Creeps' mobs are that shape. **R-ASYNC-5's wording is amended by this phase**, with the reason recorded. See D22 for what "free" means in a `queue_free` world |
| D10 | **A rebuild cancels every suspended task and reports a count** | Both engines cancel. GDScript: `GDScriptCompiler` calls `cancel_pending_functions(true)` (`gdscript_compiler.cpp:2721`), which warns *"Canceling suspended execution of \"X\" due to a script reload."* (`gdscript.cpp:1516-1529`). C# does not even try — a pending `await` dies with the ALC. We report a **count**, not a per-method name, because reading a decorated name back off a suspended continuation may not be free |
| D11 | **OQ-16 closes: the task is the owner.** An awaiting continuation is anchored to its task, which is anchored to its scope | R-ASYNC-5 and OQ-16 become one mechanism: free the node → the scope goes → the task cancels → the connection drops. Unbound callbacks *outside* a task stay refused, as Phase 4a decided |
| D12 | **ABI v6, a major bump, is acceptable** | Pre-1.0, no compatibility obligation, and `run_tests.py --build` already rebuilds the test binaries. Design the right ABI and bump it |
| D13 | **Exit is DtC wall 3 down plus the by-hand checks run** — at the *end*, covering all three phases' entries in one windowed session | Same gate shape Phase 4a used: the port is rewritten in place and the diff is the measurement. Met; [`by-hand-findings.md`](by-hand-findings.md) is what they found |
| ~~D14~~ | ~~A void virtual may be `<suspends>`; a value-returning one may not~~ — **retired by S-0 F7** | A virtual override **cannot carry `<suspends>` at all**, and the refusal is not the effect error the decision assumed. `_Ready<override>()<suspends>:void` is glitch **3532** *"must have a distinct domain from these other functions with the same name"* plus **3523** *"has an `<override>` attribute, but could not find a parent function to override"* — the specifier makes it a **different function**, so there is nothing to override. §4.1's virtual-descriptor `Reject` machinery was built for a case the compiler refuses two steps earlier, and it is struck. What replaces it is **D19**, and a sentence in the manual rather than a diagnostic rewrite |
| D15 | **Each resumption opens its own nested `AutoRTFM::Transact`** | Under D3 a task resumes inside `emit_signal`, usually inside a Godot callback inside another method's transaction. Without nesting, a raise in the task aborts *that* method's transaction and drops its writes — D2's blast radius back again, in miniature. Nesting makes D2's rule literally true for tasks as well as calls. **Unmeasured: this is S-4's** |
| D16 | **Tasks run in the editor only for `@tool` scripts**, exactly as `_Process` does | Matches GDScript. It also keeps a non-tool script's task away from the scene the author is editing, which is the half of R-DIAG-3 Phase 3 could not fix |
| D17 | **`signal_ref.Await()` answers `[]variant`**, unpacked with the existing `As*` readers | Consistent with how `Callv` already answers a foreign method and how the container accessors already work. `As<Type>[V]` is a `<decides>` reader the author already uses, arity is whatever the emitter sent, and the failure lands at the unpack where it can be seen |
| D18 | **The pump resumes in FIFO order** | R-ASYNC-3 asks for deterministic, and `GEnqueuedAsyncJobs` is already a `TQueue`. Stated rather than left emergent; no other order has a claim |
| **D19** | **A task is started with `spawn`, from a wide context, and a virtual is the wide context a script already has.** The manual says so; nothing diagnoses it | Replaces D14. F6 measured that `_Ready`, `_Process` and `_EnterTree` overrides on a `node2d` may each `spawn` a task, because the mirror generates a virtual with **no effect specifier** and an override therefore carries the default effect set — the widest there is, and the only one `spawn` of an awaiting body is allowed from. The refusal a curious author meets (F7's 3532/3523) is the compiler's, at their own line, and a `_validate` rewrite was considered and declined: the phase is not building suspending virtuals, and a sentence in the docs and the `.verse` template is cheaper than another diagnostic |
| **D20** | **`godot_signal.Subscribe`'s callback widens to specifier-less**, matching Verse's own `subscribable` | `subscribable<native>(t:type) := interface: Subscribe<public>(Callback(:t):void)<transacts>:cancelable` (`Subscribable.native.verse`) — Epic's callback parameter carries the **default** effect set and only the *method* is `<transacts>`. `GodotApi.native.verse` declares `Callback(:t)<transacts>:void` instead, and the comment justifying it — *"`subscribable.Subscribe` fixes its callback at a no_rollback domain that could not touch Godot"* — has the lattice backwards: the default set **contains** transacts, so a wide callback can touch Godot and more. Measured (F10, F11): an existing `<transacts>` handler still satisfies a widened parameter, so **no script breaks**; a widened handler can `spawn`; and the reason Epic's `signalable.Signal` is wide while `Subscribe` is not is that `Signal` *invokes* the callback and a `<transacts>` body may not — which does not apply here, because our host invokes subscribers from C++ through `VFunction::Invoke`, where Verse's effect checking is not in the way. **This is the one line that unblocks wall 3** |
| **D21** | **`Await` is Verse's own `event(t)` inside `godot_signal(t)`. No parametric native** | F8 measured that `/Verse.org/Verse`'s `event(t)` is nameable from the script package, that an ordinary parametric Verse class may hold one, and that `Await<public>()<suspends>:t = Ev.Await()` compiles and answers a typed `t`. F9 measured a full suspend-across-`vh_tick`-and-resume cycle over it. That removes the hardest unknown the first draft carried — a native cannot be parametric in its return type, which is why `VhSignalSubscribe` takes `Callback:any` — and it removes the need for any new native on the signal path. **S-5 is what is left of it**: how the host delivers a *typed* payload into that event |
| **D22** | **Cancellation triggers on the real free, not on `queue_free`.** The window is documented rather than closed | `~GDScriptInstance` is GDScript's trigger and `vh_release_instance` is ours. Godot defers the actual free to `_flush_delete_queue`, so a `queue_free`d node's task keeps running until then and may emit or write in that window. Matching GDScript exactly is worth more than closing a window Godot leaves open for its own scripts, and a new `is_queued_for_deletion` hook would diverge in a way every author would have to be told about. §7's ordering table gets the row |
| **D23** | **A per-instance scope is created lazily**, on the instance's first `spawn`, not at `vh_instantiate` | Answers §13's "a GC object per instance" risk directly: a project where three nodes await pays for three scopes rather than one per scripted node. It keeps R-ASYNC-4 literally — the boundary is the instance — while the hot path pays only when there is something to guard. S-2 measures what the guard costs on the entries that do have a scope |
| **D24** | **A terminated scope is replaced, not revived, and the replacement is immediate** | `ReviveContentScope` exists because one script's first raise ended Verse for the process; it un-terminates the single global scope at the next `vh_tick`. Epic never revives — `ContentScopeRepository` hands out a *fresh* one (`ContentScopeRepository.h:80-92`). With D1 and D2 there is nothing project-wide left to revive, so the instance whose scope terminated gets a new one at its **next call**, not at the next frame boundary. `GHaltedUntilTick`, `GTasksLostToError` and `TickScripts`'s resume message all go with it. What is lost is that instance's suspended work, which is what a raise costs and what D10's count-reporting shape should report |

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

## 2. S-0 — what the compiler actually allows

**This ran.** Twelve questions put to the Verse compiler through `tests/verse_probe` in one sitting,
against the host built at `bin/verse_host.build.txt`'s revision. The fixtures are committed, so
every row below can be re-run rather than taken on trust:

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        tests/verse_probe/async_probe.verse --class async_probe

and the same for `narrow_suspends.verse`, `event_probe.verse`, `event_resume.verse`,
`wide_callback.verse`, `virtual_spawn.verse` and `async_reject.verse` — the last of which compiles
**nothing**, on purpose: it is the file of refusals, and the *text* of each refusal is its result.

| # | question | answer |
| --- | --- | --- |
| **F1** | May a `<transacts>` body use `spawn`? | **Yes.** `AnalyzeMacroCall_Spawn` requires `Transacts & ~Dictates` and adds `NoRollback` only when `TargetVM != VerseVM \|\| !CanAccessEpicInternal()` (`SemanticAnalyzer.cpp:18655-18660`); `CanAccessEpicInternal()` is true for `EVerseScope::InternalUser` (`SemanticScope.cpp:807-811`), which is what `AddScriptPackage` sets (`HostScript.cpp:295`). Round one's glitch landed on the *callee* at column 14, not on the macro |
| **F2** | May a `<reads>` body? | **No.** *"This 'spawn' macro has effects that are not allowed by its context: writes, allocates"*. So 4.5's `<reads>` narrowing and starting a task are mutually exclusive, which is worth one line in the manual |
| **F3** | Does `spawn` take a block, or only a coroutine call? | **A block**, with as many expressions as you like. The *"Currently, `spawn` expects a single coroutine call"* refusal is on the non-VerseVM path only |
| **F4** | Can `<suspends>` be narrowed? | **Yes.** `<suspends><transacts>`, `<transacts><suspends>` and `<suspends><reads>` all compile, in either order, and `race`, `sync`, `rush` and `branch` all compile inside a narrowed body. Narrowing stays contagious downward: a narrowed body calling a wide one is 3512 |
| **F5** | Can an *awaiting* body be narrowed? | **No, and this is the finding that shaped the phase.** `event.Await` carries `no_rollback` — `WaiterNarrow()<suspends><transacts>:void = Ev.Await()` is *"calls a function (`(/Verse.org/Verse/event:)Await`) that has the 'no_rollback' effect"*. `awaitable.Await` is declared with no specifier (`Awaitable.native.verse`), exactly as `signalable.Signal` is. So an awaiting body is **always wide**, and F1's permission does not reach it |
| **F6** | Which contexts can therefore start an awaiting task? | **Specifier-less ones.** A mirrored virtual override is specifier-less — the mirror generates `_Ready<public>():void = {}` (`GodotClasses.native.verse:28976`) — and `_Ready`, `_Process` and `_EnterTree` overrides on a `node2d` each `spawn` an awaiting body cleanly. A `Subscribe` handler fixed at `<transacts>` **cannot**, which is what forced D20 |
| **F7** | Can an override be written `<suspends>`? | **No**, and not for the reason D14 assumed. It is **3532** *"must have a distinct domain from these other functions with the same name"* plus **3523** *"has an `<override>` attribute, but could not find a parent function to override"*, plus a third 3532 calling the definition *ambiguous*. The specifier makes it a different function. D14 is struck |
| **F8** | Is `/Verse.org/Verse`'s `event(t)` reachable from a script? | **Yes.** A parametric Verse class may hold `Ev:event(t) = event(t){}` and declare `Await<public>()<suspends>:t = Ev.Await()`, typed. `event.Signal` is wide, so a specifier-less body may signal it and a `<transacts>` one may not |
| **F9** | Do tasks already suspend and resume across `vh_instance_call` and `vh_tick`? | **Yes, end to end, with no host work at all.** A `<transacts>` call spawned a task, the task suspended on `Ev.Await()`, the call returned `VH_OK`, `vh_tick` ran, the next call's `Signal` resumed it **synchronously** — the resumed body's output printed before the signalling call returned — and the member the task wrote read back correctly from a third call. **R-ASYNC-1 is a test rather than a feature**, and D3's event-driven resumption is already how Verse's own event behaves |
| **F10** | Does widening `Subscribe`'s callback break existing handlers? | **No.** A `<transacts>` handler satisfies a specifier-less callback parameter — effects are contravariant in that position, measured rather than reasoned. Every handler in `dodge-the-creeps` keeps compiling |
| **F11** | May a `<transacts>` body *invoke* a wide callback? | **No** — 3512, no_rollback. This is why Epic's `signalable.Signal` carries the default set while `subscribable.Subscribe` is `<transacts>`: `Signal` invokes, `Subscribe` only stores. It does not constrain us, because our host invokes subscribers from C++ through `VFunction::Invoke` |
| **F12** | Is the `await{}` macro reachable? | **Yes** — it resolves for our package (VerseVM + epic_internal, `SemanticAnalyzer.cpp:18385-18399`) and fails only on effects, not as an unknown identifier. **Not used by this design**; recorded because a future reader will wonder |

**One more, free:** `vh_method_desc` already carries a `Suspends` flag and the probe already prints
it, so the bridge can already tell a suspending method from an ordinary one with no ABI work.

### 2.1 The two places the first draft was wrong

- **It assumed a `<suspends>` method could be called from wherever a script wanted one.** F5 says an
  awaiting body cannot be narrowed and F1 says `spawn` is how a body is reached — so `spawn` is part
  of the taught surface (D5, amended) and the context it is spawned *from* has to be wide (D19, D20).
- **It assumed `Await` needed a native, and that a parametric native return was the hard part.**
  F8 and F9 say Verse's own `event(t)` does the whole job in ordinary Verse (D21), and the hard part
  is the much smaller question of how a typed payload gets *in* (S-5).

### 2.2 The four spikes that remain

**S-2 — can `FContentScope`s be per-instance?** D1's and D23's feasibility, and the phase's first
item. Questions, in order: can several scopes coexist; is `FContentScopeGuard` a stack or a single
active slot (`GetActiveScope()` suggests one active at a time, so every `vh_instance_call` would push
and pop); what does that cost per `_Process` call on a project with many scripted nodes; and does the
scope's `UObject` outer have to be rooted per instance the way
`UPlaceholderObjectForContentScope::MakeRooted()` does today. **Take the cost with
`tools/build_bench.py`**, the way `phase-2-design.md` §3.1 took its numbers. If per-instance scopes
are infeasible, D1 falls and R-ASYNC-4 needs a different mechanism, which is a design-retiring answer.
**While you are in the scope lifetime code**, look at §13's dangling-instance risk: a continuation
holds `self`, and a Godot Node is freed explicitly rather than refcounted.

**S-3 — can a native `<suspends>` be declared through VNI in our package?** `Sleep`'s feasibility.
Epic's shape is `FVerseResult Sleep(TVerseCall<void> Call, double Seconds)` with
`Call.Suspend(ExecContext)`, a `TStrongVerseCall<void>` captured into the engine callback calling
`Call.Return(ExecContext)`, and `Call.Defer(ExecContext, …)` to unregister on cancellation
(`Simulation.cpp:82-150`). **Simpler than the first draft thought**: under D21 nothing on the signal
path needs a native `<suspends>`, so `Sleep` is the only one, it is void, it is not parametric, and
F5 means it may as well be wide. Declare `VhSleep<native>(Seconds:float)<suspends>:void` in
`Godot.native.verse`, implement it over `FPlatformTime::Seconds()` and the pump, build the host, and
run `tests/verse_probe` — remembering that **a host build passing is not enough to know a `.verse`
file compiles**. **Also ask in the same sitting:** does `Call.Return` work from inside a Godot signal
callback that is already in `AutoRTFM::Open`?

**S-4 — what happens to a suspended task's deferred writes?** **Still the design-retiring one.**
`vh_instance_call` wraps `Invoke` in `AutoRTFM::Transact`, and every Godot write defers to
`AutoRTFM::OnCommit`. When the call yields, that transaction commits with the task still running —
so which transaction do the task's *later* Godot writes belong to? F9 measured that the resumption
happens, but the probe has no Godot behind it and so could not see where a write landed. Host-side,
on a tick-loop harness: a `<suspends>` body that writes a Godot property, yields, writes another, and
a variant that fails after the yield. Assert when each write lands and what the failure undid. Then
the re-entrant case: a method that emits a signal whose handler is an awaiting task, from inside a
transaction. **Also ask:** does a nested `AutoRTFM::Transact` around the resumption confine an abort
to the task's own writes (D15)? If not, D3 needs the pumped fallback it declined, and §14 records why.

**S-5 — how does a typed payload reach the event?** All that is left of the `Await` question. Under
D21 `godot_signal(t)` holds an `event(t)` and `Await()` forwards to it; the missing half is
`Signal`. It is wide (F8), so `godot_signal.Signal`, which is `<transacts>`, cannot call it. Two
shapes, and the spike picks one:

- **the host invokes a specifier-less `Deliver(Val:t):void = Ev.Signal(Val)`** on the signal object,
  the way it already invokes a subscriber with a converted payload. Needs the host to reach the
  `godot_signal` VObject from the signal id, which it may not have today.
- **`godot_signal` subscribes its own `Deliver` through the existing `VhSignalSubscribe` path** at
  construction, so the host needs no new ability at all — it is already invoking a Verse callback
  with a converted payload. Smallest possible change; costs one permanent subscription per signal
  whether or not anything ever awaits it.

Take the second unless it does not work, and record which and why.

---

## 3. R-ASYNC-4 — two-tier scopes

**First, because it is a correctness bug rather than a feature** — and because F9 made the phase's
headline feature cheap, which makes it tempting to do second. Do not. Cancellation, D11's
callback→scope link and G9 all hang off this, and building `Await` on a project-wide scope means
building its cancellation twice.

Today one `verse::FContentScope` made in `GodotVerse::EnterContentScope` (`HostScript.cpp:577`)
serves the whole project, so a raise's `Terminate()` cancels every script's suspended work and
`ResetTerminationState()` replaces the task group wholesale.

**Build:**

- A scope per `vh_instance`, created **lazily on that instance's first `spawn`** (D23), terminated
  and released at `vh_release_instance`.
- A scope per script class per generation, for tasks started with no instance on the stack (D1).
  Cancelled at rebuild, which is when everything is cancelled anyway (D10).
- Every entry point that runs Verse enters the right scope's guard. **`EnterVerse` is the one place
  this goes** — it exists precisely so a seventh entry point cannot be added that forgets the revive,
  and the same argument applies to the guard.
- **`ReviveContentScope` goes away** (D24), and so do `GHaltedUntilTick` and `GTasksLostToError`.
  Read the comment above `ReviveContentScope` before deleting it — it records the defect it was
  written for, and **that record should move rather than vanish**: the right home is §14 here and
  R-DIAG-3's status in `spec.md`.
- **`VH_ERR_HALTED` keeps a narrower meaning**: a call into an instance whose scope is terminated and
  not yet replaced. It is no longer a project-wide state.

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

Two lines of Verse and one spike's worth of host, rather than the native surface the first draft
budgeted for.

**D20, first, because it is what makes the rest reachable** — one line in `GodotApi.native.verse`:

```verse
	Subscribe<public>(Callback(:t):void)<transacts>:cancelable =
		godot_subscription{Subscription := VhSignalSubscribe(Id, Callback)}
```

The callback loses its `<transacts>`, matching `subscribable`. Nothing else changes: F10 measured
that every existing handler still satisfies it, and the host was never effect-checked anyway.

**Then `Await`, on the class `GodotApi.native.verse` already declares** (D21):

```verse
godot_signal<public>(t:type) := class(vh_signal):
	Ev:event(t) = event(t){}

	Await<public>()<suspends>:t = Ev.Await()
```

One method covers a script's own declared signals **and** all 489 mirrored engine-signal accessors
with no per-signal work:

```verse
	MessageTimer.Timeout.Await()
	GetTree().CreateTimer(1.0).Timeout.Await()
	GetTree().ProcessFrame.Await()
	Hit.Await()                    # the script's own
```

**The payload is `t`**, typed, which is what D21 buys over D17's `[]variant`. The existing machinery
already handles it in both directions: a struct payload crosses as one Godot argument per field
outbound, and `InstanceCall`'s rule that N arguments satisfy one struct parameter with N fields
brings it back (G21). `Await` answers the same shape `Subscribe`'s callback receives.

**How an emission reaches the event is S-5** and is the only unknown left on this path.

**Cancellation.** A task cancelled between the connect and the emission must not be resumed by a
stale connection, and must not leave a Godot connection behind. Under D21 the Verse side is the
event's own business — a cancelled task is simply no longer awaiting — but the *Godot* connection
that feeds the event is ours, and D11 anchors it to the scope. Use `CONNECT_ONE_SHOT` where the
connection is genuinely one-shot.

**A foreign signal** — one declared by a GDScript or C# script, or made with `add_user_signal` — has
no mirrored accessor. It crosses today as a `signal_ref` (`VhToSignal`, tag 26), so **give
`signal_ref` an `Await()<suspends>:[]variant`** (D17). There is no `t` to be typed by, so the author
unpacks with the `As*` readers they already use on `Callv`'s result, arity is whatever the emitter
sent, and a wrong expectation fails at the unpack rather than silently. That is the R-INT-1 half.

### 4.1 How a script starts a task

**D19, and it is documentation rather than machinery.** A virtual override is specifier-less and
therefore wide, so it is the context a script already has:

```verse
	_Ready<override>():void =
		spawn{Sequence()}

	Sequence()<suspends>:void =
		MessageTimer.Timeout.Await()
		...
```

Three things go in the manual and in `_make_template`'s comment, and nothing goes in `_validate`:

- **`spawn` is how you start one**, and `<suspends>` on the override is not — it is 3532/3523, at the
  author's own line, saying the override matches no parent (F7).
- **an awaiting body cannot be narrowed** (F5). It carries no specifier, and `spawn` reaches it from
  a virtual or, after D20, from a signal handler.
- **a `<reads>` body cannot `spawn` at all** (F2) — which is the one place 4.5's narrowing and this
  phase's concurrency actually collide.

`_Notification` is hand-written on the native root rather than generated; it is specifier-less like
the rest, so nothing special is owed it here.

**Done when** `race(A.Await(), B.Await())` leaves exactly zero Godot connections behind, and a Verse
task awaiting a GDScript-declared signal resumes.

### 4.2 The compensated connect Phase 4.5 handed over

**Added after Phase 4.5 ran, by decision**, because it is the same machinery and doing it separately
would build the same thing twice.

Phase 4.5 made `godot_signal.Subscribe` genuinely rollback-safe — the host registers an
`AutoRTFM::OnAbort<SameAsClosed>` that disconnects, and `tests/integration` aborts it three ways.
What it could not reach is the *other* way a script connects: `Object.Connect`, which is how R-SIG-6
receives a signal a GDScript or C# node declares, and which the bridge merely forwards to Godot. It
is in [`nonatomic-methods.md`](nonatomic-methods.md) with the other 1072, and measured: a raise after
it leaves the connection behind.

Phase 5 is where that closes, because §4's `signal_ref.Await()` already needs the bridge to *own* a
connection to a foreign signal rather than forward one. The shape to reach for is a `Subscribe` on
`signal_ref` beside that `Await`, compensated the way `godot_signal.Subscribe` is — and with D20's
widening applied to it from the start, so the two spellings agree. `Object.Connect` then stays what
it is, an unforgiving direct call, and the rollback-safe spelling is the one a script reaches first.

**Not a spike.** Nothing about it is uncertain — the mechanism is built and tested, and this is
where to point it.

---

## 5. `Sleep`

    VhSleep<native>(Seconds:float)<suspends>:void

Host-side, on `FPlatformTime::Seconds()`, resuming from the pump (D4, D6). Deliberately **not**
`SceneTreeTimer`, because it must work with no tree: `host_smoke`, the probe, a `@tool` script. It is
the **only** native `<suspends>` this phase needs, because D21 took the signal path off that list,
and it is S-3's whole subject.

**Its divergences are documented, not hidden:** it ignores `Engine.time_scale`, it counts wall-clock
under `--fixed-fps`, and it keeps counting in a paused game (which is consistent with D8). The manual
sentence is *"`Sleep` is real time. For game timing, await a `Timer` or
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
- **`queue_free` is not the trigger; the free is** (D22). Godot flushes the delete queue on the next
  idle (`_flush_delete_queue`), so a queued node's task runs until then and may emit or write. That
  window is a documented row in §7, not a bug.
- **A scene change cancels what it unloads**, which falls out of the first two rules — the nodes are
  freed, on that same deferred schedule. Verify rather than assume.
- **A rebuild cancels everything, reporting a count** (D10).
- **A raise cancels the raising instance's tasks and nothing else** (D2, D24) — which is the
  narrowing this whole phase exists for, and the one `host_smoke` case §3 exits on.

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
| a task on a `queue_free`d node | keeps running until the delete queue flushes | D22 |

The one rule we own is the fifth row, and it exists because nothing in Godot fires it. Within that
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
  load-bearing rather than advisory and the header should say so. Under D23 the *create* side is
  lazy, so `vh_instantiate` gains a promise rather than an allocation.
- An entry point to cancel an instance's tasks without releasing it, if §6 finds a case that needs
  it (scene change may, given D22's deferred free).
- **Note what is not needed.** A delta parameter on `vh_tick` was considered for `Sleep` and is not
  needed under D6 — `Sleep` is real time. And `vh_method_desc` already carries `Suspends`, so
  nothing is owed there.
- Overrun/queue reporting for §8, which can ride the existing diagnostic callback rather than a new
  entry point.

Bump `VH_ABI_VERSION_MAJOR` to 6, rebuild **both** DLLs, and remember the mismatch surfaces at
`vh_init` rather than at compile time.

---

## 10. What Phase 5 does not do

- **OQ-6 / R-ASYNC-7.** Phase 5 must not foreclose it. R-ASYNC-8's refusal stands. **The scoping
  document is not a deliverable of this phase** — the roadmap's "spawn the threading scoping
  document" means the OQ-6 row gets updated with what this phase settled and what it left.
- **Bounding a runaway task.** `spawn` in a `_Process` makes sixty tasks a second on one instance,
  and this phase creates that hazard by making `spawn` the taught entry (D5, D19). It is OQ-13's
  shape pointed at tasks rather than at raises. **Record it on the OQ-13 row and do not solve it**:
  whatever bounds a script that raises every frame should bound this too, and both want per-instance
  scopes first, which is what this phase builds.
- **OQ-13** — bounding a script that raises every frame. Phase 6, with the rest of R-DIAG-3. D2 and
  D24 change its shape and that should be noted in the OQ row.
- **A `_validate` rewrite for F7's 3532/3523.** Considered and declined (D19). The compiler already
  refuses it at the author's own line, and the phase is not building suspending virtuals.
- **OQ-17** — C# has still never been run against this bridge, and `Await` on a `signal_ref` makes
  the claim bigger without testing it. Record it; do not pretend otherwise.

---

## 11. Tests

Three layers, all existing. `run_tests.py` stays the one command (R-QUAL-3).

**`tests/verse_probe`, already used and kept.** Seven fixtures land with this design and they are the
record behind §2 — re-runnable, not recalled. `async_reject.verse` is the file of refusals and
compiles nothing on purpose. Anything of the form "does `race` over an awaiting body do what we
think" goes here first, because it is the fastest place in the repo to ask.

**An eighth landed during the work: `sleep_probe.verse`**, which answered S-3 and the question S-3
did not know to ask. It spawns a body that `Sleep(0.0)`s and asserts it resumed at the next tick;
it runs a `race` whose loser sleeps for an hour and shows that the loser's **`defer` ran on
cancellation**, which is the whole basis of `Await`'s connection lifetime; and it subscribes both a
`<transacts>` handler and a wide one to the same widened `Subscribe`, which is D20 measured rather
than argued.

**`host_smoke`, a new tick-loop layer.** No Godot at all: instantiate, call a method that spawns,
drive `vh_tick`, assert resumption. F9 says the mechanism works; this is where it becomes a test that
stays passing, and where **the two-tier scope behaviour, `Sleep` and S-4's transaction findings get
pinned** — there is no scene tree here to confuse the question. §3's exit case lives here: a raise in
one script's task leaving another's suspended work running.

**`tests/integration/test_main.gd`** — GDScript can `await`, so a case can emit a signal, await a
frame, and assert the Verse task resumed. R-SIG-5, the foreign-signal `Await`, R-ASYNC-5's
free-cancels-tasks rule, D22's `queue_free` window and D10's rebuild cancellation live here. Keep the
one-line-per-case shape and `quit(1)` on failure. **Note the transaction section already runs a step
per frame** because a raise halts the project; once D2 and D24 land, that constraint is gone and the
section can be simplified — which is itself a test that D2 worked.

**Not a new project.** `coverage_diagnostic` is separate because its script deliberately does not
compile; a task that hangs is a different failure and the tick-loop layer catches it before the
integration project ever sees it.

---

## 12. Exit

**Dodge the Creeps wall 3 falls.** `hud.verse`'s game-over sequence goes back to the readable lines
the GDScript has, reached with `spawn` from a handler that D20 widened:

```verse
	# main.verse
	GameOver<public>():void =
		spawn{Overlay.ShowGameOver()}

	# hud.verse
	ShowGameOver<public>()<suspends>:void =
		ShowMessage("Game Over")
		MessageTimer.Timeout.Await()
		...
		GetTree().CreateTimer(1.0).Timeout.Await()
		...
```

The `hud_phase` enum goes, the **second Timer node the port added** goes (`TitleTimer`, which existed
only because a `SceneTreeTimer`'s timeout had no spelling), and the two timeout handlers that existed
only to read the phase go. Per D7 the port uses the timer awaits, not `Sleep`, so `--fixed-fps 60`
stays and `headless_check.gd`'s checks stay deterministic. **The diff is the measurement**, and
`dodge-the-creeps.md`'s wall table gets its seventh row struck.

**Wall 8 narrows again, and the port should say so.** D20 means a signal handler is no longer fixed
at `<transacts>`, so the `<transacts>` cascade `hud.verse`'s comment describes is gone from that
path — a handler carries the default set like any other unspecified function. What survives is a
helper that *writes* and is called from a genuinely narrowed body. `dodge-the-creeps.md` §8 and the
wall table both need editing, and `hud.verse`'s own comment at its lines 22-27 is now wrong.

**`Object.Connect`'s rollback gap closes** (§4.2) — a `Subscribe` on `signal_ref`, compensated the
way `godot_signal.Subscribe` is, with a `tests/integration` case that aborts it and checks no
connection was left.

**And the by-hand checks are run** — one windowed session covering all three phases' entries:
Phase 3's windowed yardstick run and editor session, Phase 4's Node panel, `_make_function`,
`_HasPoint` and `_CanDropData` flows, and whatever Phase 5 adds. **Done**, and
[`by-hand-findings.md`](by-hand-findings.md) is the record — the list itself is deleted, and two of
the entries turned out to be reachable headless after all. **Ask before launching the editor** —
`tools/run_tests.py`'s headless Godot is fine unprompted; a window is not.

---

## 13. Risks

- **S-4 is the design-retiring one.** If re-entrant resumption inside an open transaction is unsafe,
  D3 needs the pumped fallback that was offered and declined, and every ordering claim in §7 changes.
  Find out first, not in §4. F9 shows the resumption *happens*; it says nothing about where a Godot
  write inside it lands, because the probe has no Godot behind it.
- **S-2's cost.** A guard push/pop per `vh_instance_call` is on the hot path — every `_Process` on
  every scripted node, every frame. D23's laziness bounds how many scopes exist but not what the
  guard costs on the ones that do. Measure it with `tools/build_bench.py`.
- **A GC object per awaiting instance.** If the scope's outer must be rooted the way
  `UPlaceholderObjectForContentScope::MakeRooted()` roots the one today, a scene with a thousand
  *awaiting* nodes has a thousand rooted UObjects. D23 makes that "awaiting" rather than "scripted",
  which is the difference between a yardstick that notices and one that does not.
  `HostEventLoop.cpp`'s `TickGC` already watches `GetObjectArrayEstimatedAvailable()`.
- **D2 and D24 change something that was recently fixed deliberately.** R-DIAG-3's project-wide halt
  was written in Phase 3 and `phase-3-design.md` §11 explains it. Changing it means editing that
  requirement, not quietly contradicting it — and moving `ReviveContentScope`'s comment somewhere it
  survives.
- **D20 widens what a handler may do.** A specifier-less handler may perform `no_rollback` effects
  inside the AutoRTFM transaction a Godot callback runs in. Compile-time evidence (F10, F11) says
  nothing about that, and 4.5's "what a failure undoes" measurements were taken against `<transacts>`
  handlers. **Re-run that section of `tests/integration` after the widening** rather than assuming
  it still holds.
- **A suspended task keeps its instance reachable.** A continuation holds `self`, and `vh_instance`
  is a UObject. A Godot Node is freed explicitly rather than refcounted, so the script instance can
  be destroyed underneath a task that still references it — either the task is cancelled first (D9,
  D22, and it must be) or there is a dangling reference. S-2 should look at this while it is already
  in the scope lifetime code, and D22's deferred-free window is exactly where it would show.
- **`Await` enlarges R-INT's untested claim.** OQ-17 stands, and D17 makes the untested surface
  bigger by reaching GDScript-declared signals that no fixture exercises.

---

## 14. What building this corrected

**Written after the work, and this is the section to read first.** §2 was filled in the same way
before it, which is why §3 onward needed so little correcting: the four remaining spikes all
answered positively, and none of them retired a decision. What follows is where the *document* was
wrong rather than where the plan was.

### 14.1 The four spikes, answered

**S-2 — per-instance scopes are feasible, and cheaper than §13 feared.** `FContentScopeGuard` is a
**stack**, not a single slot: its constructor saves the previous guard as `Parent` and its
destructor restores it (`VVMContentScope.cpp:39-77`), so scopes nest and `EnterVM_Internal` reads
the innermost one to pick the task group a `spawn` joins. Nothing had to be invented.

Three things §13 worried about turned out not to be true:

- **No rooted UObject per instance.** A scope holds its `InstantiationOuter` as a
  `TWeakObjectPtr`, so every instance scope shares the one `UPlaceholderObjectForContentScope` the
  process already rooted. `MakeRooted()` is still called exactly once.
- **No `FGCObject` registration per scope.** `FContentScopeGCReferencer` batches every live scope
  behind one registration (`VerseContentScope.cpp:29-60`) — Epic had already solved that, for the
  same reason we would have had to.
- **The guard is not on the hot path in any measurable sense.** Measured with
  `tools/build_bench.py`, which grew a section for it: **1.09 µs** per `vh_instance_call` with the
  guard against **1.03 µs** without, which is inside the run-to-run spread. What the scope costs is
  **memory**: 12.3 KB retained per scripted instance against 9.7 KB with no scope at all, so
  **~2.6 KB per scripted node** — a scope, its control block, its debug name, and the `VTaskGroup`
  the first VM entry makes. A thousand scripted nodes is about 2.6 MB.

**S-3 — a native `<suspends>` declares through VNI in our package, with Epic's exact shape.**
`Sleep<public><native>(Seconds:float)<suspends>:void` generates
`FVerseResult Sleep(TVerseCall<void> Call, double Seconds)`, and `Call.Return` works from the pump.
Better than the design hoped: `TVerseCall::Return` **re-enters the suspended task's own content
scope and declines if that scope was terminated** (`VVMCoroutine.h:431-490`), so a sleeping task on
a freed node is dropped with no work from us. R-ASYNC-5 got that half for free.

**S-4 — re-entrant resumption inside an open transaction is safe, and D15 is what makes it so.**
The design called this the design-retiring one. It retired nothing. A task awaiting a Godot signal
resumes inside `DeliverToAwaiter`, which opens its own `AutoRTFM::Transact` nested in whatever
transaction the emitting call is already in — and `tests/integration` runs the whole thing: a signal
handler that spawns, a `race` whose loser is cancelled mid-emission, and a raise in one instance
while another instance's task is suspended. What made it undramatic is that `InstanceCall` already
had exactly this shape, so the new path is a copy of a tested one rather than a new idea.

**S-5 — neither of the two shapes, because both were unreachable.** The design offered two ways to
get a typed payload into the event and said "take the second unless it does not work". Both assumed
the host would invoke a *Verse* callback on the signal object, and `MakeCallableFor` accepts only a
method bound to a **script instance** — `DescribeBoundFunction` requires a `vh_object` and walks the
script class's method list. A `godot_signal` is a `vh_signal`, so neither shape could have been
built as written.

What the implementation does instead is simpler than either: **`verse::event` is a UObject with a
public C++ `Signal(FVerseValue const&)`**, so the host reads the `Ev` field off the signal object
and signals the event directly. Epic's own code then does the FIFO resumption, the per-task content
scope, and the dropping of a cancelled awaiter. No Verse-side `Deliver` method, no decorated-name
lookup, and nothing new on the Verse side of the signal path at all.

### 14.2 Where the document was wrong

- **D23 is retired: the scope is made at `vh_instantiate`, not at the first `spawn`.** "Lazily on
  that instance's first `spawn`" is not implementable — there is no hook that fires when a task is
  started, and the guard has to be *already active* at that moment for the task to join the right
  group. Making it at instantiation also reduces the rule to one sentence ("every entry that runs
  this object's code runs in its scope") and covers a constructor that spawns. The cost is §14.1's
  2.6 KB per scripted node rather than per *awaiting* node; measured, and judged not worth a
  cleverer trigger.
- **D17 is amended: `signal_ref.Await()` answers a `godot_array`, not `[]variant`.** `variant` has
  no description on this bridge — `DescribeType` classifies it as a mirrored *object* class, so
  `[]variant` would cross as an array of handles. Godot's own Array of the arguments is a type the
  wire already carries in both directions, and the author unpacks it with the `Get*` accessors a
  `godot_array` already has rather than with the `As*` readers. Same argument, a type that exists.
- **Nothing could produce a `signal_ref`, which no one had noticed.** `AsSignal(Value:variant)` was
  the only way in and nothing hands a script a `variant`, so `signal_ref.Await()` and `.Subscribe()`
  would have been unreachable however good they were. **`MakeSignal(Owner, Name)`** closes it —
  Godot's own `Signal(object, "name")`, the direct analogue of `MakeCallable`, over one new ABI
  callback.
- **§9 under-counted the ABI.** It expected lifetime *meaning* changes and no new entry points. v6
  adds two callbacks to `vh_godot_api` (`SignalTarget`, `MakeSignalRef`) and changes `vh_tick`'s
  signature to take a `vh_tick_stats*`. The last is what R-ASYNC-6 needed: a budget whose effect
  nothing can see is a number nobody can set.
- **`api_connect_signal`'s "no flags in 4a" note is now spent.** One `Await` resumes once, so
  `CONNECT_ONE_SHOT` is exactly the lifetime and saves the disconnect.

### 14.3 What is load-bearing and not obvious

- **`defer` runs when a task is cancelled, not only when it returns** — measured in
  `tests/verse_probe/sleep_probe.verse`, with a `race` whose loser sleeps for an hour.
  `defer { VhSignalAwaitEnd(Token) }` is what disconnects a wait the emission never reached, and
  `tests/integration` asserts the connection count is back to zero on both sides of a race.
- **It does not cover the node dying, and that is what G9 is for.** Terminating a task group does
  *not* unwind the tasks in it, so freeing a node while one of its tasks awaited left a live Godot
  connection, a held object and a callback row behind — found by a test written to assert the
  opposite. The fix is Epic's own rule and Epic's own hook: the wait registers on
  `FContentScope::OnContentScopeCleanup`, which is where `event::SubscribeInternal` registers too.
  So `phase-4-gaps.md` G9 closed here not as tidiness but as the other half of cancellation.
- **An engine-signal accessor mints a *fresh* `godot_signal` on every call.** `Timer.Timeout()` is a
  method, and several of its results share one binding id — which is why `VhSignalAwait` takes the
  *object* rather than the id, and why the host holds it strongly for the life of the wait.
- **The event field is found by walking the shape, not by naming it.** A data member's key is
  `(<declaring class' scope path>:)<name>`, and there is more than one plausible spelling of that
  path for a member of a *parametric* class. The walk asks the only question that cannot be got
  wrong: which field holds an event.
- **`vh_tick`'s budget governs the queue and nothing else.** A task awaiting a Godot signal resumes
  inside the emission and is unbudgeted, exactly as a GDScript coroutine's resume is. Sleepers are
  woken before the queue and are not budgeted either: a sleep that is due is due, and holding one
  over is a frame of drift an author cannot see.

### 14.4 What Phase 5 did not do

- **The pump does not spread an editor burst.** §7.1 asked whether a `@tool` script's tasks, stalled
  for the length of a background analysis, should be drained or spread over the frames that follow.
  They are drained, because `Sleep` is the only thing that queues and a burst of real-time deadlines
  that have all passed is one that has genuinely passed. If that is ever wrong it will be wrong
  visibly, in the editor, with a number on it.
- **D16 is inherited rather than implemented.** "Tasks run in the editor only for `@tool` scripts"
  is true because `_Process` is, and because the only way to start a task is from a method Godot
  calls — a non-tool script's methods are not called in the editor, so nothing spawns. There is no
  new rule and nothing enforces one.
- **OQ-13's runaway task is created and not bounded**, as §10 said to leave it. `spawn` in a
  `_Process` makes sixty tasks a second on one instance and nothing says so.
- **C# has still never been run against this bridge** (OQ-17), and `signal_ref.Await()` makes the
  untested claim larger rather than smaller.
