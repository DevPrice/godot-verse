# Packages, modules and initialization

Status: reviewed by the lead 2026-09-24; `ops.md` §15.1 overrides this file where they disagree. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/spec/sidecar.md`; UE
`Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/{VVMAssembler.cpp,VVMCodeGenerator.cpp}`
(package, module and initializer emission, and the order the assembler runs them in),
`Engine/Source/Runtime/CoreUObject/{Public,Private}/VerseVM` (the module, class-binding and
default-object ops, the intrinsics package, the runtime-error table),
`Engine/Source/Runtime/VerseCompiler/Public/uLang/Diagnostics/Glitch.h`; this repository's
`host/Private/HostCooked.cpp` (how the UE runtime host loads a cook). Probes:
`tests/verse_probe/vm_modules_probe.verse`, `tests/verse_probe/vm_modules_var_reject.verse`,
`tests/verse_probe/vm_modules_varfield_probe.verse`.

This file says how a compiled program is organized into packages and modules, what running a
program's initialization does, and — the part that decides the loader — **why a `.vbc` loader must
not run it again** (§2). Calls are `calls.md`'s; classes, archetypes and object construction are
`objects.md`'s; natives are `natives.md`'s; tasks are `tasks.md`'s; the container is `format.md`.

## 1. Vocabulary

| Term | Meaning |
| --- | --- |
| **package** | the unit Epic's compiler emits code for: a name (`GodotScripts_1`), a verse root path (`/user@localhost`), and a **definitions table** from decorated path to value. `package` cell; `format.md` §7 |
| **module** | a Verse `module` — a directory with a `.vmodule` marker, a `name := module:` block, or a package's root. A package may hold several; one module may be spread over several packages (each package's share is compiled and initialized separately) |
| **compilation unit** | a set of packages that depend on each other cyclically (a strongly connected component of the package-dependency graph). Units are ordered so that every unit comes after the units it depends on. Most units are one package |
| **package procedure** | one per package: builds that package's classes and binds its functions (§3.2). Returns the integer 42 |
| **module procedure** | one per (package, module) pair that has module-level data or functions: computes that module's data (§3.3) |
| **global initializer** | one per program, linker-generated: runs every module procedure and constructs class default objects, in dependency order (§3.4). Returns 42 |
| **the sentinel** | the integer 42 that a package procedure and the global initializer return. It carries no information; Epic's assembler checks it as a sanity test |

### 1.1 The definitions table

Every top-level definition of a package has an entry, keyed by the compiler's decorated path string.
One source definition can own several entries:

| Definition | Entries |
| --- | --- |
| class, struct, interface | the class; its constructor function; its archetype; for a class (not a struct or interface), its blocks function |
| constructor function (`<constructor>`) | the function; its constructor; its archetype |
| function with a body | the function value; separately, its procedure |
| native function | the function value; separately, its native procedure; and, when it needed one, its wrapper procedure (`calls.md` §1) |
| module-level data | its value |
| enumeration | the enumeration, and one entry per enumerator |
| union | the union, and one entry per variant tag |
| module (one per module the package contributes to) | the module object (§4.1) |

The key spellings are the compiler's and the interpreter treats them as opaque. It never needs to
look a definition up by name while running — every reference in code is already a cell (§1.2) — and
a host finds classes through `format.md` §8's class index, not through keys. The one key the
interpreter must know is the missing-procedure entry of the built-in package (`calls.md` §10.1).

### 1.2 Cross-package references

A linked program contains no import-by-name. When code in one package uses a definition from
another, the compiler put the definition's **value** into the using procedure's constant pool, and
after linking that constant is the cell itself — the other package's class, function, archetype or
data value. `format.md`'s single cell table carries this directly: a reference across packages is a
cell index like any other.

Two things are still by name, and neither is a Verse-level reference:

- a `native procedure` names the implementation to bind (`natives.md`, §6);
- `LoadImport` names an asset by package and asset path (§4.6). A Godot script cannot produce one.

## 2. What a loader does: nothing runs

**The `.vbc` is a snapshot of a program whose initialization has already run.** The cooker calls
Epic's compiler, and compiling *is* running initialization: the assembler runs every unit's package
procedures and then the global initializer, and only then does the writer walk the program
(design §2, T2.1). After that, three things are true of what the writer sees:

1. Every definitions-table entry holds its final value — the class, the function, the computed
   module data — rather than the unbound placeholder the compiler first put there.
2. Every constant pool that referred to one of those placeholders has had it replaced by the value
   it was bound to. Epic's assembler does this explicitly as the last step of linking.
3. Module-level data objects exist, with their fields filled.

A loader must therefore **reconstitute the snapshot and run none of the initialization code**:
neither the package procedures, nor the module procedures, nor the global initializer. This is what
the UE runtime host does with a cook: it loads the serialized packages, rebinds native
implementations, and reads the sidecar; it runs no Verse (`host/Private/HostCooked.cpp`,
`LoadCookedProject`; that file is under `host/Private`, which the clean room does not read, and this
sentence is what it says). The interpreter's behaviour is judged against that host (design §10.2),
so it must match it.

Running them again would not be harmless (source, unprobed):

- A package procedure creates a class with `NewClass` and then unifies the new class with the
  definition's constant. In the snapshot that constant is the class the first run created, so the
  second run's fresh class is not equal to it, the unification fails, and the procedure never reaches
  its sentinel.
- A module procedure recomputes each datum and unifies it with the constant holding the first run's
  value. For an integer or string that happens to succeed; for an object it fails, because a second
  construction is a different object.

It would not add anything either: module-level initializers cannot have side effects (§5), so there
is no output or state a re-run could reproduce that the snapshot lacks.

What loading consists of, in order:

1. Refuse the file on any `format.md` §9 condition, with the sentence that section requires.
2. Allocate every cell, then fill every cell (`format.md` §4).
3. Intern every string and name (`format.md` §2.2).
4. Supply the built-in package's own definitions — at least the missing-procedure function
   (`calls.md` §10.1) — and resolve the `builtin package` cell to it.
5. Bind every `native procedure` by decorated name (§6).
6. Locate the VM's well-known definitions (§7).
7. Record the package cells as roots for the collector (design §7.3).

Nothing is executed. The first Verse that runs is whatever the host calls first.

`format.md` §7 currently says the opposite — that loading runs every unit's package procedure and
then the initializer. §8 is what the file needs instead.

## 3. What initialization does (for reference)

This section describes what Epic's assembler runs at compile time, so that the snapshot's shape
makes sense and so that nobody "fixes" the loader by adding it back. None of it runs at load (§2).

### 3.1 Order of the whole

1. Compilation units, in dependency order. Only units whose packages are being compiled
   (role **Source**) run; library packages (role **External** — `/Verse.org`, `/Godot.org`) were
   initialized when they were themselves built.
2. Within a unit: first bind the native implementations of every module in the unit that declares
   native functions or native data; then run each package's package procedure, in the unit's package
   order. Each must return the integer 42.
3. After every unit: run the global initializer, once, **as a task** (`tasks.md`). It must complete
   and its result must be the integer 42.

### 3.2 A package procedure

No parameters; register 0 (Self) is `false`; register 1 is `false`. In the package's definition
order it:

- for each class, runs `NewClass` (unless the compiler could build the class outright, in which
  case the class is already a constant), unifies the class, archetype, constructor and blocks with
  their definitions-table entries, and runs `BindNativeClass` on it (`objects.md`); for the one
  `message` class of the Verse library it also runs `ConstructNativeDefaultObject` at this point;
- for each function, unifies the function value (built at compile time) with its entry;
- ends with `Return` of the constant 42.

Enumerations and unions are built entirely at compile time and emit no ops. Module-level data is not
computed here but in the module procedures (§3.3), which the package procedure does not call.

### 3.3 A module procedure

No parameters. It runs:

1. `Move` the module's definitions-table constant into a register, then `BeginModule` with that
   register as `Dest`, the package constant, and the module's name (§4.1).
2. For each module-level data definition of this module in this package, in the package's
   definition order (source order within a file): compute
   the value, `Reset` a register, `Move` the value into it, `Move` the entry's constant into it
   (unifying the two), and `EndModuleData` with the datum's name and value (§4.3).
3. `EndModule` (§4.2).
4. `Return` of an uninitialized constant.

### 3.4 The global initializer

A procedure with no parameters and no package, spawned as a task. For every module part and every
class the program compiled, in **dependency order** (a depth-first walk that initializes an item's
dependencies before the item), it runs:

| Item | Op |
| --- | --- |
| a module part | a `Call` of its module procedure, `bCalleeYields` false, no arguments |
| a class brought in from an asset (`@import_as`) | `LoadImport` (§4.6) |
| any other class | `ConstructNativeDefaultObject` (§4.4) |

and then `Return` of the constant 42.

An item depends on:

- the modules whose data or functions its initializers read — for a module part, its data
  initializers; for a class, its field default initializers;
- the classes whose objects those initializers construct;
- for a class, its superclass and every interface it inherits;
- for an imported class, what the imported asset refers to.

A dependency cycle is found at compile time and is a build error, not a runtime one: glitch 9000
(`ErrAssembler_Internal`), whose text is `Linker task graph contains a cycle:` and a newline,
followed by one line per scope in the cycle — four spaces, the scope's path, a newline — starting
from the item whose dependency closed the cycle and walking back along the chain of dependents until
the repeated item, which is the last line (source, unprobed).

Within one package, reading a datum from the initializer of a datum *above* it is refused before any
of this, at semantic analysis: glitch 3502, `Accessing a variable from the initializer of a variable
that precedes it in the same snippet, or is located in a different snippet, isn't implemented yet.
You can fix this by either (1) changing the order of definitions in the same snippet, or (2) moving
definitions from another snippet to this one, or (3) place the definitions in the other snippet into
a submodule.` (measured while writing `vm_modules_probe.verse`; the file's header records it).

## 4. The module and native-module ops

What each op does in Epic's VM, and what an interpreter that does not model Unreal objects must do.
Under §2 none of these runs during loading; an interpreter still implements them, because the
bytecode contains them and an implementation of the ops is cheap. The design's decision that
"modules are our own objects" is what the right-hand column follows.

| Op | Epic's VM | A reimplementation |
| --- | --- | --- |
| `BeginModule` (`Dest`, `Package`, `ModuleName`) | finds or creates an engine class for the module inside the package's engine package, gets its default object, enters **module top level** (below) for this package, and unifies the module class into `Dest` | unify the module object (`module` cell) for this package and name into `Dest`, creating it if the package has none yet; push module top level. `ModuleName` is never empty |
| `EndModule` (`Module`) | leaves module top level; finalizes the module's default object | pop module top level. `BeginModule` and its `EndModule` are always in the same procedure run |
| `EndModuleData` (`FieldName`, `Value`) | requires `Value` concrete (parks otherwise); renames engine subobjects the value owns so they belong to the module | require `Value` concrete (park otherwise); nothing else. The datum's value already reached its definitions-table entry by unification |
| `ConstructNativeDefaultObject` (`Class`) | if the class has an engine class, constructs its class default object, running the class's field initializers on it | nothing. No Verse-visible value refers to a class default object in a Godot script (open question §9.3) |
| `BindNativeClass` (`Class`, `bImported`) | requires the class, its attributes and the types of its fields concrete (parks otherwise); builds or validates the engine class or struct that mirrors the Verse class | require `Class` concrete (park otherwise); nothing else. Native methods of a class are `native procedure` cells in its archetype and are bound by name like every other native (§6) |
| `LoadImport` (`Class`, `ImportPath`) | asks the embedder to load an engine asset and suspends the task until it has | unreachable: produced only for `@import_as`, which a Godot script cannot use. Treat as a VM invariant violation (`calls.md` §10) |
| `JumpIfDefaultSubObject` (`Object`, `OnDefaultSubObject`) | jumps when the object is an engine object flagged as a default subobject | never jumps: an interpreter creates no default subobjects |

**Module top level** is a mode that is on between a `BeginModule` and its `EndModule`. While it is
on:

| Op | Behaviour |
| --- | --- |
| `NewRef` | raises a runtime error, diagnostic `ErrRuntime_UnimplementedGlobalVariable` (table description `Allocating a global var is not yet implemented.`), message `Can't create a var at module scope.` |
| `InitializeVar` whose `bCheckIfVariableAllocationIsAllowed` is true | raises a runtime error, same diagnostic, message `Can't allocate mutable var field <field name> while initializing module.`, with the field's interned name substituted |
| `NewObject` | in Epic's VM, makes the object an engine object rather than a VM object. Not observable from Verse; an interpreter need not distinguish |

