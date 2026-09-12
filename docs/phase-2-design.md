# Phase 2 — the whole engine API

**Status:** Draft 4 · 2026-09-11 · Stage 0 measured and decided (3.1, 3.2); stages 1-7 open
**Companion to:** [`spec.md`](spec.md) (what must be true), [`roadmap.md`](roadmap.md) (why this phase is
here), [`abi-v2-design.md`](abi-v2-design.md) (the wire this builds on)

Draft 1 carried modules as well. They moved to Phase 3 — §7 says why — and what is left is one
subject: **the surface a script writes against, complete enough that a missing method is a decision
rather than an accident.** The phase touches no ABI.

---

## 0. Where the decisions came from

Sixteen forks were put to the author across four rounds and answered. The rest are marked *mine* and
are the ones to argue with first.

| | decision |
| --- | --- |
| scope | **modules and auto-import move to Phase 3**; `@GlobalScope` moves to Phase 4 with OQ-11 |
| class set | **all 1022** — measured (3.1), chosen (3.2): the build cost is nothing and the 5.4x analysis cost is a lag, where a subset is a wall |
| namespacing | flat. Submodules only once the editor writes `using` lines, which is Phase 3's |
| `variant` | the existing native struct made **nameable**, read through cast-shaped `AsInt[V]` free functions |
| `Object` | mirrored — all 46 concrete methods; the hand-written base is renamed `vh_object` |
| typed arrays | a parametric `typed_array(t)`, kept even if the downcast spike lands |
| downcasting | **spiked in Stage 0**: `is`/`as` parity via Verse's own failable cast |
| enums | real Verse `enum` types, bitfields included, combined with Verse's `BitOr`; enumerators strip Godot's prefix |
| enum `@export` | **pulled forward** from Phase 4 |
| library files | a Script that reports itself non-instantiable, as GDScript does |
| coverage | an editor diagnostic, no report file; plus a bar on what may be skipped (*bar: mine*) |
| language holes | fix if small, else record as a wall |
| the yardstick | Dodge the Creeps as a **second committed project**, beside `demo/` |
| also in scope | analysis-latency measurement |

---

## 1. What Verse actually offers — read before designing around it

Four facts, each read out of the compiler or the engine's own Verse sources rather than assumed.
They are recorded here because re-acquiring them costs an afternoon each.

**Verse has a failable dynamic cast, `type[Value]`** — the idiomatic spelling of a sum type, and of
GDScript's `is`/`as`. Its rules, from `SemanticAnalyzer.cpp:14522` (`AnalyzeInvokeType`):

- the target must be an **interface, class, `int` or `float`** — "Cast target must be an interface,
  class, int, or float";
