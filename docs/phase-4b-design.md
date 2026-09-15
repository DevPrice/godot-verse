# Phase 4b — The editor's data model: objects that are not nodes

**Status:** Designed 2026-09-15, not built. §2's six spikes ran **before** this document rather than
after it, which is the repo's habit and not 4.5's exception. All six are in `tests/verse_probe/`,
with a seventh (`default_cdo_probe.verse`) filed under §4.5 rather than §2 because it settles a
*rejected* option. Three of them changed what §4 and §8 say, and **S-6 needed a real host build**
— it is the only one a user-scope fixture could not answer alone.

**§4 was written twice.** Its first draft had R-NODE-3 reached by an `NewObject(helper{})` adopt
shape, because S-1 and S-2 closed the two routes by which a bare `helper{}` might have minted its
own Godot peer. S-4 and S-5 then found a third that neither had asked about — a class **`block:`**
clause — and the adopt shape is gone. What survives of the first draft is §4.5, which is why the
obvious-looking alternative was rejected on grounds that turned out not to hold.

**Read `roadmap.md` "Phase 4b" first** — it is the six-line version and its exit criterion is §12
here.

**Companion to:** `docs/spec.md` §5.1 and §5.4, which carry the per-requirement statuses this phase
moves, and `docs/phase-4-design.md` §13, which is why `_Get`/`_Set` were deferred out of 4a rather
than built beside the virtuals.

---

## 0. How to read this

**Scope of the first pass: §3 and §4 only**, by decision. They are the two stages with spikes behind
them, the ABI change, and the failure in §4.3 that would not announce itself; §5–§9 are conventional
Godot integration work with no spikes behind them and are reassessed once §4 is in and working.
The stage order below is unchanged — this says where to stop, not what to skip.

§1 is the subject and §2 is what is already known about it. The stages in §3–§9 are ordered by
dependency and each says what it is done when: **§3 comes first because §7 and §8 both need it**,
and §4 comes second because §5 needs a script instance on an owner that is not a node.

This phase has one property the earlier ones did not: almost every requirement in it is `none`
rather than `part`. Phase 4 was parity work against a surface that already half-existed; this is
five features that do not exist at all, and the risk is sprawl rather than regression. §14 is what
it deliberately leaves out.

---

## 1. The subject

Seven requirements, six of them in `spec.md` §5.4's inspector-and-data-model block and one in §5.1:

| requirement | today | what 4b owes it |
| --- | --- | --- |
| **R-NODE-3** (MUST) | none | a Verse class instantiated without a node, `RefCounted`/`Object` included |
| **R-EXP-6** (MUST) | none | a Verse class as a custom `Resource`, round-tripped through `.tres` |
| **R-EXP-7** (MUST) | none | a Verse script as an autoload singleton |
| **R-NODE-10** (SHOULD) | none | `_ToString`, `_Get`, `_Set`, `_GetPropertyList`, `_ValidateProperty` |
| **R-EXP-9** (SHOULD) | none | `@rpc` reported through `_get_rpc_config` |
| **R-EXP-8** (SHOULD) | part | a declared editor icon; `_get_class_icon_path` returns `String()` today |
| **R-EXP-1** (MUST) | part | the hints a Verse type cannot imply — file, dir, multiline, flags, node path |

Two of them are not really about the inspector and are here because nothing else wants them.
**R-NODE-10** needs a Verse spelling for a value of unknown Godot type, which §3 builds; **R-EXP-9**
needs a way to reach a vararg method, which §8 builds and which happens to unlock the other 31.

What the phase is *about*, in one sentence: Godot's data model has three lifetimes — a node the
tree owns, a `RefCounted` that dies with its last reference, and a bare `Object` freed by hand —
and this bridge has only ever built the first.

---

## 2. Spikes — already run

The vehicle is `tests/verse_probe` (`tools/build_verse_probe.py`), run **from `bin/`**, where
`tbbmalloc.dll` lives; without it `LoadLibrary` answers a bare 126 and names nothing.

    bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
        <absolute path>/tests/verse_probe/<fixture>.verse --class <name>

Take the fixture path as **absolute**: the probe resolves nothing relative to `bin/`.

### S-1 — can a user archetype on a mirrored base fill the inherited `Handle`?

**Fixture:** `construct_probe.verse`. **Answer: no, and the obstacle is access rather than
construction.**

Three of its four questions compile. `helper{}` where `helper := class(ref_counted)` is accepted; a
`<constructor>` on a Godot-derived class is accepted; and neither is refused for its effect in a
specifier-less or `<transacts>` body. What fails is writing the field:

    error 3593: Invalid access of internal data `(/Godot.org/Godot:)vh_object:Handle`
    from control scope `/user@localhost/construct_probe/C2`.

