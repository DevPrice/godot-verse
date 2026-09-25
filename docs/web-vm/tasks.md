# Phase 7.5 tasks

The task breakdown of `docs/phase-7.5-design.md` §12. Each task has a room (§3 of the design), the
model that runs it, what it depends on, and a **check**: a command or an inspection whose result
decides whether the task is done. A task is done when its check passes and its commit names the task
ID. Status is kept in this file.

Rooms: **dirty** may read VerseVM source; **clean** reads only `docs/web-vm/`, `src/`, `vm/`,
`include/`, `host/Verse/*.verse` and `docs/`; **neutral** touches neither VerseVM source nor the
interpreter; **lead** is done by the lead.

Status: `todo`, `doing`, `done`, `dropped` (with a reason).

## M0 — web toolchain proven

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T0.1 | Install Emscripten 4.0.11 beside the existing SDK (`emsdk install 4.0.11`), and record how to activate it | neutral / sonnet | — | `emcc --version` under the activated SDK prints 4.0.11 | done |
| T0.2 | `tools/run_web.py`: serve a directory on localhost with **no** COOP/COEP headers, run headless Chrome with its own `--user-data-dir` outside the repo, capture console output, stop Chrome by PID, exit on a sentinel line or a timeout | neutral / sonnet | — | runs against a static page that `console.log`s a sentinel and exits 0; exits non-zero on timeout | done |
| T0.3 | `tests/web_smoke/`: a minimal godot-cpp GDExtension and Godot project that prints a sentinel from `_ready`, exported with `web_dlink_nothreads_release` | neutral / sonnet | T0.1, T0.2 | `run_web.py` on the export sees the sentinel, served without COOP/COEP | done |
| T0.4 | `scons platform=web arch=wasm32 threads=no` builds godot-verse with the host loader compiled out on web | neutral / sonnet | T0.1 | the build succeeds and `gdextension.py` lists the web library | done |

## M1 — spec and format

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T1.1 | `docs/web-vm/ops.json`: every opcode's name, number, operand roles and kinds, and whether the compiler emits it at `203d764`. `tools/gen_vbc_ops.py` validates it | dirty / opus | — | 114 entries; the validator passes | done |
| T1.2 | Measure the three facts of design §6.1 with `tests/verse_probe` or `tests/cooked_probe`; fixtures kept in `tests/verse_probe/` | dirty / sonnet | — | each fact has a fixture and a recorded output | done |
| T1.3 | `spec/values.md` | dirty / opus | T1.2 | lead review recorded in its header | done |
| T1.4 | `spec/calls.md` and `spec/modules.md` | dirty / opus | T1.1 | lead review recorded | done |
| T1.5 | `spec/failure.md` and `spec/unification.md` | dirty / opus | T1.1 | lead review recorded | done |
| T1.6 | `spec/objects.md` | dirty / opus | T1.1 | lead review recorded | done |
| T1.7 | `spec/tasks.md` | dirty / opus | T1.1 | lead review recorded | done |
| T1.8 | `spec/natives.md` | dirty / opus | T1.1 | lead review recorded | done |
| T1.9 | `spec/ops.md`: every op ops.json marks as emitted | dirty / opus | T1.3–T1.8 | `tools/check_spec.py` finds every emitted op documented; lead review recorded | done |
| T1.10 | `spec/godot-natives.md`: the 46 Godot natives, the `variant` lanes, defer-to-commit | dirty / sonnet | — | every `<native>` in `Godot.native.verse` has a row; lead review recorded | done |
| T1.11 | `spec/sidecar.md` | lead | — | every field `WriteClassSidecar` writes has a row | done |
| T1.12 | `docs/web-vm/format.md`: the `.vbc` container | lead | T1.1 | reviewed against ops.json and the cell list of design §4 | done; union cells provisional until T2.1 |

## M2 — the cooker writes `.vbc`

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T2.1 | `host/Private/HostVbcWriter.{h,cpp}`, called from `CookMain` after `CompileProject`; `program.vbc` beside the sidecar | dirty / opus | T1.1, T1.12 | a cook of `tests/host_smoke` writes `program.vbc`; the cooker exits 0 | done |
| T2.2 | `tools/vbc_dump.py`, from `format.md` alone | clean / sonnet | T1.12 | dumps T2.1's output with no unknown op and every cell index resolving | done |
| T2.3 | `run_tests.py` abi layer: a `vbc` case over the `host_smoke` cook | neutral / sonnet | T2.1, T2.2 | `run_tests.py --only abi` passes | done |
| T2.4 | The writer's park-risk report and the `.vbc` sizes for `tests/integration` and `dodge-the-creeps`, recorded in design §14 notes | dirty / sonnet | T2.1 | numbers recorded in `measurements.md` | done |

