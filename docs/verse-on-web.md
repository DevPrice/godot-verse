# Verse on the web

What it would cost to run Verse in a Godot **web (wasm)** export, measured rather than estimated.

This is a scoping record, not a plan. It answers **OQ-4** ("is Verse on wasm reachable at all?") and
supplies the evidence `spec.md` R-PLAT-3 has been waiting on since Phase 7 deferred the question.
Scoped 2026-09-15 against the UE6 checkout beside this repo (`Engine/Build/Build.version`:
`MajorVersion 6`, `BranchName "UE6"`) and Godot 4.7.

The scope considered throughout is **an exported game at a reduced feature level** — R-PLAT-3's
letter, where the editor is MAY and the game is SHOULD. Browser authoring was not costed; it is
strictly harder than anything below.

Four paths were examined. Three are closed, each for a different reason, and the reasons are not
cost — no budget moves any of them. The fourth is open, is larger than a web export deserves on its
own, and pays for itself elsewhere.

| path | verdict | closed by |
| --- | --- | --- |
| A. the UE host on wasm | **closed** | pointer width: wasm32 against `sizeof(void*) == 8` |
| B. extract VerseVM into a standalone library | **closed** | AutoRTFM, conservative stack roots, Clang-only dispatch, the EULA |
| C. wait for Epic's open-source Verse | **closed as a dependency** | promised 2022, undelivered, undated, and probably the wrong artifact |
| D. a second execution path | **open** | — |

---

## 1. Path A — the UE host on wasm

### 1.1 The arithmetic

This one does not need an effort estimate, because two constants disagree.

A GDExtension for web **must** be `wasm32`. godot-cpp refuses anything else outright
(`godot-cpp/tools/web.py:10`):

```python
if env["arch"] not in ("wasm32"):
    print("Only wasm32 supported on web. Exiting.")
```

It builds the extension as an Emscripten **side module** (`-sSIDE_MODULE=1`, `.wasm`), which is
linked into Godot's own main module at load. A side module and its host must agree on pointer width.

Unreal requires 64-bit pointers, unconditionally, in the header every translation unit includes
(`Engine/Source/Runtime/Core/Public/HAL/Platform.h:1269`):

```cpp
static_assert(sizeof(void*) == 8, "Pointer size must be 64-bit.");
```

and, at `:741-748`, the engine deprecated the *concept* of a 32-bit platform:

```cpp
#define PLATFORM_32BITS UE_DEPRECATED_MACRO(5.8, "PLATFORM_32BITS is deprecated as UE only supports 64-bit.") 0
```

wasm32 has 4-byte pointers. A stock UE tree fails in Core's most basic header, by design.

The only escape is Memory64 (`wasm64`), and it does not lead anywhere useful. wasm32 and wasm64
modules cannot be linked into one program, so a UE-as-wasm64 module could not load into Godot's
wasm32 web export. Reaching it would require Godot itself, godot-cpp, the web export templates and
every other GDExtension in the ecosystem to move to wasm64 — and to pay Memory64's measured cost,
which SpiderMonkey put at "10% to over 100% — a 2x slowdown just from changing pointer size"
(spidermonkey.dev, January 2025). Memory64 shipped in Chrome 133 and Firefox 134; Safari was not
confirmed.

The community that maintained UE on the web reached the same conclusion from the other side: the
`ufna/UE-HTML5` project declines UE5 because it "has lack of 32-bit support, and emscripten hasn't
64-bit."

### 1.2 The rest of the wall, for completeness

Even with the pointer problem waved away, three more stand:

- **UBT has no wasm platform.** `Engine/Source/Programs/UnrealBuildTool/Platform/` is Android, Apple,
  Clang, IOS, Linux, Mac, Microsoft, TVOS, Windows. UE4's HTML5 platform left the engine at 4.24 and
  never returned; it was never a clean platform extension but a patch set against engine source,
  carried by the community to 4.27 and no further.
