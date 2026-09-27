#!/usr/bin/env python3
"""Writes include/verse_host_abi_layout.h: the sizes and field offsets of every ABI struct that is
handed over as an array, as static_asserts, and the digest of them vh_init compares.

An array's element size is its stride, so a field added to one of these structs without a major
bump reads as corruption on the side that was not rebuilt rather than as a refusal
(include/verse_host_abi.h's version policy). Pinning the layout turns that into a build failure
in whichever of the two builds compiles first, and the digest turns a mismatch between two
binaries that each compiled cleanly -- different packing, or a header edited on one side -- into
a vh_init refusal that names it.

The numbers are measured, not computed: clang's record-layout dump of the real header, for every
target the CI builds for. They fall into three layouts -- 64-bit pointers; 32-bit pointers with
8-byte int64/double alignment (wasm32, Windows x86, ARM32); and 32-bit pointers with 4-byte
alignment, the i386 System V ABI of Linux and Android x86 -- and the tool refuses to write a
header if any target disagrees with the others of its class.

Run it after changing one of these structs, which is an ABI major bump anyway. It needs a clang
that can target all of them; emsdk's is found through VERSE_EMSDK or ../emsdk-4.0.11. `--check`
fails instead of writing when the header on disk is stale. Not run in CI: a hosted runner has no
emsdk, and the static_asserts are what CI exercises.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "include" / "verse_host_abi_layout.h"

# Every struct the ABI hands over as an array, and where: an arena-built sequence or argument list
# (vh_value, vh_pair), a caller's input list (vh_source_file, vh_binding_class), or a list the host
# answers with.
STRUCTS = [
    "vh_value", "vh_pair", "vh_source_file", "vh_binding_class", "vh_param_desc", "vh_method_desc",
    "vh_signal_desc", "vh_rpc_desc", "vh_static_desc", "vh_export_desc", "vh_complete_item",
    "vh_module_ref", "vh_stack_frame", "vh_debug_value", "vh_profile_row",
]

# Each class's first target is the one its numbers are written from; the rest must agree with it.
CLASSES = {
    "P8": ["x86_64-linux-gnu", "x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc", "aarch64-linux-android",
           "x86_64-linux-android", "arm64-apple-macos", "x86_64-apple-macos", "arm64-apple-ios"],
    "P4A8": ["wasm32-unknown-emscripten", "i686-pc-windows-msvc", "armv7-linux-androideabi"],
    "P4A4": ["i686-linux-gnu", "i686-linux-android"],
}


def find_clang(explicit: str | None) -> Path:
    candidates = [Path(explicit)] if explicit else []
    emsdk = os.environ.get("VERSE_EMSDK")
    if emsdk:
        candidates.append(Path(emsdk) / "upstream" / "bin" / "clang.exe")
        candidates.append(Path(emsdk) / "upstream" / "bin" / "clang")
    for root in (REPO.parent, REPO.parent.parent.parent.parent):
        candidates.append(root / "emsdk-4.0.11" / "upstream" / "bin" / "clang.exe")
        candidates.append(root / "emsdk-4.0.11" / "upstream" / "bin" / "clang")
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("[gen_abi_layout] no clang found: pass --clang or set VERSE_EMSDK")


def measure(clang: Path, target: str, probe: Path) -> dict:
    """{struct: (size, [(field, offset), ...])} for one target, from clang's layout dump."""
    completed = subprocess.run(
        [str(clang), f"--target={target}", "-ffreestanding", "-fsyntax-only", "-Xclang",
         "-fdump-record-layouts", "-I", str(REPO / "include"), str(probe)],
        capture_output=True, text=True)
    if completed.returncode != 0:
        raise SystemExit(f"[gen_abi_layout] clang failed for {target}:\n{completed.stderr}")
    layouts = {}
    for block in completed.stdout.split("*** Dumping AST Record Layout"):
        lines = [line for line in block.splitlines() if "|" in line]
        if not lines:
            continue
        head = re.match(r"\s*0 \| struct (\w+)$", lines[0])
        if not head or head.group(1) not in STRUCTS:
            continue
        fields = []
        for line in lines[1:]:
            offset, _, rest = line.partition("|")
            size = re.match(r"\s*\[sizeof=(\d+), align=(\d+)", rest)
            if size:
                layouts[head.group(1)] = (int(size.group(1)), fields)
                break
            depth = len(rest) - len(rest.lstrip(" "))
            name = rest.strip().split(" ")[-1] if not rest.rstrip().endswith(")") else ""
            # Depth 3 is a direct field; depth 5 is a member of an anonymous union at depth 3, which
            # is reachable as a field of the struct itself (vh_value's lanes).
            if depth == 3 and name:
                fields.append((name, int(offset)))
            elif depth == 5 and name and fields_anonymous(lines, line):
                fields.append((name, int(offset)))
        else:
            raise SystemExit(f"[gen_abi_layout] no size for {head.group(1)} on {target}")
    missing = [name for name in STRUCTS if name not in layouts]
    if missing:
        raise SystemExit(f"[gen_abi_layout] {target}: no layout for {missing}")
    return layouts


def fields_anonymous(lines: list, line: str) -> bool:
    """Whether the depth-5 line sits under an anonymous (unnamed) depth-3 member."""
    index = lines.index(line)
    for previous in reversed(lines[:index]):
        rest = previous.partition("|")[2]
        depth = len(rest) - len(rest.lstrip(" "))
        if depth == 3:
            return rest.rstrip().endswith(")")
        if depth < 3:
            return False
    return False