- **structs are rejected on both sides** — as the cast target ("Cast target must be an interface or a
  class") and as the argument ("argument type must be a class"). So our sixteen math types can never
  participate in a cast, and neither can `variant` itself: `variant[X]` does not compile, whatever
  `X` is. Only the *infallible* form `variant(X)` skips the check, and it is an identity;
- `int[X]` and `float[X]` take an int or a float — they are *refinement* casts, for narrowing to a
  constrained type like `type{_X:int where _X > 0}`, **not conversions**. `int[SomeVariant]` is a
  compile error. A sum type holding a primitive would therefore have had to box it, which is why
  §4.1 reads a `variant` with free functions rather than with a class hierarchy;
- **casting to a parametric type is not supported** — "Dynamic casting to a parametric type is not
  yet supported". `typed_array(t)` can be built but never cast to;
- a fallible cast requires a failure context, which is why the idiom is always `if (X := t[V])`.

Seen in practice as `agent[Entity]`, `has_merge_rules[Component]` (interfaces cast too), and
`test_player_primitive_data_payload[Context.UserData]`.

**Verse overloads on parameter type.** The generated `dictionary` already carries
`GetInt(Key:string)`, `GetInt(Key:int)` and `GetInt(Key:vector2i)` side by side. Constructors like
`VariantFrom(42)` and `VariantFrom("hi")` are therefore one name, not two.

**Verse has `case`,** so enum↔int conversion is generated Verse and needs no C++.

**A `<native>` enum needs a hand-written C++ shadow** and an `@import_as`
(`NativeUnitTestLib.native.verse:7`). At 758 enums that is 758 shadows nobody should write — which
decides §4.3's implementation, not just its surface.

**Verse has bitwise intrinsics on `int`** — `BitOr`, `BitAnd`, `BitXor`, `BitNot`, registered as
binary ops in `SemanticProgram.cpp:1387` and availability-gated at Verse version 4200. A flag
combination therefore has a spelling, which is what lets bitfields be enums in §4.3.

---

## 2. The number that reframes R-SCN-1 and R-SCN-2

Full generation (`gen_verse_api.py --all`, measured) emits 1022 classes, 8166 methods and 3202
properties, and skips 8984 methods. The skip count reads like a catastrophe and is mostly
bookkeeping:

| skipped | count | reachable anyway? |
| --- | --- | --- |
| `superseded_by_property` | 5874 | **yes** — reachable *as* a property; that is the point |
| `property_*` (four reasons) | 748 | **yes** — a skipped property keeps its accessor methods (`gen_verse_api.py:770`); the loss is only the `set X.Y = …` spelling |
| `unsupported_type` | 823 | no — **the only bucket Phase 2 owns** |
| `virtual` | 1413 | no — R-NODE-7, Phase 4 |
| `static` | 114 | no — R-NODE-4, Phase 4 |
| `vararg` | 12 | no — no requirement covers these |

**2362 of 17150 method entries (13.8%) are unreachable by any spelling**, and the part this phase
owns is 823 — concentrated in four causes: `Variant` (231), the packed vector and colour arrays
(207), `typedarray::…` (~250), and bare `Object` (60). Closing them takes unreachability to roughly
9%, all of it Phase 4's or a written-down non-goal.

**So R-SCN-1 and R-SCN-2 are a type-table job, not a class-count job.** The class count is a
build-cost question wearing a reachability costume, and §3 treats it as one.

---

## 3. The class set is a measurement, and the rule comes first

Full generation is 2.9 MB / 24 423 lines of Verse (against 551 KB / 4 529 today) and a 985 KB
`verse_api_classes.h` (against 2 134 lines). Two costs, and they are not the same cost:

- **M1 — host build.** VNI compiles the package at UBT build time. Measure clean and incremental,
  curated versus `--all`, and **find where the time lands**: UBT/VNI code generation, or the runtime
  compiler reading the package's `.verse` sources — which it does, and which is why the host must
  load from `Engine/Binaries/Win64` at all.
- **M2 — per-keystroke analysis.** `RunCheck` calls `BuildAll` with `RequireComplete` on every
  keystroke, over every data source. If the mirror's sources are re-read per analysis, `--all`
  multiplies R-TOOL-2's "as you type" by six. If native packages are already external by then —
  which is what `IncrementalizeProjectSource` does for them (§14.1, OQ-8) — it may cost nothing.
  Nobody knows which, and it is the difference between a phase that works and one that quietly
  ruins the editor.

**The decision is made with the numbers in hand, not against a threshold picked blind** — there is no
baseline today, and a gate invented before one is a guess wearing a number. What is fixed in advance
is the *shape*: M1 and M2 are run curated and `--all`, all four results are written into this
document, and the choice is recorded here with its reason **before Stage 2 starts** — not at the end
of the phase, where it would be a rationalisation of whatever was already built. The fallback, if
`--all` loses, is the curated default plus a documented one-command regeneration, with R-SCN-2
reworded from "reachable" to "obtainable without writing C++".

M2 should dominate that judgement. A slow host build is paid occasionally by whoever builds it; a
slow keystroke is paid by every user on every character, and R-TOOL-2 promises "as you type".

**On precompiling rather than rebuilding** (the author's question): the real mechanism is Phase 0's
S-1 answer — cooked `.uasset` plus `WITH_VERSE_COMPILER=0` — and it needs the cooker, which is
**OQ-10 and Phase 7**. Not available here. What *is* available is finding out whether the mirror
package is already external per analysis, which is M1/M2's job and the cheap win if it is there.

### 3.1 Measured

`tests/host_bench`, built by `tools/build_bench.py`, on a 16-core Windows box against engine
`203d76492e`. It loads the host with no Godot behind it and times the three operations whose cost is
a function of the mirror's size. Not part of `run_tests.py`: it reports rather than asserts, because
R-PERF-2 asks for a recorded number and a threshold would fail on a slower machine than this one.

| | curated (538 KB, 60 classes) | `--all` (2825 KB, 1022 classes) | factor |
| --- | --- | --- | --- |
| **M1** VNI code generation, at UBT time | 0.25 s | 0.97 s | 3.9× |
| **M1** incremental host build, whole target | 13.4 s | 14.3 s | 1.07× |
| `vh_init` (engine boot, native packages) | 75 ms | 75 ms | 1.0× |
| `vh_compile_project` (the one generating build) | 421 ms | 2251 ms | 5.3× |
| **M2** `vh_check_project`, per keystroke | 158 ms | 850 ms | 5.4× |

**Where M1's time lands: nowhere.** The mirror is *ordinary* Verse — only the ~30 declarations in
`Godot.native.verse` are `<native>` — so VNI generates no C++ for it and the 2.8 MB never reaches
the C++ compiler. What grows is VNI's scan of the package, by 0.7 s, on a build that takes 14 s. The
question §3 asked ("UBT/VNI code generation, or the runtime compiler reading the package's sources")
is answered: **the runtime compiler, entirely.** M1 is not a consideration.

**So M2 is the whole decision, and it is 5.4× on the thing that matters.** Typing itself never
blocks: `check_buffer` queues, `_frame` starts the analysis, newest-buffer-wins, and a validate
answers from the previous result. What 850 ms buys is diagnostics roughly a second behind the caret —
and a stall of up to that long on completion, hover and save, because every host entry point that
reads the semantic program joins the in-flight analysis before it answers.

**The cheap win is not there, and finding out cost the editor.** `IncrementalizeProjectSource` marks
every package already compiled in this process `EPackageRole::External`, which is exactly what Phase
0's S-2 found and what §3 hoped was already happening. Called before the analysis `BuildAll` it
halves M2 — 850 → 485 ms full, 158 → 82 ms curated — and it breaks every editor feature that walks
the mirror's AST: **104 of `host_smoke`'s cases fail against 0 at HEAD**, including goto-definition,
completion and signature help, on the user's own script as well as the mirror. Externality is
sticky, so scoping the call to edited buffers does not recover them. That is the answer to **OQ-8**
for the analysis path: the mirror package is *not* already external, and making it external trades
R-TOOL-1/2/3 for latency. The latency fix is the expensive one that was already scheduled — cooked
digests, OQ-10 and Phase 7.

**There is no principled middle.** Godot's own `api_type` split removes only 79 classes and 668
methods — 8% — so "core only" is 790 ms rather than 158 ms. A hand-drawn middle is the curated list
again, with its failure mode intact.

### 3.2 Decided: the whole API, 1022 classes

`gen_verse_api.py` emits every class by default; `--classes-file` is how a subset is asked for, and
`tools/verse_api_classes.txt` stays in the repo as the documented one. The `--all` flag is gone —
the default *is* all.

The reason is that the two costs are not the same kind of cost. **A subset is not a setting, it is a
wall**: adding a class to the mirror means rebuilding `verse_host.dll`, which means a UE *source*
checkout and the Verse toolchain. For anyone but this host's own author that is not "one command",
and 1.0's bar is a Godot developer who is not the author not hitting a wall — which is the wall.
850 ms of diagnostic lag is a lag; "that class is not reachable from this build" is the end of the
evaluation. §3's fallback — the curated default plus a documented regeneration, R-SCN-2 reworded to
"obtainable without writing C++" — reads as a smaller concession than it is, because the C++ is not
what stops anyone: the 200 GB checkout is.

Recorded honestly: this is the first thing to revisit if the editor turns out to be unpleasant to
use, the number to revisit it against is in §3.1, and `tools/build_bench.py` is how to take it again.

---

## 4. Closing the type table

### 4.1 `variant` — the struct, named, with cast-shaped readers

**No wrapper and no hierarchy.** The reader is a `<decides>` free function invoked failably, which is
ordinary Verse — the generated mirror already calls `VhRefGet[Ref, Key]` this way — and it *reads*
as a cast without being one:

```verse
if (Health := AsInt[V]):
    Print("hp {Health}")

if (Pos := AsVector4[V]):
    Print("at {Pos.X}")

case (VariantKind(V)):              # branch first when the type is not known
    variant_kind.Int => Print("an int")
    _ => Print("something else")

Node.SetMeta("score", VariantFrom(42))     # overloads, per §1
```

Three things fall out, and each is why this beats the class hierarchy the earlier draft carried:

- **Nothing is boxed.** A hierarchy needs a class per lane, and a class instance allocated per read,
  to get an `int` into a castable type — §1 says a cast's argument must be a class, so a struct could
  never have participated. Free functions sidestep the restriction entirely and keep R-TYPE-6's
  no-allocation property, which was the whole point of the fixed-width struct.
- **Nothing is unwrapped.** No `.Value`, and what you hold afterwards is the `vector4` rather than a
  box containing one.
- **`variant` needs no new type.** The existing `variant<native> := struct` becomes nameable and
  nothing else changes: its lanes already carry no access specifier, so they stay module-scoped and
  a script still cannot build one the host would misread. R-TYPE-7's amendment is therefore narrow —
  the type becomes public, the plumbing does not. *Verify at implementation time that a struct with
  module-scoped fields can be named in a script's signature without its fields becoming reachable;
  the access default is assumed here, not confirmed.*

The cost, accepted: ~30 module-level names in `/Godot.org/Godot`, in scope in every script. The `As`
prefix is what makes that safe — the generator already records that Verse reports an *ambiguity*
rather than shadowing, so bare `Int`, `Color`, `Array` or `Object` at module scope would break a
user's own local of the same name at every mention.

### 4.2 Objects in containers, typed arrays, and the downcast spike

**A Godot container cannot carry an object today.** `godot_array` offers ten element accessors and
`dictionary` three key types; there is no `GetObject` and no `GetNode` anywhere in the generated
mirror (verified: zero occurrences). `GetChildren()`, `GetNodesInGroup()` and
`GetOverlappingBodies()` return containers whose elements a script cannot read. That is squarely
inside R-SCN-1's "the full node API is reachable", it is arguably the most-hit gap in the mirror
because walking children is what scene code *is*, and it is unstated in the spec.

Two mechanisms, and the phase builds both:

**`typed_array(t)`** — an ordinary parametric Verse class holding the reference id and its element
converters as function values:

```verse
typed_array(t) := class:
    Ref:godot_ref_id
    Unpack(:variant)<decides>:t      # module-scoped: a script may name a variant now, but must
                                     # not be able to hand us a converter of its own
```

Only the generated mirror constructs one, and every construction site knows its element type
statically, so the converter is a lambda written inline — **no module-level name per class and no
completion pollution.** `GetChildren()` returns `typed_array(node)`, built with a lambda that does
what generated method bodies already do. Verse supports parametric classes, including `<native>`
ones (`test_vni.native.verse:308`), so the type system is not in question; what needs proving is
that **a parametric class with function-valued members compiles under VNI and survives marshalling**
(spike **S-4**). If it does not, the fallback is a generated wrapper class per element type — uglier,
larger, certain.

**The downcast** (spike **S-5**) is the other half and the higher-leverage one. `if (Sprite :=
sprite2d[Child])` is Verse's own spelling of GDScript's `is`/`as`, it has no spelling today, and
that gap is unnamed in the spec. It works only if the Verse instance is of the derived class, and
today the mirror always builds the statically declared one. **The question:** can the host construct
the most-derived mirrored class for a handle, chosen at runtime from Godot's class name? Adjacent
machinery already exists — `vh_instantiate` finds and instantiates a top-level Verse class by name
and binds it to a handle — and the Godot class of a handle is reachable through the existing
`CallMethod` callback as `get_class`, so **no ABI change is needed either way.** What is unproven is
instantiating a class from the *native* `/Godot.org/Godot` package, which has no C++ shadow.

If S-5 lands, `is`/`as` parity arrives, and typed arrays stop being the *only* fix for objects in
containers — they stay as the typed one (R-TYPE-2), which is the author's call.

### 4.3 Enums

**Every Godot enum becomes a Verse `enum`, bitfields included.** The surface is **736 class enums
plus 22 global, 5380 enumerators**, of which 35 are bitfields. 96 bare names repeat across classes
(`Mode`, `Operator`, `Param`), so the type is class-qualified: `node_process_mode`.

A bitfield *value* can be a combination, which no enum value can hold, so a parameter typed
`bitfield::X` stays `int` and flags are combined explicitly — `BitOr(ToInt(A), ToInt(B))` where
GDScript writes `A | B`, using Verse's own int intrinsic (§1). That is wordier at one kind of call
site, and it buys one vocabulary across all 758 enums while keeping ~350 flag names out of module
scope, which is the pollution the `As` prefix was careful about in §4.1.

**Enumerator names strip the enum's own prefix and PascalCase the remainder** — `PROCESS_MODE_ALWAYS`
in `ProcessMode` becomes `node_process_mode.Always`, `KEY_SPACE` becomes `key.Space`. Godot's
prefixing is not consistent, so the rule carries a written fallback: strip only when the remainder is
a legal, non-empty identifier that is still unique within the enum, and otherwise PascalCase the name
whole. The generator asserts uniqueness rather than trusting the heuristic, so an inconsistent enum
fails generation instead of silently emitting two names that collide.

Conversion is generated **pure Verse** — a `ToInt` and a `<decides>` `FromInt` per enum, built on
`case` — because §1's native-enum finding makes the alternative 758 hand-written C++ shadows. The
wire keeps carrying an int; nothing about marshalling changes.

`SetProcessMode(node_process_mode.Always)` compiles, and `SetProcessMode(2)` stops compiling. That
is the R-AUD-1 win and also the phase's one deliberate break of existing scripts.

**`@export` of a Verse enum comes forward from Phase 4**: a dropdown in the inspector, which is one
hint type plus the enum↔int conversion that is landing anyway. It is the difference between enums
existing and enums being usable in a game.

### 4.4 `Object`, and what it drags in for free

Godot's `Object` is skipped entirely today because its API is `Variant`- and `Callable`-typed. Once
§4.1–§4.3 land, **every type it uses is supported**: 49 methods, no virtuals, only 3 vararg. Mirror
it, and several later requirements arrive as a side effect rather than as new work:

- **`callv(StringName, Array) -> Variant`** is a concrete method — that is **R-INT-2**, calling a
  GDScript-defined method dynamically, which the spec calls "the one with no existing path" and the
  roadmap puts in Phase 4;
- **`connect(StringName, Callable, int)`** is concrete — a piece of R-SIG-3 arrives early;
- `get`/`set`/`has_method`/`get_class` give dynamic property access and the class name a downcast
  wants anyway.

The three vararg methods (`call`, `call_deferred`, `emit_signal`) stay skipped; `callv` covers the
one that matters.

**The hand-written base is renamed `vh_object`, and the generated mirror takes the name `object`.**
Verse cannot reopen a class, so the 46 methods cannot be emitted into the hand-written
`Godot.native.verse` — and the alternative, a generated `godot_object` beside a native `object`, puts
two base names in completion where the one a user reaches for first is the empty one. The rename is
mechanical but touches several places that assume the name: `Godot.native.verse` and its C++ shadow
in `GodotClasses.h`, the generator's parent mapping, `verse_api_classes.h`'s fallback walk,
`_make_template`'s base-class choice, and the class-declaration scanner.

**This stage comes last of the four**, because `connect` returns `enum::Error` and `get_signal_list`
returns `typedarray::Dictionary` — mirror `Object` before §4.2 and §4.3 land and the generator skips
exactly the methods that made it worth mirroring.

### 4.5 The rest of the bucket

Bare `Object` returns (60) map to the native `object`. The packed vector and colour arrays (207) get
the reference lane and bulk converters that `MATH_PACKED_ARRAYS` already names. Multi-class union
parameters — `"BaseMaterial3D,ShaderMaterial"` and ~30 like it — map to their nearest common
ancestor. Typed dictionaries need nothing: this API version uses none.

### 4.6 Telling the user why a method is absent (R-SCN-2)

A diagnostic, no report file. The generator emits a reason table into `src/`; when a script names a
method Godot's `ClassDB` has and the mirror lacks, `_validate` says so with the reason — "`Node.foo`
exists in Godot but is not mirrored: unsupported type `Variant`". The user meets the explanation
where they hit the problem, which is the only place it prevents the failure mode R-SCN-2 exists for.

**R-SCN-2 also needs a bar, because as written it is unfalsifiable** — 8984 recorded reasons satisfy
it. *Mine:* the permitted skip categories are named exhaustively — `virtual`, `static`, `vararg` —
and **any other skip is a defect that fails the build**. That makes the requirement testable and
makes a new `unsupported_type` after a Godot version bump impossible to miss.

---

## 5. Library files (R-LANG-6's third clause)

A `.verse` that defines no class named after itself stops being invalid and becomes a Script that
reports `_can_instantiate() == false` with an empty base type, so Godot refuses to attach it and says
why. This matches GDScript, where a `.gd` extending nothing is still a Script and still cannot go on
a node. `_get_global_class_name` answers nothing for such a file.

This is the one piece of R-LANG-6 that stays in Phase 2, because it does not need modules: in a
single flat scope, same-module files already see each other, so `helpers.verse` full of module-level
functions works the day the resource loader stops calling it broken. It is most of what makes one
flat scope livable while modules wait.

---

## 6. The ABI

**Unchanged. `VH_ABI_VERSION` stays 2.0.** Everything in this phase is generator output and
GDExtension-side code; the lanes already carry what the new types need, the downcast reaches
`get_class` through the existing `CallMethod` callback, and `@GlobalScope` — the one item that *did*
need a new entry point, because a free function has no handle to ride `VhCallValue` on — left the
phase with OQ-11.

---

## 7. What moved out, and why

**Modules and auto-import → Phase 3.** Phase 3 already rewrites the machinery modules would be built
on: the host owning its script package instead of borrowing the IDE's, an `ISourceSnippet` with
settable text, and files added, renamed and deleted live. A module path is a per-file attribute of
that same source set, and "a file moved to a new directory" and "a file was renamed while the editor
ran" are one problem. Building modules here means writing that code against the borrowed-package
arrangement and rewriting it weeks later.

Three things follow. Auto-import has nearly nothing to do without modules — with one flat user scope
the only `using` is `/Godot.org/Godot`, which the template already writes — so it defers for free
rather than as a second decision. The name-stability question (a reload must not change any name a
user has written) becomes a design constraint inside one phase instead of a cross-phase trap. And
Phase 2 stops needing an ABI break at all.

The risk is small because **S-3 already prototyped it**: two same-named classes in two directories
compiled and ran, and a third file read a member off each. What is deferred is construction, not
discovery.

**`@GlobalScope` (R-SCN-3) → Phase 4, with OQ-11.** A free function has no handle, so it needs the
same by-name dispatch the deferred value-type methods need. Deciding them together costs one entry
point instead of two. The author's naming decision stands and travels with it: **Verse's own stdlib
wins** — of 114 utility functions, the ~86 that collide with `/Verse.org/Simulation` are spelled the
Verse way (`Lerp`, `Abs`, `Sqrt`) and only the ~28 with no counterpart are mirrored, per R-AUD-2.

**Value-type methods and operators → OQ-11.** The 16 math types stay data-only. Reaching `Length`,
`Normalized` and arithmetic means by-type dispatch against `extension_api.json`'s 38 builtin classes
— 998 methods and 749 operators. One cheap thing worth doing whenever it is picked up, because its
answer constrains the design: **check whether `operator'+'` can be defined at all** from an
`InternalUser`-scope package. `operator'()'` turned out to be a reserved intrinsic nothing may
define, and if `+` is the same, the surface is `A.Add(B)`.

**Not joining:** the Godot version check (R-QUAL-7) and the README rewrite, both declined. Worth
restating that the full mirror is what makes R-QUAL-7 urgent — at 1022 classes a version skew becomes
"method not found" in a player's running game rather than a diagnostic — so if not here, first in
Phase 3.

---

## 8. Spec edits this implies

Per the standing rule, each lands in the commit that makes it true.

| requirement | edit |
| --- | --- |
| **R-SCN-2** | name the permitted skip categories; require the editor diagnostic; any other skip is a defect |
| **R-SCN-5** *(new)* | Godot's per-class enums are reachable as named values, not integers |
| **R-SCN-6** *(new)* | a script can ask whether an object is of a given Godot class and use it as that class — `is`/`as` parity. Status set by S-5 |
| **R-TYPE-2** | typed containers gain their Verse spelling; status moves off **part** |
| **R-TYPE-7** | narrowly amended: the `variant` *type* becomes nameable so it can appear in a signature; its lanes, and every handle, stay unreachable |
| **R-TYPE-1** | record that containers could not carry objects before this phase, and now can |
| **R-LANG-6** | the library-file clause is met; the rest is Phase 3's |
| **R-EXP-1** | enum export lands early; the rest of the surface stays Phase 4's |
| **R-INT-2** | delivered by `Object.callv` if §4.4 lands as expected — verify rather than assume |
| **§14 / OQ-11** *(new)* | how do free functions and value-type methods cross, given no handle? Carries R-SCN-3 |

---

## 9. Work order

**Stage 0 — measure and spike. Nothing ships.**
M1 (host build cost, and where it lands), M2 (per-keystroke analysis latency), S-4 (`typed_array(t)`
with function-valued members through VNI), S-5 (runtime construction of the most-derived mirrored
class). Plus one sanity check that costs a minute: **confirm `godot-cpp`'s `extension_api.json`
matches the Godot binary the integration layer runs.** The generated header says v4.6.stable and
notes elsewhere in the tree reference 4.7; at 1022 classes a skew stops being a curiosity and becomes
a pile of runtime "method not found", and R-QUAL-7's detect-and-report was declined for this phase.
**Gate:** §3 — both measurements written down, and the `--all`-or-curated choice recorded with its
reason, before Stage 2.

**Stage 1 — library files** (§5). The smallest piece, independent of everything else, and it makes
one flat scope livable while modules wait.

**Stage 2 — the type table.** `variant` first (§4.1), because typed arrays are built on it; then
typed arrays and objects in containers (§4.2); then bare `Object` returns, the packed vector and
colour arrays, and union parameters (§4.5).

**Stage 3 — enums** (§4.3), including `@export` and the bitfield treatment.

**Stage 4 — `Object`** (§4.4), including the `vh_object` rename. **After** Stages 2 and 3, not
before: `connect` returns `enum::Error` and `get_signal_list` returns `typedarray::Dictionary`, so
mirroring it early skips exactly the methods that made it worth mirroring. Verify R-INT-2 here
rather than assuming it.

**Stage 5 — the coverage diagnostic** (§4.6). Reason table into `src/`, `_validate` consults
`ClassDB`.

**Stage 6 — R-LANG-1/2/3.** Script-to-script inheritance, interfaces, structs, enums and parametric
types get tests. A hole that is an afternoon gets fixed; anything larger becomes a recorded wall.

**Stage 7 — Dodge the Creeps.** The port as a **second committed project** beside `demo/`, committed
even while half-broken so progress across phases is a diff, and not a gate for `run_tests`.
`docs/dodge-the-creeps.md` is the wall list, each wall mapped to a requirement.

---

## 10. Exit criteria

Each is a test that would fail if it regressed (R-QUAL-2), not a judgement at the end of a long
phase.

- **`GetChildren()` yields elements a script can call methods on.** The one-line statement of §4.2.
- A `.verse` with no class of its own compiles, is usable from another file, and cannot be attached
  to a node — Godot says why rather than showing a broken script.
- A method taking or returning a `Variant` is callable, its value reads back through `AsInt[V]` and
  friends, and a script still cannot reach a lane or build a `variant` by hand.
- `SetProcessMode(node_process_mode.Always)` compiles, `SetProcessMode(2)` does not, and an
  `@export`-ed enum is a dropdown in the inspector.
- A method Godot has and the mirror lacks produces a diagnostic naming the reason.
- The coverage report's `unsupported_type` count is **zero**, or every remaining entry is named in
  the spec as permitted.
- Per-keystroke analysis latency, before and after the mirror change, is a recorded number in the
  repo — not a target, per R-PERF-2.
- If S-5 landed: a `node` known to be a `sprite2d` casts, and one that is not fails rather than
  raises.
- `demo/` runs. The Dodge the Creeps attempt is committed, and its walls are recorded one by one.
