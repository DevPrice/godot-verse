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
packages            list<package>
units               list<unit>
initializer         ref
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

A placeholder is not a value: a linked program has none, and the writer refuses to write one.

## 4. Cells

A cell is `u8` kind, then the kind's fields. Cells may refer to any cell, before or after them;
the loader allocates every cell before filling any.

| Kind | Name | Fields |
| --- | --- | --- |
| 1 | `false` | — (the loader's own singleton; also the empty option) |
| 2 | `true` | — (the loader's own singleton) |
| 3 | `builtin package` | — (the intrinsics package; the loader supplies it) |
| 4 | `name` | `sid` — an interned string as a value |
| 5 | `array` | `u8` element kind (0 empty, 1 value, 2 int32, 3 char8, 4 char32), then the elements: `list<value>`, `list<sv>`, `str` (the raw bytes), or `list<uv>` |
| 6 | `mutable array` | as `array` |
| 7 | `map` | `list<(value key, value)>`, in insertion order |
| 8 | `mutable map` | as `map` |
| 9 | `option` | `value` (a non-empty option; the empty one is `false`) |
| 10 | `heap int` | `u8` sign (0 non-negative, 1 negative), `list<u8>` magnitude, little-endian bytes |
| 11 | `rational` | `value` numerator, `value` denominator (each an int or a heap int) |
| 12 | `procedure` | §5 |
| 13 | `native procedure` | `sid` decorated name, `uv` positional-parameter count |
| 14 | `function` | `ref` procedure or native procedure, `value` self (uninitialized for none), `ref` parent scope (0 for none) |
| 15 | `scope` | `ref` parent scope (0 for none), `list<value>` captures |
| 16 | `class` | §6 |
| 17 | `archetype` | §6 |
| 18 | `access specifier` | `u8` access level, `list<sid>` scope paths |
| 19 | `enumeration` | `sid` name, `list<ref>` enumerators in order |
| 20 | `enumerator` | `ref` enumeration, `sid` name, `uv` ordinal |
| 21 | `union` | fields confirmed by T2.1 |
| 22 | `union variant` | fields confirmed by T2.1 |
| 23 | `union variant tag` | fields confirmed by T2.1 |
| 24 | `package` | §7 |
| 25 | `module` | `sid` verse path, `sid` name |
| 26 | `value object` | `ref` class, `list<(sid field name, value)>` — a struct or class instance held as global data |
| 27 | `int type` | `value` lower bound or uninitialized, `value` upper bound or uninitialized |
| 28 | `float type` | as `int type`, with floats |
| 29 | `tuple type` | `list<value>` element types |
| 30 | `map type` | `value` key type, `value` value type |
| 31 | `simple type` | `u8`: 0 any, 1 void, 2 comparable, 3 logic, 4 rational, 5 char, 6 char32, 7 range, 8 type, 9 array, 10 generator, 11 weak_map, 12 pointer, 13 reference, 14 option, 15 concrete, 16 castable, 17 function, 18 persistable, 19 false |

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
| unwind edges | `list<(uv first op, uv last op, uv landing op)>`, sorted, non-overlapping; an op is covered when `first <= index <= last` |
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
| `value` | `uv`: `index << 1` for a register, `index << 1 \| 1` for a constant-pool index |
| `value_imm` | a value (§3) |
| `cell:VUniqueString` | `ref` to a `name` cell |
| `cell:VArray` | `ref` to an `array` cell |
| `cell:VPackage` | `ref` to a `package` cell |
| `cell:VArchetype` | `ref` to an `archetype` cell |
| `cell:VProcedure` | `ref` to a `procedure` cell |
| `label` | `uv` op index |
| `live_range` | `uv` first op index, `uv` last op index |
| `failure_context_id` | `uv` |
| `asset_path` | `sid` package name, `sid` asset name |
| `bool` | `u8` |
| `i32` | `sv` |
| `u32` | `uv` |
| `enum:ClassKind`, `enum:ClassFlags` | `uv` |

Then two modifiers, applied in this order. A **variadic** operand is `list<>` of its element encoding.
An **optional** operand is a `u8` present flag, then the operand if present; an optional `ref` is
also allowed to be the `ref` 0.

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
| attributes | `list<value>`, then `list<uv>` attribute indices |
| inherited | `list<ref>`: the superclass first if there is one, then interfaces |
| archetype | `ref` |
| constructor | `ref` (a function) |
| blocks | `ref` or 0 |
| native bound | `u8` |

**Archetype:**

| Field | Encoding |
| --- | --- |
| class | `ref` |
| next | `ref` the superclass's archetype, or 0 |
| entries | `list<entry>` |

An **entry** is `sid` name, `ref` access specifier or 0, `value` type (uninitialized for none), `value`
(uninitialized when the constructor initializes it), and `uv` flags (bit 0 `var`, bit 1 native
representation; further bits as T2.1 confirms).

## 7. Packages, units and the initializer

**Package:** `sid` name, `sid` root path, `list<(sid decorated path, value)>` definitions.

**Unit:** one compilation unit, in dependency order: `list<ref>` its packages, then `ref` its package
procedure. Loading runs every unit's package procedure in order — after binding the natives its
packages declare — and each must answer the integer 42.

**Initializer:** the linker-generated global initializer procedure, run once as a task after every
unit.

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
inline-cache opcode; a truncated file; a missing end marker.
