# Exporting Verse properties to Godot's inspector

Research notes and roadmap, done against a local UE source checkout on branch `ue6-main`
(read-only reference, not part of this repo). Every claim below is cited `path:line`; where the
checkout does not answer the question, that is stated rather than guessed.

## What an attribute is

`@editable` is not syntax. An attribute in Verse is an ordinary class deriving from the
compiler's intrinsic `attribute`, applied with `@` to a definition. `Engine/Plugins/Verse/
VerseSimulationMetadata/Source/VerseSimulationMetadata/Verse/Simulation/Editable.native.verse:8-25`:

```verse
@attribscope_class @attribscope_struct @attribscope_data @customattribhandler
editable<public> := class<computes>(attribute):
    ToolTip<public>:message = editable_empty_message
    Categories<public>:[]message = array{}
    EditInline<epic_internal>:logic = true
```

`attribute`, `attribscope_*` and `customattribhandler` are compiler intrinsics, created in
`Engine/Source/Runtime/VerseCompiler/Private/uLang/Semantics/SemanticProgram.cpp:856-867` — no
`.verse` file in the engine defines them, unlike every attribute built on top. The
`@attribscope_*` line declares where the attribute may legally appear, so applying `@editable` to
a function is a compile error rather than a silently ignored annotation.

Attributes are `SAttribute` records on the definition, and `SAttribute::_Type` distinguishes the
`@attr` prefix form from the `<spec>` suffix form
(`Engine/Source/Runtime/VerseCompiler/Public/uLang/Semantics/Attributable.h:19-31`). `<public>`
and `@editable` are the same mechanism wearing different syntax.

## Why `editable` doesn't fit

`AddSuperType` in
`Engine/Source/Runtime/VerseCompiler/Private/uLang/SemanticAnalyzer/SemanticAnalyzer.cpp:5301-5306`:

```cpp
// Don't allow inheriting from an attribute class.
const Vst::Node* VstNode = AstNode.GetMappedVstNode();
if (!Class.IsAuthoredByEpic())
{
    ValidateNonAttributeType(NegativeSuperType, VstNode);
    ValidateNonAttributeType(PositiveSuperType, VstNode);
}
```

`godot_export := class<computes>(attribute)` fails with `V3552: Attributes cannot be used as data
types` for any class that is not `Class.IsAuthoredByEpic()`. The `VerseScope.InternalUser` this
package builds under does not help on its own:
`Engine/Source/Runtime/VerseCompiler/Public/uLang/Semantics/SemanticScope.h:200-203` keeps
`IsAuthoredByEpic()` distinct from the predicate that "differs from IsAuthoredByEpic by allowing
packages with Scope=InternalUser to access epic-internal definitions". InternalUser buys *access*,
which is what lets scripts say `<native>`; it does not buy authorship by itself — see below for
what does.

**Subclassing an existing attribute is barred by the same line.** `ValidateNonAttributeType`
(`SemanticAnalyzer.cpp:22508-22529`) tests `SemanticTypeUtils::IsAttributeType`, which is true of
the whole attribute hierarchy — so `godot_export := class(editable)` fails exactly as
`class(attribute)` does. There is no alias route; owning authorship is the only route, below.

**Even authored, `editable` carries a rule this bridge does not want.** It is declared
`@customattribhandler`
(`Engine/Plugins/Verse/VerseSimulationMetadata/Source/VerseSimulationMetadata/Verse/Simulation/Editable.native.verse:8-25`),
so evaluating a module that applies it calls `ICustomAttributeHandler::FindHandlerForAttribute`
(`Engine/Source/Runtime/CoreUObject/Private/VerseVM/VVMAttribute.cpp:126-133`), which fails the
whole build with `No custom handler for attribute: %s` (`VVMAttribute.cpp:130`) unless
`VerseSimulationMetadata` has been loaded to register one — `FVerseSimulationMetadataModule::StartupModule`
does the registering, and linking the module in a monolithic program does not run that, so
`EnsureIde` now loads it explicitly. Loading it buys nothing worth having, though: the handler's
rules (`VerseSimulationAttribHandler.cpp`, `UVerseSimulationAttribConfig::IsAllowedEditableProperty`
in `VerseSimulationAttribConfig.cpp:13-26`) are written for UEFN's inspector, and refuse a struct
that is not concrete — which rejects a `color` member outright. Godot draws colours fine, so
inheriting UEFN's refusal along with the attribute would be borrowing a limitation that has
nothing to do with this bridge.

## How we define our own

An attribute with no handler is just a type applied to a definition, which is all `export` (and
`global_class` beside it) needs — so the fix is not to patch `editable`'s rules, it is to stop
using `editable` and own an attribute with none.

Authorship is a string prefix on our own package path, not a signature.
`CScope::IsAuthoredByEpic` (`SemanticScope.cpp:796-805`) builds the scope path and asks whether it
starts with any entry of `CSemanticProgram::_EpicInternalModulePrefixes`, seeded at
`SemanticProgram.cpp:329-332` to exactly:

    /Verse.org/
    /UnrealEngine.com/
    /Fortnite.com/

Naming a package under one of those three would satisfy the gate with no compiler patch — and is
the route an earlier draft of this note considered — but it means claiming Epic's own namespace:
every other `IsAuthoredByEpic` call site would then treat our package as Epic's too, and the path
would be a standing lie to anyone reading it.

What is done instead adds a fourth prefix, honestly ours. `FGodotAuthorshipInjection`
(`host/Private/HostScript.cpp`) is a `uLang::IPreSemAnalysisInjection` whose `Ingest` calls
`ProgramContext._Program->_EpicInternalModulePrefixes.AddUnique("/Godot.org/")`. It is registered
once, for the life of the process, through `uLang::TModularFeatureRegHandle<FGodotAuthorshipInjection>`
— and still has to run on every build regardless, because `CProgramBuildManager::Build` resets the
semantic program, prefix list included, before every compile and every analysis, so a one-shot
grant would not survive the first one. Nothing else this bridge's packages need to dodge tests
`_EpicInternalModulePrefixes` the way `PublicUser` packages do: both `SolIdeDataSources` and the
attribute package below declare `VerseScope.InternalUser`, so the sanity check at
`SemanticAnalyzer.cpp:3889-3911` that flags an Epic-internal path inside a `PublicUser` package
never sees them.

With `/Godot.org/` authored, `export`, `global_class`, `export_category`, `export_group` and
`export_subgroup` are declared as Verse source compiled at runtime (`AttributePackageSource` in
`HostScript.cpp`), rather than shipped as `.native.verse` — a second package sharing the native
one's verse path, so a script's existing `using { /Godot.org/Godot }` reaches them with no new
import needed. It has to be added to the IDE's source project before the first `AddDataSource`:
`FSolarisIde::EnsureDataSourcePackageExists` snapshots the project's other packages as the script
package's dependencies exactly once, so a package added afterward is never depended on and its
attributes do not resolve.

None of the five declares `@customattribhandler` — a bare `class<computes>(attribute) {}` needs no
handler to exist, and existing is all `export` and `global_class` do. `export_category`,
`export_group` and `export_subgroup` each carry one `Name:string`, read back the same way the
retired `clamp_min`/`clamp_max`/`category` attributes were (see below).

A related detour worth recording: a `.verse` file in a VNI-capable package must be named
`.native.verse` (`V3012`), whether or not it declares anything `<native>`. Epic's own attribute
definitions are `.native.verse` for this reason; the runtime-compiled package here is exempt
because it is never staged through VNI at all — it is source text handed to the IDE directly.

## How the export list is harvested

The list comes out of the **semantic program**, not the VM. The chain:

- `ISolarisIde::GetBuildManager()`
  (`Engine/Source/Runtime/Solaris/SolarisBridge/Public/ISolarisIde.h:227`)
- → `CProgramBuildManager::GetProgramContext()`
  (`Engine/Source/Runtime/VerseCompiler/Public/uLang/Toolchain/ProgramBuildManager.h:62`)
- → `SProgramContext::_Program`, a `TSRef<CSemanticProgram>` described as "persistent data from
  consecutive toolchain runs"
  (`Engine/Source/Runtime/VerseCompiler/Public/uLang/CompilerPasses/CompilerTypes.h:208-212`)
- → `CSemanticProgram::FindDefinitionByVersePath<CClass>`
  (`.../Semantics/SemanticProgram.h:688`)
- → `CLogicalScope::GetDefinitionsOfKind<CDataDefinition>()`
  (`.../Semantics/SemanticScope.h:269`)
- → `CDefinition::HasAttributeSubclass` (`.../Semantics/Definition.h:237`)

`HasAttributeSubclass` rather than `HasAttributeClass`, matched against `/Godot.org/Godot/export`
— so a subclass of `export` would count the same way `export` itself does, though nothing in
`AttributePackageSource` defines one today.

**Why the semantic program and not the VM.** Analysis re-runs on every keystroke —
`bSemanticAnalysisOnly` publishes nothing, which is what already gives the script editor live
diagnostics — while code generation happens only when a generation is built. Anything read from the
VM would therefore be a build behind the text, so the export list would not refresh until the next
Play. This is the view of a script's shape that changes per keystroke.

*(Written when code generation could happen only once per process, which made the argument
starker: the VM's answer would have been frozen at startup for the life of the editor. Phase 3
made builds repeatable — spec §10 — and the conclusion is unchanged, because "a build behind" is
still wrong for a list the inspector redraws as the author types. It is also why R-ITER-3's two
halves move at different speeds: a newly declared member appears on save, its default on build.)*

The cost of that choice is that it describes *source*, not running state: it cannot give values.

## What the semantic program will and will not give

**Presence: exact.** `HasAttributeClass` / `HasAttributeSubclass` are proper API.

**Single string arguments: available.** `SAttribute::GetTextValue()`
(`Engine/Source/Runtime/VerseCompiler/Private/uLang/Semantics/Attributable.cpp:62-86`) pulls a
`Literal_String` out of any single-argument attribute invocation.
`CAttributable::GetAttributeTextValue` (same file, `302-329`) wraps it. Marked `@HACK: SOL-972`
for lack of general compile-time evaluation, but the string case is solid — which is exactly what
the three section attributes need and nothing more:

| Verse | file | Godot |
| --- | --- | --- |
| `@export_category("Movement")` | `export_category_attribute`, `AttributePackageSource` | `PROPERTY_USAGE_CATEGORY` |
| `@export_group("Movement")` | `export_group_attribute`, `AttributePackageSource` | `PROPERTY_USAGE_GROUP` |
| `@export_subgroup("Movement")` | `export_subgroup_attribute`, `AttributePackageSource` | `PROPERTY_USAGE_SUBGROUP` |

Each row names the attribute *class* (`export_category_attribute`), not the `<constructor>`
function beside it (`export_category`): `GetAttributeTextValue` matches on the invocation's
**return type**. The outermost of the three that a member names wins, since a member cannot open a
group and the category above it at the same position.

**The heading above the whole list is not an attribute at all.** Every scripting language shows the
script's own name above the properties it contributes, and that heading is a fourth
`PROPERTY_USAGE_CATEGORY` entry — one the script pushes at the head of its property list rather than
one a member asks for. `GDScript::_update_exports` does it with `get_class_category()`
(`modules/gdscript/gdscript.cpp:556`) and `VerseScript::refresh_exports` does the same, only when
the walk produced at least one property: a heading over nothing is noise. It cannot come from the
script *instance* instead, because in the editor there is no instance — `_can_instantiate` is false
for a non-tool script, so Godot builds a `PlaceHolderScriptInstance`, whose `get_property_list`
replays the list it was handed (`core/object/script_language.cpp:648`) and adds nothing to it.

The name on that heading is one deliberate divergence. `Script::get_class_category()`
(`core/object/script_language.cpp:140`) reads `Resource::get_name()`, which is empty for any script
loaded from a file, so GDScript falls through to the file name and shows `player.gd` even for a
`class_name Player`. Here a script with `@global_class` shows the registered name — `Mover` — because
that is the name Godot knows the script by everywhere else: the node creation dialog, a typed
property's filter, the class documentation. The file name is the fallback, not the rule. The name
comes from `VerseScript::_get_global_name`, which reads it out of the source text through
`verse_scan_class_decl`, so the heading agrees with what was registered during the filesystem scan
without asking the host.

**Everything else comes off the declared type instead of a second attribute.** A range, an enum's
choices and a mirrored class's name are all already spelled in the member's own type —
`DescribeExportType` in `HostScript.cpp` reads a bounded `CIntType`/`CFloatType`'s min and max, a
`CEnumeration`'s enumerators in declaration order, and a `CClass` that derives from
`/Godot.org/Godot/object` — so there is no `clamp_min`/`clamp_max` string left to keep in sync with
what the compiler enforces, and no attribute for a bound that could ever disagree with the type. A
one-sided constraint (`type{_X:int where 0 <= _X}`) is spelled with the bound it has and
`or_greater`/`or_less` on the Godot side (`range_hint_for` in `src/verse_script.cpp`), which is a
Godot convention the host has no business knowing.

**A strict bound needs nothing said about it.** `type{_X:float where _X < 500.0}` normalises to
the double immediately below `500.0` — the exact value the constraint admits — and nothing in the
type remembers that the author wrote `<` rather than `<=`. Nothing needs to. Every value a spinbox
hands back is a multiple of its step, so `rounded_inward` in `src/verse_script.cpp` puts each bound
on that grid before spelling the hint: `ceil` for the minimum, `floor` for the maximum, which is
Godot's own `Math::snapped` arithmetic without the half-step that would round to nearest. The
strict bound lands on 499.999 and the inclusive one does not move, because it is already on the
grid. An int is normalised further still — `0 < _X` is `1 <= _X`, the same statement — and its
step of one leaves every integer bound where it is.

Rounding inward rather than outward is the part that matters: a bound rounded the other way leaves
the field offering a value the type rejects. It also means a bound finer than a single step
(`_X <= 0.0005` against the default `0.001`) rounds down to `0`, which reads as harsh and is right.
Every other value that field can produce violates the constraint.

`@display_name` was never carried over. Godot's `PropertyInfo` has no display-name field — the
inspector derives a label from the property name — so there was nowhere to put it, and renaming
the property itself would break get and set, which address a member by its real name.

**Structured attribute fields would still need SOL-972.** `@editable_slider(float): MinValue :=
option{0.0}` parses and applies, but reading `MinValue` back needs the general compile-time
evaluation SOL-972 is waiting on. That is precisely why a range is read off the *type* now rather
than off a payload: `CIntType::GetMin()`/`GetMax()` are ordinary type-checker output, not attribute
evaluation, so there is nothing here for SOL-972 to block.

**Defaults: not available here.** `CDataDefinition::HasInitializer()`
(`.../Semantics/DataDefinition.h:126-130`) says an initializer exists, not what it evaluates to.
Defaults come from an instance instead — see below.

## Values

Both directions are implemented. Several things about them were wrong on first attempt and are
worth recording, because each cost a build cycle — and the last one cost more than that.

**Epic's attributes need their handler module loaded.** `editable` and its neighbours are declared
`@customattribhandler`, so evaluating a module that applies one calls
`ICustomAttributeHandler::FindHandlerForAttribute`
(`Engine/Source/Runtime/CoreUObject/Private/VerseVM/VVMAttribute.cpp:126-133`) and fails the whole
build with `No custom handler for attribute: editable` when none is registered. The handler is
registered by `FVerseSimulationMetadataModule::StartupModule`, and in a monolithic program linking
the module does not run that. `EnsureIde` calls
`FModuleManager::Get().LoadModule(TEXT("VerseSimulationMetadata"))`, the same shape as the
existing `IVerseModule::Get()` call.

The bridge's own attributes carry no handler and need none — an attribute with no handler is just a
type applied to a definition. The load stays because a script is free to import
`/Verse.org/Simulation` and apply one of Epic's, and a build that does so with no handler
registered fails outright rather than ignoring the attribute.

**`LoadField` by name asserts on a missing field.** The by-name overload
(`VVMVerseClassInline.h:56-64`) does `Shape.GetField(FieldName)` and passes the result straight
into `PeekField`, which asserts `Field` non-null. A field the shape does not carry is a crash,
not a failure. Resolving the shape entry first and null-checking it is what makes "no such
member" an answer. `PeekField` is then preferable to `LoadField` for an inspector read: an unset
member reads back uninitialized rather than raising a Verse runtime error.

**The shape keys members by decorated name.** Not `Speed` but
`(/user@localhost/exports:)Speed` — qualified by the *declaring* class, the same decoration
`FindGodotClass` applies to class names and `VerseScriptInstance` to methods. Dumping
`VShape::CreateFieldsIterator()` is how this was settled:

    (/user@localhost/exports:)Speed
    (/user@localhost/exports:)Enabled
    (/Godot.org/Godot/object:)Handle
    (/Godot.org/Godot/object:)Ready

**Defaults come from an instance, not the CDO.** `UVerseClass` runs the Verse constructor from
`PostInitInstance`, which `NewObject` drives and class-default-object construction does not, so a
CDO's members read back uninitialized. `ReadClassDefaultField` builds a transient instance with
`NewObject<UObject>(GetTransientPackage(), NativeClass)` — the same call `Instantiate` makes —
and reads that. Its `Handle` is left unset; reading a plain data member never consults it.

**A `var` member's type is wrapped.** `CDataDefinition::GetType()` for `var X:logic` returns a
pointer around the value type. `CNormalType::GetInnerType()` is the wrong tool for unwrapping it:
it also unwraps arrays, and Verse's `string` is `[]char`, so every string reports as a char.
Unwrap `ETypeKind::Pointer` and `Reference` specifically via `CInvariantValueType::PositiveValueType()`.

**A member's storage is three different shapes, and `PeekField` hides two of them.** This is the
one that shipped a crash. `VShape::VEntry::Type` says where a field lives
(`VVMShape.h:23-41`), and `VClass::CreateShape` picks between them
(`VVMClass.cpp:754-784`): a type with a native representation becomes `FProperty`, the same type
declared `var` becomes `FPropertyVar`, and a type with no native representation becomes
`FVerseProperty` — a `VRestValue` slot. So:

| declaration | `EFieldType` | what the storage holds |
| --- | --- | --- |
| `X:float` | `FProperty` | the double, in an `FProperty` |
| `var X:float` | `FPropertyVar` | the double, in an `FProperty` |
| `X:string` | `FVerseProperty` | a `VArray` in the slot |
| `var X:string` | `FVerseProperty` | a **`VMutableArray`** in the slot |
| `var X:some_object` | `FVerseProperty` | a **`VRef`** box in the slot, value inside |

`UVerseClass::PeekField` (`VVMVerseClass.cpp:1823-1875`) answers `FPropertyVar` with
`VNativeRef::New(...)` — a *reference*, not the value — because that is what an assignment needs.
An inspector wants the other thing, so the read follows it: `VNativeRef::Peek` for `FPropertyVar`,
and a `VRef` unwrapped wherever one comes back. This is why every `var` read as "unconvertible"
before, non-vars included in the failure only by luck of being unwrapped already.

The write is the mirror, and getting it wrong is silent. Verse hangs mutability off the container
rather than off a reference around it, so a `var` of a *container* type holds a mutable container
in the slot while a `var` of a *scalar* holds a `VRef` box. Writing a bare value over either one
leaves storage the compiled code will misread — and nothing notices until Verse itself touches
the member, where it surfaces as `Unexpected ref type VValue` in `VVMInterpreter.cpp:2256` or as
a fatal `Freeze` of a cell that has no `FreezeImpl`. `WriteFieldOf` therefore peeks the current
value first and matches its representation: through a `VRef` where there is one, `VMutableArray`
against `VMutableArray`, `VNativeRef::Set` for anything native.

**A read/write round-trip cannot test any of this.** The first version of the write path passed
the smoke test on every `var`, because a write in the wrong representation and a read in the same
wrong representation agree with each other. Only the VM disagreed, and only when a script read the
member. `exports.verse` now carries `Bump()` and `BumpLabel()`, which read and assign each member
from Verse, and the test calls them — the interpreter is the oracle, not the ABI.

### References

**A mirrored class needs no VM object construction at all**, which is the thing that made this
cheap. The fear going in was `VClass::NewVObject` and archetypes: building an instance field by
field, against a shape whose slot order `VShape::New` assigns from a hash table
(`VVMShape.h:105-107`), with the VM not re-checking a slot it was told holds one thing. None of that
applies here. Every mirrored class derives from `object`, `object` is the one `<native>` class in the
package, and a Verse class over a native ancestor compiles to a `UVerseClass` — so its instance is a
UObject, and `NewObject<UObject>(GetTransientPackage(), NativeClass)` plus `Handle.Init` builds one.
That is the same pair of lines `Instantiate` already uses for a script's own class, and
`UVerseClass::PostInitInstance` has run the Verse constructor by the time `NewObject` returns. On the
wire the value is then just `VValue(UObject*)` — `VVMValue.h:66` — which is why the storage
discipline above has nothing extra to say about it.

The `UClass` is found by decorated name across every package in the program rather than in a named
one. `FindGodotClass` can spell the package the host compiles (`SolIdeDataSources`), but the Godot
API is a VNI package whose VM name is assembled out of the mount point and the C++ module name
(`Names::GetVersePackageNameForVni`, `VVMNames.cpp:304-307`), and `(/Godot.org/Godot:)node2d`
identifies the class without anything having to predict that.

**An empty option and `logic` false are the same cell.** `VValue::IsFalse()` is pointer identity
against the `VFalse` singleton (`Inline/VVMValueInline.h:116-119`), and Verse's `false` *is* the empty
option; `true` is `VOption(VFalse)`, so `IsLogic()` catches both logic values and a `DynamicCast` to
`VOption` succeeds on `true` as well. A set option around an object is therefore the only one of the
three the value identifies on its own. The empty case has to ask what the author declared, which
`DescribeMemberType` does against the semantic program — the same place `IsVarMember` asks its
question, and for the same reason: the storage says where a value lives, not what Verse permits there.

**A script class's reference is the other node's own object, not a copy of its handle.** Constructing
a second `mover` for a node that already has one would give it two Verse objects: two sets of members,
two identities, and an author reaching through whichever one they happened to hold. So the instance
itself crosses, through `vh_instance_set_field_instance`, and the host checks the class the slot
expects before writing — `IsChildOf` against the member's declared class, because nothing downstream
will. That check is also what lets a *mirrored* member take an instance: `?node2d` assigned a node
carrying a `mover` script holds the mover, which is both type-correct and the identity-preserving
answer.

**Which picker the slot gets is a different question from what it filters by, and conflating them is
how the first version drew a resource picker for `?mover`.** Godot decides the *kind* of slot from the
class's native — ClassDB — ancestry: `PROPERTY_HINT_RESOURCE_TYPE` under Resource,
`PROPERTY_HINT_NODE_TYPE` under Node, and for an object property with neither hint the inspector falls
back to a resource picker. It decides what the slot *accepts* from the narrowest name it can put in the
hint string, which for a registered class is that name. GDScript keeps the two apart explicitly —
`export_type.native_type` for the hint, `_find_narrowest_native_or_global_class` for the string
(`modules/gdscript/gdscript_parser.cpp:4796-4811`).

Only one of those the Godot side can work out on its own. `Mover` is registered with the script
language, not with ClassDB, so `ClassDB::is_parent_class("Mover", "Node")` is false and nothing local
can say otherwise. The host sends the answer instead: `NativeClassUtf8` carries the nearest *mirrored*
class in the referenced class's own superclass chain — `node2d` for a `?mover` — which is a name
ClassDB does know. For a mirrored reference it is the class itself, so the consumer has one rule rather
than two.

Referring to a script class at all requires `@global_class` on it. That is the one rejection in this
set that is about Godot rather than about Verse: the inspector filters a slot by a Godot class name,
an unregistered class has none, and Verse will happily let the member be declared anyway. The host
reads the attribute off the `CClass` with `HasAttributeSubclass`, which makes two readers of
`@global_class` — this one and `verse_scan_class_decl` on the Godot side, which answers from the
source text because Godot asks during the filesystem scan, before a host exists. They have to agree:
one decides whether the reference can be exported, the other decides whether the name it would be
filtered by exists.

### Structs

`vector2`, `vector3` and `color` cross as a tuple of their components, tagged with the Godot type to
rebuild from them. That encoding was already there: `src/verse_value.cpp` has converted those three
Variant types in both directions since method calls needed them, so the wire needed nothing new and
the Godot side needed nothing at all — `variant_type_for` reads the tag, and Godot draws a colour
picker or a pair of spinboxes off the property type with no hint.

**This is the one place the bridge really constructs a VM object, and the trap is shape constants.**
A field the class declares with an initializer — which is every field of `vector2` — is raised to the
*shape* as a `Constant`, shared by every instance, so it has no per-instance slot to write:
`VObject::SetField` reaches `VERSE_UNREACHABLE` on one (`Inline/VVMObjectInline.h:73`), which is how
the first attempt died. `VClass::GetArchetype()` is therefore the wrong archetype to build from. The
right one is assembled by hand out of `VArchetype::VEntry::ObjectField` entries, one per field, which
is what asks for the slots; `CreateField` then marks each present and `SetField` writes it. That is
the sequence `VNativeRef::FromNativeStruct` uses (`VVMNativeRef.cpp:492-513`) to hand a native struct
to ordinary Verse code, and it is the one to copy. `NewVObject` does the allocation rather than
`VValueObject::NewUninitialized`, because it is also what calls `SetIsStruct` (`VVMClass.cpp:333-336`)
— an object built without that does not compare or freeze as a struct.

Nothing here needs the class looked up by name: the struct already in the slot supplies its own class,
which is the same "match what is there" discipline the rest of the write path follows.

**One Godot-side simplification came out of this.** `VerseRuntime::set_instance_field` had a switch
of its own over the four Variant types it could pack. A struct would have made it five, and an array
six, each needing somewhere for its items to live. It now packs with `variant_to_vh` — the same
function a method argument goes through — over a `VerseArena` the GDExtension owns, since that is the
one direction where the host does not supply the arena. Which types a member can *hold* is the host's
answer anyway; a second, narrower list on this side could only disagree with it.

### Arrays

An array becomes the packed container Godot has for its element, and a plain `Array` where it has
none: `[]float` → `PackedFloat64Array`, `[]int` → `PackedInt64Array`, `[]string` →
`PackedStringArray`, `[]vector2`/`[]vector3`/`[]color` → the matching packed array, `[]logic` →
`Array`. The int case is deliberately the 64-bit one — a Verse int is 64 bits wide, and
`PackedInt32Array` would silently discard the top half of a value the language accepts.

Only `[]logic` needs anything said about its element, which is why `vh_export_desc` grew one field
rather than a hint: a packed array's own tag says what it holds, and a plain `Array` told nothing
gives the author an editor that adds rows of anything, which the host then refuses the whole member
for. `PROPERTY_HINT_TYPE_STRING` with the element's Variant type is Godot's spelling for it.

**Arrays of references are refused, and for the same reason a bare reference is.** `[]node2d` cannot
hold the empty element an array editor starts a new row as, so the correct spelling would be
`[]?node2d` — an array of options, which is another layer of marshalling and a different question
about what an unfilled row means. Left refused, which is a supported state: the author gets a warning
rather than a missing property.

**An element's mutability follows its container's, and getting that wrong is fatal rather than
merely wrong.** Reading a `var` container hands out an immutable snapshot, and
`VMutableArray::FreezeImpl` builds one by freezing each element in turn — so every element has to be
freezable. A `VArray` is not: every `VArrayBase` constructor calls `SetIsDeeplyMutable`
(`VVMArrayBase.h:288-370`) and nothing ever clears it, while `VArray` has no `FreezeImpl` override. So
freezing a `VArray` is not the no-op it reads as; it is `VCell subtype 'VArray' without FreezeImpl
override called!` and the process is gone. The elements of a `var []string` are therefore
`VMutableArray`s, and the elements of a plain `[]string` are `VArray`s.

That cost a build cycle and was worth recording for the shape of the bug as much as the fix: the ABI
round-trip passed, the Verse-side read of *another* array passed, and only the one array whose
elements were cells died — inside the interpreter, three assertions later. The fixture reads each
container from Verse through a function of its own for exactly that reason. One oracle for all five
said only that something was wrong.

### Enums

The ordinal, which is what GDScript and C# store, with `PROPERTY_HINT_ENUM` listing the enumerators in
declaration order — so the stored number indexes into that order, and reordering a Verse enum silently
reinterprets every scene already saved. That trap is inherited deliberately: the alternative is storing
the name, which no other Godot language does and which would make a renamed enumerator the breaking
change instead.

The value in the slot is a `VEnumerator` cell, not a number. So the read answers before the `IsInt`
test ever sees it, and the write takes the enumeration from the enumerator already there
(`VEnumerator::GetEnumeration`) and asks it for the one at the ordinal. Both of those were worth
*checking* rather than assuming: an earlier version wrote a plain `VInt`, and the ABI round-trip passed
on it — the read agreed with the write, exactly as the string-representation bug did, and only
comparing the member against `mode.Running` from Verse disagreed.

The bound on the ordinal comes from the declared enum rather than from `VEnumeration::NumEnumerators`,
which is not the same number — an ordinal past the author's last enumerator was accepted when the VM's
count was the test. An out-of-range write is refused rather than clamped, because the case that
produces one is a scene saved against a longer version of the enum, and silently moving that value to
the nearest enumerator would be a wrong answer that looks like a right one.

## Roadmap

Ordered to front-load the cheap work. Bands, not estimates; item 4 is the one with real unknowns.

1. ~~**Confirm the class verse path**~~ — **done**. `/user@localhost/<stem>` is correct;
   `tests/host_smoke` now asserts it against a live program.
2. ~~**Read values + defaults**~~ — **done**. `vh_instance_get_field` and
   `vh_class_default_field` at ABI 5, covering logic, int, float and string.
3. ~~**Hints and groups**~~ — **done**. A bounded int or float, an enum and a mirrored class each
   carry their own hint off the declared type rather than off a second attribute; `@export_category`,
   `@export_group` and `@export_subgroup` mark Godot's three section depths. `@display_name` has
   no Godot counterpart and was dropped.
4. ~~**Writing values**~~ — **done**, at ABI 8, along with the constructor-vs-assignment question:
   both, split by when. An instance is *unsealed* between `vh_instantiate` and the first call into
   it, which is exactly the window Godot uses to apply a scene's stored values. Unsealed, any
   `@export` member may be written — that is initialization, and it is how a non-var gets a
   value at all. Sealed, only a `var` may be. The rule needs no new ABI call and no cooperation
   from the GDExtension: `InstanceCallVoid` sets the flag, and `Ready` is a call.
5. ~~**Type coverage**~~ — **done** for everything the plan set out, at ABI 27: an optional reference
   to a mirrored class or to a registered script class, filtered in the inspector by node or resource
   type; `vector2`/`vector3`/`color` as tagged tuples; an array as the packed container Godot has for
   its element, or an `Array` naming what it holds; and an enum as its ordinal. What is left refused is
   refused for a reason rather than for want of work: an array *of* references, because `[]node2d`
   cannot hold the empty element an array editor starts a row as; a map, which Godot has no shape for;
   and `?float`, because Godot has no option type, so it would have to become either a nullable Variant
   or a two-property pair. Each of those reaches the author as a warning on the line that declared it.
6. **Per-instance values in the editor.** A non-tool script gets a `PlaceHolderScriptInstance`,
   which holds Godot's own copy of the values and never reaches Verse. Declared defaults and
   stored overrides both display correctly through it, but an `@export` member whose value the
   script computes shows its declared default until the game runs.
7. **`get_property_list_func` on the real script instance** still returns 0. Godot falls back to
   the script's list, which is why the inspector works anyway; a per-instance list is what would
   let one node expose members another does not.
8. **A changed initializer does not change the default on screen until the editor restarts.** The
   same one-generation-per-process rule, seen from the author's chair: `ReadClassDefaultField` builds
   a transient instance of the *compiled* class to let the Verse constructor run, and that class is
   whatever the session's single `vh_compile_project` produced. Editing `= array{60.0, 120.0}` moves
   the semantic program, which is why the member's type, range and hint all refresh, and leaves the
   generated constructor alone, which is why its value does not. The placeholder is not what holds
   the stale number: `PlaceHolderScriptInstance::update` drops any stored value equal to the default
   (`core/object/script_language.cpp:748-754`), so it would show a new one the moment the VM had one.
   Nothing short of repeatable code generation fixes this, which is why it sits here rather than in
   the list above — and repeatable code generation was investigated and is not reachable from this
   side of the boundary. `README.md`'s first constraint has the finding: a `#if !WITH_EDITOR` in
   `NotifyCompiledVersePackage` decides whether a second publish pins the first publish's exports and
   asserts, three ways around it are closed, and the one that is left trades the abort for a leak.
   `IncrementalizeProjectSource` with `EBuildMode::All` is the part that does work, and narrows the
   obstacle to the two packages the host compiles at runtime.