So R-TYPE-7 is what stands in the way, working exactly as designed. `Godot.native.verse:125` already
recorded that `node{Handle := ...}` builds the class the *signature* named rather than the class the
object *is*; what S-1 adds is that a user cannot write that expression at all. **The current
behaviour is a footgun**: `helper{}` compiles today and yields an object whose `Handle` is 0, which
every call then misdirects, with no diagnostic. Whatever §4 settles on, that has to stop being
silent.

### S-2 — can the mirror fill it on the user's behalf?

**Fixture:** `ctor_delegate_probe.verse`. **Answer: not by either route Verse appears to offer.**

- **Delegation does not exist.** `MakeBase(9)` inside another constructor's body is
  `error 3552: Unsupported argument to archetype instantiation`. A user constructor cannot call a
  base constructor that would write the field for it.
- **A data member's default may not be a call.** `Computed:int = Minted()` is
  `error 3582: Divergent calls (calls that might not complete) cannot be used to define
  data-members`. So the mirror cannot give `vh_object.Handle` a default that mints a peer.

The two errors the fixture still reports **are** its result, the way `async_reject.verse`'s are.

It also produced two findings it was not asked for. Declaring those constructors `<computes>` over
classes that carry `transacts` is `error 3512`, which re-confirms CLAUDE.md's archetype-effect rule
from a second direction. And `<converges>` is `error 3565: only allowed on native functions` — which
matters, because 3582 bans *divergent* calls specifically, so a `<converges><native>` default may
not be divergent and may not be banned. **That is the one lead S-2 leaves open**, and §4 says why it
was not simply taken.

### S-6 — does the block clause work where §4 actually puts it?

**Fixture:** `native_block_probe.verse`. **Answer: yes, and §4 is proven rather than assumed.**

S-4 and S-5 both used a plain Verse class in the user's own package. §4 puts the clause on
`vh_object`, which is `<native>`, compiled by VNI at UBT time, and expects it to fire for a user
class in `/user@localhost` reached through 1036 generated mirror classes. CLAUDE.md is explicit that
the first does not imply the second: *"A host build passing is not enough to know a `.verse` file
compiles."* So this one needed a real host build behind it rather than a user-scope fixture alone —
`var Handle` plus `block: set Handle = 4242` on the native root, a public reader beside it, and a
revert afterwards.

| | answers | what it establishes |
| --- | --- | --- |
| N1 | 4242 | VNI accepts `block:` on a `<native>` class, **and it fires across the package boundary** for `class(ref_counted)` in `/user@localhost` |
| N2 | 4242 | and for a class two deep, since a script may extend another script |

**What it also measured, which §4.2 had guessed at.** Making `Handle` a `var` is not free on the C++
side. The shadow must become `verse::TPtr<int64> Handle;` rather than `TVal<int64> Handle;`, and the
two host writes then fail because `TPtr` has no `Init` — `HostScript.cpp:3152` and `:7829` become
`Handle.Set(Handle, Shadow)` (`VersePointer.h:52-62`). Reads through `.Get()` are unaffected. Four
lines in three files, and `V_STATIC_ASSERT_HAS_DATA` names the exact declaration it wants, so the
failure is self-diagnosing rather than mysterious.

### S-3 — does Verse accept the overloads a generated vararg would need?

The odd one out topically — it belongs to §8 rather than to R-NODE-3 — and kept in sequence because
the numbers are the order they were run in.

**Fixture:** `overload_probe.verse`. **Answer: yes, in all four forms.** CLAUDE.md records that
Verse's overloading is "far narrower than `phase-2-design.md` §1 claimed" without saying where the
edge is; it is not here. Arity overloads, same-arity-different-type overloads, a scalar beside a
`[]variant`, and a single signature with an optional `?Args` all compile, and the class's `UseD`
ran and answered.

**The finding it was not asked for is the one that constrains §8.** The probe's method listing
reports the overloads with *identical descriptors* —

    [probe]   CallB  params=1 result=2  virtual=
    [probe]   CallB  params=1 result=2  virtual=

— so two same-named methods are indistinguishable to anything that dispatches by name, which is what
Godot does and what `vh_instance_call` does. **Overloads are therefore fine for the mirror's
outbound calls and not for a method a script declares**, because Godot resolves the latter out of a
name-keyed list. §8 generates the varargs as real overloads on that basis; §7's hooks each keep one
signature.

### S-4 — is there a third thing in a class body that runs per instance?

**Fixture:** `class_block_probe.verse`. **Answer: yes — a `block:` clause, and it is the whole of
R-NODE-3.**

