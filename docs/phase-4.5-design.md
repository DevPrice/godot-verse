# Phase 4.5 — Effects and transactions: what a rollback actually undoes

**Status:** Plan · 2026-09-13 · **not started.** Written before the work rather than after it, which
is the opposite of this repo's habit and is deliberate: Phase 5 was planned in the same sitting and
needs this phase's answer to exist before it designs a second effect axis on top of the first.

**Read `roadmap.md` "Phase 4.5" first** — it is the two-paragraph version, and the table in it is
the whole subject. This document is what that table turns into.

**Companion to:** `docs/spec.md` §14 **OQ-15**, which this phase closes, and R-AUD-1 and R-AUD-3,
which it makes testable.

**Prerequisite for Phase 5**, by decision. `<suspends>` is a second effect axis and it lands on
top of this one; designing both at once was the alternative and was rejected.

---

## 0. How to read this

§2 is the spikes, and **nothing in §3 onward should be trusted until they have run** — Phase 4's
design was written after its spikes for exactly this reason, and one of those spikes retired the
design the phase would otherwise have been built around. The stages in §3–§6 are ordered by
dependency; each says what it is done when.

§11 is deliberately empty. It is where whoever builds this writes what the building corrected, the
way `phase-2-design.md` §11 and `phase-3-design.md` §11 did. That section is the most valuable part
of both of those documents and it only exists because someone wrote it afterwards.

---

## 1. The subject

The mirror labels every one of its **14,933** methods `<transacts>`. The label is a promise, and the
C++ keeps it three different ways — or does not keep it at all:

| | count | how the promise is kept |
| --- | --- | --- |
| mutating, returns nothing | 6813 | **deferred** to `AutoRTFM::OnCommit`; an aborted transaction never performs it. Honest |
| const, returns a value | 6728 | nothing to undo. Honest, but mislabelled |
| const, returns nothing | 38 | honest, trivially |
| **mutating, returns a value** | **1354** | **not kept.** The result is needed now, so the call cannot be deferred, and nothing compensates it |

Three things follow, and they are the phase:

1. The **6766** const methods are labelled with an effect they do not have. That is not merely
   untidy — `<transacts>` is *wider* than `<reads>`, and in Verse an explicit specifier **narrows**,
   so a `<reads>` author function cannot call any of them. Every read of Godot state forces
   `<transacts>` onto its caller, and from there onto that caller's callers, a file at a time.
2. The **1354** are a lie the author cannot see. `Node.move_and_slide`-shaped calls — mutate and
   answer — happen, and then a failure unwinds past them and they stay happened.
3. **Nobody has written down what a failed expression undoes.** R-AUD-1 asks for a rule; there is
   none. `dodge-the-creeps.md` wall 8 is what an author hits instead, and the phase is what replaces
   guessing with a paragraph.

### 1.1 What is already known, and should not be re-derived

- **An explicit effect specifier narrows, and narrowing is contagious downward.** A function with no
  specifier carries the *default* set, which is wider than `<transacts>` — it contains `no_rollback`.
  So a `<transacts>` function may not call a specifier-less one, and anything that forces
  `<transacts>` on a method (a `Subscribe` handler; a failure context) forces it on everything that
  method calls. The compiler reports it at the **call** site, not at the declaration that needs
  changing.
- **Wall 8's first write-up blamed the host's AutoRTFM transaction, and that was wrong.** Phase 4's
  probes corrected it: the cause is the Verse effect lattice, not the C++ transaction. The two are
  related but not the same thing, and the document must not re-conflate them.
- **The generator does not read `is_const`.** `extension_api.json` carries it 17,344 times;
  `gen_verse_api.py` reads `is_static` and `is_vararg` and not that. The classification in the table
  above is therefore *available* and merely unused.
- **Property accessors are `<transacts>` too**, which the roadmap's "a property read is already
  `reads`" obscures. A generated getter is
  `GlobalPositionGetter<epic_internal>(Accessor:accessor)<transacts>:vector3 = VhToVector3(VhCallValue(…))`.
  So the const surface is not 6766 methods, it is 6766 methods **plus 3232 property getters**, and
  the `var`-accessor protocol is a second question (S-1 below) that the method surface does not have.

---

## 2. Spikes — run these before writing §3 onward

Each is a question whose answer changes what gets built, not a task. The vehicle for all four is
`tests/verse_probe` (`tools/build_verse_probe.py`), which is what made six of Phase 4's decisions in
one sitting: it compiles whatever `.verse` files it is handed, prints every diagnostic, and calls a
class's zero-argument methods.

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        tests/verse_probe/<fixture>.verse --class <name>

Run it **from `bin/`** — `tbbmalloc.dll` lives there and without it `LoadLibrary` answers a bare 126
and names nothing.