A rejection reaches the author as a warning rather than as silence. `refresh_export_warnings` in
`src/verse_script_language.cpp` turns each refused member into a `_validate` warning on the line it
was declared on, and it runs from `poll_check` — where an analysis has just been reaped and the
host is therefore known to be idle. Asking from `_validate` instead would block the editor's thread
on whatever analysis is in flight, which is the stall `vh_check_project_begin` exists to remove.
Two of the three messages read as instructions because the author can act on them; the third says
plainly that the bridge cannot carry the type yet, since a suggestion nobody can act on is worse
than an admission.

## Current state

Implemented and covered by `tests/host_smoke` (`exports.verse` fixture): the harvest
(`HostScript.cpp` `GetClassExports`), `vh_class_export_list`, `vh_instance_get_field`,
`vh_class_default_field`, `vh_instance_set_field` and `vh_instance_set_field_instance` at ABI 27,
`VerseScript::_get_script_property_list` with type-derived hints, a class header and group headers,
`_get_property_default_value`, and the script instance's `get_func` and `set_func`.

The smoke test asserts the verse path resolves, that only `@export` members are listed, that a
`var logic` reports as a var logic and a `string` as a string, that an unmarked member stays out,
that a bounded int and float each report their range and an unbounded one carries none, that a
bound on one side only reports just that side, that `@export_category`/`@export_group`/
`@export_subgroup` mark their entries and an unhinted member carries none, that defaults read back
correctly, that an absent member reports not-found rather than asserting, that a non-var can be
initialized before the instance seals and not after, that a refused write leaves the value alone,
that a bare Godot reference and an option around a non-reference are both rejected — with the
reason and the declaration's line — and, the assertion that matters, that Verse can read and
assign each member afterwards.

