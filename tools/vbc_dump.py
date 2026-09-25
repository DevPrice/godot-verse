"""Reads a `.vbc` container, from docs/web-vm/format.md and docs/web-vm/ops.json alone.

This is the independent second implementation of the format (docs/web-vm/tasks.md T2.2): it never
reads the cooker's writer, so a disagreement between this and a real `.vbc` means format.md, the
writer or this reader is wrong.

    python tools/vbc_dump.py program.vbc                 # summary
    python tools/vbc_dump.py program.vbc --proc <substr> # matching procedures, op by op
    python tools/vbc_dump.py program.vbc --class <name>  # a class, its archetype chain, its entries
    python tools/vbc_dump.py program.vbc --check         # validate only; one line per problem, exit 1 on any
"""

import argparse
import pathlib
import struct
import sys
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_vbc_ops

END_MARKER = 0xE5

CELL_NAMES = {
    1: "false", 2: "true", 3: "builtin package", 4: "name", 5: "array", 6: "mutable array",
    7: "map", 8: "mutable map", 9: "option", 10: "heap int", 11: "rational", 12: "procedure",
    13: "native procedure", 14: "function", 15: "scope", 16: "class", 17: "archetype",
    18: "access specifier", 19: "enumeration", 20: "enumerator", 24: "package", 25: "module",
    26: "value object", 27: "int type", 28: "float type", 29: "tuple type", 30: "map type",
    31: "simple type", 32: "accessor", 33: "array type", 34: "option type", 35: "pointer type",
}

SIMPLE_TYPE_CODES = {
    0: "any", 1: "void", 2: "comparable", 3: "logic", 4: "rational", 5: "char", 6: "char32",
    7: "range", 8: "type", 10: "generator", 11: "weak_map", 13: "reference", 15: "concrete",
    16: "castable", 17: "function", 18: "persistable", 19: "false",
}
# format.md §4: which simple-type codes carry component values, and how many.
SIMPLE_TYPE_COMPONENT_COUNT = {8: 1, 10: 1, 11: 2, 15: 1, 16: 1}


class VbcError(Exception):
    pass


class Reader:
    def __init__(self, data, filename):
        self.data = data
        self.filename = filename
        self.pos = 0
        self.n = len(data)

    def fail(self, message):
        raise VbcError(f"{self.filename}: {message}")

    def u8(self):
        if self.pos >= self.n:
            self.fail("truncated file (expected a byte)")
        b = self.data[self.pos]
        self.pos += 1
        return b

    def uv(self):
        data = self.data
        pos = self.pos
        n = self.n
        result = 0
        shift = 0
        while True:
            if pos >= n:
                self.pos = pos
                self.fail("truncated file (expected a varint)")
            b = data[pos]
            pos += 1
            result |= (b & 0x7F) << shift
            if not (b & 0x80):
                break
            shift += 7
            if shift > 70:
                self.pos = pos
                self.fail("uv exceeds 10 bytes")
        self.pos = pos
        return result

    def sv(self):
        u = self.uv()
        return (u >> 1) ^ -(u & 1)

    def f64(self):
        if self.pos + 8 > self.n:
            self.fail("truncated file (expected an f64)")
        v = struct.unpack_from("<d", self.data, self.pos)[0]
        self.pos += 8
        return v

    def take(self, count):
        if self.pos + count > self.n:
            self.fail(f"truncated file (expected {count} bytes)")
        b = self.data[self.pos:self.pos + count]
        self.pos += count
        return b

    def str_(self):
        count = self.uv()
        return self.take(count).decode("utf-8", errors="replace")


def read_value(r):
    tag = r.u8()
    if tag == 0:
        return ("uninit",)
    if tag == 1:
        return ("int32", r.sv())
    if tag == 2:
        return ("float", r.f64())
    if tag == 3:
        return ("char", r.u8())
    if tag == 4:
        return ("char32", r.uv())
    if tag == 5:
        return ("cell", r.uv())
    r.fail(f"unknown value tag {tag} at offset {r.pos - 1}")


def read_list(r, item_fn):
    count = r.uv()
    return [item_fn(r) for _ in range(count)]


def read_list_value(r):
    return read_list(r, read_value)


def read_list_ref(r):
    return read_list(r, Reader.uv)


def read_list_sid(r):
    return read_list(r, Reader.uv)