S-1 and S-2 between them asked about archetypes, constructors and data-member defaults, which is
not the entire list of what a Verse class body may contain. A **`block:`** clause is the fourth
thing, and the compiler's own comment on the restriction S-2 hit is what names it — from
`SemanticAnalyzer.cpp:15266`, explaining why a default initializer may not call anything:

> This code was previously working around this restriction by using a do clause and mutation,
> **which is evaluated each time the class is instantiated, and so we can allow it to have side
> effects.**

So the ban in 3582 is about CDO initialization specifically, and a `block:` clause is the sanctioned
way around it. Four questions, four answers, each a method that runs:

| | answers | what it establishes |
| --- | --- | --- |
| B1 | 7 | the clause runs at all, and `set` on a `var` member works inside it |
| B2 | 7 | **it runs for a derived class the archetype names** — the base declares the block, the user writes `helper{}` |
| B3 | 16 | **per instance, not once on the CDO** — two constructions with different archetype fields give `3*2 + 5*2` |
| B4 | — | a `<transacts>` call inside the block is legal, so it may reach a native |

B2 is the one that makes R-NODE-3 reachable: the mirror declares the block once on `vh_object` and
every user class below it is constructed with it.

### S-5 — can the block tell the host *which* class is being made?

**Fixture:** `class_block_self_probe.verse`. **Answer: yes, and the host's own path runs it too.**

A block on `vh_object` does not know it is being run for a `helper`. The only thing that could carry
that is the object.

| | answers | what it establishes |
| --- | --- | --- |
| S1/S2 | 1 | the clause may name `Self` and pass it to a function |
| S3 | 2 | **`Self` is already the derived type.** `Identify(Self)` dispatches a virtual and gets the *override*, not the base's answer — so a native handed `Self` can resolve the concrete Verse class |
| S4 | 99 | **a block also runs on the host's `vh_instantiate` path**, on a class built through the ABI rather than from Verse |

S3 is what makes the minting native possible without a class argument. **S4 is the architectural
consequence** and §4.4 is what it costs.

---

## 3. Stage 1 — a public `variant` façade

**First, because §7 and §8 both need it, and neither can be written without it.**

The gap is smaller than it looks and the spec's own wording is what obscured it. `variant` is
**already public** — `Godot.native.verse:38` declares `variant<public><native> := struct<computes>`,
and the comment there says the type is public deliberately, so a script can name one in a signature.
What a script cannot do is *make* one or *read* one: every lane below the declaration leaves the
specifier off, and the twenty-odd packers and readers in `GodotApi.native.verse:89-303` —
`VhFromInt`, `VhToInt`, `VhFromArray`, `VhToHandle` and the rest — are module-scoped by R-TYPE-7.

So this stage is **ordinary Verse in the mirror package**: a public constructor per Godot type and a
public failable reader per Godot type, over the packers that already exist. No ABI change, no new
native, no new lane.

**Generated by `gen_verse_api.py`, not hand-written**, and emitted beside the `godot_array` and
`dictionary` wrappers it most resembles. The reason is the one already recorded for those: a type
table is not a list to maintain by hand, and this is the *same* table — twenty-odd Godot types
against two directions. Generating it means a Godot version bump that adds a type gets its
`MakeVariant` and its reader for free rather than silently missing them, which is the failure mode a
hand-written façade would have. The packers it wraps stay hand-written in `GodotApi.native.verse`;
only the public layer above them is generated.

    MakeVariant<public>(Value:int)<reads>:variant
    MakeVariant<public>(Value:float)<reads>:variant
    MakeVariant<public>(Value:string)<reads>:variant
    MakeVariant<public>(Value:vector2)<reads>:variant
    ...
    (V:variant).AsInt<public>()<decides><reads>:int
    (V:variant).AsFloat<public>()<decides><reads>:float
    (V:variant).AsObject<public>()<decides><reads>:vh_object
    ...

Overloaded on the argument type, which S-3 says is legal, and *failable* on the way out, which is
what makes a wrong expectation an `if` that does not take rather than a silently zero lane. The
lanes stay non-public: a script never sees `Tag` or `F0`, so the R-TYPE-7 promise that "the plumbing
stays hidden" survives — what changes is that the *value* stops being unreachable while the type was
always nameable.

**Extension methods, not free functions, for the readers.** A bare `AsInt` would join the
module-level names that collide with a local of the same name, which CLAUDE.md lists as a standing
trap; `(V:variant).AsInt()` is reached through a receiver and cannot.

**What this does not do.** It does not make `variant` a general "any" type for user code. Nothing
about a script holding one changes: the lanes still read as their defaults, a `Tag` of 0 is still
Nil, and the worst a hand-built variant can still say is "nothing".