For references specifically: that a handle round-trips as a handle and a null as the empty option
while a `logic` false still reads as false, that a script-class member refuses a handle and takes an
instance, that a mirrored one takes either, that a non-reference member refuses an instance, that an
unregistered script class is rejected with its own reason — and, again the one that counts, that
Verse unwraps the written option and dispatches a method on what comes out.

For structs and arrays: that each reports the Godot type to rebuild it as, that a struct round-trips
its components and is refused when the wrong number of them arrives, that every array container
round-trips including emptied and refilled, and that Verse indexes each one afterwards — through a
function per container, so a failure says which.

For enums: that one exports as an int naming its enumerators in order, that the ordinal round-trips,
that an ordinal past the last enumerator is refused and leaves the member alone, and that Verse then
compares the member against its own enumerator — the assertion a round-trip cannot make.

Properties are published `PROPERTY_USAGE_DEFAULT | SCRIPT_VARIABLE`: exported and stored, var or
not. Verified in a running Godot 4.7 as well as the harness — `demo/main.tscn` stores a `var
Speed` and a non-var `Greeting`, and both reach the script.

---

## A second class in one file: what GDScript does, measured

**Status:** the parity half is written and unbuilt — see "Where this stands" at the end. Written
after a by-hand report (`by-hand-findings.md` B19), and every claim here was **run** against Godot
4.7 rather than read out of the source alone. The citations say *why*; the measurements say *what*.

