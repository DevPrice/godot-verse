# Phase 7.5 measurements

Numbers taken during the work, with where they came from, so §14 of the design can cite them and
nobody re-derives them.

## The `.vbc` of three projects (T2.4, from T2.1's cooks, 2026-09-24)

| Project | Size | Cells | Procedures | Ops | Script classes | Mirrored classes | Bindings |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `tests/host_smoke` | 10,971,545 B | 96,145 | 24,528 | 524,017 | 8 | 1,080 | 0 |
| `tests/integration` | 11,105,604 B | 97,572 | 24,954 | 532,569 | 32 | 1,080 | 6 |
| `dodge-the-creeps` | 10,965,985 B | 96,045 | 24,483 | 524,336 | 4 | 1,080 | 0 |

Almost all of each file is the Verse standard library and the Godot mirror; the project's own code
is a few hundred procedures at most. This is design §11 risk 5, now measured: about 11 MB before
compression, beside a 4.4 MiB extension. `--classes-file` remains the lever if it matters.

## Park risk (T2.4)

"On a register" means the op reads at least one register operand; an op reading only constants can
never meet an unbound placeholder.

| Project | May-park ops | On a register | The project's own procedures |
| --- | ---: | ---: | --- |
| `tests/host_smoke` | 166,951 | 157,009 | 72 procedures, 521 may-park (476 on a register) |
| `tests/integration` | 169,343 | 159,156 | 462 procedures, 2,794 may-park (2,520 on a register) |
| `dodge-the-creeps` | 167,114 | 157,181 | 27 procedures, 684 may-park (648 on a register) |

These are upper bounds: an op that *may* park parks only when a register it reads holds an unbound
placeholder, and `spec/unification.md` §11 argues from the compiler's refusals that ordinary code
never leaves one. Whether stage 2 of design §7.1 is needed is decided by running, not by these.

Also from the same cooks: no `NewClass` op, no union, task or native-struct cell, and every class and
struct carries flag 4096.

## Collection pauses over a tenured program (T3.11, 2026-09-25)

`verse_vm_test --gc-bench bin/vm_conformance_cook/program.vbc`: the conformance cook, 169,049
live cells after loading (everything the loader made, not only the file's 97k), collected
with nothing else live -- idle, then after bursts of dead strings. "Untenured" is T3.9's collector
as it was: every collection marks the whole program. "Tenured" is after `Heap::tenure()`, which
`Runtime::boot` now calls once the program is loaded and laid out: every survivor is permanent,
never marked or swept, and the 195 whose kind can still change after load (objects, mutable arrays
and maps) are rescanned at every collection. Two or three runs each, ranges shown; the binary built before
the change measured 11.95 ms and 6.84 ms idle, 16.89 ms and 8.61 ms after 65,536 dead cells.

| Collection | Unoptimized, untenured | Unoptimized, tenured | `/O2`, untenured | `/O2`, tenured |
| --- | ---: | ---: | ---: | ---: |
| idle | 10.9-11.3 ms | 0.02 ms | 4.2-5.3 ms | 0.01 ms |
| after 16,384 dead cells | 11.3-15.5 ms | 0.60-0.69 ms | 4.3-5.2 ms | 0.18-0.19 ms |
| after 65,536 dead cells | 13.9-14.9 ms | 1.8-2.2 ms | 7.2-7.3 ms | 1.0-1.3 ms |
| after 262,144 dead cells | 24.3-29.7 ms | 9.5-9.8 ms | 15.6-17.3 ms | 7.1-7.3 ms |
| the tenure itself (once, at boot) | | 13.5-15.2 ms | | 6.8-7.8 ms |

A collection now costs what it frees -- freeing a cell is a `delete`, which is the whole of the
tenured column -- plus whatever the running game holds that is not program data. At
`min_collect_trigger`'s 65,536 that is about 2 ms unoptimized and 1 ms optimized, under a 60 fps
frame where it used to be most of one. `python tools/build_vm_test.py --release` builds the `/O2`
binary as `bin/verse_vm_test_release.exe`.