**Done when** a probe fixture round-trips every Godot type through `MakeVariant`/`As*` in user code,
and `spec.md` R-TYPE-7's status sentence says which half is public and why.

---

## 4. Stage 2 — R-NODE-3: making an object that is not a node

### 4.1 The spelling is Verse's own

    H := helper{}
    H.Tag = 7
    H.Answer()

No new function, no new syntax, and nothing for an author to learn: the archetype a Verse programmer
would write anyway is the one that works, and it is what `Helper.new()` is in GDScript. The whole of
it is one clause on the native root:

    vh_object<public><native> := class:
        var Handle<native>:int = 0
        block:
            set Handle = VhAdoptOrMint(Self)

S-4's B2 is why it reaches a user class: the block is declared once on `vh_object` and runs for
every derived class an archetype names. S-5's S3 is why it can mint the *right* peer: `Self` is
already the derived type when the base's block runs, so the native resolves the concrete Verse class
and maps it to its nearest mirrored Godot ancestor through `GodotClassNames.gen.h`, which already
carries that mapping both ways for R-SCN-6's cast.

`VhAdoptOrMint` writes `Handle` from C++, where 3593 does not apply: access control is a
compile-time property of the Verse scope, and the host already writes that field at
`vh_instantiate` for every scripted node in the project.

### 4.2 What it costs, and what it does not

- **`Handle` becomes `var`.** It keeps no access specifier, so it stays module-scoped and 3593 still
  fires for user code exactly as S-1 recorded. **R-TYPE-7 is not amended** — the plumbing stays
  hidden and `helper{}` works anyway, which is the part worth noticing: the access rule was never
  what stood in the way, only the absence of a per-instance hook. On the C++ side it costs four
  lines, measured in S-6: the shadow becomes `verse::TPtr<int64>` and the two writes become
  `Handle.Set(Handle, Shadow)`.
- **One new native and one new Godot callback**, `ClassDB::instantiate` by name. A **minor** ABI
  bump under the header's own policy: nothing older needs it, so an older consumer behind the
  `StructSize` check is unaffected.
- **`helper{}` now carries `transacts`**, so a `<reads>` or `<computes>` body cannot construct one.
  That changes nothing an author can see — CLAUDE.md already records that a mirrored class is
  unconstructible from a narrowed body and that the way into one is a cast over what the host built.
- **The footgun closes by construction rather than by diagnostic.** A bare `helper{}` yielding
  `Handle = 0` was the thing the first draft of this section had to add a diagnostic for. There is
  nothing left to diagnose: the archetype that used to produce a dead object now produces a live one.

### 4.3 The one thing S-4 costs the host

S-5's S4 is the architectural consequence and it is not optional: **the block runs on the host's own
`vh_instantiate` path too.** The host has already made the Godot object for a scripted node — that
is what a script instance *is* — so an unconditional mint would create a second peer for every node
in the project and leak one per instance.

So the native is `VhAdoptOrMint` rather than `VhMint`: the host marks its own construction across
`vh_instantiate` (a thread-local, since every entry point that runs Verse is the `vh_init` thread's
alone) and the native answers the handle it already holds instead of making one.

**The ordering detail S-5 could not see**, and the one thing to confirm when implementing: whether
the block lands before or after the host's own write to `Handle`. A probe running in user scope
cannot observe it. The flag settles it either way — the native answers the held handle in both
orders — but the assertion belongs in `host_smoke` rather than in a comment.

### 4.4 Lifetime — Godot's three, not one

A Verse value holding a peer is dropped. What happens to the peer depends on what the peer is, and
the answer is Godot's own rather than a rule of this bridge's:

| base | when the Verse value is dropped |
| --- | --- |
| `ref_counted` and below | unreferenced, so the object dies with its last holder |
| `object` | nothing; the author calls `Free()`, as in C++ |
| `node` | nothing; the tree owns it once it is parented |

The mechanism for the first row exists: `godot_ref` is already "a UObject whose `BeginDestroy` is
what releases a reference id when Verse drops the value holding it", which is how an Array's
ref-table entry is reclaimed. A peer is the same shape of problem with a different release call, and
`vh_object` is already a UObject (`GodotClasses.h:54-62`) — it simply has no `BeginDestroy` today.

**The trap is that not every `vh_object` is ours.** Every object crossing *from* Godot arrives
through `VhObjectOf`, and it is a `vh_object` too. A `BeginDestroy` that released unconditionally
would free objects this bridge does not own — a node the scene owns, a singleton — on the next
collection that finds the Verse wrapper unreachable. So the peer carries an **owned flag**, set at
mint and clear on adopt, and `BeginDestroy` releases only what was minted and only for
`ref_counted` and below.