The question: a Verse file may declare any number of top-level classes, and only the one named after
the file is a script. What happens when a **second** class in a file is used as an `@export` type,
and what is the closest thing GDScript does?

### GDScript's equivalent is the inner class, and it lives on three planes

`class Inner extends Resource:` inside another script. The naming is ordinary and supported;
`class_name` is a *different* request, which the parser refuses in a class body (`Unexpected
"class_name" in class body.`). What matters is which planes the class reaches.

**Script plane — fully real.** An inner class is a `GDScript` object held as a subclass of the outer
script, reachable as `Outer.Inner`. It type-checks at compile time, and `GDScriptInstance::set`
checks the member's declared type on assignment. Measured: assigning a `ProbeInner` into a slot
declared `Inner` is **refused and the old value kept**, while the same assignment into a slot
declared `Resource` is accepted. This is the "it acts strongly typed" that makes an inner class feel
like a registered one.

**Engine plane — anonymous.** It reaches Godot as a `GDScript` with **no path and no global name**.
`ClassNode::get_global_name()` answers a name only when `outer == nullptr`
(`modules/gdscript/gdscript_parser.h:798`), so an inner class structurally cannot have one.
Everything keyed on a class *name* falls back to the native base through
`_find_narrowest_native_or_global_class` (`gdscript_parser.cpp:4592-4647`), whose `CLASS` branch
recurses to `base_type` when there is no global name. Measured, one script declaring all five:

    thing     : Inner             -> type=24 class_name='Resource'   hint=17 hint_string='Resource'
    named     : ProbeInner        -> type=24 class_name='ProbeInner' hint=17 hint_string='ProbeInner'
    plain     : Resource          -> type=24 class_name='Resource'   hint=17 hint_string='Resource'
    arr_inner : Array[Inner]      -> type=28                         hint=23 hint_string='24/17:Resource'
    arr_named : Array[ProbeInner] -> type=28                         hint=23 hint_string='24/17:ProbeInner'

