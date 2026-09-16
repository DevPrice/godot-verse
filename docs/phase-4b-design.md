# Phase 4b — The editor's data model: objects that are not nodes

**Status:** Designed 2026-09-15; **§3, §4 and §5 built 2026-09-15, §6–§9 not.** §15 is what the work
corrected, and it corrects more of this document than usual — including the fact that §3 describes
something that already existed. Read it before trusting §3 or §4.2.

§2's six spikes ran **before** this document rather than
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

**§7's first line is wrong about `_ToString`, and the correction is the design.** A Verse class does
not get a `_ToString` hook, because Verse already has a spelling for this and it is not a Godot
virtual: an **extension method**.

    (X:my_class).ToString<public>()<transacts>:string = "..."

That is what an author writes, and `tests/verse_probe/tostring_probe.verse` is why it is the only
thing they *can* write. Both of the alternatives are refused by the compiler, each for its own
reason:

- a class **member** named `ToString` is glitch 3532 against `/Verse.org/Verse`'s own `ToString`,
  which is reachable as an extension method — the rule that already catches `Angle` and `event`;
- a module-level **overload** `ToString(X:my_class)` is glitch 3532 against **the mirror's own**
  `ToString(:object)`. Every script class derives from `object`, and Verse does not prefer the more
  specific overload. That one is worth pausing on: it is `FREE_FUNCTION_REPLACEMENTS` mapping Godot's
  `Object.to_string`, so the bridge's own decision is what closes that door.

**The two call syntaxes do not resolve to the same definition**, which is the finding the design
rests on. `Self.ToString()` reaches the extension method; `"{Self}"` desugars to `ToString(Self)` and
reaches `ToString(:object)`, which calls Godot's `to_string()`. So interpolation does not see the
override directly — **and arrives at it anyway**, by going out to Godot and back in through
`to_string_func`. One implementation, both paths, and nothing new in the Verse surface.

What the host must find is module-level rather than a class member, which is why
`vh_class_method_list` does not carry it and `InstanceCall` cannot reach it unaided:

    (/user@localhost:)operator'.ToString'(:my_class, :tuple())

The receiver is parameter 0; the call's own arguments are a tuple in parameter 1.

The remaining four are in no part of `extension_api.json`, which is why none of them can be
generated, and they are hand-written on the native root beside `_Notification`. **The three
specifiers on each line below are all wrong and §15 has the corrections** — they are neither
`<native>` nor `<transacts>`, and `_Get` answers a bare `variant` rather than an option:

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

**Done when** `print(node)` in GDScript shows what a Verse `ToString` extension method chose — **done,
and §15 has what it cost** — and a Verse script serves a property that no `@export` declares, which
is the other four and is **also done**; §15's "Stage 5's other four" is what they cost.

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
`ToString` extension method chose (§7, corrected — not the `_ToString` this document first proposed);
and R-EXP-4 is restated with its reason.

**All seven stages are built.** The clause above is met except for its editor half — a custom
Resource edited in the *inspector* — which no automated layer can reach and which
[`by-hand-findings.md`](by-hand-findings.md) carries with steps, beside the three the later stages
added: an RPC that arrives at a second peer, the five inspector hints drawn as controls, and the
icon in the scene tree.

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

*Written after the work.*

**§3 was already built, and the document did not check.** Stage 1 — "a public `variant` façade" —
describes work that shipped in Phase 2, commit `1c3441a`, three days before this document was
written. `gen_verse_api.py`'s `emit_variant_readers` emits a public `As<GodotType>` per lane and a
public `Variant<GodotType>` beside it, plus a `VariantKind` the design did not think to ask for,
and `tests/integration/test_cases.gd` has had a section headed "R-TYPE-7 amended: a script can name
a Variant and read it" for as long. §3's §"Done when" was met before §3 was written.

Two of its sentences are worth correcting rather than deleting, because both are wrong in a way a
reader would act on:

- **The overloaded `MakeVariant` it proposes cannot exist.** §3 spells `MakeVariant(Value:int)`,
  `MakeVariant(Value:string)` and so on, on S-3's authority that Verse accepts same-named overloads
  in four forms. The compiler refused exactly this arrangement in Phase 2 — two array-typed
  overloads are ambiguous whatever their element types, because `array{}` is a call site that
  cannot resolve them, and `string` *is* `[]char`; and `VariantFrom(:logic)` is ambiguous with
  `VariantFrom(:[]char)` for no reason any reading of "overloads on parameter type" predicts. The
  name per lane is not a stylistic choice, it is the only arrangement that compiles, and the
  reasoning is already in `gen_verse_api.py` above `VariantLane`. **S-3 was not wrong**; it asked
  about a class's methods, where overloading does work, and a module-level function is a different
  question. The lesson is the narrow one: a spike answers the question it was given.
  **Both halves of that bullet have since been measured and the second is wrong** — class scope does
  not rescue the pair, and there *is* a reason. See "The variant API's two spellings" below.
- **"The lanes stay non-public" is not true and the spec already says so.** §3 says a script "never
  sees `Tag` or `F0`". R-TYPE-7's own status paragraph records that Verse **forbids** a non-public
  field on a struct — `Verse::Version::StructFieldsMustBePublic` — so a public struct has public
  fields, `V.I0` compiles, and this was verified rather than assumed. The guarantee §3 restates is
  one the spec had already withdrawn.

**What §4 cost that §4.2's list did not have.** The `var Handle` change was four lines, as S-6
measured. The block clause was not, because of where else it fires:

- **Four host construction paths run it, not one.** §4.3 names `vh_instantiate`; the other three are
  the mirror wrapper every handle crossing from Godot builds, the bare `vh_object` the fallback
  answers when Godot will not say what a handle is, and — the one that matters —
  `NewDefaultsObject`, the throwaway instance the export defaults are read off. Each has an
  `FAdoptPeerScope` now. The record carries the **class** as well as the handle, so that a *member*
  of the class being built still mints its own, whichever order the VM runs the base's block and the
  derived class's initializers in; a bare take-once flag would have been wrong under one of the two
  orders and there was no need to find out which.
- **The defaults instance needed more than adopting: it needs suppressing.** Its members'
  initializers run in full, so a class whose member is `var Held:node2d = node2d{}` — which
  `tests/host_smoke/exports.verse` has had since Phase 4 — minted a real Node2D **per exporting
  class, per analysis**, and a Verse-minted node is deliberately never freed. That is a leak on the
  per-keystroke path, and it was found by the smoke harness's peer counter reporting 30 mints where
  the test made five. `FSuppressMintScope` is the answer: nothing constructed under a reading device
  reaches Godot, however deep.
- **§4.3's ordering question has an answer and no assertion, on purpose.** The block runs
  **before** the host's own write: `NewObject` drives `UVerseClass::PostInitInstance`, which runs
  the constructor, and the host writes `Handle` after it returns. The adopt record therefore has to
  *carry* the handle rather than read it off the object, which it does. There is nothing left to
  assert — both writes put the same value in the same field, so the two orders are
  indistinguishable from outside, and what a `host_smoke` case can see is the peer count, which is
  asserted.
- **A class-default object runs the block too.** `NewDefaultsObject`'s own comment says a CDO's
  members read back uninitialized, so this may never fire in practice — but `UVerseClass::NeedsInit`
  plainly returns true for one, and a guard on `RF_ClassDefaultObject | RF_ArchetypeObject` is two
  lines against a mint for every mirrored class the process ever names.

**The release hook was written twice, and the first version was silently a no-op.** `BeginDestroy`
identified its row by comparing a `TWeakObjectPtr` against the object being collected. By the time
`BeginDestroy` runs the object is already unreachable and **every weak pointer to it reads as
null**, so the comparison matched nothing, the row was never removed and nothing was ever released.
It looked exactly like the mechanism not working, and it cost the detour below. The row now carries
both pointers and says which question each answers: the weak one for "is it still alive", which
handing the object back to Godot needs, and a raw one for "is this the object that made this row",
which is compared and never dereferenced.

**`vh_collect_garbage` is new, and it exists because the lifetime half was otherwise untestable.**
Nothing else can make a collection happen: `vh_tick` collects on object-array pressure and a Godot
project has no hook into UE's GC at all, so "the peer is released when Verse drops it" could only
ever have been a claim. It is a minor ABI addition (8.3), the GDExtension does not call it, and
`host_smoke` does.

Getting it to collect anything took two wrong answers first, both worth recording:

- **Waiting on the VM's collector from the game thread deadlocks**, which is
  `docs/abi-v2-design.md` §1a's finding arriving from a second direction:
  `FHeap::RequestFreshCollectionCycle().Wait(Context)` hangs, because the thread that would wait is
  the one holding heap access the collector needs. §1a says "from inside running Verse code"; the
  truth is wider than that.
- **A UE collection on its own frees no Verse object.** Every cell the VM still holds is a root to
  UE's collector, so a plain `CollectGarbage` destroys nothing a script let go of. The pass that
  can is UE's *coupled* to the VM's — "FrankenGC" — and `CollectGarbage` only takes that shape while
  the VM's collector is signalling that it wants to start. So the sequence is: request a fresh
  cycle, do **not** wait, give the collector the moment it needs to raise the signal, then collect.

And a third thing that is a property of the mechanism rather than a mistake: **release is "within a
cycle or two", not "on the next one"**. The VM's registers still name what the last frame held, so
`host_smoke` asserts inside a bounded loop with an unrelated call between collections. §1a's "up to
one collection cycle" is optimistic; "up to" was doing the work.

**The `node2d{}` footgun did not only close, it took an idiom with it.** §4.2 says the archetype
that used to produce a dead object now produces a live one, and treats that as pure gain. It is
also a *breaking* change to four fixtures, because "construct a mirrored class and call a method on
it" was how `transactions.verse`, `concurrency.verse` and `tasks.verse` spelled a deliberate raise —
the handle was 0 and every call on it found a freed object. That now succeeds. The replacement is
better than what it replaced: `viewport{}` is an archetype of a class Godot will not instantiate,
which raises with a sentence naming the class, and it exercises R-NODE-3's own error path while it
is at it. Anything outside this repo that leaned on the old behaviour has no such warning, which is
what a MAJOR would have been for — except that nothing about the *ABI* changed meaning, only what a
Verse expression does, and the ABI has no version for that.

**What it cost, measured.** §13's open number: `vh_instantiate` is **5.2 µs per node**, which is
what `docs/spec.md` R-PERF-2 recorded before the block clause existed. A native call per
construction, on the path every scripted node takes, did not move it. `vh_instance_call` is 0.24 µs
against a recorded 0.27, which is noise in the other direction.

**What the identity half bought, which §4 did not ask for.** `ObjectForHandle` consults the minted
table before building a mirror wrapper, so an object a script made crosses back out to Godot and in
again as *the same Verse object* — which is what lets a script's own downcast succeed on it, and
what makes "holds, and passes around" in R-NODE-3's own wording true rather than nearly true. The
table had to exist anyway for the release hook; the identity is what it costs nothing extra to also
answer. The other half of R-SCN-6's identity question, arriving three phases later.

### Stage 5's first hook, which cost the most of any of them

**`_ToString` does not exist, and §7's first line is the thing this stage corrected.** The design is
in §7 now and in `spec.md` R-NODE-10; what belongs here is what building it cost, because it is the
one stage in this phase that cost more than its document expected rather than less.