A Verse-minted `node` that never enters the tree therefore **leaks, deliberately** — exactly as
GDScript's `Node.new()` does when nothing frees it, and Godot's own orphan-node warning at exit
reports it the same way for both languages. Freeing it would be better than Godot and would diverge
from it, and an author porting a leak-by-design idiom would get different behaviour with no notice.
`godot_ref`'s recorded latency applies unchanged: release happens on the collection that finds the
object unreachable, so a peer outlives its Verse value by up to one cycle.

**The transactional half is clean, and it is clean because the block is honest about its effect.**
Minting is `<transacts>`, so a peer created inside a computation that later fails is a write that
has to come back, and the compensation is
`AutoRTFM::OnAbort<AutoRTFM::EOpenBehavior::SameAsClosed>` releasing the peer. `SameAsClosed` is
load-bearing for the reason CLAUDE.md gives: every Godot callback reaches C++ inside
`AutoRTFM::Open` and a plain `OnAbort` from open code is ignored. This is the same mechanism
`signal.Subscribe` uses and the same one Phase 4.5 tested three ways.

The UE half needs nothing from this bridge. AutoRTFM instruments allocation and the GC, not only
stores: `StaticAllocateObject` registers `UE_AUTORTFM_ONABORT` to set
`EInternalObjectFlags::AutoRTFMConstructionAborted` on an object freshly constructed inside a
transaction (`UObjectGlobals.cpp:3836-3845`, the flag at `ObjectMacros.h:680`), and such an object
is then excluded from the reference collector and from global iteration
(`FastReferenceCollector.h:610`, `UObjectIterator.h:71`). So the Verse object and its UE shadow are
reclaimed by the engine; **only the Godot-side peer needs compensating**, because nothing on that
side of `verse_host_abi.h` is a UObject and AutoRTFM has never heard of it.

### 4.5 The `<converges>` route, and an object lesson about why options get rejected

**Fixture:** `default_cdo_probe.verse`. Kept because the reasoning failed twice in an instructive
direction before landing on the fact that actually closes it.

S-2 left one lead: 3582 bans *divergent* calls in a data-member default and 3565 says `<converges>`
is native-only, so `Handle:int = SomeConvergesNative()` might have been accepted. It was rejected
first on the grounds that minting a Godot object is `<transacts>`, so a `<converges>` native would
put the peer outside the transaction that created it and a `helper{}` in a failing computation would
leak.

**That premise was false.** AutoRTFM instruments allocation and the GC, not only stores, per §4.4's
citations — "outside the transaction" is not the fate a construction in an aborted transaction
meets. The second attempt at a reason was that a `<converges>` native minting an object is
*mislabelled*, which is true but is an argument about taste, not reachability.

**What actually closes it is 3502.** A data-member default cannot read the instance at all:

    error 3502: Accessing instance member `Seed` from this scope is not yet implemented.

Not a sibling field, and therefore not `Self`. So the `<converges>` native *would* compile — the
analyzer at `SemanticAnalyzer.cpp:15256` tests only `EEffect::diverges`, which a total native has
clear — and would then be handed nothing identifying the object under construction. A `helper` on
`ref_counted` and a `helper` on `resource` reach the same argument-less call, so it could never
learn which Godot class to mint. Nor can the mirror give each generated class its own default,
because a member may not shadow an inherited one (CLAUDE.md, "Modules and names").

That is also the axis on which the block clause wins rather than merely differing: **S-5's S1 and S3
say a block may name `Self` and sees it already at the derived type.** Same placement in the class
body, opposite answer to the only question that matters.

The lesson worth carrying: this option was killed twice by objections that were not the reason, and
both times the objection sounded more authoritative than the one that turned out to be true. It
compiles, and compiling does not help.

**Done when** a Verse script writes `helper{}`, calls its methods, passes the object to another
script and to GDScript, and the object is collected when the last holder drops it — and when a
scripted node still gets exactly one peer.

### 4.6 What GDScript can and cannot do with one

`new()` is bound on `GDScript` (`gdscript.cpp:1093`) and on `CSharpScript`, **not on `Script`**, so
a GDExtension language has none and `MyVerseClass.new()` cannot work without an upstream change.
From GDScript the spelling is Godot's general one:

    var h = RefCounted.new()
    h.set_script(load("res://helper.verse"))

This is R-INT-1's business rather than R-NODE-3's and the phase does not add a factory to paper over
it. It is recorded here because "instantiate a Verse class from GDScript" will be asked, and the
answer is a Godot limitation with a citation rather than a gap in this bridge — and because it is
now the *only* asymmetry left: Verse writes `helper{}`, GDScript cannot write `Helper.new()`, and
the reason is on Godot's side.

