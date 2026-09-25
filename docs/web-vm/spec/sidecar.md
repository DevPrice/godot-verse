# The class sidecar, `verse_classes.json`

Status: reviewed. Room: lead. Sources read: `host/Private/HostSidecar.cpp`, the declared-type writers
in `host/Private/HostScript.cpp`, `include/verse_host_abi.h`. No VerseVM source.

The sidecar is this project's own format. The cooker writes it beside the cooked program, and a
runtime reads it instead of a semantic program: it is everything the class-describing `vh_*` reads
answer, and the declared types the bytecode does not carry. The interpreter's host reads the same
file the UE runtime host reads, unchanged.

It is UTF-8 JSON, pretty-printed, one object.

## Top level

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | number | **8**. Any other value is refused with: `<path> was written by sidecar version <n>; this host reads version 8. Re-export the project.` |
| `abi` | number | the `VH_ABI_VERSION` the cooker was built with |
| `hostId` | string | a digest of the host sources the cooker was built from |
| `engineCommit` | string | informational; never compared |
| `generation` | number | the generation number the cook published; package names include it (`GodotScripts_<n>`) |
| `packages` | array of string | the cooked package paths. The UE host mounts these; the interpreter can ignore them, since `program.vbc` names its own packages |
| `bindings` | array of object | R-INT-11's class-to-binding table, below |
| `engineSignals` | object | the payload shape of every mirrored engine-signal accessor, below. Optional |
| `classes` | object | one entry per script class, keyed by module-qualified class name (`player`, `gameplay/player`) |

A missing file is refused with `Verse data not found at <path>. The export is incomplete; export the
project again.` Invalid JSON is `<path> is not valid JSON`. The UE host also refuses a stamp
mismatch — `abi` or `hostId` differing from its own — with `This game's Verse data was cooked by a
different build of godot-verse (cooked <abi>/<hostId7>, host <abi>/<hostId7>). Export the project
again.` The interpreter keys its own staleness check on `program.vbc`'s header (`format.md`), and
applies this one to `abi` only.

## `bindings`

Each row names a generated-binding Verse class and exactly one of two keys:

| Field | Meaning |
| --- | --- |
| `verse` | the binding's Verse class name |
| `godot` | a ClassDB class name — matched against what `vh_godot_api.GetClassOf` answers for a handle |
| `script` | a script's global class name — matched against what `GetScriptClassOf` answers, because `GetClassOf` answers a script's native base |

A handle crossing into Verse is given the binding class its row names, in preference to the nearest
mirrored class. An empty array is valid.

## `engineSignals`

Deduplicated: `shapes` is an array of payload shapes, and `keys` maps `"<verse class>.<accessor>"`
(for example `"timer.Timeout"`) to an index into `shapes`. What a payload shape is: below.

## A class entry

| Field | Type | Answers |
| --- | --- | --- |
| `abstract` | bool | `vh_class_is_abstract` |
| `published` | bool | `vh_has_class`: a class the cook analysed but did not publish is absent |
| `exportsHarvested` | bool | separates "declares no `@export`" from "absent" |
| `toString` | string, optional | the decorated name of the class's `ToString` extension method (R-NODE-10), a module-level definition; `vh_instance_to_string` calls it with the instance as receiver |
| `methods` | array | `vh_class_method_list` |
| `signals` | array | `vh_class_signal_list` |
| `rpcs` | array | `vh_class_rpc_list` |
| `exports` | array | `vh_class_export_list` |
| `statics` | object, optional | `vh_class_static_list` |
| `types` | object, optional | declared types, used by every call and field access |

**Where the files are.** The cooker writes `verse_classes.json` and `program.vbc` in `verse_data`
itself, while the consumer hands `vh_init` `verse_data/Cooked` as `CookedDirUtf8`, which is where the
UE host's IoStore container is. A reader looks in `CookedDirUtf8` first and then its parent.

**The interpreter's own refusals** (the UE host has no equivalents, so these texts are ours):

| Case | Sentence |
| --- | --- |
| `abi` differs | `This game's Verse data was cooked by a different build of godot-verse (cooked <abi>/<hostId7>, host <abi>/verse_vm). Export the project again.` |
| the `.vbc` and the sidecar name different generations | `<dir>: program.vbc and verse_classes.json were written by different cooks. Export the project again.` |
| valid JSON that is not a version-8 sidecar | `<path> is not a valid class sidecar: <what>.` |

Two class reads are **not** answered from the sidecar:

- **`vh_class_default_field`** — an export's default is generated code. The host builds a transient
  instance of the class with peer minting suppressed (no Godot object is created for it or for any
  member whose initializer would mint one) and reads the field. Nothing is stored.