Both errors are guards the compiler makes unreachable (§5), and so the mode exists in an interpreter
only to keep them. The error rendering is `failure.md`'s.

### 4.1 The module object

A module object is the value of a package's module definitions-table entry. It is the `module` cell
of `format.md` §4 (verse path and name). Nothing a script writes can obtain one as a value, and no
op but `BeginModule` and `EndModule` consumes one, so it needs no behaviour beyond identity.

## 5. Module-scope data

A module-level data definition is a constant: its value is computed once, during initialization, and
every read of it anywhere in the program is a constant-pool entry holding that value (§1.2). There is
no op that reads module data by name.

What the compiler allows an initializer to be — measured, because every rule here is a refusal:

| Written at module scope | Result |
| --- | --- |
| a literal, arithmetic, an array, a reference to data defined above it or in another module | accepted (`vm_modules_probe` `ReadData`: `SecondData:int = FirstData + 1` reads 2, `UsesInner:int = inner_mod.InnerData + 1` reads 101) |
| a call to any function a script can write, even `<transacts>` or `<computes>` | glitch 3582, `Divergent calls (calls that might not complete) cannot be used to define data-members.` |
| an object of a class whose construction has an effect (the default) | glitch 3512, `This archetype instantiation constructs a class that has the 'transacts' effect, which is not allowed by its context.` |
| an object of a `<computes>` class | accepted (`GlobalHolder:plain_holder = plain_holder{Held := 9}` reads `Held = 9`, `Label = held`) |
| an object of a class with a `var` field | glitch 3512, `This mutable data definition has the 'allocates' effect, which is not allowed by its context.`, at the field (`vm_modules_varfield_probe.verse`) |
| `var X:int = 0` | glitch 3502, `` Module-scoped `var` must have `weak_map` type. `` — and every read of it, glitch 3502, `` Module-scoped `var` may only be partially read or written, e.g. `ModuleVar[Player]` or `set ModuleVar[Player] = ...`. `` (`vm_modules_var_reject.verse`) |