`class_name` *and* `hint_string` both collapse, in the instance list and in
`get_script_property_list()` — which is what the inspector builds from — and typed arrays collapse
too. The label is genuinely lost; only the behaviour is typed.

**Disk plane — lost entirely.** The plane that neither the parser nor the property list reveals.
Packing a node whose member holds an inner-class instance and saving it writes an **empty GDScript
sub-resource**:

    [sub_resource type="GDScript" id="GDScript_4sv2c"]

    [sub_resource type="Resource" id="Resource_grgq0"]
    script = SubResource("GDScript_4sv2c")
    v = 42

No path, no source. The exported values survive as raw properties; the class does not. The same
member typed as a file-level `class_name` writes `ExtResource("res://probe_inner.gd")` and round
trips.

**So an inner class is a script-world construct Godot's resource world never learns about.** It
cannot be authored as a `.tres` and cannot be persisted. That is not worth copying, and it is the
one point where the plan below diverges from GDScript on purpose.

### Why the registration cannot simply be granted

Godot collects global classes **one per script path**, at two levels:

- `_get_global_class_name` is a per-path virtual answering a single Dictionary
  (`core/object/script_language_extension.h:754`), and
  `EditorFileSystem::_get_global_script_class(type, path)` takes one `info.name` from it
  (`editor/file_system/editor_file_system.cpp:2092`).
