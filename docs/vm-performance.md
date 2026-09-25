# The interpreter's performance

Phase 7.5 left speed out of scope (`phase-7.5-design.md` W-6): nothing in `vm/` was optimized, and
the one number recorded for it — `dodge-the-creeps` "1.5× slower than real time" on the web — did
not measure the game. `tools/run_dtc_web.py` started its clock before launching Chrome and stopped
it after Chrome exited, and divided that by the game's 5.8 s. Most of it was Chrome starting and
the engine, the extension and an 11 MB `program.vbc` downloading and compiling; the rest was the
game running at exactly real time, because in a browser the frame is paced to the display and
`--fixed-fps` does not make the loop run flat out. The owner played the web export by hand and saw
no slowdown, which is what started this.

This document is how the interpreter is measured now, what the code made obvious before anything
was measured, what the measurements came to, and the ranking that follows from them. §3 and §4 are
the part to read.

## 1. How it is measured

Four instruments, each built for this and each reporting rather than asserting — a threshold would
fail on a slower machine, which is `tools/build_bench.py`'s reason too.

1. **`bin/verse_vm.dll` is optimized.** `tools/build_verse_vm.py` passed `cl` no optimization flag,
   so the DLL every harness loads was `/Od`. It is `/O2 /DNDEBUG /Zi` now, with a PDB, and the
   `--wasm` objects are `-O3`. What ships was never affected: a `template_release` build is `/O2`
   on Windows and `-O3` on the web.
2. **`tools/run_vm_bench.py` — the interpreter against the UE host, and against itself as
   WebAssembly, from one cook.** `tests/vm_bench/vm_bench.verse` holds fifteen workloads, each a
   zero-argument method looping enough that the entry is noise (except `Empty`, which is the entry).
   `tests/cooked_probe --bench N` calls each once to warm up and then N times, timing each call and
   the `vh_tick` after it apart — the interpreter collects only in `vh_tick`, so an allocating
   workload pays for it there. It answers Godot with stubs (`get_position` with a real `vector2`), so
   this is Verse and the Verse half of the boundary, not Godot. `--wasm` builds the same probe with
   `vm/` linked in (`COOKED_PROBE_STATIC`) by `em++ -O3` and runs it under emsdk's Node, which is
   the V8 Chrome runs.
3. **Where a real frame's time goes.** The consumer charges every interval to the innermost of
   Verse (a call into the host, or its tick) and Godot (a callback the host made), and publishes the
   last frame's two totals as the custom monitors `verse/verse_ms` and `verse/godot_ms`. Both
   backends are measured the same way, because the split lives in `src/`. The clock starts the first
   time either monitor is read, so a game nobody measures pays nothing. `dodge-the-creeps`' checks
   print a `frame_times:` line under `-- --verse-frame-times`; `tools/run_dtc_frames.py` exports the
   game for Windows on each backend and runs it, and `tools/run_dtc_web.py` does the same in
   headless Chrome and no longer reports the process's wall time as a frame rate.
4. **Where the interpreter's own time goes.** `cooked_probe --bench N --sample` runs a sampling
   profiler in-process — a thread that suspends the benchmark one each millisecond, unwinds it with
   `RtlVirtualUnwind` into a fixed buffer (nothing that allocates may run while the heap lock could
   be held) and names the frames from the PDB afterwards. No elevation, no Visual Studio UI.
   `run_vm_bench.py --wasm-profile` writes a V8 `.cpuprofile` of the WebAssembly run, whose
   function names survive because the probe is linked with `--profiling-funcs`. `ntdll` has no
   symbols here, so its heap internals are named after the nearest export —
   `RtlUpcaseUnicodeString` in a profile is heap work.

## 2. What the code made obvious before measuring

Four read-only audits of `vm/` and of the consumer's side of the boundary, ranked by how often a
path runs times what it costs. §4 is the ranking after measuring, and it differs; this one is kept
because the evidence behind each row is still right.

