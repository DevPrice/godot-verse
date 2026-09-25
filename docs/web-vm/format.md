# The `.vbc` container

Status: operand encodings complete against `ops.json`. Cell fields marked "confirmed by T2.1" are
provisional until the writer lands. Room: lead. Sources: the design, the dirty-room survey's prose,
`ops.json`. No VerseVM source.

`program.vbc` holds a whole compiled program: every cell reachable from the project's packages,
every procedure re-encoded op by op, and the tables a host needs to find classes by name. The cooker
writes it beside `verse_classes.json`. It is ours, it is versioned, and nothing in it is a pointer.

## 1. Primitives

All multi-byte values are little-endian.

| Name | Encoding |
| --- | --- |
| `u8` | one byte |
| `uv` | unsigned LEB128, at most 10 bytes |
| `sv` | signed: zigzag, then `uv` |
| `f64` | 8 bytes, IEEE 754 binary64 |
| `str` | `uv` byte length, then that many bytes of UTF-8. No terminator |
| `ref` | `uv` cell index **plus one**; `0` is "none" where the field allows it |
| `list<T>` | `uv` count, then that many `T` |

## 2. File layout

```
header
string table        list<str>
cell table          list<cell>
packages            list<ref>          the package cells, as roots
well-known          list<(sid role, ref)>
class index         list<class entry>
end marker          u8 = 0xE5
```

### 2.1 Header

| Field | Encoding | Meaning |
| --- | --- | --- |
| magic | 4 bytes | `VBC1` |
| format version | `uv` | **1**. A reader refuses any other with a sentence naming both |
| abi | `uv` | `VH_ABI_VERSION` of the cooker |
| host id | `str` | the cooker's build digest, the sidecar's `hostId` |
| engine commit | `str` | the engine commit the bytecode came from; informational |
| op schema digest | `str` | SHA-256 of the canonical `ops.json` the writer was generated from. A reader generated from a different `ops.json` refuses the file: this is what catches an engine bump that changed the op set |
| generation | `uv` | the sidecar's `generation` |

### 2.2 String table

Every string the file uses, once. Everywhere below, a `sid` is a `uv` index into this table. Interned
names (field names, named-argument names, module names) are strings here; the loader interns them,
so two equal `sid`s are the same interned name and equal contents are the same interned name even
across `sid`s.

## 3. Values

A value appears wherever the program holds a constant: a procedure's constant pool, an archetype
entry, a package definition, an array element.

| Tag `u8` | Payload | Value |
| --- | --- | --- |
| 0 | — | uninitialized (an archetype entry the constructor fills) |
| 1 | `sv` | an integer that fits in 32 bits signed |
| 2 | `f64` | a float |
| 3 | `u8` | a `char` (one UTF-8 code unit) |
| 4 | `uv` | a `char32` (one code point) |
| 5 | `ref` (not 0) | a cell |

Larger integers are `heap int` cells. `false`, `true` and the empty option are the `false` and
`true` cells (§4); the empty option **is** the `false` cell.

A placeholder is not a value. The file is the program **after** initialization (§7), so a bound
placeholder is an indirection the writer follows to its value, and an unbound one is a cook error
naming where it was found.

## 4. Cells

A cell is `u8` kind, then the kind's fields. Cells may refer to any cell, before or after them;
the loader allocates every cell before filling any.