def read_array_fields(r):
    elem_kind = r.u8()
    if elem_kind == 0 or elem_kind == 1:
        elements = read_list_value(r)
    elif elem_kind == 2:
        elements = read_list(r, Reader.sv)
    elif elem_kind == 3:
        elements = r.str_()
    elif elem_kind == 4:
        elements = read_list(r, Reader.uv)
    else:
        r.fail(f"unknown array element kind {elem_kind} at offset {r.pos - 1}")
    return {"elem_kind": elem_kind, "elements": elements}


def read_map_fields(r):
    return {"entries": read_list(r, lambda rr: (read_value(rr), read_value(rr)))}


def read_class_fields(r):
    kind = r.u8()
    flags = r.uv()
    package = r.uv()
    relative_path = r.uv()
    base_name = r.uv()
    attributes_present = r.u8()
    attributes = None
    attribute_indices = None
    if attributes_present:
        attributes = read_list_value(r)
        attribute_indices = read_list(r, Reader.uv)
    inherited = read_list_ref(r)
    archetype = r.uv()
    constructor = r.uv()
    blocks = r.uv()
    native_bound = r.u8()
    return {
        "kind": kind, "flags": flags, "package": package, "relative_path": relative_path,
        "base_name": base_name, "attributes": attributes, "attribute_indices": attribute_indices,
        "inherited": inherited, "archetype": archetype, "constructor": constructor,
        "blocks": blocks, "native_bound": native_bound,
    }


def read_archetype_entry(r):
    name_sid = r.uv()
    access = r.uv()
    type_value = read_value(r)
    default_value = read_value(r)
    entry_flags = r.u8()
    return (name_sid, access, type_value, default_value, entry_flags)


def read_archetype_fields(r):
    class_ref = r.uv()
    next_ref = r.uv()
    entries = read_list(r, read_archetype_entry)
    return {"class": class_ref, "next": next_ref, "entries": entries}


def read_simple_type_fields(r):
    code = r.u8()
    count = SIMPLE_TYPE_COMPONENT_COUNT.get(code, 0)
    components = [read_value(r) for _ in range(count)]
    return {"code": code, "components": components}


def read_package_fields(r):
    name_sid = r.uv()
    root_path_sid = r.uv()
    definitions = read_list(r, lambda rr: (rr.uv(), read_value(rr)))
    return {"name_sid": name_sid, "root_path_sid": root_path_sid, "definitions": definitions}


def read_value_object_fields(r):
    class_ref = r.uv()
    fields = read_list(r, lambda rr: (rr.uv(), read_value(rr)))
    return {"class": class_ref, "fields": fields}


# ---- ops (format.md §5.1) ----

def read_operand_base(r, kind):
    if kind == "register":
        return r.uv()
    if kind == "value":
        v = r.uv()
        if v == 0:
            return ("absent",)
        t = v - 1
        return ("const", t >> 1) if (t & 1) else ("reg", t >> 1)
    if kind == "value_imm":
        return read_value(r)
    if kind.startswith("cell:"):
        return r.uv()
    if kind == "label":
        return r.uv()
    if kind == "bool":
        return r.u8()
    if kind == "i32":
        return r.sv()
    if kind == "u32":
        return r.uv()
    if kind == "live_range":
        return (r.uv(), r.uv())
    if kind == "failure_context_id":
        return r.uv()
    if kind == "asset_path":
        return (r.uv(), r.uv())
    if kind.startswith("enum:"):
        return r.uv()
    r.fail(f"internal error: unhandled operand kind {kind!r}")


def read_operand(r, operand_schema):
    kind = operand_schema["kind"]
    if operand_schema["variadic"]:
        count = r.uv()
        return [read_operand_base(r, kind) for _ in range(count)]
    if operand_schema["optional"]:
        # format.md §5.1 is ambiguous here: it reads as if a `value` or `cell:*` (ref-encoded)
        # optional operand could skip the u8 flag and rely on its own kind's 0-means-absent
        # encoding, the way an operand ops.json does not even mark optional can be absent (e.g.
        # NewFunction's Self/ParentScope, BeginTask's Parent). Empirically that is wrong: decoding
        # EndTask (the one op pairing role=optional with kind=register) against
        # tests/integration's `RaceTwo` fixture only produces in-range registers/constants for
        # all 47 ops, matching a coherent race(A, B) implementation, when EVERY optional operand
        # -- register and value alike -- gets the flag byte unconditionally.
        present = r.u8()
        if not present:
            return ("absent",)
        return read_operand_base(r, kind)
    return read_operand_base(r, kind)