Three facts had to be measured, and each was wrong on the first attempt with no symptom but silence:

1. **Where the extension method lives.** Not among the class's own definitions — in the class's
   *enclosing* scope, which is the module, so every file in the module sees it.
2. **What key the VM holds it under.** Not `DecoratedNameOf`'s shape, which is what every class
   lookup in this file uses. It is the scope prefix wrapped around the function's *whole* decorated
   name, which already repeats that scope and carries the signature:
   `(/user@localhost:)(/user@localhost:)operator'.ToString'(:(...)game_state,:tuple())`. Reusing a
   class's key returned null and `to_string` quietly kept Godot's own text.
3. **How accessible it may be.** No more than the type it extends, so `<public>` on a method
   extending an ordinary internal script class is glitch 3593 — which talks about subpaths and never
   says `<public>`.

None of the three announces itself. A wrong answer to any of them is a script that compiles, an
editor that behaves, and an object that prints `<Node2D#27>` — which is also exactly what a script
with no ToString at all should print. That is the shape of defect this phase kept finding, and it is
why the fixture asserts the *negative* control too: a class with no ToString must keep Godot's text.

It also needed an ABI entry point of its own (8.5) and a sidecar field (version 5), because the
thing being called is not a method and an exported game has no semantic program to find it in.

**What it did not need is any new Verse surface**, and that is the part worth keeping: `"{Obj}"` in
Verse reaches the same override as `print(node)` in GDScript, by going out through the mirror's
`ToString(:object)` to Godot's `to_string()` and back in. One implementation, both languages, and a
re-entrant Verse → Godot → Verse call that the bridge had never made before and which works.

---

### Stage 7, which §9 got right, and the one thing it put in the wrong place

**§9's table is the design and it survived.** Five attributes, five new `vh_export_hint` values in
`vh_export_desc`'s existing `Hint`/`HintString`, an ABI **minor** under the header's own policy
because an older consumer that does not know a hint draws the plain field it drew before the
attribute existed. Each hint string turned out to need no translation at all: the flags names are
comma separated because that is what `PROPERTY_HINT_FLAGS` wants, and the file filter is `*.png`
because that is what `PROPERTY_HINT_FILE` wants, so the string passes from the attribute to Godot
untouched.

**What §9 does not mention is the check, and it is the half worth having.** These five exist
*because* the declared type says nothing — so `@export_flags` on a `string` has no other detector.
The attribute compiles. Godot draws a plain field. And a plain field is also what *no* attribute at
all draws, so the author sees exactly what they would have seen if they had never written it. It is
`VH_EXPORT_HINT_WRONG_TYPE`, refused at the member with a sentence naming the attribute; the
existing `Reject` machinery carried it with no new field, and the `Hint` the author asked for is
what the message reads back, because `vh_export_desc` has no room for a reject detail and adding one
would be a layout change.

**And stage 6's constraint reached this stage before it was written.** §9 spells
`@export_flags("Fire", "Water", "Earth")` — three arguments, which an attribute may not take. One
comma-separated string is what Godot's own hint wanted anyway, so this is the one place the
toolchain's limit costs nothing; `@export_file`'s `*.png,*.jpg` is the same shape for the same
reason. Worth noticing, because it is the general answer for the next multi-argument attribute: when
Godot's own hint string is already a list, the list *is* the argument.

**`@icon` is in the wrong half of §9.** It is grouped with the five as "riding along", and it rides
nothing: the five are read from the semantic program by the export harvester, and `@icon` cannot be,
because Godot asks `get_class_icon_path` of a script it has merely **scanned** — from the filesystem
thread, before any host has built anything. It belongs with `@global_class` and `@tool`, in
`verse_scan_class_decl`, and that is where it is.

**Which also means the thing it is easiest to want to test is the thing that cannot be.**
`Script::get_class_icon_path` is a pure virtual with no ClassDB entry, its one caller
`EditorData::get_script_icon_path` — so GDScript can no more call it than it can call
`_make_function`, and the integration case written for it had to be deleted. What is asserted
instead is the scanner, in the **units** layer, which is where the logic actually is: the path, an
`@icon` on another class in the same file, a bare one, and one whose argument is not a literal. That
last is worth its own line: an attribute's argument is evaluated by the compiler and this is a text
scan, so a path built from an expression is unreadable here — and is not a case Godot could be told
about anyway, since it asks before anything is compiled.

---

### Stage 6's `@rpc`, and an attribute that may not be overloaded

**§8's spelling of the attribute cannot exist, and the compiler says so in a sentence worth
keeping.** §8 proposes `@rpc("any_peer", "call_local", "reliable", 0)`, Godot's own four arguments.
Four constructor arities of one attribute name compile — and every *use* of one is:

    Referencing an overloaded function without immediately calling it is not yet implemented;
    (/Godot.org/Godot:)rpc, (/Godot.org/Godot:)rpc, (/Godot.org/Godot:)rpc, or (/Godot.org/Godot:)rpc

An attribute site **references** its constructor before calling it. So the words travel together, in
one string, and one constructor:

    @rpc("any_peer call_local unreliable_ordered 2")

That turns out to be the better shape for a second reason the design had already half-recorded.
`GetAttributeTextValue` refuses any attribute whose argument is a `MakeTuple` — it is written that
way, with SOL-972 above it saying the area waits on compile-time evaluation of attributes — so a
four-argument attribute is unreadable through the only accessor there is. The first implementation
of this stage walked the argument expression itself to get around that; one string deleted it.