- **`vh_class_base_type`** — answered from the loaded program's class graph, as the UE runtime host
  answers it (measured by `tests/class_reads_probe`): a mirrored superclass in **Godot's** spelling
  (`Node2D`, through `src/verse_api_classes.h`); a binding superclass as its `bindings` row's
  `godot` or `script` name; and `VH_ERR_NOT_FOUND` for a class that is not `published`, although
  every list read still answers that class's rows with `VH_OK`.

### Method

Each maps to a `vh_method_desc` row.

| Field | Meaning |
| --- | --- |
| `name` | the Verse name |
| `decorated` | the decorated name; the key into `types.methods` and into the program |
| `params` | array of parameter descriptions |
| `required` | how many leading parameters have no default |
| `result`, `resultTag` | the result's `vh_type` and `VH_VARIANT_*` tag |
| `resultClass`, `resultClassKind` | the class a result declares, and its `vh_class_kind` |
| `canFail` | `<decides>` |
| `suspends` | `<suspends>` |
| `virtual` | the Godot virtual it overrides (`_ready`), or empty |
| `line`, `column` | where it is declared |

A **parameter description** is `name`, `type` (`vh_type`), `tag`, `default` (has a default),
`class` and `classKind`.

### Signal

`name`, `args` (parameter descriptions), `line`, `column`, `reject` (a `vh_signal_reject` code; 0
means registered) and `rejectDetail`.

### RPC

`name`, `mode`, `callLocal`, `transfer`, `channel`, `line`, `column`, `reject`, `rejectDetail`. The
numbers are Godot's `MultiplayerAPI` enum values, as `vh_rpc_desc` documents.

### Export

| Field | Meaning |
| --- | --- |
| `name` | member name |
| `type`, `tag`, `elementTag` | `vh_type`, the `VH_VARIANT_*` tag, and an array's element tag |
| `isVar` | declared `var` |
| `hint`, `hintString` | Godot's `PropertyHint` and its string |
| `nativeClass` | for an object export, the nearest class Godot can resolve |
| `rangeMin`, `rangeMax`, `hasRangeMin`, `hasRangeMax` | `@export_range` |
| `groupKind`, `groupName` | `@export_group` and friends |
| `line`, `column` | where it is declared |
| `reject` | a `vh_export_reject` code; 0 means exported |

### Statics

`{ "members": [...] }`, each member `name`, `isFunction`, `line`, `column`, and for a non-function a
`value`. A **value** is `{ "type": vh_type, "tag": VH_VARIANT_*, "v": payload }`:

| `type` | `v` |
| --- | --- |
| logic | bool |
| int | **a decimal string**, because a JSON number cannot hold every int64 |
| float | number |
| char | number (the code point) |
| string | string |
| array, tuple | array of values |
| option | a value, or absent for the empty option |
| void, ref, map | absent |

### Declared types

`types` has three objects:

| Field | Keyed by | Holds |
| --- | --- | --- |
| `members` | the member's plain name | a **member type**. Members of the class and of every script superclass up to the first non-script class, derived first, so a redeclared member resolves the way a lookup from the subclass would |
| `methods` | the method's decorated name | `{ "params": [member type...], "result": member type }` for the class's own functions |
| `signals` | the member's plain name | a **payload shape**, recorded for every `signal(t)` and `event(t)` member whether or not it is registered |

A **member type**:

| Field | Presence | Meaning |
| --- | --- | --- |
| `described` | always | an export description (the table above) of the same type — what the Godot side was told |
| `var` | only when true | the member is `var`; a write to a non-`var` member is refused |
| `ref`, `refPath`, `refOrigin`, `refOption` | a class-typed type | the Verse class name (`node2d`), its qualified path (`/Godot.org/Godot/node2d`), the origin (0 other, 1 mirrored, 2 script) and whether it was declared `?class` |
| `struct` | a mirrored math struct | its Verse name; the field layout is `GodotMathLayout.gen.h`'s |
| `enum`, `enumerators` | an enum | its decorated name and enumerator count; an ordinal crossing in is checked against the count |
| `userStruct` | a project-declared struct | `name` (decorated), and in declaration order, base first: `fieldNames` (what Godot calls each argument), `fieldKeys` (the decorated field key) and `fieldTypes` (member types) |

A **payload shape** is how a signal's payload becomes Godot arguments:

| Field | Meaning |
| --- | --- |
| `kind` | 0 bare (one argument, the payload), 1 tuple (one argument per element, named `Arg0`...), 2 struct (one per top-level field, named by the field) |
| `reject`, `rejectDetail` | why the payload cannot be carried, if it cannot |
| `whole` | a member type for the payload as a whole |
| `args` | per argument: `name`, `key` (the struct field key; empty for a tuple) and `type` (member type) |

The same shape is used in both directions: an emission takes the payload apart this way, and a
delivery back in reassembles it.
