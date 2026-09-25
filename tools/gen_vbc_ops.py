"""Validates docs/web-vm/ops.json and prints the digest a .vbc header carries.

    python tools/gen_vbc_ops.py            # validate; one line per failure, exit 1 on any
    python tools/gen_vbc_ops.py --digest   # print the canonical SHA-256

The digest is taken over the canonical form (sorted keys, no whitespace) so reformatting the file
does not change it and any change of fact does. The writer stamps it into every .vbc and a reader
refuses a file stamped with any other, which is what catches an engine bump that changed the op set.
"""

import argparse
import hashlib
import json
import pathlib
import sys

OPS_JSON = pathlib.Path(__file__).resolve().parent.parent / "docs" / "web-vm" / "ops.json"

CATEGORIES = {
    "arith", "failure", "move_control", "call", "task", "ref", "container", "object",
    "native_module", "inline_cache", "debug",
}
OP_KEYS = {"number", "name", "category", "emitted", "may_park", "yields", "captures", "operands"}
OPTIONAL_OP_KEYS = {"emitted_note"}
OPERAND_KEYS = {"name", "role", "kind", "variadic", "optional", "cache"}


def load(path=OPS_JSON):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def digest(schema):
    canonical = json.dumps(schema, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def validate(schema):
    errors = []
    roles = schema.get("operand_roles", {})
    kinds = schema.get("operand_kinds", {})
    enums = schema.get("enums", {})
    captures = schema.get("capture_flags", {})
    ops = schema.get("ops", [])

    width = schema.get("opcode_width_bits")
    if width != 16:
        errors.append(f"opcode_width_bits is {width}, expected 16")

    numbers = [op.get("number") for op in ops]
    if numbers != list(range(len(ops))):
        errors.append("op numbers are not 0..n-1 in file order")

    names = [op.get("name") for op in ops]
    if len(set(names)) != len(names):
        errors.append("op names are not unique")

    used_roles, used_kinds, used_captures = set(), set(), set()
    for op in ops:
        where = f"op {op.get('number')} {op.get('name')}"
        keys = set(op)
        if not OP_KEYS <= keys or keys - OP_KEYS - OPTIONAL_OP_KEYS:
            errors.append(f"{where}: keys {sorted(keys)}")
            continue
        if op["category"] not in CATEGORIES:
            errors.append(f"{where}: category {op['category']}")
        if op["category"] == "inline_cache" and op["emitted"]:
            errors.append(f"{where}: an inline-cache op marked emitted")
        if op["captures"] and not op["may_park"]:
            errors.append(f"{where}: captures without may_park")
        used_captures.update(op["captures"])
        for operand in op["operands"]:
            if set(operand) != OPERAND_KEYS:
                errors.append(f"{where}.{operand.get('name')}: keys {sorted(operand)}")
                continue
            used_roles.add(operand["role"])
            used_kinds.add(operand["kind"])
            kind = operand["kind"]
            if kind.startswith("enum:") and kind[5:] not in enums:
                errors.append(f"{where}.{operand['name']}: undefined {kind}")
            if kind.startswith("cell:") and kind[5:] not in schema.get("cell_types", []):
                errors.append(f"{where}.{operand['name']}: undeclared {kind}")

    for label, declared, used in (
        ("role", set(roles), used_roles),
        ("kind", set(kinds), used_kinds),
        ("capture flag", set(captures), used_captures),
    ):
        for name in sorted(used - declared):
            errors.append(f"{label} {name} used but not declared")
        for name in sorted(declared - used):
            errors.append(f"{label} {name} declared but not used")
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--digest", action="store_true")
    args = parser.parse_args()
    schema = load()
    if args.digest:
        print(digest(schema))
        return 0
    errors = validate(schema)
    for error in errors:
        print(f"FAIL {error}")
    if errors:
        return 1
    emitted = sum(1 for op in schema["ops"] if op["emitted"])
    print(f"ok {len(schema['ops'])} ops, {emitted} emitted, digest {digest(schema)[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