### S-1 — does a `<reads>` function satisfy the `<getter(…)>` accessor protocol?

**Why it matters.** 3232 property getters are the larger half of the const surface, and they are not
ordinary functions — they are named by a `<getter(…)>` attribute on a `var` and invoked by the
language. If the protocol requires `<transacts>`, the property surface cannot be narrowed and the
phase's first stage covers methods only. That halves the benefit and changes what §3 claims.

**How.** A fixture declaring a `var` with a `<reads>` getter and a `<transacts>` setter, read from a
`<reads>` caller. Also try the multi-level accessor overloads the flat math types use
(`Getter(Accessor, Field:string)`), since those are the ones with the awkward shape already.

**What each answer changes.** Works → §3 covers 6766 methods and 3232 getters. Refused → §3 covers
methods only, the property surface is recorded as knowingly `<transacts>`, and the phase says why.

### S-2 — is `<reads>` actually callable from `<transacts>` code, and what native does it need?

**Why it matters.** Narrowing only helps if it is one-directional in the useful direction: a
`<transacts>` caller must be able to call a `<reads>` callee, or changing 6766 declarations breaks
every existing script. And a `<reads>` body may not call `VhCallValue`, which is `<transacts>` — so
the change costs at least one new native primitive, and the host has 22 today.

**How.** A fixture with `F()<reads>:int` called from `G()<transacts>:void`, and a `<reads>` native
declared alongside `VhCallValue`. The native half needs a host build; the Verse half does not, and
the Verse half is the one that can kill the idea.

**What each answer changes.** If the widening direction is legal, §3 is mechanical. If a `<reads>`
native cannot be declared through VNI at all, the whole stage is blocked and the phase becomes §4
and §5 only.

### S-3 — does `Verse::Stm::OnRollback` work from this bridge's call path?

**Why it matters.** It is the mechanism §4 proposes for compensating the 1354, it is what UE uses
throughout, and Phase 4's `Subscribe` is where it enters this repo. But every Godot callback here
runs inside `AutoRTFM::Open` nested in a transaction, and a rollback handler registered from open
code is not obviously the same thing as one registered from closed code.

**How.** Needs the host rather than the probe: register an `OnRollback` from inside
`vh_instance_call`'s `AutoRTFM::Transact`/`Open` nest, then make the call fail, and assert the
handler ran. `tests/host_smoke` is the place — it already has a method that raises deliberately
(`exports.TouchTarget`, kept for R-DIAG-2) and eight checks around the halted state.

**What each answer changes.** Works → §4 compensates where an action is undoable. Does not → every
one of the 1354 is documented non-atomicity, which is a much weaker answer and a much smaller phase.

### S-4 — what does a failure context undo *today*?

**Why it matters.** The rule §6 writes has to describe the behaviour that exists before it describes
the behaviour that should. Nobody has measured this. The `OnCommit` deferral says mutating-void
calls are undone; nothing has checked whether the *order* survives, whether a read after a deferred
write in the same transaction sees the old value (it must — the write has not happened), or what an
author actually observes.

**How.** A fixture that sets three properties, reads one back, then fails; and a second that does the
same and succeeds. Run under the probe with a real Godot handle is not possible — this one wants
`tests/integration`, where a GDScript case can assert what the node's properties ended up as.

**What each answer changes.** It is §6's raw material. It may also find that a read-after-deferred-
write answers stale, which is a documented sharp edge in its own right and is the kind of thing wall
8's successor will be.

---

## 3. Stage 1 — the const surface becomes `<reads>`

**Blocked on S-1 and S-2.**

`gen_verse_api.py` reads `is_const` and emits `<reads>` for a const method, `<transacts>` otherwise.
A `<reads>` body calls a new `VhCallValueReads<native>(Handle:int, Method:string, Args:[]variant)<reads>:variant`
and, if S-1 permits, a `<reads>` getter.

**Why this is worth a stage of its own.** It is measured, mechanical, and it shrinks the dishonest
set from 14,933 to exactly 1354 — which is what makes §4 a bounded piece of work rather than an
open-ended audit. It also makes the method surface agree with what a property read is supposed to
be, and it gives a library author somewhere to stand: a helper that only *reads* Godot can be
`<reads>` and stops infecting its callers.

**Watch for.** The host side of a `<reads>` native must genuinely not mutate — it may not defer to
`OnCommit`, because there is nothing to defer, and it may not take a lock that a `<transacts>` call
holds. And `VhGetValue` is `<decides><transacts>` today; whether its `<decides>`-ness survives
narrowing is part of S-2.

**Done when** `run_tests.py` is green, the generated mirror carries `<reads>` on 6766 methods (and
3232 getters if S-1 said yes), a script with a `<reads>` helper compiles, and the probe shows a
`<transacts>` caller can still reach all of it.

---