- **VerseVM is not a library.** It lives inside CoreUObject — `Runtime/CoreUObject/{Public,Private}/
  VerseVM`, 307 files, 56,896 lines. See §2.
- **The cooked format is IoStore**, and has to be: `FLinkerLoad` has no `Verse::VCell` override, so a
  `.uasset` mis-parses (`HostCook.h:39-41`). Reading it goes through `FIoDispatcher`,
  `FPackageStore` and `LoadPackage`, and the build already reaches into `PakFile/Private` and
  `Core/Internal`, legal only under a monolithic link.

### 1.3 What the port would weigh, if it were possible

Recorded so the number is not re-derived.

| module set | lines |
| --- | ---: |
| Core | 706,832 |
| CoreUObject | 386,103 |
| Solaris + VerseCompiler + ApplicationCore + Projects + PakFile | 228,525 |
| VerseVM plugin + VerseNative | 24,490 |
| **total under Emscripten** | **~1,346,000** |

And what "adding a platform" costs, from in-tree examples:

| anchor | lines |
| --- | ---: |
| UBT platform module, Linux | 2,835 |
| UBT platform module, Android | 10,771 |
| Core platform layer, Unix | 15,758 |
| Core platform layer, Android | 16,105 |
| Core platform layer, Windows | 22,309 |
| `GenericPlatform` — the surface a new platform must satisfy | 25,955 (55 files) |
| the whole VisionOS platform extension | 8,343 |

VisionOS is the cheapest because it derives from Apple/IOS. wasm derives from nothing, and is
Android-shaped in exoticness: its own toolchain, SDK and packaging, plus no process model, no real
threads by default, and a different exception model. `Core.Build.cs` also pulls **IntelTBB** (no wasm
target; this repo already ships `tbbmalloc.dll`), zlib and ICU.

This is an engine fork, re-done at every UE upgrade. It is what "prefer not to change UE's build or
source" was meant to avoid.

---

## 2. Path B — extracting VerseVM

More interesting than expected, and closed anyway.

### 2.1 What is genuinely favourable

- **There is no JIT.** VerseVM is a pure bytecode interpreter — no assembler, no code buffer, no
  executable allocation. wasm's prohibition on runtime code generation never applies.
- **VerseVM has its own garbage collector**, not UObject's: a concurrent mark-sweep collector with an
  explicit `Idle → Marking → PostMarking → Census → Destroying → Sweeping` state machine, and an
  external-control API (`EnableExternalControl`, `AddExternalMarkStack`) whose purpose is interlocking
  with a foreign collector. A real seam.
- **Engine-free already works.** Epic's own `VerseCmd.Target.cs` sets `bCompileAgainstEngine = false`
  with `bCompileAgainstCoreUObject = true`. Engine-free is exercised; CoreUObject-free does not exist
  as a configuration anywhere.
- ~24,200 lines across 202 of the 307 files reference no UObject-family type at all — heap, bignum,
  bytecode, containers, transactions, tasks, profiler. A coherent VM shape.
- `IEngineEnvironment` is a deliberate abstraction over "Verse needs to make UObjects."

A defensible split is ~34,000–38,000 lines of plausibly standalone core against ~19,000–23,000 of UE
interop.

### 2.2 Why the line count is the wrong metric

Four blockers, each roughly zero lines of VerseVM code, and each absolute.

**AutoRTFM is not optional.** `VVMInterpreter.cpp:83`:

```cpp
static_assert(UE_AUTORTFM, "New VM depends on AutoRTFM.");
```

139 files in the subsystem reference it and `VCell` is declared `struct AUTORTFM_DISABLE VCell`.
Verse's failure-and-rollback semantics are *defined* in terms of these transactions. AutoRTFM is a
Clang fork that instruments every memory write, and its toolchain component is packaged x64-only
("ClangRTFM x64"). Epic does ship `VerseCLRNoAutoRTFM.Target.cs`, which sets
`bUseAutoRTFMCompiler = false` — so the *compiler pass* is separable at target level — but that
does not produce a wasm-capable toolchain, and the VM's own `static_assert` stands.