def read_op(r, ops_by_number):
    opcode = r.uv()
    if opcode >= len(ops_by_number):
        r.fail(f"opcode {opcode} out of range at offset {r.pos}")
    op_schema = ops_by_number[opcode]
    if op_schema["category"] == "inline_cache":
        r.fail(f"inline-cache opcode {opcode} ({op_schema['name']}) in a serialized op stream at offset {r.pos}")
    operands = []
    for operand_schema in op_schema["operands"]:
        if operand_schema["cache"]:
            continue
        operands.append((operand_schema["name"], operand_schema["kind"], operand_schema["role"],
                read_operand(r, operand_schema)))
    return (opcode, operands)


def read_procedure_fields(r, ops_by_number):
    name_sid = r.uv()
    file_sid = r.uv()
    flags = r.uv()
    register_count = r.uv()
    positional_parameters = r.uv()
    named_parameters = read_list(r, lambda rr: (rr.uv(), rr.uv()))
    constants = read_list_value(r)
    ops = read_list(r, lambda rr: read_op(rr, ops_by_number))
    unwind_edges = read_list(r, lambda rr: (rr.uv(), rr.uv(), rr.uv()))
    locations = read_list(r, lambda rr: (rr.uv(), rr.uv()))
    register_names = read_list(r, lambda rr: (rr.uv(), rr.uv(), rr.uv(), rr.uv()))
    return {
        "name_sid": name_sid, "file_sid": file_sid, "flags": flags,
        "register_count": register_count, "positional_parameters": positional_parameters,
        "named_parameters": named_parameters, "constants": constants, "ops": ops,
        "unwind_edges": unwind_edges, "locations": locations, "register_names": register_names,
    }


def read_cell(r, kind, ops_by_number):
    if kind in (1, 2, 3):
        return {}
    if kind == 4:
        return {"sid": r.uv()}
    if kind in (5, 6):
        return read_array_fields(r)
    if kind in (7, 8):
        return read_map_fields(r)
    if kind == 9:
        return {"value": read_value(r)}
    if kind == 10:
        sign = r.u8()
        magnitude = r.take(r.uv())
        return {"sign": sign, "magnitude": magnitude}
    if kind == 11:
        return {"numerator": read_value(r), "denominator": read_value(r)}
    if kind == 12:
        return read_procedure_fields(r, ops_by_number)
    if kind == 13:
        return {
            "binding_key_sid": r.uv(), "decorated_name_sid": r.uv(),
            "positional_parameters": r.uv(),
        }
    if kind == 14:
        return {"procedure": r.uv(), "self": read_value(r), "parent_scope": r.uv()}
    if kind == 15:
        parent = r.uv()
        return {"parent": parent, "captures": read_list_value(r)}
    if kind == 16:
        return read_class_fields(r)
    if kind == 17:
        return read_archetype_fields(r)
    if kind == 18:
        access_level = r.u8()
        return {"access_level": access_level, "scope_paths": read_list_sid(r)}
    if kind == 19:
        name_sid = r.uv()
        return {"name_sid": name_sid, "enumerators": read_list_ref(r)}
    if kind == 20:
        enumeration = r.uv()
        name_sid = r.uv()
        ordinal = r.uv()
        return {"enumeration": enumeration, "name_sid": name_sid, "ordinal": ordinal}
    if kind in (21, 22, 23):
        r.fail(f"cell kind {kind} is a reserved union kind; version 1 has no union kinds")
    if kind == 24:
        return read_package_fields(r)
    if kind == 25:
        return {"verse_path_sid": r.uv(), "name_sid": r.uv()}
    if kind == 26:
        return read_value_object_fields(r)
    if kind in (27, 28):
        return {"lower": read_value(r), "upper": read_value(r)}
    if kind == 29:
        return {"element_types": read_list_value(r)}
    if kind == 30:
        return {"key_type": read_value(r), "value_type": read_value(r)}
    if kind == 31:
        return read_simple_type_fields(r)
    if kind == 32:
        getters = read_list_sid(r)
        setters = read_list_sid(r)
        return {"getters": getters, "setters": setters}
    if kind in (33, 34, 35):
        return {"element_type": read_value(r)}
    r.fail(f"unknown cell kind {kind} at offset {r.pos - 1}")


class Program:
    pass