So **module-scope `var` is a compile error**, and so is every other route to mutable module state a
script could take; the runtime guards of §4 are backstops that no compiled Godot script reaches. The
one module-scope `var` the language permits, a `weak_map` keyed by a session or persistent player
class, cannot be written in a Godot project, which has no such key class.

Two facts follow that the interpreter depends on:

- **Initialization has no side effects a script can observe.** No initializer can print, write or
  call anything but a converging native. The order of §3.4 is therefore not observable, and a
  snapshot loses nothing by not re-running it (§2).
- **Module data is immutable.** A module-level object has only immutable fields, so a snapshot's
  copy of it is exact for the life of the program.

## 6. Binding natives

Every `native procedure` cell names its implementation by decorated name. At load (§2 step 5) the
loader binds each one it has an implementation for: the Verse-library natives and intrinsics
(`natives.md`) and the Godot natives (`godot-natives.md`), which the embedder supplies through its
native-binding table (design §2).

A `.vbc` carries every native of every reachable package, far more than a game calls, so a native
with no implementation must **not** refuse the file. It is bound to a stand-in that, when called,
raises a runtime error naming the decorated name. The UE runtime host has no equivalent — an
unbound native there is a null function pointer and calling it crashes the process with no
diagnostic, which is why that host rebinds them itself after loading — so the text is this
project's own and no differential test can depend on it. Suggested:
`The native function <decorated name> is not implemented by this runtime.`