---

## 5. Stage 3 — a Verse class as a custom `Resource`

A `Resource` is the first consumer of "a script instance on an owner that is not a node", which is
why it is after §4 and not before it.

Most of the machinery is already general. `_get_instance_base_type` reads the declared Verse
superclass and answers whatever mirrored class it finds
(`verse_script_language.cpp:2742`), so `class(resource)` already answers `Resource`;
`_instance_create` takes any `Object *`; the export list, the property get/set path and
`get_property_list` are keyed on the class rather than on nodehood. What has never been exercised is
any of it against a non-`Node` owner, and the parts likely to assume one are the script instance's
own vtable and `VerseScript::_can_instantiate`, which gates on `is_editor_hint()` — a Resource is
*always* edited in the editor, so a non-`@tool` Verse resource that answers false there would be a
placeholder in the only place it is ever used.

**The bar is the editor round-trip**, which is the roadmap's exit clause: created from the editor's
New Resource dialog against a `@global_class` Verse class, edited in the inspector, saved to
`.tres`, loaded back with its exported values intact, and its Verse methods callable on the loaded
object.

**`@global_class` is a prerequisite rather than a separate feature.** The New Resource dialog lists
what the global class registry carries, and `_get_global_class_name` already answers from the source
text with `base_type` filled from `base_types_for` — so a Verse resource class registers the way a
Verse node class does, with no new attribute.

**Out of scope for the stage:** binary `.res`, and a resource that holds another resource as an
exported member. The second is `VH_EXPORT_HINT_SCRIPT_CLASS`, which exists; whether it draws a slot
that accepts a Verse resource is worth a case but not worth blocking the stage.

**Done when** the five steps above run by hand and the loaded resource's exported values match what
the inspector saved.

---

## 6. Stage 4 — a Verse script as an autoload

Godot's rules, read out of `editor_autoload_settings.cpp`, decide this stage almost entirely:

- `_create_autoload` takes the script's `get_instance_base_type()` and fails unless
  `ClassDB::is_parent_class(ibt, "Node")` (`:354-355`). So an autoload Verse class must extend
  `node` or below, and a `class(ref_counted)` is refused by Godot with its own message.
- `in_editor` is `scr.is_valid() && scr->is_tool()` (`:390`, and again at `:527` and `:590`). So
  **a `@tool` Verse autoload is instantiated in the editor too**, and a plain one is not.

The phase follows both rather than deviating. The consequence for a `@tool` autoload is the bargain
`@tool` already documents — it runs the **last built** generation, per §10's trigger — and it is the
same bargain a C# `[Tool]` script makes. What it means in practice is that an editor session that
has never built runs an autoload with no class behind it, and the honest behaviour there is the one
a failed script already has: report and carry on, not a silent no-op.

`_get_instance_base_type` answering `StringName()` for a library file is already right for this: a
`.verse` with no class of its own cannot be an autoload and Godot's own check says so.

**Done when** a Verse autoload answers from every scene in a running game, a `@tool` one also
answers in the editor, and a `class(ref_counted)` named as an autoload is refused with Godot's own
sentence rather than a crash.

---

## 7. Stage 5 — the script-level hooks (R-NODE-10)

Five virtuals that are in no part of `extension_api.json`, which is why none of them can be
generated and all five are hand-written on the native root beside `_Notification`:

    _ToString<public><native>()<reads>:string
    _Get<public><native>(Property:string)<transacts>:?variant
    _Set<public><native>(Property:string, Value:variant)<transacts>:logic
    _GetPropertyList<public><native>()<transacts>:godot_array
    _ValidateProperty<public><native>(Property:dictionary)<transacts>:void

`_Get` and `_Set` are why §3 comes first: both carry a value whose Godot type is not known until it
arrives, and `variant` is the only thing in this bridge that can hold one. With §3's façade the
author writes `Value.AsInt()` in a failure context and the wrong expectation is an `if` that does
not take.

`_GetPropertyList` answers a `godot_array` of `dictionary`, which is Godot's own shape and needs
nothing new. `_ValidateProperty` takes the property `dictionary` and mutates it in place, which is
also Godot's shape — and it is the one of the five that is purely additive, since a property that
does not exist cannot be validated.

**Composition with `@export` is the design question inside this stage**, and the answer is Godot's:
`_Get`/`_Set` are consulted for names the property list does not already carry, so an `@export`ed
member is never routed through them. Anything else re-implements the export machinery in Verse for
no gain, and would make an `@export` and a `_Get` of the same name a silent race.