**Both refusals describe themselves as unfinished, and this is a thing to come back to.** One says
*"not yet implemented"* and the other is a `@HACK` with a ticket number on it. Neither is a decision
about what attributes should be; both are work the Verse toolchain has not done. When a future
engine drop lands either one, `@rpc("any_peer", "call_local", "reliable", 2)` becomes writable, and
it is the spelling to move to — it is GDScript's, and it would put the argument *count* and the
channel's *type* in the compiler's hands where the bridge checks them by hand today. Moving is
additive rather than breaking: the words already split on spaces or commas, so the one-string form
keeps working beside a tuple-reading one, and what changes is which shapes are *also* accepted. The
constraint is not `@rpc`'s — it governs every multi-argument attribute the bridge might want, which
is why stage 7's `@export_flags` takes its names as one comma-separated string for the same reason
rather than by coincidence.

**And it was silent.** The refusal is reported against the *script*, but the probe prints nothing
for it and `vh_compile_project` answers status 4 with zero diagnostics — which is what a whole
morning of this stage looked like before the same fixture was put through the integration layer,
where Godot's own diagnostic path printed it at once. `tests/verse_probe` cannot see this class of
error, and that is worth knowing about the probe rather than about `@rpc`: a *user* package may not
declare `class(attribute)` at all, so the probe cannot even be handed the attribute package to check
in isolation.

Two smaller things the stage settled:

- **The defaults belong in the host.** `@rpc("any_peer")` means three other things as well, and
  writing them in one place rather than in both halves of the bridge is the only way the two cannot
  drift. They are GDScript's own and `SceneRPCInterface::_parse_rpc_config`'s both.
- **A refused config is dropped, not half-applied.** An `@rpc` with a misspelled word is a method the
  author believes is remote-callable; registering what survived parsing would make that belief
  *nearly* true, which is worse than not at all. `_validate` says why at the method's line, which is
  the same bargain `vh_signal_desc`'s Reject makes.

**It needed a sidecar field, and the export layer is what said so.** Everything was green in the
editor and every `@rpc` case failed in an exported game, because `vh_class_rpc_list` reads the
analysis snapshot and a runtime host has no semantic program to have built one from. Version **6**.
That is the export layer catching a junction of two features that each had tests, for the fifth time
in this phase, and it is the same sentence §15 already wrote about stages 2 and 3.

**What the sending half can be asserted to do is less than it looks.** With no peer connected, the
editor-side driver and an exported game stop at *different* guards inside Godot — the driver's
SceneTree has no MultiplayerAPI, while a game's has one whose default offline peer reports itself
connected, so the call gets as far as being sent to nobody and answers OK. Asserting either number
would be asserting which of Godot's guards fired. What the case says instead is that the call left
Verse and came back as an Error ordinal, and R-EXP-9's cross-peer half is owed as a by-hand check.

---

### Stage 6's other six, which are not on an object at all

**§8 counts `Callable.call` and `Signal.emit` among the varargs and does not notice that they are a
different problem.** The other 27 are methods of a Godot *Object* and ride `VhCallValue`, which
takes a `vh_handle`. These six are methods of a builtin *type* — a Callable, a Signal — and none of
Godot's builtin types is an Object, so no spelling of a vararg would have reached them. They needed
a primitive: `VhRefCall` over a new `RefCall` callback (ABI **8.7**), which is `Variant::callp` on
whatever the reference table holds.

The reward is wider than the six: **every** method of an Array, a Dictionary, a Callable or a Signal
that the mirror does not wrap is now reachable, and an unknown name is Godot's own
`INVALID_METHOD` rather than a table of bindings to rewrite each release.

**None of the six has a zero-argument arity**, and that is the `GDScript.new` finding again rather
than a second one: a Verse function's parameters are its tuple, so `Call()` beside
`Call(:[]variant)` is one argument type rather than two arities. `C.Call(array{})` is what an empty
argument list is written as, and a case says so.

---

### One builder after all — `MakeVariant`, and the name it could not have

*Written after the section below, which concluded that a single builder was impossible. That
conclusion was right about **overloading** and wrong about the goal: a builder that takes `any` never
overloads, so neither reason applies to it.*

`MakeVariant<public>(Value:any)<decides><reads>:variant` is one function. The type dispatch it needs
happens in the host, and the machinery was already there — **the debugger had the identical problem
first**. A stopped frame carries no declaration either, so `ReadDebugValue` already answered "what
does this value say about itself", including the ordering trap that would otherwise have cost a day:
`true` is an option around `false`, so a Godot object must be recognised *before* anything reads the
cell as a logic. That cascade is now `GodotVerse::ReadSelfDescribingValue`, with the debugger and the
builder sharing one copy.

It reaches **int, float, logic, string, a Godot object and the 16 math structs**. The math structs
are the interesting half: nothing declares them, and they are found by *naming their own class*,
which keys the generated layout table — the same table, baked into the binary, that a cooked host
carries. It is `<decides>` because what it cannot reach is real: a tuple, a map, a class of the
author's own, and an array — which cannot say whether it is one of Godot's three integer packings,
and if empty cannot say anything at all. **That is the compile-time ambiguity of `array{}` arriving
at run time instead**, which is the same rule one layer down rather than a new problem. Failing
rather than answering `variant{}` is the whole point: an empty variant is a value someone may have
meant.

The six lanes that share a Verse type keep their named builder, and those names lost their `From`:
`VariantFromInt` is now `VariantInt`. Both families now read as what they are — `MakeVariant[X]` when
the value knows, `VariantStringName(S)` when only the author does.

**It is not called `Variant`, and that cost three builds to learn.** In this package a module-level
function may not differ from a *type* in the package only by case:

| in the mirror | result |
| --- | --- |
| function `Variant` beside the `variant` type | **whole package refused** |
| function `Node` beside the mirrored `node` class | **whole package refused** |
| function `VHFROMINT` beside function `VhFromInt` | fine |
| in a script package: `Widget` beside a `widget` struct, or `Variant`, or `Vector2` | fine |

So it is function-versus-*type*, in a VNI package only. And it fails the worst way available: VNI
accepts it, the host builds and links, and the **runtime** compiler then refuses the package with
`status 4` and **zero diagnostics** — the same silence as a refused attribute, and as a mistyped
path. `MakeVariant` also happens to be the name the API already uses for this shape: `MakeArray`,
`MakeDictionary`, `MakeCallable`, `MakeSignal`.

**The mechanism was then read out of the engine rather than left as a rule.** Verse's own naming is
case-*sensitive*: `CSymbolTable::FindOrAddInternal` compares bytes (`Symbol.cpp:88-122`), and so does
the native-thunk lookup `VNativeProcedure::SetThunk`, over a `VNameValueMap` defaulting to
`ESearchCase::CaseSensitive` (`VVMNativeProcedure.cpp:53-72`, `VVMNameValueMap.h:74-90`) — which is
exactly why function-versus-function is fine. What folds case is **`FName`, unconditionally**
("case-insensitive, but case-preserving", `NameTypes.h:629`; every `operator==` compares only
`ComparisonIndex`), and a Verse *type* in a VNI package becomes a UObject keyed by one —
`NewObject<UVerseStruct>(UEPackage, FName(UEName), ...)` (`VVMClass.cpp:1016-1047`). The type's
identity folds where the function's does not, and the two meet.

Two things follow that are worth more than the rule itself. **Epic hit this and used to report it**:
the legacy BPVM assembler's `FUObjectGenerator::FindOrCreateUObject`
(`VerseUObjectGenerator.inl:44-116`) does a case-insensitive package-scoped `FindObject` and raises
*"Found existing type '%s' that is already being created this compile. Please rename the %s to be
case insensitive unique."* through `AppendGlitch`. **And the silence is a regression rather than a
decision**: the VerseVM path has no equivalent check, and the adjacent bind failure is reported by
`UE_LOGF(..., Error, ...)` out of a `void` `TryBindVniType`/`TryBindVniModule` whose callers wrap it
in `ensure()` and discard the result (`VerseVMEngineEnvironment.cpp:111-125`), so nothing reaches
`uLang::Diagnostics` — the only thing the ABI's diagnostic callback listens to. That is why three
builds were needed to find a one-word problem, and it is worth reporting upstream.

One link is unclosed and is recorded as such: what the *function's* colliding registration is in the
current pipeline was not found, only that the collision happens. There is no escape hatch — the
`cpp_name` attribute overrides a **type**'s C++ identity (`DefinitionInfo.cpp:195-205`) and has no
function equivalent — so renaming is the fix rather than a workaround for a restriction one could
otherwise keep.

*A receiver spelling was measured and not built.* `(X:any).ToVariant<public>()<decides><reads>` works
as a one-line forward and reads as the exact mirror of `V.AsInt[]`. It is out because two names for
one operation costs a module-level name, and a module-level name makes every local of that spelling
*ambiguous* rather than shadowed.

---

### The variant API's two spellings, and the rule behind the one that was refused

*Written after the work, prompted by an author asking for both halves at once: move the readers onto
the value, and collapse the builders into one overloaded `AsVariant`. The first was free. The second
is the more interesting answer, because §15 above had already recorded a refusal and recorded it as a
mystery.*

**The readers moved, and cost nothing.** `AsInt[V]` is now `V.AsInt[]`, which is how Epic's own JSON
API reads and what an author reaches for first. It is an **extension method** rather than a method on
the struct, and it had to be: `variant` is hand-written in `Godot.native.verse`, the readers are
generated into `GodotClasses.native.verse`, and Verse cannot reopen a class. The part worth measuring
was whether `<decides>` survives the desugaring — `V.AsInt[]` is a failure-call of
`operator'.AsInt'(V, ())`, and a failure-call of a *sugared* call is a different question from a
failure-call of a plain one. It does.

Each lane therefore has two definitions, and the second is not redundancy. `typed_array` carries its
element reader in an `Unpack` **member** — a function *value*, 74 of them — and an extension method
has the wrong shape for one: receiver plus an argument tuple, not one variant. So the tag check stays
a plain `VhUnpack<GodotType>`, non-public, and the extension method forwards to it. Verse having no
anonymous functions is what makes that a rule rather than a preference.

**The builders did not move, and here is the rule that stops them.** §15 recorded that
`VariantFrom(:logic)` was ambiguous with `VariantFrom(:[]char)` "for no reason any reading of
overloads-on-parameter-type predicts", and concluded that module-level overloading was the problem.
That conclusion is wrong on its face and the evidence was already in the tree:
`GodotMath.native.verse` overloads `Abs` across **nine** receiver types and `Length` across seven.
So the pairs were run one at a time, in `tests/verse_probe/variant_api_probe.verse`:

| compiles | refused |
| --- | --- |
| int + float | logic + string |
| int + string | logic + `[]int` |
| int + logic | string + `[]int` |
| logic + vector2 | `[]int` + `[]float` |
| | logic + `?int` |
| | `?int` + `[]int` |

> **An overload set may hold at most one parameter from the emptiable family: `logic`, any option,
> and any array.** `string` is `[]char`, so it is in that family too.

Refused at the *definitions*; refused identically as class methods, which is the half of §15's bullet
that turns out to be false; and refused identically as extension methods on the receiver, so
`42.ToVariant()` is not an escape either. The likely mechanism is that `false` is both a `logic` value
and the empty option, and an option is a 0-or-1 array — one value inhabiting all three families is a
call site that resolves none of them, which is the same shape of argument as `array{}` having no
element type. **The mechanism is a conjecture; the ten measurements are not**, and the conjecture is
what turned two apparently unrelated refusals into one rule that predicted the other eight.