def load_program(path):
    data = pathlib.Path(path).read_bytes()
    r = Reader(data, str(path))
    prog = Program()

    magic = r.take(4)
    if magic != b"VBC1":
        r.fail(f"bad magic {magic!r}, expected b'VBC1'")
    version = r.uv()
    if version != 1:
        r.fail(f"format version {version}, this reader only understands version 1")
    prog.abi = r.uv()
    prog.host_id = r.str_()
    prog.engine_commit = r.str_()
    prog.op_digest = r.str_()
    prog.generation = r.uv()

    schema = gen_vbc_ops.load()
    schema_errors = gen_vbc_ops.validate(schema)
    if schema_errors:
        r.fail("this reader's own ops.json is invalid: " + "; ".join(schema_errors))
    expected_digest = gen_vbc_ops.digest(schema)
    if prog.op_digest != expected_digest:
        r.fail(
            f"op-schema digest {prog.op_digest!r} does not match this reader's ops.json "
            f"({expected_digest!r}); the file was written from a different op set")
    prog.schema = schema
    prog.ops_by_number = schema["ops"]

    prog.strings = read_list(r, Reader.str_)

    cell_count = r.uv()
    cells = [None] * cell_count
    for i in range(cell_count):
        kind = r.u8()
        cells[i] = (kind, read_cell(r, kind, prog.ops_by_number))
    prog.cells = cells

    prog.packages = read_list_ref(r)
    prog.well_known = read_list(r, lambda rr: (rr.uv(), rr.uv()))
    prog.class_index = read_list(r, lambda rr: (rr.u8(), rr.uv(), rr.uv()))

    marker = r.u8()
    if marker != END_MARKER:
        r.fail(f"missing end marker (found 0x{marker:02X} at offset {r.pos - 1}, expected 0x{END_MARKER:02X})")
    if r.pos != r.n:
        r.fail(f"{r.n - r.pos} trailing bytes after the end marker")

    return prog


# ---- lookups and formatting ----

def get_sid(prog, sid):
    if 0 <= sid < len(prog.strings):
        return prog.strings[sid]
    return f"<bad sid {sid}>"


def resolve_ref(raw):
    return None if raw == 0 else raw - 1


def decode_heap_int(sign, magnitude):
    n = int.from_bytes(magnitude, "little")
    return -n if sign else n


def describe_cell_detail(prog, idx):
    kind, fields = prog.cells[idx]
    if kind == 4:
        return repr(get_sid(prog, fields["sid"]))
    if kind == 10:
        return str(decode_heap_int(fields["sign"], fields["magnitude"]))
    if kind == 12:
        return get_sid(prog, fields["name_sid"])
    if kind == 13:
        return get_sid(prog, fields["decorated_name_sid"])
    if kind == 16:
        return get_sid(prog, fields["base_name"])
    if kind in (19, 20, 24, 25):
        return get_sid(prog, fields["name_sid"])
    if kind in (5, 6):
        n = len(fields["elements"])
        return f"len {n}"
    if kind == 31:
        return SIMPLE_TYPE_CODES.get(fields["code"], str(fields["code"]))
    return None


def describe_ref(prog, raw):
    if raw == 0:
        return "none"
    idx = raw - 1
    if idx < 0 or idx >= len(prog.cells):
        return f"<ref {raw} out of range>"
    kind, _ = prog.cells[idx]
    name = CELL_NAMES.get(kind, f"kind {kind}")
    detail = describe_cell_detail(prog, idx)
    return f"cell#{idx} ({name}{': ' + detail if detail else ''})"


def format_value(prog, value):
    tag = value[0]
    if tag == "uninit":
        return "<uninitialized>"
    if tag == "int32":
        return str(value[1])
    if tag == "float":
        return repr(value[1])
    if tag == "char":
        c = value[1]
        return f"'{chr(c)}'" if 32 <= c < 127 else f"'\\x{c:02x}'"
    if tag == "char32":
        cp = value[1]
        try:
            ch = chr(cp)
            return f"'{ch}' (U+{cp:04X})" if ch.isprintable() else f"U+{cp:04X}"
        except ValueError:
            return f"U+{cp:04X}"
    if tag == "cell":
        return describe_ref(prog, value[1])
    return str(value)


def format_enum(prog, enum_name, value):
    values = prog.schema["enums"].get(enum_name)
    if values is None:
        return str(value)
    if enum_name == "ClassFlags":
        names = [entry["name"] for entry in values if value & entry["value"]]
        return "|".join(names) if names else "None"
    return values[value] if 0 <= value < len(values) else str(value)