- `ScriptServer::add_global_class(name, base, language, path)` maps the name **back to the path**
  (`core/object/script_language.cpp:408`), so resolving a global class means loading that path and
  taking its one script class.

Two names for one path would both resolve to the same script. GDScript has the same ceiling (one
`class_name` per file) and so does C#. It is Godot's constraint, not this bridge's preference.

### Where Verse stands against that

| plane | GDScript inner class | Verse second class, before B19 | after |
| --- | --- | --- | --- |
| named, type-checked | yes | yes | yes |
| global registration | impossible | impossible | impossible |
| export hint | native base | **dangling name, ClassDB error** | native base |
| assignment type-checked | yes, measured | ABI says yes, **unverified** | yes, measured — A2 |
| serialises | no — class lost, values kept | no — bare `Resource`, values lost too | unchanged; measured in Stage C |

### The plan

**Stage A — parity.**

- **A1.** The export hint falls back to the nearest mirrored Godot class when the referenced script
  class has no registered name, rather than naming it and being refused. This is
  `_find_narrowest_native_or_global_class`'s *or*, which the bridge had been skipping.
- **A2. Done — it checks, and the plain handle path refuses in two different ways.** The wide
  picker is cosmetic, not a hole.

  `WriteFieldOf`'s `VH_VARIANT_OBJECT` branch refuses *before comparing anything* when the member's
  declared class is not a **mirrored** one (`HostScript.cpp:4162`): the host can build a Verse
  wrapper from a bare handle only for a class the mirror has, so a member typed as one of the
  project's own classes cannot be written by handle at all — and a plain `Resource` dropped into the
  widened picker is exactly that. Where the declared class *is* mirrored, the test is
  `Referenced->IsA(DeclaredClass)` (`:4171`), so a handle naming an object of the wrong class is
  refused there instead. The instance path checks the declared class against the value's own class
  (`:4308`), which is the case that was already documented.

  The second refusal is the one **`host_smoke` structurally cannot reach**, which is why A2 is an
  integration case rather than an ABI one: `SmokeGetClassOf` answers `VH_CALL_DEAD_OBJECT` for every
  handle, so `MirroredClassForHandle` finds nothing, `ObjectForHandle` falls back to the *declared*
  class, and every handle matches it by construction. Only a Godot that knows what a handle names
  can fail that test.

  `?stowaway` has a second property worth stating, because it is what makes the refusal safe rather
  than merely correct: **no Godot object can ever be a legal value for it.** `stowaway` is not the
  class named after its file, so nothing on the Godot side carries it as a script and neither path
  has a value to accept. Verse can fill the slot; the inspector cannot. The picker is a drawn
  affordance over a slot only script code can write.