**And a second reason stands even if that one is ever fixed**, which is what makes this a decision
rather than a workaround: there are **38 lanes over 32 Verse types**. `string` carries String,
StringName and NodePath; `int` carries Int and RID; `[]int` carries all three integer packings;
`[]float` carries both float packings. An overloaded builder selects a lane *by argument type*, so it
can only ever name one per type — six lanes would have no spelling at all, and a StringName is not a
String to a Dictionary that was keyed with one.

What the first rule costs, concretely: `AsVariant(42)` and `AsVariant("hi")` could have coexisted,
but `AsVariant(true)` could not have joined them. The most common case reading differently from its
two neighbours is worse than a uniform name per lane, so the lane stays in the name — and the
ergonomic complaint the author actually had is answered by the receiver spelling instead, which costs
no overloading at all.

`VariantKind(V)` stayed a free function on purpose. As `V.Kind()` it would put `Kind` in module
scope, and a module-level name makes every local of that name *ambiguous* rather than shadowed —
`CLAUDE.md`'s recorded trap, and `Kind` is a name a script is likely to want.

---

### The loose arities, and the rule that turned out to govern all of them

*Written after an author tried the obvious thing and it did not work.*

Stage 6 emitted a vararg as two arities — the fixed prefix, and the prefix plus one `[]variant` —
and §8 had asked for a third that was dropped without comment:

    Rpc<public>(Method:string, Args:[]variant)<transacts>:void
    Rpc<public>(Method:string, A:variant)<transacts>:void

The second is the one an author reaches for, because `Call("test", VariantInt(1))` is what
`call("test", 1)` looks like in GDScript. Without it the answer is *"No overload of the function
`Call` matches the provided arguments (:[]char,:variant)"*, which helpfully names both overloads and
unhelpfully names neither of the ones that were wanted. Varargs now carry **four** loose arities
beside the array one; four because Godot's own signals, calls and RPCs almost never carry more, and
the array form is still there for the ones that do.

**And the rule behind it is one this phase had already found twice without recognising it.** A Verse
function's parameters **are** its tuple, so:

- `New()` beside `New(:[]variant)` is ambiguous — the empty tuple *is* the empty array. That was
  stage 6's finding, recorded below as being about `GDScript.new`.
- `Call(:variant, :variant)` beside `Call(:[]variant)` is ambiguous too — a tuple of two variants
  *is* an array of variants. Same rule, and the first one was a special case of it all along.
- `Call(:variant)` beside `Call(:[]variant)` is **fine**. One parameter is not a tuple.
- With any fixed parameter in front, every arity is fine: `(string, variant, variant)` and
  `(string, []variant)` differ in shape and in their first element.

So the number of loose arities an entry point may have is decided by whether it has a prefix: four
where it does, exactly one where it does not. `GDScript.new` and the six reference varargs in
`GodotApi.native.verse` are the ones that do not.

**The cost of not knowing this was a mirror that compiled and did not work.**
`tests/verse_probe/vararg_arity_probe.verse` was written first and asked only about the *prefixed*
shape, because that is the shape §8 spells — so it answered yes, four arities went out on all 21
entry points, **VNI accepted the whole mirror at build time**, and the runtime compiler then refused
it with 44 ambiguity errors. That is CLAUDE.md's "a host build passing is not enough to know a
`.verse` file compiles", arriving from the direction it warns about, and it cost a build cycle to
find because the probe reports a package-level refusal as `status 4, 0 error(s)` — silently, like
the `@rpc` overload before it. The integration layer printed the real message at once, again.

The probe now carries the unprefixed pair as a live case, and the illegal one as a comment beside
it: a refused declaration takes the whole file with it, so the legal half is what has to stay
runnable.

---

### Stage 6's varargs, and the one arity that cannot exist

**§8's two-line sketch is right about the shape and silent about the edge.** A vararg is emitted as
two *arities* of one name — the fixed prefix, and the prefix plus one `[]variant` tail — and
`tests/verse_probe/vararg_probe.verse` is where that was asked rather than assumed, because §15 had
just finished recording that a spike answers the question it was given and S-3 had been asked about
*types* rather than arities. Two arities of one class method resolve by parameter count; `+` is
Verse's array concatenation and is how the packed prefix and the already-packed tail become the one
array `VhCallValue` wants. Both measured in one probe run.

**What the probe did not cover is a vararg with no fixed prefix, and that pair cannot exist.**
`New()` beside `New(:[]variant)` is uLang glitch 3532, *"ambiguous with this definition"* — because
a Verse function's parameters **are** its tuple, so the empty tuple and the empty array are one
argument type rather than two arities. With any fixed parameter at all the pair is fine, which is
why the probe's `Fire(:string)` / `Fire(:string, :[]variant)` compiled and said nothing about this.
`GDScript.new` is the only entry point in 4.7 in that position, and what it leaves an author is
`Script.New(array{})`.

**The 15 class methods are what stage 6 emits, and the other 18 are not a to-do list.** §8 counts 33
and treats them as one set; they are three, and only one was ever blocked by the same thing:

- the **12 utilities** are not dispatchable by name at all. `variant_get_ptr_utility_function` hands
  back a *ptrcall* wanting a signature hash, so `api_call_utility` is a fixed table of C++ calls and
  a utility costs a line there whether it is vararg or not. Nine of the twelve already have a Verse
  spelling recorded against them — `Max`, `Min`, `Print` and the print family — and Verse's own
  string interpolation says the multi-argument case better than a tail would: `Print("{A} {B}")`.