def format_operand_value(prog, kind, value):
    if value == ("absent",):
        return "<absent>"
    if kind == "register":
        return f"r{value}"
    if kind == "value":
        return f"r{value[1]}" if value[0] == "reg" else f"c{value[1]}"
    if kind == "value_imm":
        return format_value(prog, value)
    if kind.startswith("cell:"):
        return describe_ref(prog, value)
    if kind == "label":
        return f"@{value}"
    if kind == "bool":
        return "true" if value else "false"
    if kind in ("i32", "u32", "failure_context_id"):
        return str(value)
    if kind == "live_range":
        return f"[{value[0]},{value[1]}]"
    if kind == "asset_path":
        return f"{get_sid(prog, value[0])}:{get_sid(prog, value[1])}"
    if kind.startswith("enum:"):
        return format_enum(prog, kind[len("enum:"):], value)
    return str(value)


def format_operand(prog, name, kind, value):
    if isinstance(value, list):
        inner = ", ".join(format_operand_value(prog, kind, v) for v in value)
        return f"{name}=[{inner}]"
    return f"{name}={format_operand_value(prog, kind, value)}"


# ---- output modes ----

def print_summary(prog):
    kind_counts = Counter(kind for kind, _ in prog.cells)
    total_ops = 0
    opcode_hist = Counter()
    for kind, fields in prog.cells:
        if kind == 12:
            ops = fields["ops"]
            total_ops += len(ops)
            for opcode, _ in ops:
                opcode_hist[opcode] += 1

    print(f"file: abi {prog.abi}, host {prog.host_id}, engine commit {prog.engine_commit}, generation {prog.generation}")
    print(f"strings: {len(prog.strings)}")
    print(f"cells: {len(prog.cells)}")
    for kind in sorted(kind_counts):
        print(f"  {kind:3d} {CELL_NAMES.get(kind, '?'):18s} {kind_counts[kind]}")
    print(f"procedures: {kind_counts.get(12, 0)}")
    print(f"ops: {total_ops}")
    print("op histogram:")
    for opcode, count in opcode_hist.most_common():
        print(f"  {prog.ops_by_number[opcode]['name']:32s} {count}")
    print(f"packages: {len(prog.packages)}")
    print(f"well-known: {len(prog.well_known)}")
    print(f"class index: {len(prog.class_index)}")


def print_procedure(prog, idx, fields):
    print(f"procedure cell#{idx}: {get_sid(prog, fields['name_sid'])}")
    print(f"  file: {get_sid(prog, fields['file_sid'])}")
    print(
        f"  registers: {fields['register_count']}, positional params: {fields['positional_parameters']}, "
        f"flags: {fields['flags']}")
    if fields["named_parameters"]:
        params = ", ".join(f"{get_sid(prog, sid)}=r{reg}" for sid, reg in fields["named_parameters"])
        print(f"  named parameters: {params}")
    if fields["constants"]:
        print("  constants:")
        for i, c in enumerate(fields["constants"]):
            print(f"    c{i} = {format_value(prog, c)}")
    print(f"  ops ({len(fields['ops'])}):")
    for op_idx, (opcode, operands) in enumerate(fields["ops"]):
        op_name = prog.ops_by_number[opcode]["name"]
        operand_str = " ".join(format_operand(prog, name, kind, value) for name, kind, role, value in operands)
        print(f"    {op_idx:5d} {op_name:28s} {operand_str}")
    if fields["unwind_edges"]:
        print("  unwind edges:")
        for first, last, landing in fields["unwind_edges"]:
            print(f"    [{first},{last}] -> @{landing}")
    if fields["locations"]:
        print("  locations:")
        for op_index, line in fields["locations"]:
            print(f"    @{op_index}: line {line}")


def run_proc(prog, substr):
    found = 0
    for idx, (kind, fields) in enumerate(prog.cells):
        if kind == 12 and substr in get_sid(prog, fields["name_sid"]):
            print_procedure(prog, idx, fields)
            found += 1
    if not found:
        print(f"no procedure matching {substr!r}")
        return 1
    return 0


