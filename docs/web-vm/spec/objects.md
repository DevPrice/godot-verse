# Classes, archetypes and objects

Status: reviewed by the lead 2026-09-24. Room: dirty. Sources read: `Engine/Source/Runtime/CoreUObject/
{Public,Private}/VerseVM/` — `VVMClass.h`, `VVMClass.cpp`, `Inline/VVMClassInline.h`,
`VVMNamedType.h`, `VVMObject.cpp`, `Inline/VVMObjectInline.h`, `VVMValueObject.cpp`,
`Inline/VVMValueObjectInline.h`, `Inline/VVMNativeConstructorWrapperInline.h`, `VVMVerseClass.cpp`
(instance initialisation and native field loads), `VVMAccessor.h`, `VVMAccessor.cpp`,
`VVMUnion.h/.cpp`, `VVMUnionVariant.h/.cpp`, `VVMUnionVariantTag.h/.cpp`, `VVMEnumeration.h`,
`VVMEnumerator.h`, `VVMIntType.cpp`, `VVMFloatType.cpp`, `VVMNativeRef.cpp`, `VVMNativeConverter.h`,
`VVMValue.h`, `VVMCVars.cpp`, and the object, native-module, union, cast and accessor ops of
`VVMInterpreter.cpp`; `Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/VVMCodeGenerator.cpp`
(classes, class bodies, archetype instantiation, constructor functions, unions, casts, `set`) and
`VVMAssembler.cpp` (the accessor enumerator); `VerseCompiler/.../SemanticAnalyzer.cpp` (the union
gate) and `SemanticFunction.cpp` (decorated names). This repo: `host/Verse/Godot.native.verse`,
excerpts of `GodotClasses.native.verse` and `GodotApi.native.verse`, `host/Private/HostScript.cpp`
(`Instantiate`, `AdoptOrMintPeer`), `host/Private/GodotBindings.cpp` (`VhRefNewDefault`),
`docs/web-vm/format.md`, `ops.json`, `spec/calls.md`. Probes: `tests/verse_probe/vm_objects_probe.verse`,
`vm_objects_reject.verse`, `vm_objects_global_probe.verse`, `vm_objects_wideint_probe.verse`; the
existing `class_block_probe.verse`, `class_block_self_probe.verse` and `ref_block_probe.verse`
cited where they already answer.

Everything here was measured against engine commit `203d764` with the process-wide setting the
reference VM calls "UObject leniency" **off**, which is its default and what this project's hosts
run with (nothing in `host/` changes it). Several facts below — where block clauses live, what the
`BlocksDest` of `NewClass` receives — would read differently with it on; that mode is out of scope.

Probe citations read `(fixture method)`; `vm_objects_probe.verse` is abbreviated `objects`. A claim
read in source and not run is marked *(source, unprobed)*.

## 1. Vocabulary

| Term | Meaning |
| --- | --- |
| **class** | a `class` cell (§2). Covers Verse `class`, `struct` and `interface`, told apart by its kind |
| **archetype** | an `archetype` cell (§3): an ordered list of **entries**, each naming one field, plus a link to the next archetype in a chain |
| **entry** | one field description inside an archetype: name, access, type, value-or-uninitialized, flags |
| **object** | a runtime instance: a class instance or a struct value. Created by `NewObject` |
| **layout** | for one object: the set of its field names, each classified as a **slot** (stored in the object) or a **constant** (shared, taken from an entry's value). §4 |
| **created** | per slot, whether some initializer has claimed it. A slot starts uncreated; `CreateField` claims it (§7.4) |
| **constructor** | a class's body procedure, run with the object as `Self` to initialise the fields the class declares (§7.2) |
| **blocks function** | a class's second procedure, holding its `block:` clauses (§7.9) |
| **construction token** | the value threaded through every initializer of one construction (§7.1) |
| **native-represented** | a class whose objects the reference VM stores as engine objects rather than VM cells. §9 says what of this is observable |

Field names are interned names (`format.md` §2.2), compared by identity, and in a compiled program
every field name is the **decorated name of the declaration that introduced it** — the base
declaration, not an override. A data member reads `(<scope path of the declaring class>:)<Name>`,
for example `(/user@localhost/obj_point:)X` and `(/user@localhost/glob_var:)N` (both printed by the
compiler and the runtime in `vm_objects_reject.verse` R6 and `vm_objects_global_probe.verse`). A
method's name additionally carries its parameter types, so overloads have distinct names *(source,
unprobed)*. Because an override reuses the name of what it overrides, a name lookup on any object of
the hierarchy finds whichever entry the layout rule picked (§4). The interpreter never parses these
names; it only compares them.

## 2. Classes

### 2.1 What a class cell holds

| Field | What it is | Used at run time for |
| --- | --- | --- |
| kind | `Class`, `Struct` or `Interface` (`ops.json` enum `ClassKind`, values 0, 1, 2) | struct value semantics (§10), whether a blocks function exists (§7.9), casts (§13) |
| flags | 16-bit set, `ops.json` enum `ClassFlags` (§2.2) | §2.2 |
| package | the `package` cell the class belongs to | diagnostics and naming only |
| relative path, base name | two strings: the scope path inside the package, and the unqualified name | diagnostics and naming only |
| attributes, attribute indices | a flat array of attribute values, and an array of integer start indices, one per group plus a final end index. Group 0 is the class itself; group *1+i* belongs to archetype entry *i*. Both are absent when the class and all its members have none | nothing the VM itself does (§8.3); only attributes whose class carries the custom-attribute-handler marker are compiled at all, and none of this project's are (`@export`, `@export_signal` and the rest are plain `class(attribute)`s, which the compiler does not emit) |
| native type | an engine type the class reflects (`@import_as`) | nothing; must be absent in every program this project compiles (§8.4) |
| native bound | boolean: the class is declared `<native>` and its natives bind to C++ | §9 |
| inherited | the direct superclass first, **only if** the class has one and it is not a built-in class; then each directly inherited interface, in declaration order | layout (§4), construction (§7.2), casts (§13) |
| archetype | the class's own **class-body archetype** (§3.2) | layout, construction, `(super:)` |
| constructor | a `function` cell: the class-body procedure, unbound (no receiver), parent scope = the class scope | §7.2 |
| blocks | a `function` cell of the same shape over the blocks procedure. **Present exactly when kind is `Class`**; absent for structs and interfaces | §7.9 |

The **class scope** is a `scope` cell with no parent and exactly one capture: the class's own
archetype. The constructor, the blocks function and every method entry of that archetype share it as
their parent scope (`calls.md` §7.4).

### 2.2 The flags, and which ones do anything