- the **6 builtin methods** — `Callable.call`, `Signal.emit` and four more — are methods on a
  *reference*, not on an object handle, so `VhCallValue` cannot reach them however they are spelled.
  They are stage 6's other half and need a primitive of their own.

---

### Stage 5's other four, and the one thing they had to build first

**§7 got all three specifiers wrong on all four lines, and each is wrong in a way an author would
act on.** The listing there reads `_Get<public><native>(Property:string)<transacts>:?variant`. What
is written is:

    _Get<public>(Property:string):variant = variant{}
    _Set<public>(Property:string, Value:variant):logic = false
    _GetPropertyList<public>():godot_array = MakeArray()
    _ValidateProperty<public>(Property:dictionary):void = {}

- **Not `<native>`.** A `<native>` declaration is one the *host* implements, which is backwards for
  a hook the *script* implements and Godot calls. The shape is `_Notification`'s: an ordinary Verse
  method with an empty body, so that `<override>` has something to override.
- **Not `<transacts>`.** `_Notification` carries no specifier and these must not either, for the
  reason written next to R-AUD-2: an explicit effect narrows, and narrowing is contagious downward,
  so `<transacts>` on the root would force it on every helper an override calls, a file at a time.
  The measurement is in `tests/verse_probe/hooks_probe.verse` — a specifier-less helper called from
  `_Set` compiles, and would have been glitch 3512 at the call site under the §7 spelling.
- **Not `?variant`.** An option around a non-object has no representation on this wire and
  `HostScript.cpp` says so in as many words — `ValueToWire` reads a cleared option as a null
  reference, so `?int` arrives as nothing. `variant{}` is a *better* spelling anyway: a variant
  holding nothing is precisely what GDScript's `_get` returning nil means, so the Verse and the
  Godot conventions are the same one rather than two that have to agree.

**The premise under all four of them was untrue, and nothing had noticed because nothing had
asked.** §7 assumes a script can declare a `variant` parameter. It could not. Every `variant` in the
project was in a **native** declaration, marshalled by VNI's generated glue and by
`GodotBindings.cpp`; the ordinary script-call wire had never carried one, and the two ways it failed
were both silent about the cause. `DescribeType` classified it as a *user struct* — it is a struct
the layout table does not know — so a one-parameter method asked Godot for 22 arguments, one per
lane. Excluding it from that test without saying what it was instead dropped it into the *reference*
arm, where a `variant` argument was refused as a handle to a class Godot has never heard of. The
symptom of both was one line: *"Cannot convert argument 2 from int to Nil"*, which is also what an
unrepresentable type says.

**So the stage's real work is `variant` on the script-call wire, and the reward is general.** ABI
**8.6** adds `VH_TYPE_VARIANT` — a new enumerator, which the header's own policy makes a minor. It
is a **declaration** type and never a payload: a `vh_value` still carries whatever the variant
holds, and what the type tells the consumer is "describe this to Godot as accepting anything", which
Godot spells `Variant::NIL` with `PROPERTY_USAGE_NIL_IS_VARIANT`. The conversion itself is not new
and deliberately not rewritten: `GodotBindings.cpp` already had the lane rules for every native
call, and a second implementation of them is a second chance to disagree about which lane a Rect2
puts its height in — so the two directions are exported from there, and the VM half is
`FNativeConverter`'s, which is what VNI's own glue calls. **No sidecar change**, because a declared
type's number was already carried, and that is the one thing about this that went the easy way.

What it buys beyond the two hooks is that **any** script method may now take or answer a `variant`,
which is §3's façade finally reaching the wire §3 never checked it against.

**The cost of having the four hooks at all is zero, and that is the fact that made the shape
possible.** Declaring them on the native root means every script *inherits* them, which looked like
a VM entry on every property miss of every scripted node. It is not, because
`GetClassMethodsLive` reports a class's **own** declarations — an `<override>` is one and an
inherited empty body is not — so the consumer's `resolve` answers null and nothing is called. That
is the same mechanism keeping an unoverridden `_Notification` out of the notification path, and it
was already written down; what is new is depending on it.

**How they compose with `@export` needed no decision after all.** §7 calls it "the design question
inside this stage" and answers it with Godot's rule, and Godot's rule is what falls out of putting
the member lookup first in `set_func` and `get_func` — which is where it already was. The case that
asserts it is a hook and an export that both claim one name, with the hook counting its own calls:
the export wins both directions and the counter does not move.

Two things an author has to know, and neither is guessable from reading the native root. **A class
that does not derive from `object` has none of the four**, and the compiler says glitch 3523,
"could not find a parent function to override (perhaps the parent function's access specifiers are
too restrictive?)" — which blames the wrong thing entirely. And **Godot's `Variant::Type` numbers
are not a script's to write**: `TagInt` and its 38 siblings carry no access specifier, so a property
dictionary written the obvious way is glitch 3593 talking about control scopes. The public spelling
is the generated `ToInt(:variant_type)`, which is also the closer analogue of the `TYPE_INT` a
GDScript author writes. Both cost a probe round and nothing else, which is the argument for the
probe: `hooks_probe.verse` found four errors in one run, and three of them were mine rather than the
design's.

**`_ValidateProperty` reaches further than the name suggests.** Godot asks it of every **ClassDB**
property of the object, one at a time, from `ClassDB::get_property_list` with the object as the
validator — not only of the properties the script serves. So a Verse script can hide or re-hint
`Node2D`'s own `rotation`, which is what the case asserts, and which is also the only part of this
stage a headless run could observe changing something that was not the script's own.

---

### Stage 4, which cost nothing either