| # | Change | Path | Where |
| --- | --- | --- | --- |
| 1 | `api_call_method`: cache the method's `StringName`, drop the redundant `has_method`, call through `callp` with a stack array instead of building an `Array` for `callv`; a static `StringName` for `emit_signal` | every call from Verse to Godot | `src/`, both backends |
| 2 | `vh_instance_call` resolves a class and method once: no `std::string` built and hashed per call, no linear scan of the sidecar's methods, no bound `FunctionCell`, argument buffers on the stack | every call from Godot into Verse | `vm/` |
| 3 | The script instance: static `StringName`s for `_Notification`, `_Get` and `_Set`; `VerseScript::find_method` as a hash map; a property name the script lacks answered without crossing | every notification, property access and Godot call on a scripted node | `src/`, both backends |
| 4 | A 64-bit integer without a `BigInt`: Godot object ids are above 2³¹, so each is a `HeapIntCell` owning a vector | every object handle that crosses | `vm/` |
| 5 | A Godot call's result: `make_variant` builds a 23-slot object and a fresh empty string behind a heap-allocated arena | every Godot call result | `vm/` |
| 6 | An object in one allocation instead of four | every struct, every `vector2` | `vm/` |
| 7 | A frame and its registers in one allocation, arguments in a stack buffer | every Verse-to-Verse call | `vm/` |
| 8 | A per-site cache for field and method lookup | every field access and method load | `vm/` |
| 9 | A 64-bit fast path for the bitwise natives, which loop once per bit through `BigInt` | every bitwise operation | `vm/` |
| 10 | Sleepers in a min-heap, and `tick` stops walking every instance scope for a statistic | every frame | `vm/` |

## 3. Results

Measured 2026-09-25 on an AMD Ryzen 9 9950X (16 cores) with 62 GB, Windows 11, Godot 4.7.2, and
Node 24.19 from emsdk 4.0.11.

### 3.1 `dodge-the-creeps`: there is no problem to solve

`frame_times:` from the game's own 337 checked frames, after the first ten. Native runs are headless
under `--fixed-fps`, so the loop runs flat out and a frame's time is its cost; three runs each
agreed to within 10%.

| Where | Median frame | Verse per frame, mean / max | Godot on Verse's behalf, mean |
| --- | ---: | ---: | ---: |
| Windows export, UE host | 0.091 ms | 0.090 / 0.37 ms | 0.007 ms |
| Windows export, interpreter | 0.044 ms | 0.049 / 0.45 ms | 0.006 ms |
| Web export, headless Chrome | 16.6 ms (display-paced) | 0.11 / 0.7 ms | 0.049 ms |

On the web the game holds 60 fps and Verse is under 1% of each frame; natively the interpreter runs
this game's Verse in about half the UE host's time, because the game is made of short entries and
an entry is where the interpreter is cheapest (below). The web's timer is coarsened to 0.1 ms
because the page is not cross-origin isolated, which the means average out.

### 3.2 The workloads

`run_vm_bench.py --wasm`, 20 timed calls per method, median microseconds of the call plus the tick
after it. `vm` is the native interpreter, `wasm` the same code under Node, `ue` the UE runtime host.