Class-scoped natives (native methods) and module-scoped natives are the same cell kind and are
bound the same way; the distinction that forced the UE runtime host to rebind module-scoped ones by
hand does not exist in a `.vbc`.

## 7. Well-known definitions

Epic's VM gives a few library definitions a role of their own before any script runs. The loader
must find them in the snapshot:

| Definition | Why |
| --- | --- |
| the `task` class of `/Verse.org/Verse` (the class the `task` function's result type names) | task objects are instances of it (`tasks.md`) |
| the first enumerator of the `accessor` enumeration of `/Verse.org/Verse` | used by accessor references, which `format.md` version 1 does not carry; recorded so it is not forgotten |

Finding them by decorated key is fragile; §8 asks the writer to record them.

## 8. What the file must capture

Requirements on `format.md` §7 so that §2 holds. Whether §7 is sufficient: **no**. Its package
record is sufficient; its unit and initializer records are the wrong model.

| # | Requirement | `format.md` today |
| --- | --- | --- |
| M-1 | The file is the program **after** initialization. The writer writes every definitions-table entry and every constant with bound placeholders followed to their values, and refuses an **unbound** placeholder anywhere, naming where it was found | §3 says "a linked program has none", which is true of unbound ones only; bound placeholders are indirections the writer must follow rather than a kind it may refuse |
| M-2 | Loading runs nothing (§2). Units and the initializer are not executed | §7 says loading runs every unit's package procedure and then the initializer. **Must change.** The unit list and initializer reference may stay as dump-only information, or be dropped |
| M-3 | Each module definitions-table entry is written as a `module` cell. In the program the writer walks, its value is an engine class object, which the writer translates | cell kind 25 exists; the translation is the writer's (T2.1) |
| M-4 | Every module-level object is written with its fields, as a `value object` cell of its Verse class. In the program the writer walks, module-level objects are engine objects whose fields live in engine properties, so the writer reads them through the class's field list | cell kind 26 exists; reading engine-object fields is the writer's (T2.1) |
| M-5 | A function's receiver keeps three states — uninitialized, `false`, an object (`calls.md` §6) | §4 kind 14 says "uninitialized for none"; see `calls.md` §12.2 |
| M-6 | The well-known definitions of §7 are named in the file (a small table from role to cell), so the loader does not match decorated keys | absent |
| M-7 | Every native procedure reachable from any package is present, by decorated name and positional count | kind 13 does this |
| M-8 | The built-in package is not written; the loader supplies it (§2 step 4), including the missing-procedure function | kind 3 does this; `calls.md` §10.1 gives the one definition it must hold |

Nothing else about initialization needs recording, because nothing else about it is observable
(§5).

## 9. Open questions

1. **`format.md` §7 and design §4 both say the writer "must capture" the package procedures and the
   initializer so a loader can run them.** §2 says it must not run them. This is the one change this
   file needs from the lead, and T2.1 should be told before it writes a loader-facing unit list. It
   was established from source and from what the UE runtime host does, not by loading a `.vbc`,
   since no writer exists yet; the first `vbc_dump` of a real cook should confirm M-1 (no unbound
   placeholders in any definitions table or constant pool).
2. **Order of the global initializer's roots.** §3.4 gives the dependency rule, not the order of
   independent roots, which follows a compiler-internal table's insertion order. It is not
   observable (§5), so it is not specified.
3. **Class default objects.** §4 says a reimplementation constructs none. That rests on no
   Verse-visible value referring to one in a Godot program; `objects.md` should confirm it against
   the export-defaults path, which reads defaults off a transient instance rather than a default
   object (CLAUDE.md, "Objects that are not nodes").
4. **Initialization failures.** Under §2 the interpreter never sees one. They happen in the cooker:
   a runtime error in a package procedure is glitch 9003 (`ErrAssembler_RuntimeError`) with the text
   `Runtime error while evaluating module.`, attributed to that package, and a runtime error in the
   global initializer is the same glitch and text with no package; either fails the cook and so the
   export. What the interpreter's host sees at load is only a `format.md` §9 refusal, which it
   reports the way `vh_init` failures are reported today (an exported game shows the sentence and
   quits). No probe produces a module runtime error, because §5 leaves an initializer nothing that
   can raise; this was read, not run.
5. **Natives a game calls that nobody implemented.** §6's stand-in turns a missing binding into a
   runtime error at first call. Whether the loader should also print, once, the list of unbound
   natives it saw is a design choice left to the clean room.