def run_class(prog, name):
    matches = [
        idx for idx, (kind, fields) in enumerate(prog.cells)
        if kind == 16 and get_sid(prog, fields["base_name"]) == name
    ]
    if not matches:
        print(f"no class named {name!r}")
        return 1
    kind_names = ["class", "struct", "interface"]
    for idx in matches:
        _, fields = prog.cells[idx]
        print(f"class cell#{idx}: {get_sid(prog, fields['base_name'])}")
        print(f"  kind: {kind_names[fields['kind']] if fields['kind'] < 3 else fields['kind']}")
        print(f"  flags: {format_enum(prog, 'ClassFlags', fields['flags'])}")
        print(f"  relative path: {get_sid(prog, fields['relative_path'])}")
        print(f"  package: {describe_ref(prog, fields['package'])}")
        print(f"  native bound: {bool(fields['native_bound'])}")
        print(f"  inherited: {[describe_ref(prog, ref) for ref in fields['inherited']]}")
        print(f"  constructor: {describe_ref(prog, fields['constructor'])}")
        print(f"  blocks: {describe_ref(prog, fields['blocks'])}")

        chain = []
        raw = fields["archetype"]
        seen = set()
        while raw != 0 and raw - 1 not in seen:
            aidx = raw - 1
            seen.add(aidx)
            chain.append(aidx)
            akind, afields = prog.cells[aidx]
            if akind != 17:
                print(f"  <archetype chain broken: cell#{aidx} is not an archetype>")
                break
            raw = afields["next"]
        for aidx in chain:
            _, afields = prog.cells[aidx]
            print(f"  archetype cell#{aidx}, class {describe_ref(prog, afields['class'])}:")
            for name_sid, access, type_value, default_value, entry_flags in afields["entries"]:
                bits = []
                if entry_flags & 1:
                    bits.append("native-repr")
                if entry_flags & 2:
                    bits.append("has-default")
                if entry_flags & 32:
                    bits.append("var")
                print(
                    f"    {get_sid(prog, name_sid)}: type={format_value(prog, type_value)} "
                    f"default={format_value(prog, default_value)} flags={'|'.join(bits) or '0'} "
                    f"access={describe_ref(prog, access)}")
    return 0


# ---- validation (format.md §9 and the class-describing invariants) ----

def validate_procedure(prog, cell_idx, fields, problems, filename):
    where_base = f"procedure cell#{cell_idx} ({get_sid(prog, fields['name_sid'])})"
    n_strings = len(prog.strings)
    n_cells = len(prog.cells)
    reg_count = fields["register_count"]
    n_consts = len(fields["constants"])
    n_ops = len(fields["ops"])

    def check_ref(raw, where):
        if raw == 0:
            return
        idx = raw - 1
        if idx < 0 or idx >= n_cells:
            problems.append(f"{filename}: {where}: ref {raw} out of range ({n_cells} cells)")

    def check_sid(sid, where):
        if sid < 0 or sid >= n_strings:
            problems.append(f"{filename}: {where}: sid {sid} out of range ({n_strings} strings)")

    def check_value(value, where):
        if value[0] == "cell":
            check_ref(value[1], where)

    check_sid(fields["name_sid"], where_base)
    check_sid(fields["file_sid"], where_base)
    for sid, reg in fields["named_parameters"]:
        check_sid(sid, where_base)
        if not (0 <= reg < reg_count):
            problems.append(f"{filename}: {where_base}: named parameter register {reg} out of range ({reg_count} registers)")
    for c in fields["constants"]:
        check_value(c, f"{where_base} constants")

    def check_operand(kind, value, op_idx, name):
        where = f"{where_base} op {op_idx} ({name})"
        if value == ("absent",):
            return
        if kind == "register":
            if not (0 <= value < reg_count):
                problems.append(f"{filename}: {where}: register {value} out of range ({reg_count} registers)")
        elif kind == "value":
            if value[0] == "reg" and not (0 <= value[1] < reg_count):
                problems.append(f"{filename}: {where}: register {value[1]} out of range ({reg_count} registers)")
            elif value[0] == "const" and not (0 <= value[1] < n_consts):
                problems.append(f"{filename}: {where}: constant index {value[1]} out of range ({n_consts} constants)")
        elif kind == "value_imm":
            check_value(value, where)
        elif kind.startswith("cell:"):
            check_ref(value, where)
        elif kind == "label":
            if not (0 <= value < n_ops):
                problems.append(f"{filename}: {where}: label {value} out of range ({n_ops} ops)")
        elif kind == "live_range":
            # Observed on ResetNonTrailed's LiveRange: (n_ops, 0), a Begin>End pair used as an
            # "empty range" sentinel, and End==n_ops as a one-past-the-end bound. Neither is
            # documented in format.md (which calls the pair "first op index, last op index"
            # without saying whether it is inclusive-inclusive or half-open, or whether Begin>End
            # is legal) -- ops.json's own field names, Begin/End, read as half-open, which is what
            # is validated here.
            begin, end = value
            if not (0 <= begin <= n_ops) or not (0 <= end <= n_ops):
                problems.append(f"{filename}: {where}: live range ({begin},{end}) out of range ({n_ops} ops)")
        elif kind == "asset_path":
            check_sid(value[0], where)
            check_sid(value[1], where)

    for op_idx, (opcode, operands) in enumerate(fields["ops"]):
        for name, kind, role, value in operands:
            if isinstance(value, list):
                for item in value:
                    check_operand(kind, item, op_idx, name)
            else:
                check_operand(kind, value, op_idx, name)

    for i, (first, last, landing) in enumerate(fields["unwind_edges"]):
        where = f"{where_base} unwind edge {i}"
        if not (0 <= first < n_ops):
            problems.append(f"{filename}: {where}: first op {first} out of range ({n_ops} ops)")
        if not (0 <= last < n_ops):
            problems.append(f"{filename}: {where}: last op {last} out of range ({n_ops} ops)")
        if not (0 <= landing < n_ops):
            problems.append(f"{filename}: {where}: landing op {landing} out of range ({n_ops} ops)")
        if first > last:
            problems.append(f"{filename}: {where}: first {first} > last {last}")

    prev = -1
    for i, (op_index, line) in enumerate(fields["locations"]):
        where = f"{where_base} location {i}"
        if not (0 <= op_index < n_ops):
            problems.append(f"{filename}: {where}: op index {op_index} out of range ({n_ops} ops)")
        if op_index < prev:
            problems.append(f"{filename}: {where}: not sorted by op index")
        prev = op_index

    for i, (register, name_sid, first_op, last_op) in enumerate(fields["register_names"]):
        where = f"{where_base} register name {i}"
        if not (0 <= register < reg_count):
            problems.append(f"{filename}: {where}: register {register} out of range ({reg_count} registers)")
        check_sid(name_sid, where)
        if not (0 <= first_op < n_ops):
            problems.append(f"{filename}: {where}: first op {first_op} out of range ({n_ops} ops)")
        if not (0 <= last_op < n_ops):
            problems.append(f"{filename}: {where}: last op {last_op} out of range ({n_ops} ops)")