**One signature each, no overloads.** S-3's second finding is why: Godot resolves a script method by
name out of a name-keyed list, and two `_Get`s would be indistinguishable in it.

**Done when** `print(node)` in GDScript shows what a Verse `_ToString` chose, and a Verse script
serves a property that no `@export` declares.

---

## 8. Stage 6 — varargs, and R-EXP-9 on top of them

Godot has **33** vararg entry points and the mirror skips every one of them under R-SCN-2's
`vararg` category: 15 class methods (`Object.call`, `Object.call_deferred`, `Object.emit_signal`,
`Node.rpc`, `Node.rpc_id`, `SceneTree.call_group`, `TreeItem.call_recursive` and eight more), 12
utility functions (`print`, `str`, `max`, `min` and the print family), and 6 on the builtin types
(`Callable.call`, `Callable.bind`, `Callable.rpc`, `Callable.rpc_id`, `Signal.emit`,
`Callable.call_deferred`).

S-3 says they can be generated as real overloads, and they are outbound calls rather than
script-declared methods, so the name-collision finding does not reach them:

    Rpc<public>(Method:string, Args:[]variant)<transacts>:void
    Rpc<public>(Method:string, A:variant)<transacts>:void

The element type is `[]variant` because Godot's own is `Variant...`, which is what "typed correctly
to the varargs type" means here — there is no narrower truth to tell. §3's façade is what makes the
argument list writable by a script, and that is the whole reason this stage is after it.

**R-EXP-9 is then two halves.** Receiving is an attribute in the runtime-compiled package beside
`@tool` and `@export_group`, carrying Godot's four fields, and `_get_rpc_config` reporting it
instead of the empty `Variant` it returns today (`verse_script.cpp:1047`):

    @rpc("any_peer", "call_local", "reliable", 0)
    TakeDamage<public>(Amount:int)<transacts>:void = ...

Strings rather than enums, for the reason `@statics` takes a string: uLang hands back an attribute's
*text* value and there is no equivalent for a typed argument. Checked rather than trusted, so a
misspelled mode is a diagnostic at the method's line the way a bad `@statics` association is.
Sending is `Rpc`/`RpcId` from the vararg set above.

**Done when** a Verse method annotated `@rpc` is called across a peer connection and a Verse script
sends one, and when `Object.call` and `Signal.emit` are reachable from Verse.

---

## 9. Stage 7 — the remaining `@export` surface, and the icon

The rule the spec already states: **type-driven where the Verse type can say it, an attribute only
where it cannot.** Everything a bounded number, an enum or a mirrored class can imply is already
done. What is left is the set where the declared type is `string` or `int` and says nothing:

| attribute | why the type cannot say it |
| --- | --- |
| `@export_file("*.png")` | a path is a `string` |
| `@export_dir` | so is a directory |
| `@export_multiline` | so is a paragraph |
| `@export_flags("Fire", "Water", "Earth")` | a bitmask is an `int`; Verse has no flag enum |
| `@export_node_path("Node2D")` | a `NodePath` is a `string` on this wire |

Each is a class in the runtime-compiled attribute package (`HostScript.cpp`, `AttributePackageSource`)
and a new `vh_export_hint` value carried in `vh_export_desc`'s existing `Hint`/`HintString` fields —
so this is an ABI **minor** bump under the header's own policy, not a major one: an older consumer
that does not know a hint ignores it behind the `StructSize` check and draws a plain field.

Deliberately not in this set, and recorded so the omission is visible: `color_no_alpha`,
`exp_easing`, `global_file`/`global_dir`, `placeholder_text`, and enum-on-`string`. They are real
parts of GDScript's surface and none of them is reached by a first project; R-EXP-1 stays **part**
with this list as the reason.

**R-EXP-8 rides along** and needs no mechanism of its own: `@icon("res://player.svg")` in the same
attribute package, read out of the source text the way `@tool` is — because Godot asks
`get_class_icon_path` of scripts it has only scanned — and returned from
`VerseScript::_get_class_icon_path`, which answers `String()` today (`verse_script.cpp:395`).

**Done when** each of the five draws its Godot editor, and a script with `@icon` shows it in the
scene tree and the create-node dialog.

---

## 10. R-EXP-4 — restated rather than built

R-EXP-4 asks that the inspector reflect a changed export "without restarting the editor — including
a changed *default value*, which today requires code generation and therefore a restart."

**The restart is already gone and the requirement's own clause is what is stale.** The export
*shape* — the member list, the hints, the Reject reasons — refreshes per keystroke, because
`vh_class_export_list` answers from the snapshot the last analysis left. What waits is a changed
default, and it waits for a **build**, not a restart, because a default is generated code and
§10's trigger is build-on-Play-or-Build-action. That is the same bargain C# makes, and it is a
deliberate decision rather than a limitation.

