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
| T1.3 | `spec/values.md` | dirty / opus | T1.2 | lead review recorded in its header | todo |
| T1.4 | `spec/calls.md` and `spec/modules.md` | dirty / opus | T1.1 | lead review recorded | done |
| T1.5 | `spec/failure.md` and `spec/unification.md` | dirty / opus | T1.1 | lead review recorded | todo |
| T1.6 | `spec/objects.md` | dirty / opus | T1.1 | lead review recorded | todo |
| T1.7 | `spec/tasks.md` | dirty / opus | T1.1 | lead review recorded | todo |
| T1.8 | `spec/natives.md` | dirty / opus | T1.1 | lead review recorded | todo |
| T1.9 | `spec/ops.md`: every op ops.json marks as emitted | dirty / opus | T1.3–T1.8 | `tools/check_spec.py` finds every emitted op documented; lead review recorded | todo |
| T1.10 | `spec/godot-natives.md`: the 46 Godot natives, the `variant` lanes, defer-to-commit | dirty / sonnet | — | every `<native>` in `Godot.native.verse` has a row; lead review recorded | done |
| T1.11 | `spec/sidecar.md` | lead | — | every field `WriteClassSidecar` writes has a row | done |
| T1.12 | `docs/web-vm/format.md`: the `.vbc` container | lead | T1.1 | reviewed against ops.json and the cell list of design §4 | done; union cells provisional until T2.1 |

## M2 — the cooker writes `.vbc`

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T2.1 | `host/Private/HostVbcWriter.{h,cpp}`, called from `CookMain` after `CompileProject`; `program.vbc` beside the sidecar | dirty / opus | T1.1, T1.12 | a cook of `tests/host_smoke` writes `program.vbc`; the cooker exits 0 | todo |
| T2.2 | `tools/vbc_dump.py`, from `format.md` alone | clean / sonnet | T1.12 | dumps T2.1's output with no unknown op and every cell index resolving | todo |
| T2.3 | `run_tests.py` abi layer: a `vbc` case over the `host_smoke` cook | neutral / sonnet | T2.1, T2.2 | `run_tests.py --only abi` passes | todo |
| T2.4 | The writer's park-risk report and the `.vbc` sizes for `tests/integration` and `dodge-the-creeps`, recorded in design §14 notes | dirty / sonnet | T2.1 | numbers recorded | todo |

## M3 — sequential VM

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T3.0 | `tests/vm_conformance/` fixtures and `tools/run_vm_conformance.py`: cook once, run `tests/cooked_probe` on each fixture against `verse_host_runtime.dll` and against `verse_vm.dll`, diff transcripts; the UE host's transcripts are recorded as expected output | clean / sonnet | T2.1 | runs against the UE runtime host alone and records every expected transcript | todo |
| T3.1 | `vm/` skeleton; `tools/build_verse_vm.py` (the DLL exporting `vh_*`); `tools/build_vm_test.py` (unit tests); the decoder tables generated from `ops.json` by `tools/gen_vbc_ops.py`; an `em++` compile of `vm/` | clean / sonnet | T1.12 | the DLL and the test binary build natively; `vm/` compiles under `em++`; `vh_abi_version` answers through `cooked_probe` | todo |
| T3.2 | Values: ints with bignum and rational, floats and their printing, chars, strings, arrays, maps, options, equality | clean / opus | T1.3, T3.1 | `verse_vm_test` value cases pass | todo |
| T3.3 | Loader and ABI shell: `.vbc` into cells, natives bound by name, unbound natives stand in and raise; the sidecar reader; `vh_init`, `vh_has_class`, `vh_class_method_list`, `vh_instantiate`, `vh_instance_call`, `vh_release_instance`; the `Print` native | clean / opus | T1.11, T3.1, T2.2 | `vh_init` loads all three cooks; `vh_class_method_list` answers what the UE runtime host answers for every `host_smoke` class | todo |
| T3.4 | Interpreter core: moves, control flow, arithmetic, calls, closures, scopes, fast failure | clean / opus | T1.4, T3.2, T3.3 | the conformance fixtures for these pass | todo |
| T3.5 | Failure contexts and the undo log; runtime errors unwind to entry | clean / opus | T1.5, T3.4 | failure fixtures pass; undo-log unit cases pass | todo |
| T3.6 | Objects, classes, construction, fields, overrides, interfaces, module initialization | clean / opus | T1.6, T3.4 | object and module fixtures pass | todo |
| T3.7 | Unification and placeholders, stage 1: a runtime park is an error naming op and line | clean / opus | T1.5, T3.4 | unification fixtures pass | todo |
| T3.8 | `$BuiltIn` intrinsics and the non-concurrent Verse-library natives | clean / sonnet | T1.8, T3.4 | native fixtures pass | todo |
| T3.9 | Precise collector | clean / opus | T3.6 | a GC-stress fixture agrees; unit cases pass | todo |
| T3.10 | Leniency stage 2, **only if** T2.4 or a later run shows a runtime park | clean / opus | T3.7 | the parking fixtures pass | todo |

**M3 exit:** `run_vm_conformance.py --sequential` agrees on every fixture.

## M4 — concurrent VM

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T4.1 | Tasks and the task ops; `spawn`, `branch`, `sync`, `race`, `rush`; semaphores | clean / opus | T1.7, T3.6 | concurrency fixtures pass | todo |
| T4.2 | Cancellation, unwind edges, `defer`, terminate | clean / opus | T4.1 | cancellation fixtures pass | todo |
| T4.3 | `event(t)`, `task(t)`, `Sleep` and its tick | clean / opus | T4.1 | event and sleep fixtures pass | todo |

**M4 exit:** `run_vm_conformance.py` agrees on every fixture.

## M5 — Windows export on the VM

| ID | Task | Room / model | Depends | Check | Status |
| --- | --- | --- | --- | --- | --- |
| T5.1 | The rest of the runtime `vh_*` subset in `vm/` (fields, callbacks, `vh_tick`, statics, exports, signals, garbage collection), and the file reader `src/` sets over `FileAccess` | clean / opus | T1.11, M4 | a headless run of a one-script project on the vm backend prints from `_Ready` | todo |
| T5.2 | The other 45 Godot natives | clean / opus | T1.10, T5.1 | integration cases touching each native pass | todo |
| T5.3 | Backend selection: `scons verse_vm=yes`, `verse/runtime/backend`, the static fill of `VerseHostLibrary` | clean / sonnet | T5.1 | both backends run from one release build | todo |
| T5.4 | Export plugin: ships `program.vbc`; omits the host DLL and `tbbmalloc.dll` on the vm backend | clean / sonnet | T5.3 | an export on the vm backend has no DLL beside the executable | todo |
| T5.5 | Export layer runs on both backends with named counts; fix until green | clean / opus | T5.2, T5.4 | `run_tests.py --only export` passes on both | todo |

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
