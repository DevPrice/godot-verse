# Phase 4.5 — Effects and transactions: what a rollback actually undoes

**Status:** **Built, 2026-09-13.** §1–§10 were written *before* the work, which is the opposite of
this repo's habit and was deliberate: Phase 5 was planned in the same sitting and needed this
phase's answer to exist before it designed a second effect axis on top of the first.

**§11 is the part to read.** It was written after, and it is where the plan turned out to be wrong:
two of the four spikes came back the opposite way from what §3 and §4 expected, §1's table has two
bad rows, and the audited set is **1127** rather than the 1354 estimated below. §0's warning that
"nothing in §3 onward should be trusted until the spikes have run" now reads the other way round —
nothing in §1 through §10 should be trusted over §11.

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

**Status: built, 2026-09-13.** Written after the work, which is the point of the section. The
spikes ran first, as §0 asked, and two of the four came back the opposite way from what §3 and §4
were written to expect.

### 11.1 The spikes

| | question | answer |
| --- | --- | --- |
| **S-1** | does a `<reads>` function satisfy the `<getter(…)>` protocol? | **No**, and it did not matter |
| **S-2** | is `<reads>` callable from `<transacts>`, and can a native be `<reads>`? | **Yes**, both halves |
| **S-3** | does `Verse::Stm::OnRollback` work from this bridge's call path? | **No.** It was a no-op, and had been since Phase 4 wrote it |
| **S-4** | what does a failure context undo today? | More than §4 assumed, and one thing less |

**S-1 is refused, in the compiler's own words.** The accessor protocol matches an *exact signature*
rather than accepting a narrower effect:

    `Score`'s accessors contain the following errors:
    Incorrect definitions:
        ScoreGetter(:accessor)<reads>:int
            - needs the <transacts> effect
            - the signature of this accessor should be: ScoreGetter(:accessor)<transacts>:int

Under §2 that answer was supposed to halve the phase's benefit. **It cost nothing**, because of a
thing nobody had asked: a `<getter(…)>` var's *read* is not effect-checked against its getter. A
`<reads>` function may read `Position` and the call reaches `VhCallValue` — a `<transacts>` native —
at runtime, which `tests/verse_probe/reads_property.verse` shows compiling *and* running. The
*write* is checked and correctly refused ("This assignment has the 'transacts' effect"). So the 3232
property getters stay `<transacts>` and are usable from `<reads>` code anyway, and stage 1 needed to
touch only the method surface. The fixture is kept because this is a rule that could change.

**S-3 found a bug rather than answering a question.** `Verse::Stm::OnRollback` is the *Solaris
interpreter's* STM, and `VerseStm.h` says of it: *"Noop if StmActive() returns false"*, `StmActive`
being "true if in a failure context" — a BPVM-era notion VerseVM never sets from here. Phase 4's
`Subscribe` compensation therefore never ran, and nothing had noticed: the integration case that
would have caught it did not exist until this phase wrote it. Measured, the connection survived all
three kinds of failure. The working mechanism is
`AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>`, and `SameAsClosed` is the load-bearing
half — every Godot callback reaches C++ through `AutoRTFM::Open`, and a plain `OnAbort` from open
code is documented to be *ignored*. Fixed, and `tests/integration` now aborts a Subscribe three ways
and checks the connection came back.

### 11.2 What §1's table got wrong

The counts were right — 6813 / 6728 / 38 / 1354, exactly. Two of the four rows were not.

**"const, returns nothing — 38 — honest, trivially" is wrong.** Godot's `const` means "does not
mutate the C++ object", not "has no effect". The 38 are `OS.set_environment`, `OS.unset_environment`,
`OS.delay_msec`, `OS.delay_usec`, `DisplayServer.beep`, `Texture2D.draw`, `CanvasItem.draw_string`
and 31 more of that shape — every one does something a later read can see. They stay `<transacts>`,
and the test the generator applies is const **and** answering a value.

**"1354" is the wrong number for the audited set**, twice over. 102 of them are statics, which the
mirror emits as free functions and which are a separate question; and of the remainder only **1127**
survive into the mirror under their own names. 1127 is the number in `spec.md`, and it is generated
rather than asserted — `docs/nonatomic-methods.md` is written by the same pass that writes the
mirror, the way `verse_api_skipped.h` is, so it cannot drift.

The property-accessor point in §1.1 was right and led nowhere: the getters are `<transacts>`, they
stay `<transacts>`, and it does not matter. See 11.1.

### 11.3 What S-4 measured

The rule in `spec.md` next to R-AUD-1 is written from these, and `tests/integration` keeps every one
as a case. Four were expected; two were not.