**§6 predicted this and was right: "Godot's rules decide this stage almost entirely."** They decide
all of it. `_create_autoload` gates on `get_instance_base_type()` being a Node, `VerseScript` has
answered `Node` for a `class(node)` since Phase 2, and stage 3's own `vh_class_base_type` had
already fixed the one place that answer went missing — an export, where the source it used to be
read from is a one-byte stub. So a Verse autoload worked in an exported game the first time it was
asked to, with **no source change at all**: a fixture, six cases and this paragraph.

**What §6 did not anticipate is where the cases could run.** It says "done when a Verse autoload
answers from every scene in a running game", and the editor-side test driver is not a running game:
`--script` replaces the main loop *before* Godot sets up any autoload, so `/root` has no children at
all there — measured, and not even the suite's own `VerseExportCheck` is present. The five singleton
cases are therefore the exported run's alone and are skips in the editor run, which is the
export-side skip discipline pointing the other way for the first time. The one thing that *is*
testable in both is the predicate the gate applies, on both sides of it.

**And "from every scene" is not something a single-scene run can assert**, so what is asserted
instead is the structural fact underneath it: the node is a child of the root, beside the current
scene rather than inside it, which is exactly what a scene change leaves alone.

Two halves stay by-hand and are in `by-hand-findings.md`: the editor's autoload dialog refusing a
non-Node class with its own sentence, and a `@tool` autoload being instantiated in the editor.
`is_editor_hint()` is false in every headless run, so neither is reachable from here.

---

### Stage 3, and the one defect it found that it did not own

**§5 cost nothing it predicted and found something it did not.** The stage §13 calls "most likely to
cost more than it looks" needed **no code**: a `Resource` with a Verse script attached reads and
writes its exported values, saves to `.tres`, loads back and answers its methods, in the editor run
and in an exported game both, against the bridge exactly as R-NODE-3 left it. Seventeen test cases
and no source change. §5's premise — "what has never been exercised is any of it against a
non-`Node` owner" — was right that nobody had looked and wrong that anything would break.

**Its named risk is parity, not a defect.** §5 worries that `_can_instantiate`'s `is_editor_hint()`
gate makes a non-`@tool` Verse resource "a placeholder in the only place it is ever used". GDScript
does exactly the same thing: `can_instantiate()` tests `ScriptServer::is_scripting_enabled()`, and
the editor sets that false (`editor_node.cpp:8476`). A placeholder is what the inspector edits for
every GDScript resource in every Godot project, `ResourceSaver` writes its stored values, and the
editor is not "the only place" a resource is used — a running game is. Nothing to change.

**What it did find is `get_instance_base_type()` answering *empty* in an exported game**, for every
Verse script and not only for a resource. The consumer reads the declared superclass out of the
source text, and an export ships every `.verse` as a one-byte stub, so there has never been a
superclass to read. Nothing in a shipped game had noticed, because the runtime callers are narrow —
a typed array of a script class in a serialised resource (`variant_parser.cpp`, `marshalls.cpp`,
`json.cpp`) and a custom `ResourceFormatLoader`/`Saver` written in a script (`resource_loader.cpp`,
`resource_saver.cpp`) — but nothing had asked either, in three phases of having an export.

It is pre-existing and it is not R-EXP-6's, and it was fixed anyway because the fix turned out to be
cheap: **ABI 8.4's `vh_class_base_type`**, which is the walk R-NODE-3 already mints a peer from,
asked of a class name rather than of an object. No sidecar change and no snapshot field, because a
runtime host has the class's `UClass` chain loaded — which is the same fact the R-NODE-3 cases
passing in the export run had already proved. The consumer asks it only where the text scan found no
class *and* the host has no compiler, so the editor's per-keystroke path never reaches a VM entry it
did not have before.

**And the thing §5 put out of scope was the thing that mattered.** §5 says a resource held as an
exported member of another "is worth a case but not worth blocking the stage". It was worth blocking
the stage: an `@export` typed as a `@global_class` Verse Resource **in a module** filtered its
inspector slot by the bridge's own module-qualified name, so the editor answered *"Cannot get class
'Gameplay/myResouce'"* the moment a slot of that type was drawn. Reported from somebody else's
project within minutes of stage 3 landing, and `by-hand-findings.md` B18 is the entry. One line in
`filter_class_from_hint`: take the **leaf** of the qualified name before PascalCasing it, because
ClassDB is one flat namespace and `@global_class` registers the file stem and nothing else.

B19 is the same report's second half, and it turned into research rather than a patch: `@global_class`
on a class that is not named after its file registers nothing either, so the slot named a class
ClassDB had never heard of by a second route. What GDScript does in the same position had to be
measured before anything could be called parity. **Now closed, in three stages** --
`property-export.md` §"A second class in one file" carries the measurements and the plan. The hint
falls back to the nearest mirrored Godot class (A1), the write is refused by class on both ABI paths
(A2), an inert `@global_class` is a warning rather than silence (B), and serialisation is answered as
C1: a class to be authored as a `.tres` lives in its own `.verse`, because what carries a value
across a save is *being a script* and only the file-named class can be one.

Nothing had caught it because no fixture was both things at once — module fixtures existed, resource
exports existed, and the combination did not. That is the general shape of what this phase kept
finding, and it is worth saying once: **every defect in stages 2 and 3 was at a junction of two
features that each had tests.** The double-mint, the defaults-object leak, the stripped-source base
type, and this.

The lesson is the export layer's, again: it is the only layer that runs what it built, and what it
catches is never the thing the stage was about. The lesson beside it is the by-hand session's, and
it arrived before the session did.

**Still true, and worth saying because §4 rests on all of it:** the block clause fires for a user
class across the package boundary, at two levels of derivation, with `Self` already at the derived
type; `helper{}` needs no new syntax; and R-TYPE-7 is not amended by any of it.
