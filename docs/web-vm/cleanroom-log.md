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