## M3 — sequential VM

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T3.0 | `tests/vm_conformance/` fixtures and `tools/run_vm_conformance.py`: cook once, run `tests/cooked_probe` on each fixture against `verse_host_runtime.dll` and against `verse_vm.dll`, diff transcripts; the UE host's transcripts are recorded as expected output | clean / sonnet | T2.1 | runs against the UE runtime host alone and records every expected transcript | done |
| T3.1 | `vm/` skeleton; `tools/build_verse_vm.py` (the DLL exporting `vh_*`); `tools/build_vm_test.py` (unit tests); the decoder tables generated from `ops.json` by `tools/gen_vbc_ops.py`; an `em++` compile of `vm/` | clean / sonnet | T1.12 | the DLL and the test binary build natively; `vm/` compiles under `em++`; `vh_abi_version` answers through `cooked_probe` | done |
| T3.2 | Values: ints with bignum and rational, floats and their printing, chars, strings, arrays, maps, options, equality | clean / opus | T1.3, T3.1 | `verse_vm_test` value cases pass | done |
| T3.3 | Loader and ABI shell: `.vbc` into cells, natives bound by key, unbound natives stand in and raise; the sidecar reader; `vh_init`, `vh_shutdown`, `vh_has_class` and every class-describing read the sidecar answers | clean / opus | T1.11, T3.2 | `vh_init` loads all three cooks; every class read answers what the UE runtime host answers, for every `host_smoke` and `tests/integration` class | done |
| T3.4 | Interpreter core: moves, control flow, arithmetic, calls, closures, scopes, fast failure; the construction protocol (`NewObject`, the constructor, `CreateField`, `UnifyField`, `InitializeVar`, `UnifyNativeObject`, blocks); `vh_instantiate`, `vh_instance_call`, `vh_release_instance` and the `Print` native, so a fixture runs end to end | clean / opus | T1.4, T3.2, T3.3 | the conformance fixtures for these pass | done |
| T3.5 | Failure contexts and the undo log; runtime errors unwind to entry | clean / opus | T1.5, T3.4 | failure fixtures pass; undo-log unit cases pass | done |
| T3.6 | The rest of objects: casts, interfaces, accessors, native fields, `LoadFieldFromSuper`, struct equality with class constants, `vh_class_default_field` | clean / opus | T1.6, T3.4 | object and module fixtures pass | done |
| T3.7 | Unification and placeholders, stage 1: a runtime park is an error naming op and line | clean / opus | T1.5, T3.4 | unification fixtures pass | done in T3.4: unify-into-destination and the counted park error, with unit cases; no conformance fixture parks |
| T3.8 | `$BuiltIn` intrinsics and the non-concurrent Verse-library natives | clean / sonnet | T1.8, T3.4 | native fixtures pass | done |
| T3.9 | Precise collector, run after M4 so it roots suspended tasks; and move the process-lifetime state T3.8 had to leave as function-local statics in `vm_natives.cpp` (`GetSecondsSinceEpoch`'s frozen sample, the random generator) onto the `Runtime` a native can reach, and give `NativeCall` a frame walk so `CanCallerAccessEpicInternal` stops answering a hardcoded `true` | clean / opus | T4.3 | a GC-stress fixture agrees; unit cases pass; no mutable global in `vm/` but the one `Runtime` | todo |
| T3.10 | Leniency stage 2, **only if** T2.4 or a later run shows a runtime park | clean / opus | T3.7 | the parking fixtures pass | todo |

**M3 exit:** `run_vm_conformance.py --sequential` agrees on every fixture.

## M4 — concurrent VM

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T4.1 | Tasks and the task ops; `spawn`, `branch`, `sync`, `race`, `rush`; semaphores; and hidden per-object native state, which `event(t)`, `task(t)` and `classifiable_subset_var` all need (T3.8 left `classifiable_subset`'s ten natives unbound for want of it) | clean / opus | T1.7, T3.6 | concurrency fixtures pass | todo |
| T4.2 | Cancellation, unwind edges, `defer`, terminate | clean / opus | T4.1 | cancellation fixtures pass | todo |
| T4.3 | `event(t)`, `task(t)`, `Sleep` and its tick; live-variable `await` and `batch`, including element, map-value and field registration, and `set live` bindings (`MoveNonComparable`, `RefSetLive`, `CallSetLive`, `SetFieldLive` — `spec/ops.md` §3) | clean / opus | T4.1 | event, sleep and live-variable fixtures pass | todo |

**M4 exit:** `run_vm_conformance.py` agrees on every fixture.

## M5 — Windows export on the VM

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T5.1 | The rest of the runtime `vh_*` subset in `vm/` (fields, callbacks, `vh_tick`, statics, exports, signals, garbage collection), and the file reader `src/` sets over `FileAccess` | clean / opus | T1.11, M4 | a headless run of a one-script project on the vm backend prints from `_Ready` | todo |
| T5.2 | The other 45 Godot natives | clean / opus | T1.10, T5.1 | integration cases touching each native pass | todo |
| T5.3 | Backend selection: `scons verse_vm=yes`, `verse/runtime/backend`, the static fill of `VerseHostLibrary` | clean / sonnet | T5.1 | both backends run from one release build | done: the static fill is checked; a full boot waits for T5.4. Delete the test-only `debug_check_vm_backend` once T5.5 runs the vm backend end to end |
| T5.4 | Export plugin: ships `program.vbc`; omits the host DLL and `tbbmalloc.dll` on the vm backend | clean / sonnet | T5.3 | an export on the vm backend has no DLL beside the executable, and ships no `program.vbc.report.txt` (the writer puts it in the cook directory) | todo |
| T5.5 | Export layer runs on both backends with named counts; fix until green | clean / opus | T5.2, T5.4 | `run_tests.py --only export` passes on both | todo |
| T5.7 | Fix the UE host's runtime-error frames (`spec/failure.md` findings): every frame list begins with a bogus frame (path `Callstack`, function `follows:`), because Solaris's formatter adds a "Callstack follows:" header that the frame splitter reads as a frame; and, from source, a cooked runtime host may deliver no frames at all. Until fixed, the differential harness compares a runtime error's message line only. Also: the sidecar writes an uninitialised `tag` for a static's value (4522041 and 6815744 on `statics_probe`'s `MaxSpeed` and `Label`, found by T3.3) | dirty / sonnet | — | a runtime error in the editor host and in an export shows only real frames; a host_smoke case asserts it | done |
| T5.8 | Fix the UE runtime host defect T3.0 found: every method of a `task(t)` value — `Await`, `Cancel`, each phase query — segfaults `verse_host_runtime.dll` (0xC0000005), though the editor host runs them. Likely a `/Verse.org/Concurrency` class-scoped native left unbound in the cooked path, the family `CLAUDE.md` "The cooked path" records. Until fixed, `tests/vm_conformance` records no `task(t)` method, and the interpreter's `task(t)` natives are checked against `tests/verse_probe` transcripts from the editor host instead | dirty / opus | — | a `host_smoke` or conformance case calls `T.Active[]`, `T.Cancel()` and `T.Await()` on the runtime host and passes; the conformance fixtures regain them | done |
| T5.6 | Measure `spec/objects.md` §18 Q2 on the UE host: an integration case with two nodes of one script declaring `Items:godot_array = godot_array{}`, appending to one and reading the other's length, in the editor run and in the export. If the export shares one array or holds a dead reference, that is an existing defect: record it in `by-hand-findings.md` style and fix it on the UE side | neutral / sonnet | — | the case exists and its result is recorded; the vm backend matches the corrected behaviour | done on the UE host (B42); the vm backend is checked in T5.5 |

## M6 — web

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T6.1 | Web build with the VM; `web` leaves `UNREACHABLE_PLATFORMS` and requires the vm backend; `verse_data` inside the `.pck` | clean / sonnet | T0.4, T5.5 | a Web export of `tests/integration` completes | todo |
| T6.2 | The web layer in `run_tests.py`, over `run_web.py` | neutral / sonnet | T0.2, T6.1 | the layer runs and skips with a reason when Chrome is absent | todo |
| T6.3 | Fix until the web layer is green, with named counts | clean / opus | T6.2 | `run_tests.py --only web` passes | todo |
| T6.4 | `tools/run_dtc_web.py`: export `dodge-the-creeps` for Web, run its checks in headless Chrome | neutral / sonnet | T6.3 | all 30 checks pass | todo |

## M7 — close

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T7.1 | Design §14, written after the work | lead | M6 | written | todo |
| T7.2 | `spec.md` R-PLAT-3 and OQ-4, `roadmap.md` Phase 7.5, `CLAUDE.md` map entries | lead | T7.1 | updated | todo |
| T7.3 | Full `run_tests.py`, all layers | lead | T7.2 | green, every skip with a reason | todo |