- A write inside a **failure context that declines** is dropped. Expected.
- A write in a **`<decides>` method that declines at the top level** is dropped too. **Not
  expected** — `InstanceCall` reads that as `FOpResult::Fail` and its own `AutoRTFM::Transact`
  commits normally, so the host's transaction is not what drops it. VerseVM wraps the invocation of
  a `<decides>` function in a failure context of its own, and that is the transaction the deferral
  was registered against.
- A write followed by a **raise** is dropped. Expected.
- A **read after a deferred write in the same call sees the old value.** Expected, and it is the
  sharp edge the rule had to name.
- **Deferred writes commit in order**, so the last write to a property wins.
- **A raise stops every script until the next `vh_tick`** (R-ASYNC-4), which is *why*
  `test_main.gd`'s transaction section runs a step per frame. Three of its cases raise deliberately,
  and a second Verse call in the same frame answers `VH_ERR_HALTED` and never runs. This is recorded
  behaviour, but it had never bitten a test before, and it is the first thing anyone writing one of
  these will trip over.

### 11.4 What stage 1 cost that §3 did not predict

§3 said "`gen_verse_api.py` reads `is_const` and emits `<reads>`", and that part was one line. Three
others were not, and all three came from **one fact nobody had written down: an archetype
instantiation carries the constructing class's own effect.** `variant{Tag := …}` is how every packer
in the bridge works, so narrowing the packers made them refuse to build the thing they exist to
build. The repo already knew — `emit_math_structs` carries a comment saying `<concrete><computes>`
is why the math structs can be built from `<computes>` code — but nothing connected it to `variant`.

- `variant` and `godot_ref` are `struct<computes>` / `class<computes>` now, and so are the container
  wrappers (`godot_array`, `dictionary`, `typed_array`, `typed_dictionary`, `callable`,
  `signal_ref`).
- The **singleton accessors** could not follow: a mirrored class descends from the native
  `vh_object`, and making 1023 classes `<computes>` would change what a *script's* own class may
  put in a field initializer. They construct no longer — `GetInput()` is
  `input[VhObjectOf(VhSingleton["Input"])]`, a cast over what the host built. That is R-SCN-6's own
  rule, which the accessors had been quietly violating since Phase 2, so the change is a correction
  and not a workaround.
- Effects are **contravariant** in a function type, which had to be checked because
  `typed_array(t)` carries its packers as typed members: a `<reads>` function satisfies a
  `<transacts>` member. `tests/verse_probe/effects_variance.verse` is that check.

Two things the mirror could not narrow, and both are worth an author knowing. **`ToString` stays
`<transacts>`**, because `Object.to_string` is not `const` in Godot — `_to_string` is a script hook
that can do anything — so `Print("hit {Body}")` is not available inside a `<reads>` function. And
**no static narrows**, because not one of Godot's 114 statics is `const`. Of the 114 utilities only
the eight random ones are dispatched at all, and those genuinely move the RNG; the five look-up
utilities (`type_string`, `error_string`, `instance_from_id`, `is_instance_id_valid`,
`rid_from_int64`) got a `<reads>` dispatch native of their own, chosen by hand because
`extension_api.json` carries no `is_const` for a utility.

### 11.5 What it came to

| | before | after |
| --- | --- | --- |
| mirror methods labelled `<transacts>` | 9597 | 5655 |
| mirror methods labelled `<reads>` | 0 | 3942 |
| labels knowingly untrue | unknown, unlisted | **1127**, generated into `docs/nonatomic-methods.md` |
| native primitives | 22 | 24 (`VhCallValueConst`, `VhCallUtilityConst`) |
| compensated mutate-and-answer calls | 1, not working | 1, working and tested three ways |

Nothing in `dodge-the-creeps/` or `tests/integration/scripts/` needed a change: narrowing 3942
declarations is invisible to a caller, which is what S-2 promised and what the yardstick's 29 checks
confirm.

### 11.6 What is still owed

- **`Object.Connect` has no compensated spelling.** `godot_signal.Subscribe` is rollback-safe;
  connecting to a *GDScript-declared* signal (R-SIG-6) goes through `Object.Connect`, which is in
  the non-atomic list. A `Subscribe`-shaped wrapper over an arbitrary Godot signal would close it
  and is not this phase's.
- **The 708 "answers a value" rows are believed rather than audited.** `Tween.is_running` is not
  `const` and reads like a query. The mirror believes Godot's annotation rather than second-guessing
  708 of them, which is the conservative direction: `<transacts>` claims less than it could, where
  a wrong `<reads>` would claim more than it should.
- **`docs/by-hand-checklist.md` gains nothing from this phase** and still owes what it owed. The
  editor-session check would now also see the new template text and the effect diagnostic in the
  script editor's error list, which no headless run can show.
