# External editor tooling

Research notes for pointing VS Code at Epic's Verse tooling, done against the UE checkout at
`C:/UnrealEngine` (read-only reference, not part of this repo). Every claim
below is cited `path:line`; where the checkout does not answer the question, that is stated
rather than guessed.

## uLangLSP

`Engine/Source/Runtime/Solaris/uLangLSP/uLangLSP.Build.cs:6-32` declares `uLangLSP` as an
ordinary `ModuleRules` — a static library, not a `TargetRules` Program. Its only sources are:

- `Engine/Source/Runtime/Solaris/uLangLSP/Public/uLang/LSP/LSP.h`
- `Engine/Source/Runtime/Solaris/uLangLSP/Private/uLang/LSP/LSP.cpp`
- `Engine/Source/Runtime/Solaris/uLangLSP/Public/uLang/LSP/LSPUtils.h`
- `Engine/Source/Runtime/Solaris/uLangLSP/Private/uLang/LSP/LSPUtils.cpp`

That is the whole module: LSP message/struct types and JSON conversion helpers, the same shape as
`uLangDAP` below. There is no `main`, no stdio loop, no socket listener anywhere in it.

**Nothing in this checkout links it into a Program.** Searching every `*.Build.cs` under
`Engine/Source` for `uLangLSP` finds exactly three files: `uLangLSP.Build.cs` itself,
`Engine/Source/Runtime/Solaris/VerseAssist/VerseAssist.Build.cs`, and
`Engine/Source/Runtime/Solaris/ProtoLem/ProtoLem.Build.cs` — both of those are libraries too.
`VerseAssist` (`Engine/Source/Runtime/Solaris/VerseAssist/Public/VerseAssist.h:1-206`) provides
completion/definition/hover-style queries over an already-parsed `CSemanticProgram`; `ProtoLem`
is a Slate-based visual-scripting editor widget module (`SVplWidget.cpp`, `VplModel.cpp`, etc. —
an in-editor IDE panel, not a standalone process). Searching all `*.Build.cs` under
`Engine/Source/Programs` for `uLangLSP` finds nothing.

**No prebuilt executable exists.** `Engine/Binaries/Win64` contains exactly one Verse-toolchain
executable, `VerseCompilerCmd.exe`, plus `verse_host.dll`/`.lib`/`.pdb`/`.target`. There is no
`uLangLSP.exe`, `VerseLanguageServer.exe`, or similarly-named binary, and no `uLangLSP.dll` either
(an unreferenced module produces no build output at all in UBT, so this is expected, not merely
unobserved).

**Command-line interface: cannot be determined, because there is nothing to invoke.** The module
has no entry point to read arguments, speak stdio, or open a socket. The closest comparable
Program, `VerseCompilerCmd`
(`Engine/Source/Programs/Solaris/VerseCompilerCmd/Private/VerseCompilerCmd.cpp:41-82`), takes a
single positional project-file path via uLang's own `CommandLine` parser and exits after one
batch compile — but it does not depend on `uLangLSP` (`VerseCompilerCmd.Build.cs:9-17` lists no
such dependency) and is not a server of any kind.

**What would have to be built:** a new UBT Program target (a `.Target.cs` + `.Build.cs` +
`main`-equivalent source, analogous to `Engine/Source/Programs/Solaris/VerseCompilerCmd/`) that
links `uLangLSP`, `VerseAssist`, `VerseCompiler`, `uLangCore`, and implements an LSP stdio loop
itself — reading `Content-Length`-framed JSON-RPC from stdin, dispatching into `VerseAssist`'s
completion/definition calls, and writing responses back. No such target exists in this checkout,
under any name, so there is no build command to give — `python tools/build_host.py` builds only
the `VerseHost` Program and has nothing to do with this.

## uLangDAP

`Engine/Source/Runtime/Solaris/uLangDAP/uLangDAP.Build.cs:6-32` is the same shape: a `ModuleRules`
library, sources `Public/uLang/DAP/DAP.h` and `Private/uLang/DAP/DAP.cpp`. The header's own
comment says what it is: "adapted from https://microsoft.github.io/debug-adapter-protocol/specification"
(`Engine/Source/Runtime/Solaris/uLangDAP/Public/uLang/DAP/DAP.h:19`) — it is the DAP request/
response/event struct definitions, not a running adapter.