def render(by_class: dict) -> str:
    out = []
    facts = []
    for struct in STRUCTS:
        sizes = [by_class[c][struct][0] for c in CLASSES]
        out.append(f"VH_STATIC_ASSERT(sizeof({struct}) == VH_LAYOUT({sizes[0]}, {sizes[1]}, {sizes[2]}), "
                   f"\"{struct} changed size: its stride is the ABI's, so that is a major bump\");")
        facts.append(f"sizeof({struct})")
        names = [name for name, _ in by_class["P8"][struct][1]]
        for name in names:
            offsets = [dict(by_class[c][struct][1])[name] for c in CLASSES]
            out.append(f"VH_STATIC_ASSERT(VH_OFFSETOF({struct}, {name}) == VH_LAYOUT({offsets[0]}, {offsets[1]}, {offsets[2]}), "
                       f"\"{struct}::{name} moved\");")
            facts.append(f"VH_OFFSETOF({struct}, {name})")
        out.append("")
    fact_lines = ",\n".join(f"\t\t{fact}" for fact in facts)
    return TEMPLATE.format(asserts="\n".join(out).rstrip() + "\n", facts=fact_lines)


TEMPLATE = """/* Generated by tools/gen_abi_layout.py from clang's layout of include/verse_host_abi.h. Do not
 * edit by hand: rerun it after changing one of these structs, which is an ABI major bump.
 *
 * The layout of every struct the ABI hands over as an array, pinned. An element's size is the
 * stride the other side indexes by, so a change that is not a major bump is a build failure here in
 * whichever binary compiles first -- and VH_LAYOUT_DIGEST is what vh_init compares, so two binaries
 * that each compiled cleanly against different layouts are refused by name rather than read as
 * corruption. Included by verse_host_abi.h; nothing else includes it.
 */
#ifndef VERSE_HOST_ABI_LAYOUT_H
#define VERSE_HOST_ABI_LAYOUT_H

#ifdef __cplusplus
#	define VH_STATIC_ASSERT(Condition, Message) static_assert(Condition, Message)
#	define VH_ALIGNOF(Type) alignof(Type)
#	define VH_CONSTEXPR constexpr
#else
#	define VH_STATIC_ASSERT(Condition, Message) _Static_assert(Condition, Message)
#	define VH_ALIGNOF(Type) _Alignof(Type)
#	define VH_CONSTEXPR
#endif

/* MSVC's C++ offsetof is a reinterpret_cast, which a constant expression may not contain. */
#if defined(_MSC_VER) && !defined(__clang__) && defined(__cplusplus)
#	define VH_OFFSETOF(Type, Field) __builtin_offsetof(Type, Field)
#else
#	define VH_OFFSETOF(Type, Field) offsetof(Type, Field)
#endif

/* The three layouts these structs have: 64-bit pointers; 32-bit pointers with an 8-byte-aligned
 * int64 and double (wasm32, Windows x86, ARM32); and 32-bit pointers with a 4-byte-aligned one, the
 * i386 System V ABI of Linux and Android x86. vh_value holds an int64, so its alignment is the one
 * that tells the last two apart. */
#define VH_LAYOUT(P8, P4A8, P4A4) \\
	(sizeof(void*) == 8 ? (size_t)(P8) : VH_ALIGNOF(vh_value) == 8 ? (size_t)(P4A8) : (size_t)(P4A4))

{asserts}
/* FNV-1a over every size and offset above, in order. It differs between the three layouts, which is
 * right: it is compared between two binaries on one machine, never across. */
static inline VH_CONSTEXPR uint32_t vh_layout_digest(void)
{{
	const size_t Facts[] = {{
{facts}
	}};
	uint32_t Hash = 2166136261u;
	for (size_t Index = 0; Index < sizeof(Facts) / sizeof(Facts[0]); ++Index)
	{{
		Hash = (Hash ^ (uint32_t)Facts[Index]) * 16777619u;
	}}
	return Hash;
}}

#define VH_LAYOUT_DIGEST (vh_layout_digest())

#endif /* VERSE_HOST_ABI_LAYOUT_H */
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clang", default=None)
    parser.add_argument("--check", action="store_true", help="fail if the header on disk is stale")
    args = parser.parse_args()
    clang = find_clang(args.clang)

    with tempfile.TemporaryDirectory() as scratch:
        probe = Path(scratch) / "layout_probe.c"
        probe.write_text("#define VH_LAYOUT_MEASURING\n#include \"verse_host_abi.h\"\n"
                         + "".join(f"char probe_{name}[sizeof({name})];\n" for name in STRUCTS),
                         encoding="utf-8")
        by_class = {}
        for layout_class, targets in CLASSES.items():
            first = measure(clang, targets[0], probe)
            for target in targets[1:]:
                other = measure(clang, target, probe)
                if other != first:
                    differs = [name for name in STRUCTS if other[name] != first[name]]
                    raise SystemExit(f"[gen_abi_layout] {target} does not lay out {differs} the way "
                                     f"{targets[0]} does, so {layout_class} is not one layout")
            by_class[layout_class] = first
    for struct in STRUCTS:
        names = [[name for name, _ in by_class[c][struct][1]] for c in CLASSES]
        if names[1:] != names[:-1]:
            raise SystemExit(f"[gen_abi_layout] {struct} has different fields in different layouts")

    text = render(by_class)
    if args.check:
        current = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
        if current != text:
            print(f"[gen_abi_layout] {HEADER} is stale; rerun tools/gen_abi_layout.py", file=sys.stderr)
            return 1
        print(f"[gen_abi_layout] {HEADER} is current")
        return 0
    HEADER.write_text(text, encoding="utf-8", newline="\n")
    print(f"[gen_abi_layout] wrote {HEADER}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
