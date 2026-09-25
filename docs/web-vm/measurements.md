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