- **A3.** No diagnostic for merely naming a second class in a file. That is ordinary in both
  languages.

*Done when* an integration case shows a second-class export drawing a `Resource` picker and refusing
a wrong-class assignment, in the editor run and the exported run both.

**Stage B — stop accepting a request that cannot be served. Done.** GDScript gives no syntax for
asking an inner class to register; this bridge let `@global_class` be written on a non-file-named
class and ignored it in silence, which is the defect — the limitation is Godot's and is not going
anywhere, but passing over the request without a word is the bridge's own.

Consumer-side only, as planned: no ABI change and no host rebuild.
`VerseClassDecl::inert_global_classes` carries every *other* top-level class in the file that
carries the attribute, with the row of the **attribute** rather than of the class, because the
attribute is the thing that does nothing. Six cases in `verse_class_decl_test`, godot-cpp-free and
with no Godot in the loop.

One thing the plan had wrong, and it is worth knowing before writing another diagnostic: **a
`_validate` warning never reaches a log.** It is returned to the editor's own C++, which draws it in
the gutter and the warnings panel, so *Done when `tests/coverage_diagnostic` asserts the sentence*
could not have been met by `_validate` alone — the module diagnostics it is modelled on are
`push_warning`/`push_error` from `report_name_collisions`, which is a different mechanism that
happens to produce a similar-looking sentence. So there are two reporters over **one**
`inert_global_class_message`:

- `_validate` puts it on the attribute's line, read from the buffer, so it appears and clears as the
  attribute is typed. This is what an author actually sees.
- `report_name_collisions` prints it once per build. This is the half a test can read, and it sits
  beside the other checks about what reaches Godot's one flat registry.

The scan had to stop returning early to do it: it used to stop at the file's own class, so a second
`@global_class` written *below* the script's class was invisible. `tests/coverage_diagnostic`'s
`scripts/inert_global.verse` is the fixture, and `COVERAGE_EXPLANATIONS` asserts the reason rather
than the whole sentence, the way the module ones do.

**Stage C — serialisation.** Spiked, measured, and **C1 is taken**. The spike changed two of the
things the plan assumed, so the reasoning below is the measurement rather than the prediction.

### What actually happens on save, measured

A `settings_resource` holding both kinds of reference member, saved to `.tres` and loaded back:

    [gd_resource type="Resource" script_class="SettingsResource" format=3]
    [ext_resource type="Script" path="res://scripts/settings_resource.verse" id="1_1jej0"]

    [sub_resource type="Gradient" id="Gradient_evxac"]
    offsets = PackedFloat32Array(0, 0.5, 1)
    colors = PackedColorArray(0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1)

    [sub_resource type="Resource" id="Resource_vju0x"]

    [resource]
    script = ExtResource("1_1jej0")
    Title = "spike"
    Stowaway = SubResource("Resource_vju0x")
    Palette = SubResource("Gradient_evxac")