So this phase **restates the clause** — "…without restarting the editor; a changed default lands at
the next build, per §10's trigger" — and moves R-EXP-4 to **done**. It builds nothing. Recorded as a
stage so that the change is a decision with a reason attached rather than a quiet edit to a MUST.

---

## 11. Tests

`run_tests.py` stays the one command (R-QUAL-3) and no layer is added.

- **units** — the generator gains cases for the five new hints and for the vararg overload shape, in
  `tests/verse_api_gen/`.
- **abi** — `host_smoke` gains the `variant` façade round-trip; `VhAdoptOrMint`'s peer lifetime
  including the abort path; **the peer count after instantiating a scripted node**, which is what
  catches §4.3's double-mint; the block-versus-host-write ordering; and the `@rpc` descriptor.
- **integration** — a Verse script creating and dropping a `ref_counted` object; a Verse autoload
  answering; `_ToString` and a dynamic `_Get`; each of the five hints as an export whose descriptor
  is asserted. New cases go in `tests/integration/test_cases.gd`, one line each, so the editor run
  and the exported run cannot disagree about what passing means.
- **by hand** — the Resource round-trip and the `@tool` autoload in the editor. Both are inside the
  category `by-hand-findings.md` exists for, and both get steps written there rather than a claim
  made here.

Two things cannot be tested from here and should be said in `spec.md` rather than discovered:
`_Get`/`_Set` are reachable from GDScript, but `_ValidateProperty` is called by the *inspector*, so
its only test is the by-hand session; and an autoload's editor half exists only under
`is_editor_hint()`, which is the same wall `@tool`'s B8 sits behind.

---

## 12. Exit

The roadmap's clause, unchanged: **a Verse custom Resource is created, saved to `.tres`, edited in
the inspector and loaded back, and a Verse autoload answers from every scene.**

Plus, for the requirements this document adds to that: a Verse script creates an object that is not
a node and the object is collected when it should be; `print(node)` in GDScript shows what a Verse
`_ToString` chose; and R-EXP-4 is restated with its reason.

---

## 13. Risks

- **§4.3's double-mint is the failure that would not announce itself.** Every scripted node in a
  project runs `vh_object`'s block on the host's own construction path, so a bug in the
  adopt-or-mint flag leaks one Godot object per node — and a project with a working scene would
  look entirely normal while doing it. The flag wants a `host_smoke` assertion on the peer count,
  not just a working demo.
- **The peer lifetime is the part that can be subtly wrong.** A dropped `ref_counted` that is not
  released is a leak nothing reports; one released twice is a crash. `godot_ref`'s `BeginDestroy`
  path is the precedent and the abort compensation is the part with no precedent, so it wants a
  `host_smoke` case that aborts three ways, as Phase 4.5's did.
- **A block on the native root runs for every mirrored object in the project**, which is 1036
  classes' worth of construction path, not just a user's `helper`. The cost is one native call per
  construction and it has never been measured; `tools/build_bench.py`'s `vh_instantiate` figure
  (5.2 µs per node) is the number that must not move much.
- **Stage 3 assumes the script-instance machinery is nodehood-agnostic and has never proved it.**
  `_can_instantiate`'s `is_editor_hint()` gate is the one place a Resource is known to differ, and
  there may be others that only a real `.tres` finds. This is the stage most likely to cost more
  than it looks.
- **Stage 7 is the one that sprawls.** Five hints is a decision; the eleven GDScript actually has is
  a list that grows while it is being worked. The set in §9 is closed by decision and the rest is
  recorded as why R-EXP-1 stays `part`.
- **`variant` becoming public is a one-way door.** Once scripts write `MakeVariant`, the façade is
  API. It is small and it is over packers that already exist, but R-TYPE-7 is a MUST and its status
  sentence has to say which half is public and why, in this phase, not later.

---

## 14. What this phase does not do

- **`_CanDropData`** stays the one Godot virtual never exercised. It is not in this family.
- **R-INT-6** — a Verse class extending a GDScript or C# class. Still a MAY and still not worth it.
- **A `new()` for GDScript.** §4.4 is the citation; the fix is upstream in Godot and this phase
  records rather than works around it.
- **The `@export` hints listed as omitted in §9**, and binary `.res` in §5.
- **OQ-17 — C# has never been run against this bridge.** This phase enlarges the claim R-INT-1
  makes without testing it, exactly as Phase 4 did, and says so here rather than letting the
  statuses imply otherwise.

---

## 15. What building this corrected

*Written after the work. Every phase document in this repo has this section and several of them
contradict their own §1; leave this heading in place until there is something true under it.*