| Kind | Name | Fields |
| --- | --- | --- |
| 1 | `false` | — (the loader's own singleton; also the empty option) |
| 2 | `true` | — (the loader's own singleton) |
| 3 | `builtin package` | — (the intrinsics package; the loader supplies it) |
| 4 | `name` | `sid` — an interned string as a value |
| 5 | `array` | `u8` element kind (0 empty, 1 value, 2 int32, 3 char8, 4 char32), then the elements: `list<value>` for 0 and 1 (for 0 it is the empty list), `list<sv>` for 2, `str` of the raw bytes for 3, `list<uv>` for 4 |
| 6 | `mutable array` | as `array` |
| 7 | `map` | `list<(value key, value)>`, in insertion order |
| 8 | `mutable map` | as `map` |
| 9 | `option` | `value` (a non-empty option; the empty one is `false`) |
| 10 | `heap int` | `u8` sign (0 non-negative, 1 negative), `list<u8>` magnitude, little-endian bytes |
| 11 | `rational` | `value` numerator, `value` denominator (each an int or a heap int) |
| 12 | `procedure` | §5 |
| 13 | `native procedure` | `sid` binding key (`(`*scope*`/`*decorated name*`:)Native`, `spec/natives.md` §3.2 — what the loader binds by, since the name alone is ambiguous), `sid` decorated name (what call stacks show), `uv` positional-parameter count |
| 14 | `function` | `ref` procedure or native procedure, `value` self, `ref` parent scope (0 for none). Self has three states that behave differently (`spec/calls.md` §6): uninitialized for a method not yet bound to an object, the `false` cell for a function that takes no receiver, or the receiver itself |
| 15 | `scope` | `ref` parent scope (0 for none), `list<value>` captures |
| 16 | `class` | §6 |
| 17 | `archetype` | §6 |
| 18 | `access specifier` | `u8` access level, `list<sid>` scope paths |
| 19 | `enumeration` | `sid` name, `list<ref>` enumerators in order |
| 20 | `enumerator` | `ref` enumeration, `sid` name, `uv` ordinal |
| 21–23 | reserved | unions. The feature is behind a compiler setting this project never enables (`spec/objects.md` §15), so version 1 has no union kinds and the writer refuses one by name |
| 24 | `package` | §7. A package is a cell because ops reference packages as immediates; the top-level packages list names the roots |
| 25 | `module` | `sid` verse path, `sid` name |
| 26 | `value object` | `ref` class, `list<(sid field name, value)>` — a struct or class instance held as global data. The class may be native-represented: every cook reaches one such module-level object, `(/Verse.org/Simulation:)editable_empty_message` of class `message`, so refusing them would refuse every cook. The fields are the object's slot fields, not the class's constants |
| 27 | `int type` | `value` lower bound or uninitialized, `value` upper bound or uninitialized |
| 28 | `float type` | as `int type`, with floats |
| 29 | `tuple type` | `list<value>` element types |
| 30 | `map type` | `value` key type, `value` value type |
| 31 | `simple type` | `u8` code: 0 any, 1 void, 2 comparable, 3 logic, 4 rational, 5 char, 6 char32, 7 range, 8 type, 10 generator, 11 weak_map, 13 reference, 15 concrete, 16 castable, 17 function, 18 persistable, 19 false. Five codes are built from other types and are followed by their components as values: 8 `type` (1), 10 `generator` (1), 11 `weak_map` (2: key, value), 15 `concrete` (1), 16 `castable` (1). Codes 9, 12 and 14 are unused: those types are kinds 33–35 |
| 32 | `accessor` | `list<sid>` getter names, `list<sid>` setter names; a getter taking *n* parameters is at index *n*−1, a setter taking *n* at index *n*−2, and the empty string marks an absent slot. The names are decorated method names of the same class (`spec/objects.md` §16) |
| 33 | `array type` | `value` element type |
| 34 | `option type` | `value` element type |
| 35 | `pointer type` | `value` element type |

Kinds the survey lists as rare in globals (native struct, ref, accessor, task) are not in version 1.
The writer refuses a program that holds one, naming it, and the kind is added when a fixture needs
it.

## 5. Procedures

| Field | Encoding |
| --- | --- |
| name | `sid` |
| file | `sid`, the source path as the compiler recorded it |
| flags | `uv`: bit 0 can access `epic_internal` |
| register count | `uv` |
| positional parameters | `uv` |
| named parameters | `list<(sid name, uv register)>` |
| constants | `list<value>` |
| ops | `list<op>` |
| unwind edges | `list<(uv first op, uv last op, uv landing op)>`, sorted, non-overlapping. A frame is covered when `first <= i <= last`, where `i` is **the index of the op the frame will resume at, minus one**. For every op but `Yield` that is the op the frame is stopped in (the op that suspended it, or the call op an outer frame waits in); for a frame stopped in `Yield` it is the op before its `ResumeOffset`. This is the reference VM's own rule (`spec/tasks.md` §7.3), and the writer derives `first` and `last` from it |
| locations | `list<(uv op index, uv line)>`, sorted by op index; an op's line is the last entry at or before it; line 0 means none |
| register names | `list<(uv register, sid name, uv first op, uv last op)>` |

Op positions are **op indices** within the procedure, never byte offsets. The writer converts every
label and every unwind range.

### 5.1 An op

`uv` opcode, then each operand `ops.json` lists for that opcode **whose `cache` is false**, in the
schema's order, encoded by its `kind`:

| Operand kind | Encoding |
| --- | --- |
| `register` | `uv` register index |
| `value` | `uv`: `0` for absent; otherwise `1 + (index << 1)` for a register and `1 + (index << 1 \| 1)` for a constant-pool index. The compiler leaves some operands absent that `ops.json` does not mark optional (`NewFunction`'s self and parent scope, `BeginTask`'s parent), and an absent operand reads as uninitialized |
| `value_imm` | a value (§3) |
| `cell:VUniqueString` | `ref` to a `name` cell |
| `cell:VArray` | `ref` to an `array` cell |
| `cell:VPackage` | `ref` to a `package` cell |
| `cell:VArchetype` | `ref` to an `archetype` cell |
| `cell:VProcedure` | `ref` to a `procedure` cell |
| `label` | `uv` op index |
| `live_range` | `uv` first op index, `uv` last op index, each in `[0, op count]`. `(op count, 0)` — first after last — is the empty range |
| `failure_context_id` | `uv` |
| `asset_path` | `sid` package name, `sid` asset name |
| `bool` | `u8` |
| `i32` | `sv` |
| `u32` | `uv` |
| `enum:ClassKind`, `enum:ClassFlags` | `uv` |

Then two modifiers, applied in this order. A **variadic** operand is `list<>` of its element encoding.
An operand `ops.json` marks **optional** is always a `u8` present flag, then the operand if present —
whatever its kind, and even when the kind has an absent encoding of its own. `EndTask`'s `Write` and
`Switch` are the optional registers this decides; a reader that skips the flag misreads every
`race`. An operand not marked optional never has the flag, and may still be absent through its
kind's own encoding (`value` 0, `ref` 0).

The 31 cache operands (`ops.json`'s `cache: true`) are runtime state, and a freshly compiled program
has nothing meaningful in them, so they are not written. A reader that wants the slots for its own
caching allocates them itself. The kinds `class_ref_ic` and `u64` belong only to cache operands and
so have no encoding.

The ten inline-cache opcodes never appear. The writer serializes the base op they were rewritten
from, and a reader treats an inline-cache opcode as a malformed file.

## 6. Classes and archetypes

**Class:**

| Field | Encoding |
| --- | --- |
| kind | `u8`: 0 class, 1 struct, 2 interface |
| flags | `uv`: the class flags, bit for bit, as `ops.json`'s class-flag enum numbers them |
| package | `ref` |
| relative path | `sid` |
| base name | `sid` |
| attributes | `u8` present; if present, `list<value>` attributes, then `list<uv>` attribute indices (one more index than there are attribute groups) |
| inherited | `list<ref>`: the superclass first if there is one, then interfaces, verbatim |
| archetype | `ref` |
| constructor | `ref` (a function). Its parent scope is the class scope, so the class needs no separate field for it |
| blocks | `ref`, always written: the blocks function when kind is class, 0 otherwise |
| native bound | `u8` |

`flags` are the final, derived flags (`spec/objects.md` §2.2). At engine commit `203d764` every
class and struct carries flag 4096 (`EmulateCaseInsensitiveOverrides`), script classes included,
because the compiler's version gate that would clear it never opens; only interfaces lack it. The
writer refuses a class whose engine type is not Verse-generated (an `@import_as` type); every
other class has an engine type of its own after linking, and that is not written.

**Archetype:**

| Field | Encoding |
| --- | --- |
| class | `ref`, or 0 for an instantiation-expression archetype |
| next | `ref` the superclass's archetype, or 0 |
| entries | `list<entry>` |

An **entry** is `sid` name — qualified like a decorated name, so `message`'s `DefaultText` is
`(/Verse.org/Verse/message:)DefaultText`, and code that looks a field up by its bare name must
compare the part after the last `:)` — then `ref` access specifier or 0, `value` type (uninitialized for none), `value`
(uninitialized when the constructor initializes it; may be an `accessor` cell), and `u8` flags:
the engine's eight entry-flag bits verbatim. The ones the interpreter reads are 1 (native
representation), 2 (has a default value) and 32 (`var`); `spec/objects.md` §3.3 defines all eight.

## 7. Packages, and why nothing runs at load

**The file is a snapshot of a program whose initialization has already run.** Compiling runs every
package procedure and the global initializer, and the writer walks the program afterwards, so every
definitions-table entry holds its final value and every module-level object exists with its fields.
A loader reconstitutes the cells, binds natives and runs **no** Verse: re-running initialization
would build a second class and fail to unify it with the first (`spec/modules.md` §2). The UE runtime
host, which is the differential reference, loads a cook the same way.

**Package:** `sid` name, `sid` root path, `list<(sid decorated path, value)>` definitions, in the
program's order. Each module entry is a `module` cell; each module-level object is a `value object`
cell of its Verse class.

**Well-known definitions:** `list<(sid role, ref)>`, so a loader never matches decorated keys.
Version 1 roles: `task_class`, the class of `/Verse.org/Verse`'s task objects (`spec/tasks.md`),
and `accessor_enumerator`, the one enumerator of `/Verse.org/Verse`'s `accessor` enumeration, which
every getter and setter call receives (`spec/objects.md` §14, §16).
A reader refuses a file missing a role it needs.

The built-in package is not written; the loader supplies it (`spec/modules.md` §2), including the
missing-procedure function.

## 8. Class index

What a host needs to find a class by the names it is given, since the program itself is reached by
cell:

| Field | Encoding |
| --- | --- |
| origin | `u8`: 0 script, 1 mirrored Godot class, 2 binding |
| name | `sid`: the sidecar's module-qualified key for a script class (`gameplay/player`); the Verse class name for a mirrored class (`node2d`); the Verse name for a binding |
| class | `ref` |

## 9. What a reader must refuse

Each refusal is a sentence naming the file, never a crash: wrong magic; another format version; an
`ops.json` digest other than the reader's own; a `ref` out of range; an opcode out of range or an
inline-cache opcode; one of the six opcodes version 1 does not implement — `Mod`, `MutableAdd`,
`NewMutableArrayWithCapacity` (never emitted, and computing a value whose convention is known only
from source) and the three union ops (`spec/ops.md` §13); a truncated file; a missing end marker;
bytes after it; a `ref` to a cell of the wrong kind for its field; and a register, constant, label,
live range, unwind edge or location out of its procedure's range.
