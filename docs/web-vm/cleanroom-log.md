# Clean-room log

One entry per task, appended and never rewritten: the task, the room, and what was read. The wall
is `docs/phase-7.5-design.md` §3.

| Date | Task | Room | Read | Notes |
| --- | --- | --- | --- | --- |
| 2026-09-24 | survey | dirty | VerseVM runtime, bytecode generator, codegen plugin, library `.native.verse` files | prose report to the lead; no code |
| 2026-09-24 | survey | lead-side | this repository only | no UE file opened |
| 2026-09-24 | T1.1 | dirty | bytecode generator, generated op headers, codegen plugin | produced ops.json; lead reviewed: facts only |
| 2026-09-24 | T1.2 | dirty | probe runs; interpolation lookup in the compiler | facts.md, measured |
| 2026-09-24 | T1.3 | dirty | VerseVM values, equality, float printing; probes | values.md; lead reviewed |
| 2026-09-24 | T1.4 | dirty | VerseVM interpreter, frames, codegen, assembler; this repo's HostCooked.cpp; probes | calls.md, modules.md; lead reviewed |
| 2026-09-24 | T1.5 | dirty | VerseVM interpreter, trail, transactions, runtime errors; probes | failure.md, unification.md; lead reviewed |
| 2026-09-24 | T1.6 | dirty | VerseVM classes, objects, accessors, unions; codegen; probes | objects.md; lead reviewed |
| 2026-09-24 | T1.7 | dirty | VerseVM tasks, events, content scopes; probes | tasks.md; lead reviewed |
| 2026-09-24 | T1.8 | dirty | Verse library .native.verse files and native implementations; probes | natives.md; lead reviewed |
| 2026-09-24 | T1.10 | dirty | this repo's host/Private (our own code) | godot-natives.md; lead reviewed |
| 2026-09-24 | T1.11, T1.12 | lead | this repo's HostSidecar.cpp and HostScript.cpp writers; ops.json; spec prose | sidecar.md, format.md; no VerseVM source |
| 2026-09-24 | M0 | neutral | this repo, godot-cpp, Godot's own web platform files | no VerseVM source |
| 2026-09-24 | T2.1 | dirty | VerseVM procedure, bytecode, emitter, interpreter (unwind, frames, handshake), cell headers, codegen plugin and assembler, Solaris VNI JIT; this repo's host/Private; spec/objects.md; format.md; ops.json | host/Private/HostVbcWriter.*, HostVbcOps.gen.h, tools/gen_vbc_writer.py; format proposals to the lead in prose |
| 2026-09-24 | T3.1 | clean | docs/phase-7.5-design.md, tasks.md, format.md, ops.json, facts.md; spec/calls.md, spec/godot-natives.md, spec/modules.md, spec/sidecar.md (reviewed); spec/objects.md header only (draft, not read further); CLAUDE.md, README.md; include/verse_host_abi.h; src/verse_lexer.{h,cpp}, src/verse_module_map.{h,cpp}; tools/gen_vbc_ops.py, tools/build_lexer_test.py, tools/build_cooked_probe.py, tools/build_module_map_test.py, tools/emsdk_env.py, .gitignore; tests/cooked_probe/cooked_probe.cpp, tests/verse_module_map/verse_module_map_test.cpp | vm/ skeleton (vbc_reader.{h,cpp}, vbc_ops.gen.h, vm_value.h, vm_heap.{h,cpp}, vm_interpreter.{h,cpp}, vm_file_reader.{h,cpp}, vm_abi.cpp); tools/gen_vbc_ops.py --emit-cpp; tools/build_verse_vm.py, tools/build_vm_test.py; tests/vm/verse_vm_test.cpp; CLAUDE.md row, .gitignore; no VerseVM source |
| 2026-09-24 | T1.9 | dirty | every spec file; VerseVM interpreter, references, accessors, live variables, profiling ops; probes | ops.md; lead reviewed and resolved its ten inconsistencies (§15.1) |