**Nothing in the checkout depends on it.** Searching every `*.Build.cs` under `Engine/Source` for
`uLangDAP` finds only `uLangDAP.Build.cs` itself. In particular
`Engine/Plugins/VerseVM/Source/VerseVMSocketDebugger/VerseVMSocketDebugger.Build.cs:9-23` (the
module that actually runs the debug server the host links) does **not** list `uLangDAP` as a
dependency, and none of its `.cpp` files include a `uLang/DAP/*.h` header. So the shipped socket
debugger builds its own JSON messages by hand rather than through `uLangDAP`'s types — see below.

**No prebuilt binary.** Same as `uLangLSP`: no `.exe` or `.dll` anywhere under
`Engine/Binaries/Win64` matches `uLang` at all, which is consistent with a module nothing links.

## VerseVMSocketDebugger — the debug server the host already links

Module: `Engine/Plugins/VerseVM/Source/VerseVMSocketDebugger/` (a plugin module, not
`Engine/Source/Runtime`). `host/Private/VerseHost.cpp:21` includes its public header
(`VerseVM/VVMSocketDebugger.h`), and `host/Private/VerseHost.cpp:84-87` is exactly the call this
project's `EnableDebugger` flag reaches:

```
if (Desc->EnableDebugger)
{
    GDebuggerScope = Verse::SocketDebugger::Listen();
}
```

**Default port: 1963.** Declared as a console variable, not a compile-time constant that call
sites pass explicitly:

```
int32 DefaultPort = 1963;
FAutoConsoleVariableRef CVarDefaultPort(TEXT("verse.DebuggerPort"), DefaultPort, ...);
```

(`Engine/Plugins/VerseVM/Source/VerseVMSocketDebugger/Private/VerseVM/VVMSocketDebugger.cpp:377-379`).
`Listen()` with no arguments — what `vh_init` calls — resolves to `Listen(DefaultPort)`
(`VVMSocketDebugger.cpp:442-445`), which binds `ISocketSubsystem::GetLocalAddress` on that port
(`VVMSocketDebugger.cpp:432-440`). It can be overridden per-process by setting the
`verse.DebuggerPort` console variable before `vh_init` runs; this host does not currently expose a
way to do that (out of scope for this task — the ABI has no console-variable passthrough).

**Wire protocol is custom framing, not standard DAP framing.** Standard DAP (and LSP) transports
frame each JSON message with an HTTP-style `Content-Length: N\r\n\r\n` header, which is what VS
Code's built-in debug-adapter machinery expects to read from a socket or pipe. This server does
not do that. `Engine/Plugins/VerseVM/Source/VerseVMSocketDebugger/Private/VerseVM/VVMSocketUtil.cpp:132-168`
shows `RecvJsonValue`/`SendJsonObject` framing every message as a 4-byte big-endian length prefix
followed by the raw UTF-8 JSON bytes (`RecvUint32BE`/`SendUint32BE`,
`VVMSocketUtil.cpp:15-56`). The JSON bodies themselves are assembled by hand with Unreal's `Json`
module (`TSharedRef<FJsonObject>`, e.g. `VVMClient.cpp:194-220`) and use DAP-flavored vocabulary
(a `"stopped"` event, reasons `"pause"`/`"next"`/`"stepIn"`/`"stepOut"`,
`VVMSocketDebugger.cpp:163-257`) — but through this custom length-prefixed framing, not the
`uLangDAP` structs and not standard DAP transport.

**Conclusion: no off-the-shelf DAP client, including VS Code's own generic attach machinery,
can talk to this socket without a translating adapter**, because the framing differs. No such
adapter exists in this checkout — `uLangDAP` is unused (see above), and nothing else in the
engine source bridges the two. This is why `.vscode/launch.json` in this repo is marked
unverified rather than presented as a working attach configuration.

## Summary for Task 3/4

- The language server (`uLangLSP`) cannot be launched: there is no executable, and no Program
  target to build one from. `tools/run_verse_lsp.py` reports this rather than guessing a CLI.
- The debugger socket is real and reachable — `verse/host/enable_debugger` (added in this repo)
  turns it on, listening on port 1963 by default — but its wire protocol is not standard DAP
  framing, so whether any existing VS Code extension can drive it is unverified.