## 4. Stage 2 — the 1354

**Blocked on S-3.**

Classify all 1354 by whether the action is undoable, and act per class:

- **Undoable** — compensate with `Verse::Stm::OnRollback`: record the prior state before performing
  the action, restore it if the transaction aborts. This is the mechanism Phase 4's `Subscribe`
  introduced and UE uses throughout.
- **Not undoable** — documented non-atomicity. `spawn`-shaped calls, allocations, anything that
  hands out an id. This is what GDScript offers anyway, and saying so plainly is a better answer
  than a compensation that half works.

**The list is the deliverable, not the code.** Whatever the split turns out to be, it goes in
`spec.md` as the enumerated set of knowingly-untrue labels, because R-AUD-3 needs something to point
at and because "we audited it" with no list is indistinguishable from not having audited it.

**Signal emission joins the 1354 by decision** — Phase 4 made `godot_signal.Signal` immediate rather
than deferred, and the reason is in `phase-4-design.md`. It is in this set and should be listed in
it.

**Done when** every one of the 1354 is in exactly one of the two buckets with a reason, the
compensated ones have a test that aborts and checks the state came back, and the count of
knowingly-untrue labels is a number in the spec.

---

## 5. Stage 3 — say it where the fix is

**`no_rollback` is the first sharp edge a library file hits** (`dodge-the-creeps.md` wall 8, and it
fired again *during* Phase 4's re-port, four declarations deep in two other files). The message names
an effect the author never wrote and points at a **caller** rather than at the declaration that needs
changing. Two places can do better:

- **The `.verse` template** (R-TOOL-12's neighbour) — a new script's functions could carry
  `<transacts>` by default, so the trap never fires for the common case.
- **The R-SCN-2 diagnostic machinery** — `verse_script_language.cpp` already turns a compiler
  diagnostic into a better sentence when it recognises one (that is what `verse_api_skipped.h` is
  for). An "effect not allowed" diagnostic naming `no_rollback` is recognisable, and the sentence to
  add names the *callee's declaration* and says to write `<transacts>` on it.

**Done when** the trap, reproduced from wall 8's own shape, produces a message that names the file
and line to edit.

---

## 6. Stage 4 — the rule, in a paragraph

R-AUD-1 asks what a Verse author may assume a failed expression undid. Written against what S-4
measured and what §4 decided, it has to cover:

- what a failed `if (X := …)` undoes, and what it does not;
- that a deferred write is not visible to a read in the same transaction;
- which Godot calls are in the knowingly-untrue set, by shape rather than by enumerating 1354 names —
  "a method that both mutates and answers" is the shape, and the list is the appendix;
- what a `<reads>` function guarantees after stage 1.

**Done when** it is one paragraph an author can act on, it is in `spec.md` next to R-AUD-1, and
**OQ-15 is closed with a written answer** rather than with an implementation that assumes one.

---

## 7. Tests

Nothing here needs a new layer. `run_tests.py` is still the one command (R-QUAL-3).

- **units** — the generator gains cases for `is_const` → `<reads>`, in `tests/verse_api_gen/`.
- **abi** — `host_smoke` gains S-3's rollback checks and the compensation cases from §4.
- **integration** — S-4's fixture stays as a behavioural case: mutate, fail, assert what the node's
  properties are.

---

## 8. Exit

Every effect label in the mirror is either true or on a written list of knowingly-untrue ones, with
the list in `spec.md`; a failed expression's guarantees are one paragraph an author can act on; the
`no_rollback` trap says where the fix is; and **OQ-15 closes**.

---

## 9. Risks

- **S-1 or S-2 says no**, and stage 1 does not happen. The phase is then §4–§6 only, the dishonest
  set stays at 14,933 rather than 1354, and the `<reads>` decision becomes a recorded blocker with a
  named cause. This is survivable and should be written up, not worked around.
- **Stage 1 is a whole-mirror regeneration**, which is a 1.27 s generation and ~1.3 MB retained per
  the Phase 3 correction, and it changes 10,000 declarations at once. Every existing script and
  fixture compiles against it or the stage is not done.
- **The 1354 audit is the part that can sprawl.** It is 1354 rows; the bucket decision is per *shape*
  rather than per row wherever possible, and the doc should say what the shapes are before anyone
  starts reading method names.

---

## 10. What this phase does not do

- It does not touch `<suspends>`. That is Phase 5 and it is deliberately after this.
- It does not revisit `<decides>`, which is already honest.
- It does not attempt R-ASYNC-anything.

---

## 11. What building this corrected

*Empty. Fill it in after, the way `phase-2-design.md` §11 and `phase-3-design.md` §11 were filled
in — those are the most-read sections of both documents and they only exist because someone wrote
them afterwards. Record what §1's table got wrong, what the spikes retired, and what the
implementation could not do.*
