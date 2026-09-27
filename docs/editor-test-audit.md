# Editor test audit

**Status:** 2026-09-26 audit and plan, 2026-09-27 built — see [Status](#status) at the bottom for
the nine steps and what each shipped. It classifies every by-hand check in
[`by-hand-findings.md`](by-hand-findings.md) and in the checklist for
[`architecture-review.md`](architecture-review.md) item 6, and the body below is the plan as it was
proposed, kept for *why* each layer is shaped the way it is.

By-hand checks are rarely run, so a check that stays by hand is effectively untested. The goal is
to keep by hand only what needs a person to judge what Godot draws.

## Summary

Most of the by-hand list isn't visual. It stayed by hand because of three beliefs, and experiments
on Godot 4.7.2 show that all three are wrong:

- **"A placeholder only exists under `is_editor_hint()`, so nothing automated can see it"** (B8,
  B26, B39). A headless editor (`godot --headless --editor`) has placeholders. An `EditorPlugin`
  can open a scene, write through a placeholder, break a script through `ResourceSaver`, save the
  scene with `EditorInterface.save_scene()`, and read the `.tscn` that was written.
- **"`ScriptLanguage` exposes nothing a script can ask, so the editor UI is untestable."** In a
  headless editor, the plugin can reach the real `ScriptTextEditor` and its `CodeEdit`. It can
  trigger completion and read the options and whether the popup is open. It can raise the real
  hover tooltip and read its text, open the class reference page, and read the syntax colours per
  column, the gutter icons, the warnings panel, the inspector's property controls, the scene dock's
  icons, and the Node dock's signal list.
- **"Everything from `EngineDebugger` inward has no automated test and never will."** A headless
  editor can press **Play**. The project setting `editor/run/main_run_args="--headless"` makes the
  child process headless. The plugin then reads Godot's own Debugger panel: the stack, Stack
  Variables, the Step Over button, and the Profiler's rows with call counts. Separately, a Python
  server of about 250 lines that speaks Godot's remote debugger protocol drives a headless game
  directly.

What's left for a person is short. It's in [The residual by-hand list](#the-residual-by-hand-list):
one glance per Godot version at how the tooltip, class page, colours, and inspector *render*, plus
three checks that need a real display or a second machine rather than a person's eye.

The implementation plan is in [Implementation plan](#implementation-plan): nine steps of 0.5 to 3
days each, about 13.5 days in total. Each step ships on its own.

## How the experiments were run

Every experiment used a plain Godot project under
`C:\Users\Devin\AppData\Local\Temp\godot_editor_probe`. None loaded the godot-verse extension or
`verse_host.dll`. Every Godot process ran with `--headless` and opened no window. Where a check
needs the Verse language itself, this document reasons from the bridge's code and names the
experiment the implementation has to run.

The Godot binary was `C:\Apps\Godot_v4.7-stable_win64.exe\Godot_v4.7-stable_win64_console.exe`,
which reports `4.7.2.stable.official.ed1daf0bf`. The engine source was read at the `4.7.2-stable`
tag of `../godot`, not at the 4.8-dev checkout's `HEAD`, because the two differ (see
[Godot version sensitivity](#godot-version-sensitivity)).

The headless editor writes editor settings to `%APPDATA%\Godot`, which the owner's real editor
shares. Every run therefore isolated them:

```sh
APPDATA=<scratch>/appdata LOCALAPPDATA=<scratch>/localappdata \
  Godot_v4.7-stable_win64_console.exe --headless --editor --path <scratch>/editor_proj
```

The project enabled one plugin (`[editor_plugins] enabled=PackedStringArray("res://addons/probe/plugin.cfg")`).
The plugin prints one `ok`/`FAIL` line per case and exits with `get_tree().quit(failures)`.
[Appendix: the probe calls](#appendix-the-probe-calls) lists the calls it makes.

## B1. Headless editor mode

**Result: feasible, and it reaches much more than expected.** The main probe's final run printed
40 `ok` lines and 0 `FAIL` lines, and exited 0. A second, shorter probe covered `_make_function`,
the Node dock, and the autoload list; it printed 3 `ok` lines and 1 `FAIL`, and the `FAIL` was
itself a finding (see the autoload row below). A plain editor boot plus the short probe took about
2 seconds.

### What exists headless

| Surface | Headless? | How the probe read it |
| --- | --- | --- |
| `Engine.is_editor_hint()` | true | direct |
| `EditorInterface`, `get_script_editor()`, `get_inspector()`, `get_base_control()` | all non-null | direct |
| Opening a scene | works | `EditorInterface.open_scene_from_path()`, then `get_edited_scene_root()` |
| A non-`@tool` script's placeholder | exists | `script.can_instantiate()` is false; `node.get("speed")` serves the default 7; `node.set("speed", 42)` is stored |
| Saving a scene | works | `EditorInterface.save_scene()` answers `OK`; the `.tscn` on disk carries `speed = 42` |
| Saving a script through the language's saver | works | `ResourceSaver.save(script)` of a broken source prints GDScript's parse error and answers `OK` |
| `ScriptTextEditor` and its `CodeEdit` | exist | `EditorInterface.edit_script()`, then `get_current_editor().get_base_editor()` |
| Completion options | synchronous | `CodeEdit.request_code_completion(true)`, then `get_code_completion_options()` in the same frame: 173 options, `local_value` first |
| Whether the popup is open | readable | `get_code_completion_selected_index()` is `-1` exactly when completion isn't active |
| The prefix table | readable and writable | `code_completion_prefixes` was `[".", ",", "(", "=", "$", "@", "\"", "'"]` |
| The re-request after confirming an option | observable | connect to `code_completion_requested`, then `confirm_code_completion()`: confirming `to_local(` re-requested once |
| The call hint | **not readable** | `CodeEdit` binds `set_code_hint` and no getter; see [B27](#completion) for the workaround |
| Syntax colours | readable | `syntax_highlighter.get_line_syntax_highlighting(line)` answers column → colour |
| Gutter icons | readable | the `connection_gutter` column carried the Slot icon and `{type: connection, method: _on_timer_timeout}` |
| The warnings panel | readable | its `RichTextLabel` text: `Line 23 (UNUSED_VARIABLE): The local variable "unused_probe"...` |
| The hover tooltip | raised through Godot's own path | `code_edit.emit_signal("symbol_hovered", word, line, column)` creates an `EditorHelpBitTooltip` child of the `CodeEdit`; its two `RichTextLabel`s read `Property Mover.speed: int = 7` and the doc comment |
| Script documentation | registered | `ScriptEditor.update_docs_from_script()` writes `DocTools` directly, so it works headless |
| The class reference page | readable | `ScriptEditor.goto_help("class_name:Mover")`, then the `EditorHelp` tab's text |
| The inspector's controls | readable | `EditorInterface.inspect_object()`, then each `EditorProperty`'s `get_class()`: `@export_file` → `EditorPropertyPath`, `@export_multiline` → `EditorPropertyMultilineText`, `@export_flags` → `EditorPropertyFlags`, `@export_node_path` → `EditorPropertyNodePath` |
| `@icon` in the scene dock | readable | the `SceneTreeEditor`'s root item icon was `res://icon.svg`, 16×16 |
| The Node dock's signals | readable | the `ConnectionsDock` tree listed `Mover`, then `probe_hit(amount: int)` |
| `_make_function` | reachable | emitting `EditorNode`'s `script_add_function_request` signal wrote `func _on_probe_made(amount: int) -> void:` into the open script |
| The autoload list | reachable | `EditorAutoloadSettings.autoload_add()` is bound. For a `Resource` script it answered `true`, and Godot printed `Failed to create an autoload, script '...' does not inherit from 'Node'.` So the refusal is the printed sentence, not the return value |
| **Play** | works, headless | with `editor/run/main_run_args="--headless"`; the verbose log shows the child's command line ending in `--headless` (see below) |
| The Debugger panel | readable and drivable | see [Play and the Debugger panel](#play-and-the-debugger-panel) |

### What doesn't work headless

- **Script documentation regeneration is skipped.** In 4.7.2, `cmdline_mode` is set when the
  display server can't draw a window (`editor_node.cpp:8360`). Under it,
  `EditorHelp::load_script_doc_cache` returns early (`editor_help.cpp:3101`), so
  `_script_docs_loaded` is never set. Every `EditorHelp::add_doc` is then queued and never applied.
  `EditorFileSystem::_update_script_documentation` is also skipped (`editor_file_system.cpp:2315`). That's why B38's
  timing cause (a regeneration discarding a doc registered before it finished) can't be reproduced
  headless. `update_docs_from_script` doesn't go through that queue, which is why hovers still work.
- **Play opens a window unless told otherwise.** `EditorRun::run` forwards only
  `get_forwardable_cli_arguments(CLI_SCOPE_PROJECT)`, which doesn't include `--headless` or
  `--display-driver` (`main.cpp:1177-1206`). The project's `editor/run/main_run_args` is appended
  instead (`run_instances_dialog.cpp:224`), and it's read at construction (`:340`), so setting it
  in `project.godot` is enough. The probe refused to press Play unless it read `--headless` there.
- **Typing a trigger character through the timer path didn't open the popup in 1 s.** After
  `insert_text_at_caret(".")`, the popup stayed closed, although the `CodeEdit` was visible in the
  tree. The cause wasn't investigated. It doesn't matter: the timer only calls
  `request_code_completion()` unforced, and calling that directly exercises Godot's actual
  decision. `Input.parse_input_event` with a key event inserted nothing at all.
- **The call hint can't be read** (above).

The run also printed `ERROR: Parameter "t" is null.` twice, at the two scene saves. It came from
Godot and didn't affect any result.

### One stock-GDScript observation worth keeping

On the first run after importing, breaking `mover.gd` and saving the scene made Godot write
`portrait = null`, `notes = null`, `elements = null`, and `target_path = null` into `main.tscn`. On
two later runs with the same restored scene it didn't. That's B39's shape, from stock GDScript, on
a first session. It wasn't investigated. It shows that the harness can observe the defect class,
and that a Verse test needs a GDScript control case beside it, so that a failure can be attributed
to the bridge or to Godot.

### Play and the Debugger panel

The probe set a breakpoint with `CodeEdit.set_line_as_breakpoint()` and called
`EditorInterface.play_custom_scene("res://main.tscn")`. The Godot debugger then paused the child
process. The `--verbose` log shows the command line of that child process:

```text
Running: .../Godot_v4.7-stable_win64.exe --verbose --path .../editor_proj --remote-debug tcp://127.0.0.1:6123 --editor-pid 17420 --breakpoints res://mover.gd:28 --scene res://main.tscn --headless
```

The breakpoint list from the gutter arrives as `--breakpoints`. The port is the one the plugin set
through `EditorSettings` (`network/debug/remote_port`). If 6007 is taken by another open editor,
`EditorDebuggerServerTCP::start` tries the next port, so the child can't reach the wrong editor.

An `EditorDebuggerPlugin` reported the break through its session's `breaked` signal. The plugin then
read and pressed the panel's own controls:

```text
ok   breakpoint_stopped  after 0.2 s
ok   stack_panel_rows  ["", "0 - res://mover.gd:28 - at function: _ready"]
ok   stack_variables_panel  { &"Locals/first": 1, &"Locals/second": <null>, &"Members/self": 26642220497, &"Members/speed": 42, ... }
ok   step_over_button
ok   step_over_moved_one_line  ["", "0 - res://mover.gd:29 - at function: _ready"]
ok   profiler_rows_have_script_function  [..., "Script Functions | 0.00 ms | ", "Mover._process | 0.00 ms | 1"]
```

Step Over is the `Button` whose `tooltip_text` is `Step Over` inside `ScriptEditorDebugger`; the
probe emitted its `pressed` signal. The profiler is `EditorProfiler`: the probe set its **Start**
button pressed, emitted `pressed`, waited 2 s, and read its `Tree`.

## B2. The remote debugger protocol

**Result: feasible.** A Python server that accepts a headless game's `--remote-debug` connection
drove a breakpoint, a stack dump, the locals, a step, and the script profiler, with no editor at
all.

```sh
python <scratch>/dbg_client.py
```

```text
ok   handshake_first_message  ['set_pid', 1]
ok   debug_enter  [True, 'Breakpoint', True, 1]
ok   stack_dump_top_frame  [3, 'res://dbg_node.gd', 11, '_ready']
ok   locals_carry_first  {'local:first': 10, 'local:second': None, 'member:self': ('object_id', 26642220497)}
ok   step_over_one_line  ['res://dbg_node.gd', 12, '_ready']
ok   profiler_reports_script_function_with_calls  {'res://dbg_node.gd::4::work': 725}
ok   profiler_total_rows_named  {'res://dbg_node.gd::15::_process': 727, 'res://dbg_node.gd::4::work': 727, 'res://dbg_node.gd::0::@implicit_new': 0, 'res://dbg_node.gd::10::_ready': 0}
dbg_client done, failures=0
```

The client launched the game as
`godot --headless --path debug_proj --remote-debug tcp://127.0.0.1:<port> --breakpoints res://dbg_node.gd:11`.

### The protocol, as read from 4.7.2

- **Framing.** Each message is a little-endian `uint32` length, followed by an `encode_variant`
  encoding of an `Array` (`remote_debugger_peer.cpp:115-118`).
- **Game to editor:** `[name, thread_id, data]` (`remote_debugger.cpp:104-106`). The first message
  is `set_pid`.
- **Editor to game:** also `[name, thread_id, data]`. `_poll_messages` drops anything that isn't
  exactly three elements, and anything addressed to a thread that isn't in a break (`:353-374`). The
  thread id is the fourth element of `debug_enter`'s data. **Sending `[name, data]` is silently
  ignored**, which cost the experiment one run.
- **In a break:** `debug_enter` `[can_continue, error, has_stack, thread_id]`. The editor then
  sends `get_stack_dump`, which answers `stack_dump` `[3 × frames, file, line, function, ...]`;
  `get_stack_frame_vars [level]`, which answers `stack_frame_vars [count]` followed by `count`
  messages of `stack_frame_var [name, kind (0 local, 1 member, 2 global), type, value, hint]`; and
  `step`, `next`, `out`, `continue`, `breakpoint [file, line, set]`, and `set_skip_breakpoints [on]`
  (`:459-548`).
- **Profiler:** `profiler:servers [true, [max_functions, save_native_calls]]` turns on the script
  profiler (`servers_debugger.cpp:200-216`). Frames arrive as `servers:function_signature
  [name, id]` and `servers:profile_frame`, whose script functions are the last block, five values
  per function: `sig_id, call_count, self_time, total_time, internal_time`
  (`servers_debugger.cpp:88-109`). `profiler:servers [false]` sends `servers:profile_total`, built
  from `profiling_get_accumulated_data`. **That's the path through the
  `ScriptLanguageExtensionProfilingInfo` stride trap**, and more than one named row coming back is
  the evidence CLAUDE.md says is currently reasoned rather than measured.
- **Gotcha:** the game blocked before its first frame when its stdout was an undrained pipe. Point
  its stdout at a file or at `DEVNULL`.

The decoder handles `NIL`, `BOOL`, `INT`, `FLOAT`, `STRING`, `STRING_NAME`, the vector and rect
types, `OBJECT` as an id, `DICTIONARY`, `ARRAY` with typed-container headers, and the packed
arrays. That was enough for every message a GDScript session sends. A Verse session also sends a
`vector2` local as a `Vector2` (Phase 6's fix), which is covered.

## B3. Other mechanisms

- **Godot's `LocalDebugger`** (`--debug`, commands on stdin) works headless. Piped input is
  fragile: the first line was rejected as an invalid command, and at end of input the debugger
  re-entered the same break forever. `by-hand-findings.md` recorded this route as "not taken".
  The remote protocol is strictly better.
- **A bare `CodeEdit` in a `SceneTree` script** can test the prefix table and the re-request after
  confirming an option, but not the language's answers, because the `code_completion_requested`
  handler is the editor's `CodeTextEditor`. The headless editor makes this unnecessary.
- **Headless export** is already what the `export`, `export-vm`, and `web` layers use.
- **A windowed but unattended editor** (`--editor` with the Windows display server, driven by the
  same plugin) would reach the two things headless can't: script-doc regeneration and a real
  window's mouse-enter events. It opens a window, so it wasn't tried. It's listed as an option in
  the residual list.

## The audit

**Classes:** (A) automatable through a seam that exists today; (B) automatable with the new
infrastructure named in the row; (C) only a person can judge it. For (C), the row says what can
still be asserted, so that the person's check is as small as possible.

"Editor layer" below means the harness that step 2 of the plan builds: a throwaway copy of
`tests/integration` opened with `--headless --editor`, with a driver `EditorPlugin`, the extension,
and the host loaded, and `main_run_args="--headless"`.

### Completion

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| B13: the `_Ready`/`_Process` override declarations are in the **first** popup at a bare identifier in a class body | **A** | `probe_complete`'s `first_options` | `first_options` at a class-body caret includes `_Ready<override>():void =`, before any analysis lands. `bindings.verse`'s case is the pattern |
| B13, as the editor draws it | B | editor layer: `request_code_completion(true)`, then `get_code_completion_options()` in the same frame | the same option is in the list before the next frame |
| B22: `_Set`, `_Get`, `_GetPropertyList`, `_ValidateProperty`, `_Notification` are offered as overrides | **A** | `probe_complete` | all five appear at a class-body caret in a class deriving from `node` |
| B24: no `...Getter`/`...Setter` in a member popup | **A** | `probe_complete` (its C1 rule, turned into a case) | zero options whose display ends in `Getter(` or `Setter(` at a `.` after a mirrored-class receiver |
| B24: `vector2{` opens the popup by itself with `X` and `Y` only | B | editor layer: set the line, then unforced `request_code_completion()` | `get_code_completion_selected_index() != -1`; the options are exactly `X` and `Y`. Also assert that `code_completion_prefixes` contains `{`, `?`, and `[` after `editor_script_changed`, which is `widen_completion_prefixes`'s whole contract |
| Named-argument popup: `?E` offers `ExactMatch:logic`; confirming keeps the `?`; `if (Target?` and `:?node2d` open nothing | A + B | A: `probe_complete` at the three carets. B: editor layer, unforced request and `confirm_code_completion()` | A: the options at each caret. B: the popup opens for the first and stays closed for the other two; the line after confirming reads `?ExactMatch := ` |
| B27 cause 1: the hint appears immediately after completing `Input.IsActionPressed(` and `GetNode[` | A + B | A: `probe_complete`. B: editor layer, connect a counter to `code_completion_requested`, then `confirm_code_completion()` | A: the confirmed option's `insert_text` ends in `(` or `[`, and `call_hint` at the caret after it isn't empty. B: exactly one re-request fires. The hint string itself can't be read (no getter), so A supplies it |
| B27 after **Play**: complete a call straight away, and the hint arrives without another keystroke | B | editor layer: `play_custom_scene()` (headless child) runs `EditorNode::call_build()`, which is `VerseEditorPlugin::_build`; then complete at once | within N frames, and with no further input, `code_completion_requested` fires again from `refresh_completion_if_current`, and `probe_complete` at the same caret has a non-empty `call_hint` |
| B27 cause 2: the same with a save in between | B | as above, plus `ResourceSaver.save()` of the edited script and a wait for `_validate` | the re-ask still fires; the completion request isn't displaced |
| The popup's drawing, the hint box above the caret | C | — | nothing further. Once the options and the active state are asserted, drawing them is Godot's own `CodeEdit`. Drop from the list |

### Hover and documentation

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| `Prose`: two paragraphs, `span` as code, `word` bold, `Floor[X]` with brackets, one code block with `Nested := 1` indented | A (string) + B (drawn text) | A: `probe_hover` already asserts the whole BBCode string. B: editor layer, `emit_signal("symbol_hovered", ...)` on the `CodeEdit`, then read the `EditorHelpBitTooltip`'s `RichTextLabel`s | B: `get_parsed_text()` contains `Floor[X]`, contains no literal `[b]`, `[code]`, or `[lb]`, has two prose paragraphs, and contains `Nested := 1` on its own line after `Result := Floor[X]` |
| `Blocked` and `Indented`: one paragraph each, no `>`, no missing second line | A + B | as above | B: each tooltip's text is exactly the expected sentence, with no `>` |
| `RootConstant`: says "Method" and carries "From helpers.verse" | A + B | A: `probe_hover` asserts `left/widget` and the description. B: the tooltip | B: the title label starts with the member's kind and names `left/widget`; the body contains `From helpers.verse` |
| The class reference page for `hover_probe`: the member renders the same; the brief is the first paragraph alone | B | editor layer: `ScriptEditor.goto_help("class_name:hover_probe")`, then the `EditorHelp` tab's text | the page's text contains the member's description; the text under the class name is the first paragraph and not the second |
| B40: `Emit` draws as a method (`event.Emit(: t) -> void`) with the comment, not as a Local Constant | A + B | A: `CLASS_METHOD` under `event`, already asserted. B: the tooltip, whose page `publish_api_method` registers through `update_docs_from_script` (headless-safe) | B: the title reads as a method of `event` with `-> void`; the body is the `GodotApi.native.verse` comment. Same for `Subscribe` and `Await` |
| B38 cause 1: a **fresh** editor, hovering without saving, shows the doc | B for the contract; C* for the race | B: editor layer, hover before any save | B: the tooltip carries the comment, which is `ensure_script_doc_published`'s contract. C*: the regeneration race needs `EditorHelp`'s regeneration, which headless skips (`cmdline_mode`). No person is needed, only a display server, so it's the unattended-windowed option |
| B29: Ctrl+click on a binding opens `main.gd` rather than scrolling to the top | A + B | A: `probe_hover.py` H9. B: editor layer, `emit_signal("symbol_lookup", word, line, column)` (connected at `script_text_editor.cpp:2681`) | B: `get_current_script()` becomes `res://main.gd` (the same technique as `symbol_hovered`; not run in this audit) |
| Rendering: bold is bold, code has the code font and background, the code block has a copy button | C | — | one glance per Godot version bump. Godot renders standard doc BBCode, which A has already pinned, exactly as it does for GDScript's own docs |

### Syntax colours

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| No type in a colour-plain line: `vector2i`, `variant`, `node_internal_mode`, `[]char`, `event(int)` | B | editor layer: `syntax_highlighter.get_line_syntax_highlighting(line)` on the `CodeEdit` holding a fixture, with `VerseSyntaxHighlighter` in place | the colour at each name's column isn't `text_editor/theme/highlighting/text_color` |
| The three tiers: engine types, base types, and user types, including a binding and a second class, struct, and enum declared in the file | B | as above | each column's colour equals the corresponding editor setting: `engine_type_color` for `node2d` and `node_internal_mode`, `base_type_color` for `vector2i` and `variant`, `user_type_color` for the project's classes and bindings, including on the declaration line itself |
| A binding for a class that a GDExtension registered is engine-coloured | B, blocked | as above | needs a fixture project with a real addon; nothing in the repository has one |
| Whether the tiers read well | C | — | not needed. The colours come from the same theme settings GDScript uses |

### Gutter, warnings panel, and diagnostics

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| B4: a connected handler gets the Slot icon, including in a script under a `.vmodule` | B | editor layer: a scene connecting a signal to a Verse method; `get_line_gutter_icon(line, connection_gutter)` | the icon isn't null and the metadata names the method, for a root-module and a module script |
| `refresh_script_warnings`: each export, signal, and Stage C warning is on its member's line and clears when fixed | B | editor layer: read the `ScriptTextEditor`'s warnings-panel `RichTextLabel`; edit the line with `set_line()`, wait for the idle `_validate`, read it again | the panel names the member's line; after the fix, it doesn't |
| Stage B: the `@global_class` warning is on the **attribute's** row, clears when the attribute is deleted, comes back when it's retyped, and only one of two attributes is flagged | B | as above, on `inert_global.verse` | the panel's line number is the attribute's; the three states follow the edits |
| R-EXP-8 step 6: `Mismatched` is absent from the inspector and its sentence is on its line | B | inspector rows (below) plus the warnings panel | no `EditorProperty` for `Mismatched`; the panel carries the sentence at its line |
| The gutter's warning glyph being drawn | C | — | not needed once the panel's line is asserted. Drawing a marker Godot was given is Godot's |

### Placeholders, saving, and reload

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| B26 (1): with an `int` and a `node2d` export set in the inspector, breaking the script and saving keeps both values | B | editor layer: open a scene; set both through the placeholder; `save_scene()`; break the `.verse` through `ResourceSaver.save()`, which is `VerseResourceFormatSaver::_save` and so `_reload`; `save_scene()` again | the `.tscn` still carries both values; `node.get()` still answers both |
| B26 (2): fixing the script keeps both values, and the node runs the new code | B | as above, then fix and save; then Play (headless child) | values survive; the game's output shows the new code ran |
| B26 (3) and B39 (2): a break in a **different** `.verse` file | B | as above, breaking another fixture file | the same assertions, and no `= null` line appears |
| B39 (1): an un-overridden inherited export isn't written as `= null` | B | editor layer: instance the node's scene into a second scene; break; save the second scene | the second `.tscn` has no line for the export |
| B39 (3): an override in the second scene survives a break and a save | B | as above, with the value overridden | the override's line is unchanged |
| A GDScript control beside each of the above | B | the same steps on a `.gd` | Godot's own behaviour, so that a failure can be attributed. See [the stock-GDScript observation](#one-stock-gdscript-observation-worth-keeping) |
| B8: adding `@tool` to an attached script needs a scene reload | B | editor layer: add `@tool` to the source, `ResourceSaver.save()`, then ask the node | whether the node now holds a real instance: `script.can_instantiate()`, and a method call answering rather than a placeholder's null. Today the expected answer is "still a placeholder"; the case records it, and it flips when B8 is fixed |
| R-EXP-6 steps 2–5: a new `SettingsResource` shows `untitled`, `3`, `1.5`; edits survive a save and a reopen; `script_class="SettingsResource"` is on disk; Play reads the edited values | B | editor layer: create the resource from the script; set values through the placeholder; `ResourceSaver.save()` to a `.tres`; reload it with `CACHE_MODE_IGNORE`; Play a scene that loads it | the defaults, the reloaded values, the `.tres` text, and the game's `Describe()` output |
| R-EXP-6 step 1: the Create New Resource dialog finds `SettingsResource` under `Resource` | A + B | A: `ProjectSettings.get_global_class_list()` carries the class with base `Resource`. B: `EditorInterface.popup_create_dialog(callback, "Resource")`, then search the dialog's tree | A: the row. B: the tree has the class under `Resource` (not run in this audit) |

### Inspector, docks, and dialogs

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| R-EXP-8 steps 1–5: `Portrait` is a file field filtered to `.png`/`.jpg`; `SaveFolder` browses to a directory; `Notes` is multi-line; `Elements` is three flags; `Target` is a node picker | A (hints) + B (controls) | A: `get_script_property_list()`, already asserted. B: editor layer, `inspect_object()`, then each `EditorProperty`'s class | `EditorPropertyPath` for both paths, `EditorPropertyMultilineText`, `EditorPropertyFlags`, `EditorPropertyNodePath`. Proven for GDScript's equivalents |
| R-EXP-8 step 4: ticking Fire then Earth stores 5 | B | drive the `EditorPropertyFlags`'s check boxes | the node's value is 5 |
| R-EXP-8 step 7: the scene dock and create dialog show `icon.svg`; with the file deleted, Godot falls back | B | editor layer: the `SceneTreeEditor` tree's item icon | the icon's `resource_path` is `res://icon.svg`; after deleting it, the icon is the base class's. Proven for GDScript. This is also the only reachable test of `_get_class_icon_path` |
| Node panel: an `@export_signal` `Own<private>:event(int)` is listed beside the public ones, with its payload | B | editor layer: select the node; read the `ConnectionsDock` tree | a row `Own(...)` under the script's class, with the payload. Proven for a GDScript signal |
| Node panel: the connection saves into the scene and fires at runtime | A + B | A: the integration layer already asserts connect and deliver. B: `connect(..., CONNECT_PERSIST)`, which is what the dialog does, plus `save_scene()` | the `.tscn` has the `[connection]` line; a headless Play prints the handler's output |
| B3: the `_make_function` stub compiles | B | editor layer: emit `EditorNode`'s `script_add_function_request (object, name, args)`, which is what "Make Function" in the Connect dialog emits and what reaches `_make_function`; then `ResourceSaver.save()` and check the diagnostics | the stub text ends with `{} # Replace with function body.` and the file analyses with no error. This retires CLAUDE.md's claim that the editor's own C++ is the only caller |
| R-EXP-7: the autoload dialog refuses `settings_resource.verse` and accepts `game_state.verse` | B | editor layer: find `EditorAutoloadSettings` and call its bound `autoload_add()` | Godot's sentence `Failed to create an autoload, script '...' does not inherit from 'Node'.` for the first and none for the second. `autoload_add` itself answered `true` for the refused one, so assert the printed sentence, not the return value |
| R-EXP-7: a `@tool` Verse autoload is instantiated in the editor; a plain one isn't | B | editor layer, with `game_state.verse` as a `@tool` autoload in the copy's `project.godot` | the editor's scene tree root has the autoload node, and it answers a method from the last built generation |
| B5: the Attach Script dialog lists the template and an `Empty` row | B, optional | `ScriptEditor.open_script_create_dialog()`, then its template `OptionButton`'s items | both rows exist, and the `Empty` row's content is empty |

### Debugger and profiler

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| A breakpoint set in the gutter stops the game (the `--breakpoints` path) | B | editor layer: `set_line_as_breakpoint()` on a `.verse`, then `play_custom_scene()`; `EditorDebuggerPlugin` session's `breaked` | `breaked` fires; the stack panel's top row names the `.verse` file and line |
| A breakpoint toggled while the game runs arms (the live `breakpoint` message) | B | as above, toggled after `is_playing_scene()` | it stops at that line |
| Stack Variables shows locals, members, and `self` under **members** | B | editor layer: the `EditorDebuggerInspector`'s `EditorProperty` rows | `Locals/<name>` rows with values; `Members/self` present and no separate `self` row; a `vector2` local arrives as a `Vector2` |
| Step Over moves one line; Step In and Step Out; Continue | B | editor layer: press the panel's buttons by `tooltip_text` | the stack's top row after each, including `Total := Helper()`, whose line is reported twice and must not be stopped on twice |
| Skip Breakpoints | B | the panel's skip button | a second pass over the line doesn't stop |
| The profiler shows Verse functions with call counts | B | editor layer: `EditorProfiler`'s Start button, then its tree | a row for a Verse function with a non-zero call count |
| The profiler's accumulated table (the stride trap) | B | B2's wire client or the editor layer: stop the profiler | `servers:profile_total` has more than one row, each with a signature that names a Verse function. That turns "reasoned from the engine's version" into a measurement |
| All of the above without the editor | B, alternative | B2's Python server | the same assertions over the wire. Useful if the panel's internal structure proves brittle across Godot versions |

### Other entries

| Check | Class | Mechanism | What to assert |
| --- | --- | --- | --- |
| B43: a host fatal error during Play is reported when Play ends, once | B | editor layer: start the editor with `VERSE_HOST_TEST_FATAL=check` in its environment, which the headless child inherits; Play; wait for `is_playing_scene()` to become false; read the Output panel's `EditorLog` text | the panel has `The game ended in a Verse host fatal error:` and a stack naming `GodotVerse::FireTestFatal()`; a second Play without the variable adds nothing |
| R-EXP-9: an RPC reaches a second peer, with `authority`, `any_peer`, and `call_local` behaving | B | a two-process harness: two headless game processes from a copy of `tests/integration`, each with a GDScript autoload making an `ENetMultiplayerPeer` on localhost, and the same node path | the six steps' results, printed by each process and compared by `run_tests.py`. No editor is involved |
| B14 / R-DIST-10: the exported game runs with `UE_ROOT`, `VERSE_HOST_DLL`, and `VERSE_COOKER` unset, `PATH` cut, and its own working directory | A | the `export` layer, launching the exported game with that environment | the same case list, green. A clean machine is still C (see below) |
| B11: `_CanDropData` accepts a drop and `_DropData` runs | C* | — | headless has no window-enter event, which is what sets the drag target (`viewport.cpp:3525`), and `_can_drop_data` has no ClassDB method. It's not visual: it needs a real display server. Keep it by hand until an unattended windowed run is allowed |

## The residual by-hand list

After the plan, this is everything left for a person:

1. **Rendering, once per Godot version bump, about five minutes.** Open `tests/integration` in a
   windowed editor. Hover `Prose` and check that bold is bold, code spans have the code font and
   background, and the code block has a copy button. Look at the class page for `hover_probe`.
   Glance at the three colour tiers in `hover_probe.verse`, and at the `hints.verse` inspector.
   Everything these draw from is asserted as text or colour values beforehand, so this check is
   about Godot's renderer, not the bridge.
2. **`_CanDropData` (B11).** Not visual, but unreachable headless (see the table).
3. **B38's regeneration race in a fresh windowed editor.** Not visual: it needs `EditorHelp`'s
   script-doc regeneration, which only a non-headless display server runs.
4. **A clean machine for R-DIST-10 (B14).** Not visual: it needs a machine that has never built
   anything, which is Phase 8's to arrange.

Items 2 and 3 need a display server, not a person. If an unattended windowed run becomes
acceptable, for example on a dedicated machine, the editor layer's plugin runs unchanged there and
both become automated. Item 4 needs a second machine.

## Implementation plan

Each step ships on its own and leaves every layer green. The estimates include writing the cases
and running the suite, not reviews. Steps 2 onward share the editor layer from step 2. Steps 3 to 8
can then be done in any order, but the order below puts the checks that are owed and have no test
first.

### Step 1. Turn the existing seams into cases (1 day)

Add cases to `tests/integration/test_cases.gd`, inside the existing editor-only hover-and-completion
block, for B13 (`first_options` has `_Ready<override>():void =`), B22 (all five hooks), B24 (no
`Getter`/`Setter` options), B27 cause 1 (`insert_text` ends in `[`, and `call_hint` isn't empty at
the next caret), and the named-argument popup's three carets. In the `export` layer, launch the
exported game with the B14 environment scrubbed. No new infrastructure.

### Step 2. Build the editor layer (2 days)

- **`tests/editor/`**: a driver `EditorPlugin` (`addons/verse_editor_cases/`) that waits for the
  filesystem scan and prints one record per case in the `tools/test_records.py` shape, with a
  watchdog that fails the run by name rather than hanging.
- **`run_tests.py --only editor`**: copy `tests/integration` to a throwaway directory, as the
  `export-vm` layer does, and add the plugin to `[editor_plugins]` and
  `editor/run/main_run_args="--headless"` to the copy's `project.godot`. Stage the extension, run
  the import pass (`--headless --editor --quit`), then run `--headless --editor`. Pass
  `APPDATA`/`LOCALAPPDATA` pointing into the copy, so the owner's editor settings are never
  touched, and set `network/debug/remote_port` to a port no other editor uses.
- **Guards:** the plugin refuses to press Play unless it reads `--headless` in `main_run_args`, and
  does nothing at all unless `DisplayServer.get_name() == "headless"`. A copy opened in a windowed
  editor therefore does nothing.
- It loads the host, so it's a host-dependent layer like `integration`: local only, and it runs
  under the host token.
- The first cases are the harness's own: the editor hint, a placeholder for a `.verse` node, the
  `CodeEdit` for a `.verse`, and `VerseSyntaxHighlighter` as its highlighter.

### Step 3. Placeholders and saving (2 days)

B26 steps 1–3, B39 steps 1–3, a GDScript control for each, B8 (recorded as its current answer),
and R-EXP-6 steps 1–5. These three entries are all marked "fixed, by-hand check owed" and have no
test at all, so this is the highest-value step.

### Step 4. The code editor (3 days)

The completion popup (`vector2{`, `?`, the prefix table), B27's re-request after confirming, after
Play, and with a save in between; the hover tooltip for `Prose`, `Blocked`, `Indented`,
`RootConstant`, `Emit`, `Subscribe`, and `Await`; the class reference page; B29's `symbol_lookup`;
the three colour tiers; B4's connection gutter; and the warnings panel for R-EXP-8's `Mismatched`,
Stage B, and `refresh_script_warnings`.

### Step 5. The debugger and profiler through Play (2 days)

Both arming paths, the stack panel, Stack Variables (including `self` under members and a
`vector2` local), Step In, Step Over, Step Out, Continue, Skip Breakpoints, a live toggle, the
profiler's rows with call counts, and the accumulated table on stop. Use a copy of
`tests/host_smoke/debug_probe.verse` as the fixture, whose line numbers are already part of a test.

If walking the panel's internal nodes proves brittle, add `tools/debug_wire.py` (B2's client, about
250 lines with the decoder) and assert the same things over the wire: 1 more day.

### Step 6. The inspector, docks, and dialogs (1.5 days)

R-EXP-8's controls, the flags value, and `@icon` with its fallback; the Node dock's `<private>`
signal and a persisted connection; B3's `_make_function` stub through
`script_add_function_request`; R-EXP-7's autoload refusal and the `@tool` autoload; and,
optionally, B5's template rows.

### Step 7. B43's host fatal during Play (0.5 days)

Start the editor layer's Godot with `VERSE_HOST_TEST_FATAL=check` for one extra run, and read
`EditorLog`.

### Step 8. R-EXP-9's second peer (1 day)

A two-process ENet harness in `run_tests.py`, over a copy of `tests/integration`, asserting the six
steps.

### Step 9. Retire the prose (0.5 days)

Cut `by-hand-findings.md` §"What is still open" down to [the residual list](#the-residual-by-hand-list).
Replace "no automated test and never will", "Nothing automated can see any of this", and the
entries in CLAUDE.md's "What cannot be tested from here" with the case that now covers each, as
architecture-review item 7 asks.

**Total: about 13.5 days, or 14.5 with the wire client.**

## Godot version sensitivity

The editor layer walks editor internals. Those move between Godot versions, and on 4.7.2 against
the 4.8-dev checkout two already have:

- **The hover path.** In 4.7.2, `ScriptTextEditor` connects `CodeEdit`'s `symbol_hovered` to
  `_show_symbol_tooltip` (`script_text_editor.cpp:2682` at the tag). The 4.8-dev source replaces
  that with its own hover timer ("ScriptTextEditor uses its own timer instead of the CodeEdit
  symbol_hovered signal"). On a bump, raise the tooltip through the `script_text_editor/show_tooltip`
  shortcut (Alt+/, `SHOW_TOOLTIP_AT_CARET`) instead.
- **The lookup field** B29 records (`script` in 4.7, `script_path` in 4.8).

Everything else found by class name or by an English `tooltip_text` (`EditorHelpBitTooltip`,
`ScriptEditorDebugger`, `EditorDebuggerInspector`, `EditorProfiler`, `ConnectionsDock`,
`SceneTreeEditor`, `EditorAutoloadSettings`, `Step Over`) is the same kind of foreign contract as
architecture-review item 4. Each lookup should fail with a sentence naming what wasn't found,
rather than as a missing row. The isolated editor settings should pin `interface/editor/editor_language`
to `en`, so that the tooltip strings match.

## Appendix: the probe calls

The scratch files aren't committed. These are the calls that proved each row, so that step 2 can
reproduce them.

```gdscript
# Placeholder and save
EditorInterface.open_scene_from_path("res://main.tscn")
var root := EditorInterface.get_edited_scene_root()
root.get_script().can_instantiate()        # false: a placeholder
root.set("speed", 42)
EditorInterface.mark_scene_as_unsaved()
EditorInterface.save_scene()               # OK; main.tscn now has `speed = 42`
scr.source_code = broken; ResourceSaver.save(scr)

# Script editor
EditorInterface.edit_script(scr, 17, 0)
var ce: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
ce.request_code_completion(true); ce.get_code_completion_options()
ce.get_code_completion_selected_index()    # -1 unless the popup is active
ce.code_completion_requested.connect(counter); ce.confirm_code_completion()
ce.syntax_highlighter.get_line_syntax_highlighting(line)
ce.get_line_gutter_icon(line, connection_gutter)

# Hover and class page
EditorInterface.get_script_editor().update_docs_from_script(scr)
ce.emit_signal("symbol_hovered", "speed", line, column)   # an EditorHelpBitTooltip child of ce
EditorInterface.get_script_editor().goto_help("class_name:Mover")

# Inspector, docks
EditorInterface.inspect_object(root)       # then each EditorProperty's get_class()
EditorInterface.get_selection().add_node(root)            # then the ConnectionsDock's Tree
EditorInterface.get_base_control().get_parent().emit_signal(
		"script_add_function_request", root, "_on_probe_made", PackedStringArray(["amount: int"]))

# Play and the Debugger panel
EditorInterface.get_editor_settings().set_setting("network/debug/remote_port", 6123)
ce.set_line_as_breakpoint(line, true)
add_debugger_plugin(probe)                 # its session's `breaked` signal
EditorInterface.play_custom_scene("res://main.tscn")
# then the ScriptEditorDebugger's Trees, EditorDebuggerInspector rows, the "Step Over" button,
# and the EditorProfiler's "Start" button and Tree
```

`find_all(node, class_name)` in the probe is a recursive walk over `get_children(true)` testing
`is_class()`, which works for editor classes that aren't exposed to ClassDB, because `GDCLASS`
still answers `get_class()`.

## Status

All nine steps of the plan are done, and `docs/by-hand-findings.md` and `CLAUDE.md` are retired
down to [the residual list](by-hand-findings.md#what-is-still-checked-by-hand):

| step | what it built | commit |
| --- | --- | --- |
| 1 | the existing completion seams turned into integration cases (B13, B22, B24, B27 cause 1, the named-argument popup); the export layer's launch scrubbed to B14's environment | `8d5ebc3` |
| 2 | the `editor` layer itself: the driver plugin, `run_tests.py --only editor`, the harness's first eighteen cases | `4e716e8` (isolated from Devin's real profile in `2016dfd`) |
| 3 | placeholders and saving: B26, B39, B8, R-EXP-6 steps 1–5 | `3e4ef82` |
| 4 | the code editor: the completion popup, B27's re-request, the hover tooltip, the class page, B29, the colour tiers, B4's gutter, the warnings panel | `27fbdce` |
| 5 | the debugger and profiler through Play, in the editor layer and over the wire | `5c16373` (editor), `66fedcb` (the `debug-wire` layer) |
| 6–8 | the inspector, docks and dialogs; B43's host-fatal session; the `multiplayer` layer for R-EXP-9 | `26aca64` |
| 9 | this commit: retiring the prose in `by-hand-findings.md` and `CLAUDE.md` |  |

**Final case counts** (2026-09-27): `editor` 217/0/24 plus its host-fatal session 16/0/0,
`debug-wire` 38/0/2, `multiplayer` 38/0/0, `integration` 611/0/5.

**`known defect:` skips**, read off the case sources rather than fixed — ten, across the three
layers:

- **`editor_cases.gd`**: after a Play, a call completed in the editor settles on an empty argument
  hint although the re-ask arrives; after B27's Plays and the unfinished calls typed behind them,
  every `hover_probe.verse` member hovers with no description; `signal_ref.Await`/`.Subscribe`
  still hover as a Local Constant; B40's registered page (`event.Emit`, `signal_ref`'s members) has
  an empty description, because `publish_api_method` is handed the host's own empty doc;
  `READY_DEFECT` (a breakpoint in the main scene's `_Ready` never fires — the Verse debugger
  attaches from `_frame`, after `_Ready` runs) and `TWICE_DEFECT` (a line holding a call reports
  its location twice, so a breakpoint there stops twice per arrival); `NODE_PATH_DEFECT`
  (`@export_node_path` on a `string` draws a plain text field, not a node picker);
  `TOOL_AUTOLOAD_DEFECT` (a `@tool` Verse autoload holds a placeholder rather than a real
  instance); `GLOBAL_ICON_DEFECT` (a `@global_class` script's `@icon` never reaches the scene dock
  or create dialog); and Make Function's stub not compiling (`Int:?` — the dialog's `name: Type`
  spelling, with its leading space, is handed to `verse_type_for_godot_type` unstripped).
- **`debug_wire.py`**: `READY_DEFECT` and `TWICE_DEFECT` again, in the same words, over the wire
  rather than the panel.