| Bit | Name | Set by the compiler when | Runtime effect in the VM |
| --- | --- | --- | --- |
| 1 | `NativeRepresentation` | the class is native-bound, **or** any inherited class has it, **or** any entry of its own archetype has the entry flag `NativeRepresentation` or `Predicts`, **or** the class has `Predicts` | chooses the object representation and the layout rule (§4.3, §9) |
| 2 | `NativeStructWithObjectReferences` | a native-represented struct holds engine-object references (computed while binding, §8.3) | none (garbage-collector bookkeeping in the reference VM) |
| 4 | `Concrete` | `<concrete>` | none |
| 8 | `FinalSuper` | `@final_super` | none |
| 16 | `ExplicitlyCastable` | `<castable>` | none — casts do not consult it (§13) |
| 32 | `Parametric` | the class is an instance of a parametric type | none |
| 64 | `UniversallyAccessible` | public and in a public package | none |
| 128 | `EpicInternal` | epic-internal, or its package is not public | none |
| 256 | `Predicts` | any inherited class or own entry has `Predicts` | forces `NativeRepresentation` |
| 512 | `PersonaConstructible` | persona-constructible | none (JSON only) |
| 1024 | `CustomAttribute` | the class is itself a custom attribute | none that is observable here |
| 2048 | `Unique` | `<unique>` | equality: two distinct objects of the class are unequal (§12) |
| 4096 | `EmulateCaseInsensitiveOverrides` | the package predates a Fortnite version gate. Script packages compile at the latest version, so it is expected clear for them | layout: §4.4 |
| 16384 | `Deprecated` | `@deprecated` | none |

A **class cell in the file already carries its final flags**, derived bits included. The `Flags`
operand of a `NewClass` op does not: the interpreter derives the first row's and `Predicts`' bits
itself when it executes `NewClass` (§8.1).

## 3. Archetypes and entries

### 3.1 An archetype

| Field | What it is |
| --- | --- |
| class | the class this archetype initialises, or **none** for an instantiation-expression archetype (§3.2) |
| next | the next archetype in the chain (§4.1), or none |
| entries | ordered list of entries |

### 3.2 Three roles

| Role | Where it comes from | class | next | entries |
| --- | --- | --- | --- | --- |
| **class body** | the class cell's `archetype` field | the class | the superclass's class-body archetype if the first inherited is a class; otherwise none | one per data member and per implemented method, in source order (§3.3) |
| **constructor function** | a package definition for each `<constructor>` function, and the `Archetype` operand of the `NewObject` in its wrapper (§7.3) | the class it constructs | the archetype of the constructor it delegates to, or, if it delegates to none, the class's class-body archetype | one per field it initialises, in source order |
| **instantiation expression** | the constant `Archetype` operand of the `NewObject` compiled for `c{...}` (§7.1) | none | the archetype of the constructor it delegates to, if any; otherwise none | one per field it initialises, in source order |

A class body archetype **is** the class's own archetype exactly when `archetype.class.archetype` is
itself; that identity is how a chain tells a class body from a constructor function (§4.1).

### 3.3 An entry

| Field | Values it takes |
| --- | --- |
| name | an interned name (§1) |
| access | an `access specifier` cell, or none for public. Nothing in the VM consults it |
| type | the declared type as a type value, or uninitialized. A `var` member's type is a **pointer type** over the value type. Method entries and instantiation-expression entries have none |
| value | see below |
| flags | 8 bits, below |

What **value** holds decides almost everything:

| Value | Kind of entry | Appears in |
| --- | --- | --- |
| uninitialized | a data member some initializer will fill: one with no default, one whose default is computed by code, and every entry of a constructor-function or instantiation-expression archetype | all three roles |
| a constant (int, float, char, string, `false`/`true`, another cell…) | a data member whose default the compiler reduced to a value at compile time — a literal, or a reference to another definition | class body |
| a `function` with an uninitialized receiver | a method: its procedure (or native procedure) wrapped with the class scope as parent | class body |
| an accessor cell (§16) | a data member declared with `<getter>`/`<setter>` | class body |

A data member with a constant value **also** has an initializer in the constructor (§7.2); the
constant is what makes it cheap, not what makes it initialised.

Entry flags, bit for bit as the engine numbers them:

| Bit | Name | Meaning | VM effect |
| --- | --- | --- | --- |
| 1 | `NativeRepresentation` | the member is declared `<native>` (never set on an accessor member) | the class becomes native-represented (§2.2); the member is stored as a native field (§9.2) |
| 2 | `HasDefaultValueExpression` | the member has a default | none observable (§8.4) |
| 4 | `Instanced` | engine instancing semantics | none |
| 8 | `UseCRCName` | an interface data member's engine property name | none |
| 16 | `Predicts` | `<predicts>` | forces `NativeRepresentation` on the class |
| 32 | `Var` | the member is a `var` | none; `var` behaviour comes from the ops the compiler emits (§7.6) |
| 64 | `AccessibleFromEngineGameplay` | public | none |
| 128 | `PredictsExtern` | `@predicts_extern` | none |

`format.md` §6 currently gives entry flags as "bit 0 `var`, bit 1 native representation"; §17
recommends writing the engine's bits verbatim instead.

## 4. The layout rule

Every object has a layout fixed when it is created. It decides, for each field name, whether reads
come from a per-object slot or from a shared constant, and it is where overriding happens.

### 4.1 The chain, and the order it is visited in

A layout is built by visiting archetypes in a fixed order and reading their entries in entry order.
For `NewObject` with archetype *A* and class *C* (and for the class alone, with *A* = *C*'s class-body
archetype), visit *A* as belonging to class *C* with continuation *N*, where *N* is *A*'s `next` if
it has one and otherwise *C*'s class-body archetype (when *A* is not itself that). Visiting an
archetype *X* belonging to class *K* with continuation *N* means, in order:

1. Read *X*'s entries.
2. If there is a continuation *N*: when *N* is a **constructor-function archetype of a different
   class** than *K* (a delegation to a base class's constructor), visit *K*'s class-body archetype
   next — whose own superclass continuation is replaced by *N* — so the subclass body comes before
   the delegated base constructor. Otherwise visit *N*, belonging to *N*'s class, with *N*'s own
   `next` (for a class body: its superclass's body) as continuation.
3. If *X* is *K*'s class-body archetype, then after everything step 2 visited, visit each interface
   *K* inherits directly, in order, each as its own class body (an interface's continuation is its
   own inherited interfaces, visited the same way).

So for an ordinary class the order is: instantiation expression, the class's own body, its
superclass's body and so on up the chain, then — on the way back down — each level's interfaces
after everything above that level. An interface reachable twice is visited twice; the second visit
changes nothing (§4.2).

A delegating constructor names the class's immediate superclass's constructor in every program the
compiler accepts *(source, unprobed)*; nothing here is defined for any other shape.

### 4.2 First entry seen wins

For each entry in visit order, if its name has not been seen: an entry whose value is uninitialized
makes the name a **slot**; any other entry makes it a **constant** with that value. If the name has
been seen, the entry is ignored — with the one exception of §4.4's accessor rule.

That single rule is overriding:

- A subclass's `X<override>:int = 2` is visited before the base's `X:int = 1`, so `X` is the
  constant 2 for every `obj_derived` (`objects O1`: `derived.X=2`, `base.X=1`).
- `c{X := 7}` puts an uninitialized entry for `X` **first**, so `X` becomes a slot, and the slot's
  initializer runs before any class body could create the field (§7.4): an archetype-supplied field
  wins over an overridden default (`objects O1`: `derived(X:=7).X=7`). The same mechanism is why
  `godot_array{Ref := R}` never calls the `VhRefNewDefault` that `godot_array`'s
  `Ref<override>:int = VhRefNewDefault(TagArray)` would otherwise run: `Ref`'s class-body initializer
  finds the field already created and jumps over the call (`ref_block_probe.verse`).
- A base-class **constructor function** reached by delegation is visited *after* the subclass body
  (§4.1 step 2), so a field the subclass overrides with a constant cannot be changed by the base
  constructor: `MakeDerived(4)` delegating to `MakeBase(40)`, which says `X := N`, yields `X=2`, the
  subclass's override (`objects O10`).
- A method override works the same way: the subclass's entry for the base method's name is seen
  first, and every load of that name returns it (`objects O3`).

### 4.3 Slots versus constants, by representation

| Class is | A name becomes a slot when | And a constant when |
| --- | --- | --- |
| not native-represented | the first entry seen for it has an uninitialized value | the first entry seen for it has a value (a data constant, a method, an accessor) |
| native-represented | the first entry seen for it is a **data member**, whatever its value — a constant default is still per-object storage | the first entry seen is a method or an accessor |

For a native-represented class the layout is **per class**, computed from the class-body chain alone;
an instantiation expression's entries never add or change a name, because every data member is
already a slot. For any other class the layout is **per instantiation**: two objects of one class
may have different slot sets (`obj_point{}` has `X` as a constant 0, `obj_point{X := 0}` has `X` as a
slot). Nothing a program can observe depends on which one an object got (§10.2).

The observable consequences of the two rules agree for every program the compiler accepts: in both,
the first initializer to run for a name decides its value (§7.4). (Reasoned from §7.2–7.4; the
probes above ran on non-native classes, and O10's result is what the reasoning predicts for a
native one.) An implementation may use the
native rule for every class if it prefers; it must not use the non-native rule for a
native-represented class, whose constant-default members are written by initializers and read back
as per-object values.

### 4.4 Two exceptions

- **Accessor entries replace.** In a non-native layout, an accessor entry makes its name a constant
  holding the accessor **even if the name was already seen** — an instantiation expression that sets
  an accessor-backed member contributes an ordinary uninitialized entry for it first, and that entry
  must not turn the member into a slot.
- **Case-insensitive method overrides** (only when the class has flag 4096): a method entry whose
  name equals, ignoring case, the name of a method entry seen earlier in the walk is given that
  earlier entry's function as its value. Expected never to apply to this project's classes *(source,
  unprobed)*.

## 5. Methods

A method is a constant entry holding an unbound `function`. `calls.md` §6 owns what loading one does
(it is bound to the loaded-from object; a function field that already has a receiver is returned
unchanged) and §7.4 owns `LoadFieldFromSuper`, whose "archetype chain" is §4.1's visit order starting
at the defining class's body. Method values keep their receiver (`objects O4`), and dispatch is
decided at the load by the object's layout (`objects O3`).

Structs have no methods, so no method entry is ever loaded from a struct *(source, unprobed)*.

## 6. Loading a field: `LoadField`

`LoadField` (`Dest`, `Object`, `Name`) waits until `Object` is concrete (parks otherwise, `unification.md`)
and then, by what `Object` is:

| `Object` is | Result unified into `Dest` |
| --- | --- |
| a class instance or struct value, name is a **slot** | the slot's content. For a `var` member that is the member's **variable** (reference) cell, not its value; reading or setting the value is a separate op (`values.md`). A slot not yet written reads as an unbound placeholder that the member's initialization later binds (§7.10) |
| same, name is a **constant** holding an unbound function | the function bound to `Object` (`calls.md` §6) |
| same, constant holding an accessor | an **accessor reference** pairing `Object` with the accessor (§16) |
| same, any other constant | the constant |
| an object still under construction that is native-represented (between `NewObject` and `UnwrapNativeConstructorWrapper`, §7.8) | for a slot not yet created, an unbound placeholder that the member's initialization will bind; for a created slot, its content as above; for a method, the function bound to a placeholder that stands for the finished object and is bound to it at `UnifyNativeObject` |
| an accessor reference (a deeper path such as `set T.A[0].B = …`) | a new accessor reference that appends `Name`'s unqualified form to the path (§16) |
| a native reference to a native struct (a `var` holding a `variant`) | that struct's field; a reference that no longer resolves raises `ErrRuntime_InvalidRef` *(source, unprobed)* |
| anything else | not produced by the compiler; the reference VM aborts the process. The interpreter treats it as a malformed program (fatal, naming the op and line) |

A name the object's layout does not contain is never emitted by the compiler, and the reference VM
does not check for it. The interpreter should treat it as a malformed program, not as a Verse
runtime error or a failure.

For a native-represented object, a slot whose native storage holds no value (only an engine object
reference can) raises `ErrRuntime_NativeInternal` with the text `Uninitialized value reading field
'<name>' of type '<type>' on '<path>'.` in the reference VM, where the three substitutions are
engine names *(source, unprobed)*. No Verse-reachable path to it was found; the interpreter need not
reproduce the substitutions.

## 7. Construction

### 7.1 Instantiating an archetype: `c{F := v, …}`

The compiler turns an archetype instantiation into this sequence, and nothing else constructs an
object from Verse:

1. `NewObject` (`Dest`, `Archetype`, `Class`) — `Archetype` is the instantiation-expression archetype
   (a constant) and `Class` the class value. Both must be concrete, and so must the archetype's `next`
   (park otherwise). It makes a new object of `Class` whose layout is §4's, with **no slot created**,
   and unifies it into `Dest`. A struct class yields a struct value. A native-represented class
   yields the object wrapped as "under construction" (§7.8).
2. The **construction token** register is set to the marker value: the integer 12 774 014
   (hex `C2EA7E`). It is an ordinary integer constant in the constant pool; the VM treats it as "no
   deferred setters yet" (§7.6).
3. For each argument of the expression, in source order: a field initializer (§7.4) for `F := v`,
   whose value expression is evaluated there, in order (`objects O2`: `eval Extra` before `eval X`);
   a `block`/`let` in a constructor-function body is compiled inline at its position
   (`objects O10`). At most one argument is a delegating constructor call (§7.3).
4. Unless there was a delegating call, the class's **constructor** is called with `CallWithSelf`:
   `Self` = the object, positional arguments = the current token, uninitialized ("do not skip blocks",
   unused in this mode), uninitialized ("no init-super"). Its result becomes the token.
5. `UnifyNativeObject` (`Token`, `Object`) — §7.8: runs deferred setters, then the class's blocks.
6. `UnwrapNativeConstructorWrapper` (`Dest`, `Object`) — §7.8: the finished object, unified into the
   expression's result register.

A `concrete_subtype` instantiation `T{}` whose class is only known at run time does the same, but
takes the constructor with `LoadConstructor` (`Dest`, `Class`: the class's constructor function,
after waiting for `Class`) instead of a constant *(source, unprobed)*.

Each initializer takes the token from the previous one and hands one on, and each one waits for the
token it takes to be concrete, so initializers run in the order the code gives even under
unification-based execution. Every step of this protocol either passes the token through unchanged
or extends it (§7.6).

### 7.2 The constructor procedure

Each class (and struct, and interface) has one. Positional parameters, after `Self` (register 0)
and the class scope (register 1, `calls.md` §2.1):

| # | Parameter | Values |
| --- | --- | --- |
| 0 | **token** | the construction token (§7.1) |
| 1 | **skip-blocks** | uninitialized, or `true`. Forwarded to superclass and interface constructors; otherwise unused while leniency is off |
| 2 | **init-super** | uninitialized, or a function of two parameters (token, object) that performs a delegated base-class constructor call (§7.3) |

Its body, in order:

1. `JumpIfDefaultSubObject` on `Self`, to the end (§8.4) — never taken in the interpreter.
2. For each member of the class body, **in source order**: a data member compiles to a field
   initializer (§7.4) whose value code is the member's default — or, for a member with no default,
   to nothing; an accessor member (`external{}`) compiles to a `CreateField` with nothing between the
   two branches (the member is marked created, no setter runs); a method compiles to nothing (its
   entry is the whole of it); a `block:` clause compiles into the blocks procedure instead (§7.9).
3. The superclass step: if **init-super** is initialized (`JumpIfInitialized`), call it with
   (token, `Self`); otherwise, if the class has a non-built-in superclass, call the superclass's
   constructor with `CallWithSelf` (`Self`, token, skip-blocks, uninitialized). Its result is the
   token.
4. For each directly inherited interface, in order, call its constructor the same way (never with an
   init-super).
5. Return the token.

So initialization runs **subclass first**: a class's own members, then its superclass's, then — as
each level returns — that level's interfaces. With §7.4's rule this makes the most-derived
initializer the one that counts, matching §4.2.

### 7.3 Constructor functions and delegation

A `<constructor>` function `MakeC(args) := c: …` compiles to two procedures:

- The **wrapper**, which is what callers call: `NewObject` with the constructor function's archetype
  and the class; a `CallWithSelf` of the **body** with `Self` = the new object and the arguments
  (marker token, then the caller's positional arguments; named arguments forwarded by name); then
  `UnifyNativeObject` and `UnwrapNativeConstructorWrapper` exactly as §7.1 steps 5–6; then `Return`
  of the unwrapped object.
- The **body**, whose positional parameters are the token followed by the function's own. It is §7.1
  steps 3–4 over `Self`, and returns the token.

A delegation `MakeB<constructor>(…)` inside the body:

- **To a constructor of the same class**: a `CallWithSelf` of that constructor's body on the same
  object, token first. Its archetype is this archetype's `next`.
- **To a constructor of a base class**: the compiler wraps the delegated call in a closure of two
  parameters (token, object) whose body is that `CallWithSelf`, and instead of step 4 calls **this**
  class's constructor with that closure as init-super. So the subclass body's initializers run, then
  the delegated base constructor (in place of the superclass constructor call), then the subclass's
  interfaces. Observed order for `MakeDerived(4)`: the constructor function's own `block`, its
  `Extra := …`, then `MakeBase`'s `block`, then the class blocks base-first, and the base
  constructor's `X := 40` does not take effect (`objects O10`).

A field initializer written **after** a delegating call is a compile error for current content; for
content uploaded before a Fortnite version gate the compiler instead moves the delegating call after
all initializers *(source, unprobed)*.

### 7.4 One field initializer, and `CreateField`

Every field initializer — in a class body, a constructor-function body or an instantiation
expression — has the same shape:

1. `CreateField` (`LeniencyIndicator`, `Token`, `Object`, `Name`, `OnFailure`) opens it. It waits
   until the incoming effect token, `Token` and `Object` are concrete (park otherwise). Then:

   | `Name` in `Object`'s layout | `CreateField` |
   | --- | --- |
   | a slot not yet created | marks it created and **falls through** into the initializer |
   | a slot already created | **jumps** to `OnFailure`, skipping the initializer |
   | a constant data value or method | **jumps** (the value is already there) |
   | an accessor not yet created | marks it created and falls through |
   | an accessor already created | jumps |

   A jump also marks `LeniencyIndicator` failed, as any fast-failure op does (`failure.md`). For an
   object under native construction, creating a slot also resets its storage to the field type's
   zero value before the initializer runs *(source, unprobed; not observable)*.
2. The initializer: evaluate the value, then `UnifyField` (§7.5) for an ordinary member or
   `InitializeVar` (§7.6) for a `var`. For an accessor member there is nothing.
3. Both paths meet with the token handed on unchanged — except that an `InitializeVar` may extend it
   (§7.6).

`CreateField` is the whole of "an archetype-supplied field wins": whichever initializer reaches a
name first creates it, and every later one jumps.

### 7.5 `UnifyField`

`UnifyField` (`Object`, `Name`, `Value`) waits for the incoming effect token and `Object` (park
otherwise), then stores `Value` into the created slot `Name` **by unification**: if an earlier read
left a placeholder in the slot (§7.10), binding it wakes whatever waited on it. On a native field
(§9.2) the value is first converted to the field's native storage, which can raise. It passes the
effect token through. It is only ever emitted right after the `CreateField` that created the slot;
aimed at a constant it is a malformed program.

### 7.6 `InitializeVar`

`InitializeVar` (`TokenDest`, `Token`, `Object`, `Name`, `Value`, `ValueDomain`,
`bCheckIfVariableAllocationIsAllowed`) initialises a `var` member:

1. Wait for the incoming effect token and `Object` (park otherwise). Then, if
   `bCheckIfVariableAllocationIsAllowed` is true and a module is being initialised (a package
   procedure is running module-level data initialisers, `modules.md` §5), raise
   `ErrRuntime_UnimplementedGlobalVariable`. The run prints
   `ErrRuntime_UnimplementedGlobalVariable: Allocating a global var is not yet implemented. (Can't
   allocate mutable var field (/user@localhost/glob_var:)N while initializing module.)` — the detail
   in parentheses names the field by its decorated name — and the package procedure's module fails to
   evaluate (`vm_objects_global_probe.verse`: a module-level `GVar:glob_var = glob_var{}`). The flag
   is true for every `var` except, in content predating a version gate, a member declared in an
   interface.
2. If `Name` is a slot: make a new **variable** cell holding `Value` (with `ValueDomain` attached when
   present; ordinary declarations have none) and store the variable into the slot. On a native field
   the variable's content is held in native storage instead, with the same observable behaviour
   (§9.2).
3. If `Name` is an accessor: do **not** call the setter now. Append a deferred setter (the accessor
   reference for `Self` and `Value`) to the construction token — replacing the marker with a
   one-element chain, or extending the chain — and run it at `UnifyNativeObject` (§7.8).
4. Unify the (possibly extended) token into `TokenDest`.

### 7.7 `SetField` and `SetFieldLive`

`set O.M = v` on a class member that is a `var` compiles to a `LoadField` of the variable followed by
a reference write, not to `SetField`. `SetField` (`Object`, `Name`, `Value`) is emitted only for
`set S.F = v` where `S` is a struct value in a `var` and `F` a plain field; the compiler at this
commit refuses that source form (`vm_objects_reject.verse` R6: "Cannot assign to this value because
`(/user@localhost/rej_point:)X` is not declared with `var`"), so no reachable program contains
it. Its defined behaviour, for completeness *(source, unprobed)*: wait for `Object` and the incoming
effect token; if the slot holds a variable, write through it as a reference write; otherwise
overwrite the slot, recording the old value for rollback (`failure.md`); on an accessor reference,
call the setter along the path; on a native reference, set the field behind it.
`SetFieldLive` adds a `Task` operand for `set` inside a `live` scope (`tasks.md`), which no Godot
script reaches.

### 7.8 Finishing: `UnifyNativeObject` and `UnwrapNativeConstructorWrapper`

The names are the reference VM's; both run for **every** construction, native-represented or not.

`UnifyNativeObject` (`Token`, `Object`) waits until `Token` and `Object` are concrete. For an object
under native construction it also waits until every slot the layout has is concrete, then binds the
"finished object" placeholder that method loads during construction were bound to (§6). Then, in
order:

1. If `Token` is a deferred-setter chain (§7.6), call each setter in the order they were deferred,
   each with the object as receiver (§16).
2. If the object's class — its **actual** class, not the static one — has kind `Class`, call its
   blocks function with the object as `Self` and no arguments (§7.9).

A raise in either propagates as from any call.

`UnwrapNativeConstructorWrapper` (`Dest`, `Object`) waits for `Object` and unifies into `Dest` the
finished object: for an object under native construction, the object itself (its "under
construction" wrapping is dropped); for anything else, `Object` unchanged. The compiler also emits
it inside constructor bodies wherever source code names `Self`, which is why `Self` inside a block or
initializer is the finished object (`class_block_self_probe.verse` S3: the base's block already sees
the derived type).

For an interpreter that does not model engine objects, "under construction" can be a flag on the
object, and both ops reduce to: run deferred setters, run blocks, pass the object on.

### 7.9 Block clauses and the blocks function

A `block:` in a class body does not run where it is written. Every class of kind `Class` has a blocks
procedure (no parameters besides `Self` and scope) that:

1. `JumpIfDefaultSubObject` on `Self`, to the end.
2. If the class has a non-built-in superclass, calls the **superclass's** blocks function with
   `CallWithSelf` on `Self`.
3. Runs the class's own `block:` clauses in source order.
4. Returns.

It is called once per construction, by `UnifyNativeObject`, **after every field of every level has
been initialised**. So blocks run base-first, and every block sees final field values — including a
subclass's override and an archetype-supplied value (`objects O2`: `base block: Name=derived X=3
Count=0`, then `derived block: Count=1`; final `Count=11`). Blocks run once per instance
(`class_block_probe.verse` B3). Interfaces and structs cannot have block clauses — the compiler says
"'block' macro may only be used at class or function scope." (`vm_objects_reject.verse` R3, R4) — and
a class's blocks function does not call anything for its interfaces.

### 7.10 Reading a field before it is initialised

A slot that exists but has not been written reads as an unbound placeholder (§6), and the write that
eventually initialises it binds that placeholder. Under stage 1 of the design's parking plan this is
a fatal park; the compiler arranges that ordinary programs do not do it — data-member defaults cannot
read the instance at all (glitch 3502, `default_cdo_probe.verse`), and blocks run after every
initializer.

### 7.11 Objects the host constructs

When the host builds an object — `vh_instantiate` for a scripted node, and a fresh wrapper for a
Godot handle crossing into Verse (`godot-natives.md` §4) — there is no Verse archetype expression.
What must be observable:

- every data member holds the value its initializers would give it (including an object built by a
  member default, which is fully constructed with its own blocks run);
- the class's block clauses then run once, base-first, as in §7.9;
- blocks of objects built by member defaults run **before** the owner's blocks (`objects`: at
  instantiation the probe printed `base block: …` for its `Held:obj_base = obj_base{}` member, then
  `vm_objects_probe block`; later `O11 Held.Count=1 HostBlockRuns=1`).

The interpreter meets all three by constructing a host-built object exactly as §7.1 does with an
instantiation-expression archetype that has **no entries**: `NewObject`, the constructor with (marker,
uninitialized, uninitialized), then §7.8. `godot-natives.md` §4 says how the peer handle reaches the
object's `vh_object` block during it.

The reference host does not do it that way: it copies member values from a per-class default object
computed once, during program initialization (§8.4), and runs only the blocks per instance. The two
agree on everything above. They differ on one thing: a member default that calls a native with an
effect runs **once per class** in the reference host and **once per instance** in the interpreter.
The only such native in this project is `VhRefNewDefault` (`godot_array{}` and the other containers'
`Ref` default), so the two can disagree about whether host-built objects whose member default is a
container share one Godot container. §18 Q2 asks for the measurement.

### 7.12 Construction during module initialization

A class value can be instantiated by a module-level data definition only if the class carries
`<computes>` or `<allocates>` — a plain class is refused at compile time with "This archetype
instantiation constructs a class that has the 'transacts' effect, which is not allowed by its
context." (glitch 3512). A `<allocates>` class with a `var` member compiles and then raises at run
time, §7.6 step 1 (`vm_objects_global_probe.verse`).

## 8. Creating classes at run time

### 8.1 `NewClass`

In a linked program nearly every class is already a `class` cell. The compiler emits `NewClass` only
when it could not build the class while compiling: when an inherited class was not yet built at that
point (for example a class declared before its superclass in the same package — `objects O16`
declares `obj_late_derived` before `obj_late_base`, and it works), or when an attribute value is not
a compile-time value *(source, unprobed; the writer can count them)*. It appears in package
procedures.

Operands and behaviour:

| Operand | Role |
| --- | --- |
| `ClassDest`, `ArchetypeDest`, `ConstructorDest`, `BlocksDest` | the new class, its class-body archetype, its constructor function and its blocks function are unified into these. `BlocksDest` is written only when `ClassKind` is `Class`; for a struct or interface it is left untouched |
| `Package`, `RelativePath`, `ClassName` | the naming fields (§2.1) |
| `AttributeIndices` (immediate integers), `Attributes` (values) | the attribute arrays; built only when `Attributes` is non-empty |
| `ImportStruct` | the native type; absent in this project's programs |
| `Inherited` | the inherited classes, superclass first. Each must be concrete (park otherwise) |
| `Archetype` | a **template** class-body archetype: its entries hold raw procedures and native procedures rather than functions, its class is none, its `next` is uninitialized. It must be concrete, and so must its `next` |
| `ConstructorBody`, `Blocks` | the two procedures. `ConstructorBody` is always present; `Blocks` is present exactly when `ClassKind` is `Class` |
| `bNativeBound`, `ClassKind`, `Flags` | as §2.1–2.2. The final flags are `Flags` plus `NativeRepresentation` when `bNativeBound` is true, any inherited class has it or has `Predicts`, or any template entry has the entry flag `NativeRepresentation` or `Predicts`; plus `Predicts` when any inherited class or template entry has it |

What it builds: a new class with those fields, whose archetype is a **copy** of the template in which
`class` is the new class, `next` is the superclass's class-body archetype when `Inherited[0]` is a
class (none otherwise), and every entry whose value is a procedure or native procedure is replaced by
an unbound function over it with the new **class scope** (§2.1) as parent. The constructor and blocks
functions are made the same way over `ConstructorBody` and `Blocks`. The class scope is created here,
with the copied archetype as its one capture — which is what `LoadFieldFromSuper` later reads.

### 8.2 `BindNativeClass`

`BindNativeClass` (`Class`, `bImported`) follows every class definition, `NewClass` or not. In the
reference VM it creates the engine type the class reflects and, for a native-represented class,
fixes the per-class layout (§4.3) and runs attribute handlers. Observable requirements on the
interpreter: it waits until `Class`, every attribute value, every inherited class and every entry
type are concrete (park otherwise); after it, the class's layout is final. Nothing else. A class with
`bImported` true never occurs in this project (§8.4).

### 8.3 `ConstructNativeDefaultObject` and `LoadImport`

The global initializer (`modules.md` §3.4) runs `ConstructNativeDefaultObject` (`Class`) once per
class and interface (a struct is passed over), in dependency order; it is also emitted right after `BindNativeClass` for the
localization message class. In the reference VM it builds the class's **default object**: it runs
the class's constructor on an engine-side instance with skip-blocks `true` and an init-super that,
instead of constructing the superclass, marks every superclass member that has a default as created
(its value is inherited from the superclass's default object). Blocks are not run. An archetype
instantiated during it is built as a "default sub-object", and `JumpIfDefaultSubObject` in its
constructor and blocks function jumps straight to the end, so none of its initializers or blocks run.

Its only uses are the reference host's own: host-built objects copy their member values from it
(§7.11), and engine serialization. An interpreter that constructs host-built objects as §7.11 says
**must treat `ConstructNativeDefaultObject` as a no-op**, and then no object is ever a default
sub-object and `JumpIfDefaultSubObject` never jumps. The observable cost of the difference is
§7.11's last paragraph. Running it faithfully instead would run member-default natives once per class
at load, which only matters for `VhRefNewDefault`.

`LoadImport` accompanies a class whose native type is a delayed `@import_as`; this project has none,
and the writer should refuse a program that contains one.

### 8.4 `JumpIfDefaultSubObject`

(`Object`, `OnDefaultSubObject`): jumps when `Object` (after unwrapping "under construction") is an
engine default sub-object, and falls through otherwise. It never parks. In the interpreter, per
§8.3, it always falls through.

## 9. Native-bound and native-represented classes, and this project's native types

### 9.1 The two properties

- **Native-bound** (the class cell's boolean): the class itself is declared `<native>`. Its `<native>`
  methods are native procedures bound by decorated name (`natives.md`, `godot-natives.md`); its data
  members, construction and block clauses are ordinary bytecode.
- **Native-represented** (flag 1): the class is native-bound, or inherits from one, or has a
  `<native>` or `<predicts>` member. The reference VM stores such objects as engine objects. What
  of that is observable is §4.3 (layout per class, every data member a slot) and §9.2 (native
  fields); construction and field access otherwise follow the same protocol.

In this project **every** `object`-derived class is native-represented, because `vh_object` is
native-bound: all 1036 mirrored classes and every script class.

### 9.2 Native fields

A member flagged `NativeRepresentation` (`<native>` in source; only `host/Verse` declares any) is
stored in native form, so storing into it converts the value:

| Declared type | Storage | A value that does not fit |
| --- | --- | --- |
| `int` | signed 64-bit | raises `ErrRuntime_GeneratedNativeInternal`, printed as `An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available. (Value exceeds the range of a 64 bit integer.)` (`vm_objects_wideint_probe.verse` W1: `VariantInt(2⁷⁰)`, whose only native conversion is the store into `variant.I0`; the log carries no Verse call stack for this raise) |
| `float` | IEEE double | always fits |
| `string` | engine string | always fits |
| `logic` | boolean | always fits |
| a class | an engine object reference | — |

At commit `203d764` the reference editor host follows that raise with a fatal assertion and
terminates (the fixture records it). The interpreter must raise the runtime error and stop there.

A `var` member with a native flag — `vh_object.Handle` — behaves as any `var`: `LoadField` answers a
reference, and `set` writes through it, with the storage rule above on each write.

### 9.3 The project's native types, as compiled

| Type | Kind | Native-bound | Members | Blocks | Construction |
| --- | --- | --- | --- | --- | --- |
| `vh_object` | class | yes | `var Handle<native>:int = 0`; five hook methods with empty bodies (`_Notification` and the four in `godot-natives.md` §11) | one: `set Handle = VhAdoptOrMint(Self)` | ordinary: a class-body archetype with `Handle` and the method entries; a constructor that initialises `Handle` to 0; a blocks function |
| `variant` | struct | yes | 22 `<native>` data members — `Tag`, `Ref`, `I0`–`I3` as `int`, `F0`–`F15` as `float`, `Text` as `string` — each defaulting to its zero | none (a struct) | ordinary struct construction; each lane converts on store (§9.2) |
| `godot_ref` | class `<computes>` | yes | `Ref<native>:int = 0` | none of its own; the generated container subclasses (`godot_array`, `dictionary`, …) add `block: VhAdoptRef(Self)` and override `Ref`'s default with `VhRefNewDefault(Tag…)` | ordinary |
| `vh_signal` | class | yes | `Id<native>:int = 0` | none | ordinary |

`vh_signal` is a fourth native-bound class beside the three `CLAUDE.md` names; it is native in exactly
the same way. None of the four has a native method of its own: their natives are module-level
functions (`godot-natives.md`). So the interpreter provides nothing class-specific for them beyond
§9.2's storage rule — no native constructor, no native layout, no native block. Their equality is
§10.2 for `variant` (`objects O14`: two `VariantInt(3)` are equal, `VariantInt(3)` and
`VariantInt(4)` are not) and identity for the three classes.

## 10. Structs

### 10.1 Value semantics

A struct value is immutable once `UnwrapNativeConstructorWrapper` has produced it: no op writes a
field of a finished struct in any reachable program (§7.7). Copying is therefore sharing; an
implementation may share or copy freely (`objects O5`: after `Q := P` and `set P = …`, `Q.X` is
unchanged). A struct in a `var` changes only by replacing the whole value. Structs have no methods
(§5), no blocks (§7.9), no superclass and no interfaces *(source, unprobed for the last two)*, and
cannot be the target of a dynamic cast (§13).

### 10.2 Equality and hashing

Two struct values compare equal when they are of the **same class** and every field, slot or
constant, compares equal by name (`values.md` owns field-value equality, including its undecidable
and runtime-error outcomes). The layout does not matter: `obj_point{}` equals
`obj_point{X := 0, Y := 0}` though one holds `X` as a constant and the other as a slot (`objects O6`).
A struct never equals a non-struct. A struct's hash combines its class and each field's name and
value; a class instance's hash is its identity.

## 11. Interfaces

An interface is a class of kind `Interface`. It is never instantiated directly. It contributes:

- entries: its data members (with defaults) and its implemented methods — a method without a body
  has no entry — visited after the implementing class's superclass chain (§4.1);
- a constructor, called by each implementing class's constructor after the superclass step (§7.2);
- nothing to blocks (§7.9).

An interface's data-member default reaches the implementing object like any other
(`objects O12`: `obj_both{}.Tag = 4`).

## 12. Class equality and `<unique>`

Only `<unique>` classes are comparable in Verse. Two references to one object compare equal; two
distinct objects of a `<unique>` class compare unequal (`objects O7`: `self=eq twin=ne`). For a
non-unique class the reference VM answers "undecidable" when two distinct objects of the same class
meet in a unification; `values.md` owns what that means.

## 13. Casts

`TypeCastFastFail` (`Dest`, `LeniencyIndicator`, `Type`, `Value`, `OnFailure`) is how `T[V]` compiles
inside a fast failure context; inside a full one, the compiler instead emits a `Call` with the type
as callee and `V` as the one argument, with the same meaning (`calls.md` §4.1). Both wait for `Type`
and `Value` to be concrete, then succeed (unifying `Value`, unchanged, into `Dest`) or fail
(`failure.md`) by what `Type` is:

| `Type` | Succeeds when `Value` is |
| --- | --- |
| a class of kind `Class` or `Interface` | an object whose class is `Type`, or has `Type` among its transitive inherited classes and interfaces (`objects O8`: `obj_derived[D]` ok, `obj_derived[B]` fails, `obj_iface[obj_both{}]` ok, `obj_iface[obj_derived{}]` fails) |
| an `int type` | an integer, or a rational whose denominator is 1, within the bounds; an uninitialized bound is unbounded (`objects O13`: `type{_X:int where 0 <= _X, _X <= 10}` accepts 7, rejects 15) |
| a `float type` | a float *v* with `min <= v` and (`max` is NaN, or `v <= max`); NaN itself only when `min` is −∞ **and** `max` is NaN. A float type's bounds are always floats: −∞ as the minimum and NaN as the maximum mean unbounded |
| `any` | anything |
| any other type | not produced by the compiler; the reference VM aborts |

A cast never converts. The compiler refuses every other target: `int[V]` with `V:any` is "Dynamic
cast to `int` takes an int as its parameter, instead got any", and `rej_struct[V]` is "Cast target
`rej_struct` must be an interface or a class." (`vm_objects_reject.verse` R2, R5). A cast whose
argument's static type already satisfies the target compiles to no op at all.

## 14. Enumerations

An `enumeration` cell holds its enumerators in order; an `enumerator` cell holds its enumeration, its
name and its integer value (its position). Both are built at compile time; no op creates one. An
enumerator is a value compared by identity (`objects O9`); enumerations have no layout, no
construction and no fields. One enumerator is special: the only enumerator of the enumeration
`/Verse.org/Verse/accessor` is the value passed as the first argument of every accessor call (§16),
and the loader must be able to find it.

## 15. Unions

| Cell | Holds |
| --- | --- |
| union | the naming fields (package, relative path, name; no attributes, no native type, never native-bound) and its variants, in order. Each variant is a `union variant` whose tag is the variant's tag and whose **payload is the declared payload type** |
| union variant | a tag and a payload. As a runtime value, the payload is the value |
| union variant tag | its name (an interned name) and its union |

| Op | Behaviour |
| --- | --- |
| `NewUnionVariant` (`Dest`, `Tag`, `Payload`) | waits for both to be concrete; makes a union variant value |
| `GetUnionVariantPayload` (`Dest`, `Source`) | waits for `Source`; unifies its payload into `Dest` |
| `GetUnionVariantTag` (`Dest`, `Source`) | waits for `Source`; unifies its tag cell into `Dest` |

Two union variant values are equal when their tags are the same cell and their payloads are equal.

**Ordinary Verse source cannot produce any of this.** The `union` macro exists only when the
compiler targets this VM **and** the process-wide setting `verse.EnableUnions` is on; it is off by
default and nothing in this project sets it, so `union` is an unknown identifier
(`vm_objects_reject.verse` R1: "Unknown identifier `union`."). The three ops and the three cells will
not appear in this project's programs; §17 recommends the writer refuse them.

## 16. Accessor members

The mirror declares 3312 properties as `var P<getter(PGetter)><setter(PSetter)>:t = external{}`, so
accessors are on the path of every mirrored property read and write.

- **The entry.** Its value is an **accessor cell**: a table of getter names and a table of setter
  names, each indexed by parameter count. A getter taking *n* parameters is at getter index *n*−1; a
  setter taking *n* is at setter index *n*−2. The names are decorated method names in the same class.
  For a simple property there is one getter of one parameter and one setter of two.
- **Loading it.** `LoadField` answers an accessor reference (§6) — no call yet.
- **Reading through it.** `Freeze` and `FreezeIfAccessor` given an accessor reference call the
  getter instead of reading a cell (`RefGet` passes an accessor reference through unchanged; the
  compiler follows it with one of the two): arguments are the `accessor` enumerator (§14), then one
  argument per path step of a nested reference, and the receiver is the reference's object. The
  getter is found by name in the receiver's layout, where it is a method constant.
- **Writing through it.** `RefSet` or `SetField` on an accessor reference calls the setter the same
  way with the value appended. The call's outcome (return, failure, raise, suspension) is the
  setter's, as for any call (`calls.md`).
- **Initialising it.** A mirror property has no initializer (`external{}`): its `CreateField` only
  marks it created. A user-declared accessor member with a default initialises through
  `InitializeVar`, whose setter call is **deferred** to `UnifyNativeObject` (§7.6, §7.8), so it runs
  after every field initializer and before blocks.

## 17. The container: corrections to `format.md`

**§4 kind 16 `class` and §6.** The listed fields are the right ones, with these amendments:
`inherited` omits a built-in superclass entirely; `blocks` is present exactly when kind is class;
`attributes` and `attribute indices` are both absent (not empty) when there are none, and the
indices array has one more element than there are groups; the native type is not written, and the
writer must refuse a class that has one; `flags` are the final, derived flags (§2.2). The class
scope need not be a separate field: it is the parent scope of `constructor`.

**§4 kind 17 `archetype` and §6.** `class` must allow 0 (instantiation-expression archetypes have
none). An entry's `flags` should be written as the engine's 8 bits verbatim (§3.3) — `var` is bit 5
(32) and native representation bit 0 (1), not bits 0 and 1. An entry's `value` may be an accessor
cell, which version 1 has no kind for: **an `accessor` cell kind is required** (a list of getter
names and a list of setter names, by parameter count), or no mirrored property can be read. Entry
types that the VM uses (§9.2 needs a native member's value type through its pointer type) must keep
their element type: `simple type` 12 (pointer), 14 (option) and 9 (array) carry none today.

**§4 kind 26 `value object`.** Correct in shape: a class and the object's **slot** fields only; its
constants come from the class. The loader must build its layout as if the object had been
instantiated with an archetype that supplies exactly the listed fields (§4.1), mark all of them
created, and take struct-ness from the class's kind. A value object of a native-represented class
cannot be written as this kind; none is expected in a linked program (no module-level constant in
`host/Verse` is an instance), and the writer should refuse one if met.
**Corrected by the lead from the writer's cooks (T2.1):** every cook reaches one,
`(/Verse.org/Simulation:)editable_empty_message`, of the native-represented class `message`. It is
written as a `value object` with its slot fields, and the loader builds it as described above;
native representation changes nothing observable about it (§9.1).

**§4 kinds 21–23 (union, union variant, union variant tag).** Fields are §15's: union = package,
relative path, name, `list<ref>` variants; union variant = `ref` tag, `value` payload; union variant
tag = `sid` name, `ref` union. Since no program this project compiles can contain them (§15), the
recommendation is to leave them out of version 1 and have the writer refuse them by name.

**§4 kinds 19–20.** `enumeration` also has package and relative path in the engine; the VM needs only
the enumerators (and a name for diagnostics). `enumerator`'s fields are right. The loader needs a way
to find the `accessor` enumerator (§14): either a well-known entry in the file header or a lookup of
`/Verse.org/Verse/accessor` among package definitions (`modules.md` §7).

**§3 values.** The construction-token marker is the plain integer 12 774 014 and needs no new tag.

## 18. Open questions

1. **Does the writer ever see `NewClass`?** §8.1's conditions come from source. The M2 per-procedure
   report should count `NewClass` ops for `host_smoke`, `tests/integration` and `dodge-the-creeps`.
2. **Container member defaults on host-built objects (§7.11).** In the reference host a scripted
   node whose script declares `Items:godot_array = godot_array{}` gets its `Items` from the class's
   default object, and whether each node then holds its own Godot Array — or all share one minted
   when the default object was built, or hold a dead reference 0 because the cooker has no Godot —
   was not measured: `verse_probe` has no container callbacks. An integration case with two nodes of
   one such script, appending to one and reading the other's length, settles it, and decides whether
   the interpreter's once-per-instance behaviour is a deliberate difference or a match.
3. **Delegation to a non-immediate base** (§4.1) is taken on the compiler's word.
4. **`EmulateCaseInsensitiveOverrides` on `/Godot.org/Godot`.** Expected clear for script packages;
   the writer should report whether any class in a cooked program carries flag 4096.
   **Answered by the writer's cooks (T2.1):** every class and struct carries it, script classes
   included — the compiler's version gate that would clear it never opens at `203d764`. Only
   interfaces lack it. So the flag's effect on the layout rule (§4.4) applies to every class.
   Question 1 is answered by the same cooks: no `NewClass` op in any of the three.
5. **The `LoadField` "transparent reference" case.** When a slot holds a reference the reference VM
   marks transparent, `LoadField` answers the referenced value and caches it back into the slot. No
   Godot-reachable source that creates one was identified; `values.md` or `tasks.md` (live
   variables) should confirm it is unreachable, or specify it.
6. **`ToString(:int)` on an integer wider than 64 bits raises** `ErrRuntime_GeneratedNativeInternal`
   (`… (Value exceeds the range of a 64 bit integer.)`) — met while building this file's
   `vm_objects_wideint_probe.verse`, when string interpolation of 2⁷⁰ raised from
   `(/Verse.org/Verse:)ToString(:int)`. It belongs to `values.md`/`natives.md`, whose authors should
   know.