| Workload | vm | wasm | ue | wasm/vm | vm/ue |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Empty` (the entry) | 0.4 | 1.8 | 1.7 | 4.5× | 0.2× |
| `IntArith` | 28,047 | 23,403 | 7,912 | 0.8× | 3.5× |
| `Int64Arith` | 35,832 | 25,461 | 9,064 | 0.7× | 4.0× |
| `FloatArith` | 22,125 | 19,633 | 7,337 | 0.9× | 3.0× |
| `FunctionCalls` | 37,383 | 22,717 | 9,173 | 0.6× | 4.1× |
| `MethodCalls` | 52,119 | 30,938 | 12,613 | 0.6× | 4.1× |
| `SelfFields` | 26,287 | 22,257 | 8,447 | 0.8× | 3.1× |
| `ObjectCreation` | 124,286 | 63,167 | 32,829 | 0.5× | 3.8× |
| `VectorMath` | 301,301 | 121,963 | 60,521 | 0.4× | 5.0× |
| `Bitwise` | 10,209 | 6,006 | 930 | 0.6× | 11.0× |
| `Strings` | 4,832 | 3,483 | 8,094 | 0.7× | 0.6× |
| `Arrays` | 3,107 | 2,518 | 3,194 | 0.8× | 1.0× |
| `Maps` | 3,863 | 3,229 | 2,447 | 0.8× | 1.6× |
| `GodotReads` | 21,750 | 12,902 | 13,382 | 0.6× | 1.6× |
| `GodotWrites` | 49,872 | 33,739 | 72,320 | 0.7× | 0.7× |

What it says:

- **Running Verse, the interpreter is 3–5× slower than the UE host**; 11× on bitwise operations, for
  §2 row 9's reason exactly.
- **Entering Verse costs it a fifth of what the UE host pays**, and so do strings and writes to
  Godot. That is why §3.1 comes out the other way round: a game is made of entries.
- **The WebAssembly build is faster than the native one** — 0.4–0.9× its time on every workload that
  does real work. The gap is widest where there is allocation and collection, and the profiles say
  why: the Windows heap, and MSVC neither inlining `execute` into `drive` nor leaving out the `/GS`
  check on a function whose frame is large, where clang does both.
- **Allocation is paid late.** `VectorMath`'s own tick costs 85 ms natively — 100,000 dead
  `vector2`s swept at once — against the UE host's 0.15 ms.

### 3.3 The profiles

Share of samples, self time. Native from `cooked_probe --sample` over 100 calls per workload;
WebAssembly from V8's `.cpuprofile` over the whole run, where clang has inlined `execute` into
`drive`.

| | Dispatch (`execute`, `drive`, `read`) | Heap (`malloc`/`free`, the Windows heap) | Collection (`Heap::collect`) | Undo log and `freeze`/`melt` on a write | Field lookup (`ClassLayout::find`) | `/GS` check |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| native `IntArith` | 69% | — | — | 6% | — | 7% |
| native `MethodCalls` | 35% | 27% | 9% | 3% | 1% | — |
| native `VectorMath` | 27% | 38% | 8% | — | 3% | — |
| native `Bitwise` | 21% | 49% | — | — | — | — |
| WebAssembly, all workloads | 55% | 12% | 2% | 5% | 3% | — |

## 4. The ranking, after measuring

Nothing in §3.1 needs doing: the yardstick game spends a tenth of a millisecond per frame in Verse
on every backend. What follows is headroom for a heavier game, ordered by what the profiles and
workloads say, and it is mostly `vm/`'s core rather than the boundary §2 guessed at.

| # | Change | What it buys | Effort |
| --- | --- | --- | --- |
| 1 | **The dispatch loop**: `pc`, the op array, the operand words and the registers held in locals, re-read only where the frame changes; `execute` folded into `drive` so there is one switch and no `Step` returned through memory; a sentinel op instead of the per-op bounds check | half or more of every compute workload on both targets | M |
| 2 | **Fewer allocations per call and per object** (§2 rows 6 and 7, and `bind`'s `FunctionCell`): one allocation for a frame and its registers, arguments in a stack buffer, an object's values inline and its field names read from its layout | the heap and collection columns — a third to a half of every allocating workload | M |
| 3 | **Integers**: a 64-bit value without a `BigInt` (§2 row 4) and a 64-bit path for the bitwise natives (§2 row 9) | `Bitwise`'s 11×, `Int64Arith`'s collection, every object handle | S |
| 4 | **The undo log on a write**: `record_slot`, `freeze_value` and `melt_value` run for a local `var` in a frame nothing else can see | ~5% of every loop | M, and needs the rollback cases to stay green |
| 5 | **A per-site field cache** (§2 row 8) | 3–5% of object-heavy code | S–M |
| 6 | **The native build**: `vm/` without `/GS`, or through clang-cl | whatever of the native-over-WebAssembly gap is code generation rather than the heap | S |

§2's boundary rows (1, 2, 3 and 5) drop to the bottom: an entry already costs the interpreter a
fifth of the UE host's, and `GodotReads`/`GodotWrites` are within 1.6× of it either way. Row 1 and
row 3 are `src/` and would speed up both backends, which keeps them worth doing when a game's
profile points there — none here does.

Everything in rows 1–5 is inside `vm/`, so it is done the way Phase 7.5 was: from the spec, with the
UE tree closed. Every one of them is a generic interpreter technique; none needs to know how Epic's
VerseVM does it.
