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

## Why we cannot define our own

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
types`. The `VerseScope.InternalUser` this package builds under does not help:
`Engine/Source/Runtime/VerseCompiler/Public/uLang/Semantics/SemanticScope.h:200-203` keeps
`IsAuthoredByEpic()` distinct from the predicate that "differs from IsAuthoredByEpic by allowing
packages with Scope=InternalUser to access epic-internal definitions". InternalUser buys *access*,
which is what lets scripts say `<native>`; it does not buy authorship.

**Subclassing an existing attribute is barred by the same line.** `ValidateNonAttributeType`
(`SemanticAnalyzer.cpp:22508-22529`) tests `SemanticTypeUtils::IsAttributeType`, which is true of
the whole attribute hierarchy — so `godot_export := class(editable)` fails exactly as
`class(attribute)` does. There is no alias route.

**But authorship is a string prefix on our own package path, not a signature.**
`CScope::IsAuthoredByEpic` (`SemanticScope.cpp:796-805`) builds the scope path and asks whether it
starts with any entry of `CSemanticProgram::_EpicInternalModulePrefixes`, initialised at
`SemanticProgram.cpp:329-332` to exactly:

    /Verse.org/
    /UnrealEngine.com/
    /Fortnite.com/

The package path is ours to choose — `SetupVerse("/Godot.org/Godot", VerseScope.InternalUser)` in
`VerseHost.Build.cs`. A second VNI package declared at, say, `/UnrealEngine.com/GodotExport` would
satisfy the gate with **no compiler patch**, and could then define `godot_export` and its own hint
attributes with numeric arguments rather than SOL-972 strings.

That is not done, and the reason is not technical. It means claiming Epic's namespace for our
code: every other `IsAuthoredByEpic` call site would then treat the package as Epic's, including
the `PublicUser` sanity check at `SemanticAnalyzer.cpp:3889-3911`, and the path would be a
standing lie to anyone reading it. Epic's vocabulary covers what this needs today, so the cost is
only the borrowed name. Revisit if hint fidelity starts to matter — the change is a package
declaration plus the harvest-path constant in `GetClassExports`, not a redesign.

A related detour worth recording: a `.verse` file in a VNI-capable package must be named
`.native.verse` (`V3012`), whether or not it declares anything `<native>`. All of Epic's
attribute definitions are `.native.verse` for this reason.

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

`HasAttributeSubclass` rather than `HasAttributeClass` so that `@editable_slider(float)` and the
other `editable` subclasses count.

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
`CAttributable::GetAttributeTextValue` (same file, `302-329`) wraps it. Marked
`@HACK: SOL-972` for lack of general compile-time evaluation, but the string case is solid.

This makes an existing, reachable vocabulary useful. All of these are `@attribscope_data`,
`epic_internal` (so reachable at the `EVerseScope::InternalUser` scripts compile under, set in
`HostScript.cpp` `EnsureIde`), and take exactly one string:

| Verse | file | Godot |
| --- | --- | --- |
| `@clamp_min("0.0")` | `.../Simulation/clamp_min_attribute.native.verse:4-8` | `PROPERTY_HINT_RANGE` low |
| `@clamp_max("500.0")` | `.../Simulation/clamp_max_attribute.native.verse:4-8` | `PROPERTY_HINT_RANGE` high |
| `@category("Movement")` | `.../Simulation/category_attribute.native.verse:4-8` | `PROPERTY_USAGE_GROUP` |

The paths name the attribute *class* (`clamp_min_attribute`), not the `<constructor>` function
beside it (`clamp_min`): `GetAttributeTextValue` matches on the invocation's **return type**.

`@display_name` is deliberately not mapped. Godot's `PropertyInfo` has no display-name field --
the inspector derives a label from the property name -- so there is nowhere to put it. Renaming
the property itself would break get and set, which address a member by its real name.

Godot's range hint needs both ends, so a lone `@clamp_min` produces no hint rather than a
half-open one, and both values are validated with `String::is_valid_float` before use. The
attributes carry strings, so a typo is a missing hint at runtime, not a compile error.

**Structured attribute fields: not available.** `@editable_slider(float): MinValue := option{0.0}`
parses and applies, but reading `MinValue` back needs the general compile-time evaluation SOL-972
is waiting on. The table above is the way to carry a range instead.

**Defaults: not available here.** `CDataDefinition::HasInitializer()`
(`.../Semantics/DataDefinition.h:126-130`) says an initializer exists, not what it evaluates to.
Defaults come from an instance instead — see below.

## Values

Both directions are implemented. Several things about them were wrong on first attempt and are
worth recording, because each cost a build cycle — and the last one cost more than that.

**The attribute needs its handler module loaded.** `editable` is declared
`@customattribhandler`, so evaluating a module that applies it calls
`ICustomAttributeHandler::FindHandlerForAttribute`
(`Engine/Source/Runtime/CoreUObject/Private/VerseVM/VVMAttribute.cpp:126-133`) and fails the whole
build with `No custom handler for attribute: editable` when none is registered. The handler is
registered by `FVerseSimulationMetadataModule::StartupModule`, and in a monolithic program linking
the module does not run that. `EnsureIde` now calls
`FModuleManager::Get().LoadModule(TEXT("VerseSimulationMetadata"))`, the same shape as the
existing `IVerseModule::Get()` call.

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
    (/Godot.org/Godot/godot_object:)Handle
    (/Godot.org/Godot/godot_object:)Ready

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
3. ~~**Hints and groups**~~ — **done**. `@clamp_min`/`@clamp_max` become `PROPERTY_HINT_RANGE`
   and `@category` becomes a `PROPERTY_USAGE_GROUP` header, at ABI 6. `@display_name` has no
   Godot counterpart and was dropped.
4. ~~**Writing values**~~ — **done**, at ABI 8, along with the constructor-vs-assignment question:
   both, split by when. An instance is *unsealed* between `vh_instantiate` and the first call into
   it, which is exactly the window Godot uses to apply a scene's stored values. Unsealed, any
   `@editable` member may be written — that is initialization, and it is how a non-var gets a
   value at all. Sealed, only a `var` may be. The rule needs no new ABI call and no cooperation
   from the GDExtension: `InstanceCallVoid` sets the flag, and `Ready` is a call.
5. **Type coverage** — incremental. Today logic/int/float/string. Vector2 and object references
   are the next 80% and follow the existing `vh_variant_tag` pattern. Maps and options are
   awkward on principle: Godot has no option type, so `?float` becomes either a nullable Variant
   or a two-property pair.
6. **Per-instance values in the editor.** A non-tool script gets a `PlaceHolderScriptInstance`,
   which holds Godot's own copy of the values and never reaches Verse. Declared defaults and
   stored overrides both display correctly through it, but an `@editable` whose value the script
   computes shows its declared default until the game runs.
7. **`get_property_list_func` on the real script instance** still returns 0. Godot falls back to
   the script's list, which is why the inspector works anyway; a per-instance list is what would
   let one node expose members another does not.

## Current state

Implemented and covered by `tests/host_smoke` (`exports.verse` fixture): the harvest
(`HostScript.cpp` `GetClassExports`), `vh_class_export_list`, `vh_instance_get_field`,
`vh_class_default_field` and `vh_instance_set_field` at ABI 8,
`VerseScript::_get_script_property_list` with range hints and group headers,
`_get_property_default_value`, and the script instance's `get_func` and `set_func`.

The smoke test asserts the verse path resolves, that only `@editable` members are listed, that a
`var logic` reports as a var logic and a `string` as a string, that an unmarked member stays out,
that `@clamp_min`/`@clamp_max`/`@category` come across and an unhinted member carries none, that
defaults read back as `60.0`, `1.5` and `"hello"`, that an absent member reports not-found rather
than asserting, that a non-var can be initialized before the instance seals and not after, that a
refused write leaves the value alone, and — the assertion that matters — that Verse can read and
assign each member afterwards.

Properties are published `PROPERTY_USAGE_DEFAULT | SCRIPT_VARIABLE`: editable and stored, var or
not. Verified in a running Godot 4.7 as well as the harness — `demo/main.tscn` stores a `var
Speed` and a non-var `Greeting`, and both reach the script.