def validate(prog, filename):
    problems = []
    n_strings = len(prog.strings)
    n_cells = len(prog.cells)

    def check_ref(raw, where):
        if raw == 0:
            return None
        idx = raw - 1
        if idx < 0 or idx >= n_cells:
            problems.append(f"{filename}: {where}: ref {raw} out of range ({n_cells} cells)")
            return None
        return idx

    def check_sid(sid, where):
        if sid < 0 or sid >= n_strings:
            problems.append(f"{filename}: {where}: sid {sid} out of range ({n_strings} strings)")

    def check_value(value, where):
        if value[0] == "cell":
            check_ref(value[1], where)

    for i, (kind, fields) in enumerate(prog.cells):
        where = f"cell#{i} (kind {kind})"
        if kind == 4:
            check_sid(fields["sid"], where)
        elif kind in (5, 6) and fields["elem_kind"] in (0, 1):
            for v in fields["elements"]:
                check_value(v, where)
        elif kind in (7, 8):
            for k, v in fields["entries"]:
                check_value(k, where)
                check_value(v, where)
        elif kind == 9:
            check_value(fields["value"], where)
        elif kind == 11:
            check_value(fields["numerator"], where)
            check_value(fields["denominator"], where)
        elif kind == 12:
            validate_procedure(prog, i, fields, problems, filename)
        elif kind == 13:
            check_sid(fields["binding_key_sid"], where)
            check_sid(fields["decorated_name_sid"], where)
        elif kind == 14:
            check_ref(fields["procedure"], where)
            check_value(fields["self"], where)
            check_ref(fields["parent_scope"], where)
        elif kind == 15:
            check_ref(fields["parent"], where)
            for v in fields["captures"]:
                check_value(v, where)
        elif kind == 16:
            check_ref(fields["package"], where)
            check_sid(fields["relative_path"], where)
            check_sid(fields["base_name"], where)
            if fields["attributes"] is not None:
                for v in fields["attributes"]:
                    check_value(v, where)
            for ref in fields["inherited"]:
                check_ref(ref, where)
            check_ref(fields["archetype"], where)
            check_ref(fields["constructor"], where)
            check_ref(fields["blocks"], where)
        elif kind == 17:
            check_ref(fields["class"], where)
            check_ref(fields["next"], where)
            for name_sid, access, type_value, default_value, _ in fields["entries"]:
                check_sid(name_sid, where)
                check_ref(access, where)
                check_value(type_value, where)
                check_value(default_value, where)
        elif kind == 18:
            for sid in fields["scope_paths"]:
                check_sid(sid, where)
        elif kind == 19:
            check_sid(fields["name_sid"], where)
            for ref in fields["enumerators"]:
                idx = check_ref(ref, where)
                if idx is not None and prog.cells[idx][0] != 20:
                    problems.append(f"{filename}: {where}: enumerator ref {ref} is not an enumerator cell")
        elif kind == 20:
            check_ref(fields["enumeration"], where)
            check_sid(fields["name_sid"], where)
        elif kind == 24:
            check_sid(fields["name_sid"], where)
            check_sid(fields["root_path_sid"], where)
            for name_sid, value in fields["definitions"]:
                check_sid(name_sid, where)
                check_value(value, where)
        elif kind == 25:
            check_sid(fields["verse_path_sid"], where)
            check_sid(fields["name_sid"], where)
        elif kind == 26:
            check_ref(fields["class"], where)
            for name_sid, value in fields["fields"]:
                check_sid(name_sid, where)
                check_value(value, where)
        elif kind in (27, 28):
            check_value(fields["lower"], where)
            check_value(fields["upper"], where)
        elif kind == 29:
            for v in fields["element_types"]:
                check_value(v, where)
        elif kind == 30:
            check_value(fields["key_type"], where)
            check_value(fields["value_type"], where)
        elif kind == 31:
            for v in fields["components"]:
                check_value(v, where)
        elif kind == 32:
            for sid in fields["getters"] + fields["setters"]:
                check_sid(sid, where)
        elif kind in (33, 34, 35):
            check_value(fields["element_type"], where)

    for i, raw in enumerate(prog.packages):
        idx = check_ref(raw, f"packages[{i}]")
        if idx is not None and prog.cells[idx][0] != 24:
            problems.append(f"{filename}: packages[{i}]: ref {raw} is not a package cell")

    roles = {}
    for i, (role_sid, raw) in enumerate(prog.well_known):
        where = f"well-known[{i}]"
        check_sid(role_sid, where)
        idx = check_ref(raw, where)
        if 0 <= role_sid < n_strings:
            roles[prog.strings[role_sid]] = idx
    for required in ("task_class", "accessor_enumerator"):
        if required not in roles:
            problems.append(f"{filename}: well-known role {required!r} is missing")
    if roles.get("task_class") is not None and prog.cells[roles["task_class"]][0] != 16:
        problems.append(f"{filename}: well-known role 'task_class' does not name a class cell")
    if roles.get("accessor_enumerator") is not None and prog.cells[roles["accessor_enumerator"]][0] != 20:
        problems.append(f"{filename}: well-known role 'accessor_enumerator' does not name an enumerator cell")

    for i, (origin, name_sid, raw) in enumerate(prog.class_index):
        where = f"class index[{i}]"
        if origin not in (0, 1, 2):
            problems.append(f"{filename}: {where}: unknown origin {origin}")
        check_sid(name_sid, where)
        idx = check_ref(raw, where)
        if idx is not None and prog.cells[idx][0] != 16:
            problems.append(f"{filename}: {where}: ref {raw} is not a class cell")

    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("vbc_path")
    parser.add_argument("--proc", metavar="SUBSTR", help="print matching procedures op by op")
    parser.add_argument("--class", dest="class_name", metavar="NAME", help="print a class by exact name")
    parser.add_argument("--check", action="store_true", help="validate only; one line per problem, exit 1 on any")
    args = parser.parse_args()

    try:
        prog = load_program(args.vbc_path)
    except VbcError as e:
        print(f"FAIL {e}")
        return 1

    if args.check:
        problems = validate(prog, args.vbc_path)
        for p in problems:
            print(p)
        return 1 if problems else 0

    if args.proc is not None:
        return run_proc(prog, args.proc)

    if args.class_name is not None:
        return run_class(prog, args.class_name)

    print_summary(prog)
    return 0


if __name__ == "__main__":
    sys.exit(main())