**The GC finds roots by scanning the native C++ stack conservatively.**
`VVMContextImpl.cpp:583-641` walks raw stack words between paired entry/exit frames and asks libpas
whether each word points into an allocated object, with callee-saved registers flushed via `setjmp`.
There is no handle-scope mechanism for native locals to switch to — the design deliberately lets C++
hold bare `VValue` on the stack. In WebAssembly the machine stack is not addressable memory; V8's own
porting guidance states that "Wasm's sandboxing prevents programs from inspecting their own stack."
Retrofitting precise rooting means auditing every native call path in the VM and every embedder
entry point. Compounding it, the collector *is* libpas — WebKit's allocator, 549 vendored files, mark
bits computed by chunk-header address arithmetic, no wasm support.

**The interpreter is Clang-and-x64/ARM-only.** Dispatch is tail-call threaded on
`[[clang::musttail]]` and `__attribute__((preserve_none))`, with no fallback and no switch-dispatch
alternative. `preserve_none` is implemented for x86-64 and AArch64 only.

**`UObject` is inside the value type.** `VValue` is NaN-boxed with `UObjectTag = 0x3` consuming a tag
slot, and `VVMValue.h:360` says outright: `// VValue assumes a 48-bit address space for pointers.`
The interpreter carries UObject opcodes (`ConstructNativeDefaultObject`, `LoadImport`,
`JumpIfDefaultSubObject`). `IEngineEnvironment` abstracts UObject *creation*, not *representation*,
and representation is the part that reaches into the value.

**The VM is also not a module.** `Engine/Plugins/VerseVM` contains only `VerseVMCodeGen` and
`VerseVMSocketDebugger`. Epic modularized the compiler and left the runtime embedded — which is the
asymmetry that matters, because the portable half is the half a game does not need.

### 2.3 Where extraction converges

Replace AutoRTFM with an own transaction log, replace conservative rooting with a precise scheme,
replace libpas, rewrite interpreter dispatch, and unpick `UObject` from the value encoding — and the
result is **writing a VM**, while constrained to Epic's bytecode, inheriting their object model, and
holding no licence to ship it. The genuinely reusable remainder is perhaps 10,000–15,000 lines
(bignum arithmetic, float printing, the bytecode definitions), and it is the part least able to be
redistributed.

Extraction is strictly worse than Path D from a worse starting position.

---

## 3. Path C — waiting for Epic

The commitment is real and broader than a specification. At Haskell eXchange (December 2022) Simon
Peyton Jones and Tim Sweeney committed to publishing a spec "for anyone to implement" and to offering
a **compiler, verifier and runtime under a permissive open-source licence with no IP encumbrances.**
Sweeney restated it on X in June 2024: "Permissively licensed open source Verse compiler, runtime,
and draft specification are coming, possibly as soon as 2025."

As of 2026-09-15 nothing has shipped: no Epic repository, binary, or specification sufficient to
implement against. The one soft date has passed with no replacement. Job postings and continued
language-theory work show the effort is alive internally.

Two reasons not to sequence anything against it:

- **The artifact is probably not VerseVM.** Epic's own postings describe "a reference implementation
  and a term-rewrite-system that implements Verse" — a semantics artifact, not a game runtime. And
  CoreUObject cannot go permissive without the engine going permissive.
- **The published semantics covers a core calculus only.** *The Verse Calculus* (ICFP 2023;
  Augustsson, Breitner, Claessen, Jhala, Peyton Jones, Shivers, Steele, Sweeney) is peer-reviewed and
  confluence-proven for choice, failure, unification and one-shot generators. It does not cover
  effects and specifiers, classes, modules or concurrency — precisely the surface this bridge
  exercises.

No third party has a conformant implementation. Every "Verse" project found publicly is a name
collision or an early hobby interpreter.

This is worth tracking, not waiting on. Its practical consequence is a design constraint on Path D:
keep emission and execution separable, so a permissive reference Verse could replace the execution
half without disturbing the rest.

