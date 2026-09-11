# Exporting Verse properties to Godot's inspector

Research notes and roadmap, done against the UE checkout at
`C:/UnrealEngine` (read-only reference, not part of this repo). Every claim
below is cited `path:line`; where the checkout does not answer the question, that is stated
rather than guessed.

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
diagnostics — while code generation may happen once per process. Anything read from the VM would
be frozen at startup, so the export list would not refresh until the editor restarted. This is
the only view of a script's shape that can change while the editor is open.

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
5. **Type coverage** — incremental. Values cross today for logic/int/float/string only. An enum
   and a mirrored-class reference are already classified — a hint, and for the class a name — but
   held at `VH_EXPORT_UNSUPPORTED_TYPE` until a *value* can cross too, not just the type: an
   enum's ordinal and an object's handle are the next 80% and follow the existing `vh_variant_tag`
   pattern. Vector2, maps and options remain awkward on principle: Godot has no option type, so
   `?float` becomes either a nullable Variant or a two-property pair.
6. **Per-instance values in the editor.** A non-tool script gets a `PlaceHolderScriptInstance`,
   which holds Godot's own copy of the values and never reaches Verse. Declared defaults and
   stored overrides both display correctly through it, but an `@export` member whose value the
   script computes shows its declared default until the game runs.
7. **`get_property_list_func` on the real script instance** still returns 0. Godot falls back to
   the script's list, which is why the inspector works anyway; a per-instance list is what would
   let one node expose members another does not.

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
`vh_class_default_field` and `vh_instance_set_field` at ABI 8,
`VerseScript::_get_script_property_list` with type-derived hints and group headers,
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

Properties are published `PROPERTY_USAGE_DEFAULT | SCRIPT_VARIABLE`: exported and stored, var or
not. Verified in a running Godot 4.7 as well as the harness — `demo/main.tscn` stores a `var
Speed` and a non-var `Greeting`, and both reach the script.