**The mirrored member is fine.** `Palette` writes as a real `Gradient` with its data and comes back
as one — `PaletteIsSet` is still true after the reload. Nothing about reference members is broken in
general, which is the first thing the spike was for.

**The second-class member is lost, and the sub-resource written for it is empty.** No `script` line,
no `Value = 5`; `StowedValue` reads `-1` after the reload, so the option came back empty. Verse is
here slightly *behind* GDScript, which keeps the values as raw properties (`v = 42`) and loses only
the class.

### Why, which is the whole argument for C1

Not because Godot drops it. Because **a Verse object's members live in the VM, and its Godot peer
carries none of them.** What bridges the two is *being a script*: `settings_resource` round-trips
because the script is attached to the resource, so `get_property_list` and `get` reach the VM and
`ResourceSaver` sees real properties. A second class in the file can never be a script — that is
R-LANG-6, and Godot's one-global-class-per-path ceiling underneath it — so its peer is a bare
`Resource` with nothing on it, and an empty sub-resource is all there is to write.

So the rule is not a preference, and C1 is not a shrug:

> **A Verse class that is to be authored as a `.tres` or persisted inside one lives in its own
> `.verse` file.** What makes values persist is the script, and only the class named after the file
> can be one.

Stage B's warning already says exactly this at the attribute, which is where an author is standing
when they need it — *"Move `stowaway` into a file of its own to register it"*.

### C2 is possible, and it is a feature rather than a gap-closer

The spike's other job was to kill C2 cheaply, and it did not. The plan expected
`res://player.verse::my_resource` never to reach `VerseResourceFormatLoader`, because its extension
is `verse::my_resource`. Half right:

- Godot's default `recognize_path` compares the path's *suffix* against each recognized extension
  (`core/io/resource_loader.cpp:75-80`), so `.verse::my_resource` is declined — as predicted.
- But `recognize_path` tries the **`_recognize_path` virtual first** (`:62-66`), and
  `VerseResourceFormatLoader` simply does not override it. An extension may claim any path it likes.
- The *"Resource file not found"* the spike saw is the fallback after the loader loop (`:325`), not
  a gate before it.

And the connection the plan did not draw: **C2 is the serialisation fix, not merely an addressing
convenience.** A sub-resource script is exactly what the peer is missing — make `stowaway` loadable
as a `Script` and the ordinary property bridge applies, values and all.

It is still a feature with real surface, which is why it is not being built on the strength of
"possible": `_recognize_path`, `_get_resource_type`, a path the saver will write from
`resource->get_path()`, EditorFileSystem's scan, dependency tracking and `.verse.uid`. **Take it up
only if authoring parity beyond GDScript is actually wanted**; C1 is what a project needs to not lose
data, and C1 costs nothing.

### What is measured where

The `.tres` printed above came from a throwaway `SceneTree` script run against `tests/integration`
(`godot --headless --path tests/integration --script res://c2_spike.gd`): it set a `Gradient` into
`Palette`, called `Stow()`, saved, printed the file verbatim and read it back. It is not kept — four
calls, cheaper to rebuild than to trust a summary — but its *conclusions* are, as five cases in
`test_cases.gd` that run in the editor and in an export both:

- the mirrored member survives and comes back a real `Gradient`, which is the half worth guarding,
  because it is what says the loss below is specific rather than general;
- the second-class member reads back `-1`, the empty option.

That last one asserts a limitation on purpose, the way `get_global_name`'s does. **If it ever starts
passing a value back, C2 was built and this section is what to correct.**

**Recommended order:** A2, A1, B, then C1 as documentation. **All of it is done: A1, A2, B, and C
settled as C1.** C2's spike came back positive on routing, so it is possible rather than dead — but
it stays unbuilt unless authoring parity *beyond* GDScript is actually wanted, because it is a
feature with real surface and C1 is what stops data being lost.

### Where this stands

Committed: `9b6a7d0` fixed the module-qualified half of B19 — the hint carried `Gameplay/myResouce`
and now carries the leaf.

**A1 and A2 are built, tested and green.** All three host targets, both GDExtension targets and
`python tools/run_tests.py` pass; the editor run reports **356 passed, 0 failed, 0 skipped** and the
exported run **346, 0, 10**, which is `EXPORT_EXPECTED_PASSES` and `EXPORT_EXPECTED_SKIPS` as they
now read.

A1 needed no change to `cf75bc9`'s code — it was only unbuilt. `IsClassNamedAfterItsFile` and the
`VH_EXPORT_HINT_CLASS` fallback in `HostScript.cpp` do what the section above says, in an export as
well as in the editor: `Stowaway` is exported, drawn as a `PROPERTY_HINT_RESOURCE_TYPE` picker with
`hint_string` `Resource`, and ClassDB resolves that name.

A2 needed no host change either — the check was already there, on both paths and in two forms; what
was missing was a test saying so. Five cases in `test_cases.gd`, on their own `Resource` so nothing
below inherits what they wrote, and every one of them reads the slot back **through Verse**
(`StowedValue`, `PaletteIsSet`) rather than through `get()`, because a bad write and a matching bad
read agree with each other. They run in the export too.

**B is done too**, consumer-side only exactly as planned — no ABI change and no host rebuild. Six
scanner cases and three `COVERAGE_EXPLANATIONS` lines; see Stage B above for the one thing the plan
had wrong about where a `_validate` warning ends up.

**C is settled as C1**, on a spike rather than on the recommendation. The rule is written down here
and next to R-EXP-6 in `spec.md`, and it has a reason rather than a preference behind it: what makes
a value persist is the script, and only the class named after the file can be one. Stage B's warning
already says it at the attribute.

The spike did **not** kill C2, which is the one thing here worth knowing before anyone reopens it:
routing is a `_recognize_path` override away, and a sub-resource script would fix serialisation
properly rather than work around it. It is unbuilt because it is a feature, not because it is
impossible — the decision it waits on is whether authoring parity beyond GDScript is wanted at all.

**One thing C1 does not do is make the loss loud.** A script that writes a second-class member,
saves, and reloads gets an empty option back with no diagnostic anywhere. Stage B's principle would
say that is a request the bridge accepts and cannot serve; C1 as scoped is documentation only, so
this is recorded as a candidate rather than done. The cheap form is a warning from the save path
when an exported member holds a peer with no script.

The GDScript probe that produced every measurement above is four files — an outer script with an
inner class, a `class_name` script beside it, and a `SceneTree` driver — and is worth rebuilding
rather than trusting this section if a claim is load-bearing.