---

## 4. Licensing, which outranks the engineering

`README.md:126-131` already states the position:

> Binaries built from UE source may not be redistributed to anyone without their own license, which
> is why nothing here ships prebuilt.

Carving VerseVM out of CoreUObject and shipping it is a sharper instance of the same thing — no
longer a binary built from the engine, but a library cut from its source.

**A web export makes this maximal.** A desktop export hands one built binary to one user; a web
export redistributes to every visitor who loads the page, automatically, into public HTTP caches.
Of all delivery targets it is the one where "may not be redistributed" binds hardest.

This closes Paths A and B independently of every engineering finding above, and it is why the
engineering was costed anyway: so that the record shows the paths were closed on their merits and
not only on their terms.

---

## 5. Path D — a second execution path

**The shape.** The desktop cooker keeps full UE and gains a *backend*: lower uLang's IR to our own
serialized IR. The GDExtension gains a small *interpreter* for that IR, compiled to wasm — and, at no
extra cost, to every other platform.

The ABI seam this project already has is what makes this a swap rather than a rewrite. On web there
is no DLL to load, so `vh_*` stops meaning "symbols in `verse_host_runtime.dll`" and becomes "symbols
someone statically links." The contract is small: of **39** `vh_*` entry points, a runtime host must
implement **21**, **11** are already refused without a compiler, and **7** read the sidecar. The
**27** consumer-supplied callbacks are already written on the Godot side and do not change.
`src/verse_host.cpp:161` already carries the `"unsupported platform"` branch; web needs a third mode
— a statically linked host — not new architecture.

### 5.1 Why it is tractable

Four measurements decide it.

- **The Godot mirror is ordinary Verse, not C++.** `GodotClasses.native.verse` is 56,874 generated
  lines routing **17,430** call sites through exactly **three** dispatchers (`VhCallVoid`,
  `VhCallValue`, `VhCallValueConst`), and `GodotApi` and `GodotMath` add 1,613 more lines of plain
  Verse. All of it compiles into any backend for free.
- **The native surface is 39 functions**, matched 1:1 between `host/Verse/Godot.native.verse` and
  `host/Private/GodotBindings.cpp`, plus Epic's 18 `<native>` declarations across 11 files in
  `VerseNative`. (`CLAUDE.md:343` still says the surface went "from 8 to 24"; the count is 39.)
  `/Verse.org/Verse`'s core is compiler-intrinsic and small — roughly 17 operators and 9 built-in
  functions in `CIntrinsicSymbols`, plus `Length`, `Inf` and `NaN`.
- **The execution model is already single-threaded.** The runtime host spawns no threads;
  `vh_tick` is a synchronous queue drain and `HostEventLoop.cpp` is 165 lines with no threading
  primitive in it. Nothing has to be re-architected to meet a browser.
- **The cost anchor is exact.** Epic's own IR→bytecode backend, written by the people who designed
  the IR, is **8,911 lines across 11 files**. uLang lowers to an `Ir_*` form within a 113-kind
  `EAstNodeType`, of which the compile-time-only kinds are not the interpreter's problem.

uLang itself is nearly engine-free already — 1 of its 64 public headers includes a UE Core header, it
carries its own `ULANG_PLATFORM_*` abstraction, and `uLangCore.Build.cs:77` sets
`UE_AUTORTFM_DO_NOT_INCLUDE_PLATFORM_H=1`, deliberately refusing the very header that carries the
64-bit assertion.

### 5.2 Cost

| piece | new lines |
| --- | ---: |
| cooker backend (uLang IR → our IR) | 3,000–5,000 |
| interpreter core — values, classes, failure, loops | 4,000–6,000 |
| concurrency — tasks, `Await`, `race`/`sync`/`spawn`, `defer`-on-cancel | 1,500–2,500 |
| transactions — an undo log | 800–1,500 |
| the 39 natives + sidecar reader + ABI shim | 2,500–3,500 |
| stdlib intrinsics | 1,500–2,500 |
| **total** | **~14,000–21,000** |

Against ~28,000 hand-written lines across both halves today, roughly a doubling of the project. At
this repository's demonstrated pace: **a sprite moving in a browser at 6–10 weeks; a conformant web
export at 4–8 months.**

The transaction line is the one place this is *easier* than the desktop host: we own the value
representation, so an undo log needs no compiler instrumentation. `VerseCLRNoAutoRTFM.Target.cs` is
evidence that Verse does not require the AutoRTFM pass to run at all.

The design risk is concentrated in concurrency. With no threads, suspension must be explicit — the
interpreter has to be written in explicit-stack or CPS form from the first line, because `Await`,
`race` and `spawn` cannot be retrofitted onto a recursive evaluator. That shape decision comes before
any other.

### 5.3 The envelope

We choose it, because we write the runtime. Single-threaded is already the model, so the
`nothreads` path is reachable and cross-origin isolation is avoidable in principle — though Godot's
own documentation frames "Extensions Support" as requiring COOP/COEP, and threaded and nothreads
builds must match the template or instantiation fails with a wasm `LinkError`. The payload is an
interpreter of a few megabytes plus cooked IR, against 72.7 MB of Shipping UE host. The one open size
question is the lowered mirror, which is a measurement, and `--classes-file` already exists as the
mitigation.

### 5.4 What it costs permanently

**Two implementations of Epic's language that must agree, against no specification we control.**
`tests/integration` stops being a test suite and becomes a conformance suite; every divergence
between the desktop host and the web interpreter is a defect in one of them, and there is no
authority to appeal to — §3 records why the published semantics does not settle it. This is the real
price, and it is ongoing rather than one-off.

### 5.5 What it buys beyond web

Web is the forcing function, not the prize.

- **The other platforms.** The interpreter is platform-independent, so it also serves Linux, macOS,
  Android and iOS exported games — R-PLAT-1, R-PLAT-2 and OQ-3.
- **A redistributable artifact.** It is the only path whose shipped game contains no UE-derived
  bytes. That reaches three things `README.md:38-43` currently lists as blocked on Epic licensing: a
  download-and-unzip addon install, prebuilt binaries, and CI on a hosted runner. The cooker still
  needs a licensed UE checkout, but only on the author's machine, at author time.

---

## 6. What is not established

- The lowered size of the 56,874-line mirror, which is the only open question on the web payload.
- Which of the 113 `EAstNodeType` kinds actually occur in real projects. A cooker-side histogram over
  `dodge-the-creeps` and the mirror would size the interpreter exactly rather than by the estimate in
  §5.2, and is the single highest-value measurement left.
- Whether an explicit-stack interpreter can express `Await` and `race` as cleanly as assumed. Phase
  7's lesson applies: a spike that loads something proves only the loading.
- Whether a `nothreads` GDExtension genuinely escapes the COOP/COEP requirement, which Godot's docs
  do not carve out explicitly.
- Whether `bWithoutThreading` supports a permanently single-threaded VerseVM GC — it appears written
  for the pre-fork window. Moot for Path D; recorded because it was asked.
- No independent reader for the `.utoc`/`.ucas` container exists or was attempted; Path D does not
  need one, since it defines its own format.

---

## 7. Answers this document supplies

- **OQ-4** — Verse on wasm is **not** reachable by running VerseVM there, on grounds that are
  arithmetic (§1.1), architectural (§2.2) and legal (§4), and no one of which is the only one. It
  **is** reachable by a second execution path (§5), at a cost that exceeds what a web export alone
  justifies and that is repaid by the platforms and the redistribution story.
- **R-PLAT-3** — the blocking reason should be restated: not "UBT has no wasm Program target" alone,
  but the pointer-width contradiction, which is the one that admits no workaround.
- **R-PLAT-4** — unchanged and still correct: `VerseExportPlugin` refusing `web` at export time
  remains the right behaviour until §5 exists.
