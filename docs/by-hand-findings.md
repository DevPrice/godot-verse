# What the by-hand session found

**Status:** 2026-09-14 · the record of the windowed passes over
what was `docs/by-hand-checklist.md`, and of the work that closed what they found. **The checklist is
deleted**: all twenty-two of its entries were watched happen, and what is worth keeping is what they
found rather than the list. §"What is still open" at the bottom carries the three things that
outlived it. **Every entry below is fixed**, except the two that are not defects: B11 is a
measurement, and B8 is fixed for the half a headless run can reach and re-checkable by hand for the
other. `tools/run_tests.py` is 9/9 with **431** integration cases.

Four kinds of entry are below. **B1–B9** are what the session saw go wrong, each traced to the code
that causes it rather than left as a symptom. **B10–B11** are what the session learned about the
checklist itself: one entry did not need to be on it, and one belongs there permanently. **B12** is a
Verse fact found while writing B10's test, which the test had got wrong. **B13** is from a later
session, after the editor-performance commits, and is the one entry here that is about latency
rather than behaviour. **B14–B18** are from the sessions each later phase owed, and B18 is the one
that came from somebody else's project rather than from a checklist — which is why it found the
combination no fixture had.

Three of these were checked with the two tools this repository already has for the purpose rather
than by reasoning about them — `tests/verse_probe` for B2 and B3, a scratch Godot project driven
headless for B9, B10 and B11. §0 of [`phase-4-gaps.md`](phase-4-gaps.md) is why, and it held again
here: B4's obvious diagnosis is not its whole cause, and B11's first two plausible workarounds both
failed.

**Companion to:** [`phase-4-gaps.md`](phase-4-gaps.md) (G19, which B10 partly retires),
[`phase-4.5-design.md`](phase-4.5-design.md) §11 and [`phase-5-design.md`](phase-5-design.md) §14
(where B6 and B7 change what those phases built).

---

## B1. Override completion is dead in a class body, and has been since Phase 4 · **fixed**

Typing a bare name on its own line inside a class body is supposed to offer every inherited virtual
as the whole declaration that overrides it — `_Ready<override>():void =`, GDScript's
`func _ready() -> void:`. It offers none.

`completes_as_override` (`src/verse_script_language.cpp:974`) rejects any option whose owner is a
mirrored Godot class:

    if (verse_godot_class_for(owner) != nullptr) {
        return false;
    }

That guard was right when it was written. Phase 2 (`d188f5c`) added it because `is_overridable`
answers *"the compiler would accept an `<override>` of this"*, which is true of all 9597 mirrored
methods, and overriding `GetName` compiles and changes nothing. At that point Godot's virtuals were
hand-written on the native root, so excluding the mirror wholesale excluded exactly the useless
half.

Phase 4 (`3f4a66d`, "generate every Godot virtual, under Godot's own name") moved all 1413 virtuals
*onto* the mirrored classes — `_Ready` is `node`'s, `_Draw` is `canvas_item`'s. The guard has
rejected every one of them since. The line below it, `owner != "vh_object" || godot_method_for(...)`,
is now reachable only for `_Notification`, and the comment above the function still describes the
Phase 2 world.

**What closes it.** The guard needs to tell a Godot *virtual* from a mirrored concrete method, and
`godot_method_for` cannot: `verse_api::methods` carries every method, not only the virtuals. Add an
`is_virtual` column to `method_mapping` in `tools/gen_verse_api.py` — the generator already reads
`is_virtual` out of `extension_api.json` to decide what to emit — and admit a mirrored owner only
when the row says virtual. The one hand-written row, `{ "vh_object", "_Notification", "Object",
"_notification" }` (`src/verse_api_classes.h:13617`), has to carry the flag too.

**This is the upstream cause of B2**, and probably of half the friction the session met: the editor
never offered a correct signature, so every override was typed from memory.

## B2. `_Input` is not broken; the parameter name was · **no defect**

Reported as *"this doesn't work"*, with glitch 3532 — `The data (…/_Input:)event … is ambiguous with
… function (/Verse.org/Verse:)event(:t)`. Put to the compiler through `tests/verse_probe`, both
spellings at once:

- `_Input<override>(Event:input_event):void` — **compiles**, and the probe's method list reports
  `_Input params=1 result=0 virtual=_input`, so it binds to Godot's `_input`.
- `_Input<override>(event:input_event):void` — the reported error, exactly.

The mirror declares the parameter `Event` (`GodotClasses.native.verse:28973`) and that is the
spelling that works. Lower-case `event` is one more instance of the trap CLAUDE.md already records
— *a module-level name and a local of that name are ambiguous, not shadowing* — with
`/Verse.org/Verse`'s `event` as the module-level name.

**Nothing to fix in the bridge**, and deliberately no diagnostic: an ambiguous local is a Verse
diagnostic, not a Godot one, and B7 is the rule that says the bridge does not annotate those. What
would have prevented it is B1. Worth adding `event` to the ambiguous-name list in CLAUDE.md beside
`Angle`, `Length` and the rest, since it is now the one an author meets first.

## B3. `_make_function` writes a stub that does not compile · **fixed**

Connecting a signal with "Make Function" checked writes

    OnPlayerHit<public>()<transacts>:void =
    	# TODO

which is *"Dangling `=` assignment with no expressions or empty braced block `{}` on its right hand
side."* A comment is not an expression, so the body is empty.

`_make_function` (`src/verse_script_language.cpp:668`) ends with

    out += String(")<transacts>:void =\n\t\t# TODO\n");

Both repairs were put to the compiler and both compile — `= {}` on one line, and the comment
followed by an indented `{}`. The second keeps the shape GDScript writes, so:

    out += String(")<transacts>:void =\n\t\t{} # Replace with function body.\n");

matching the wording B6 adopts for the template. The `<transacts>` stays: `Subscribe` fixes its
callback at that effect and a specifier-less handler is wall 8 on the author's first generated line.

## B4. A connected handler gets no gutter icon, and there are two reasons · **fixed, re-check by hand**

GDScript draws a Slot icon in the gutter beside a method a persistent connection targets. A Verse
handler gets none.

`ScriptTextEditor::_update_connected_methods` (Godot's `editor/script/script_text_editor.cpp:1568`)
matches each connection's method name against `functions`, the `"functions"` key of
`ScriptLanguage::validate`. Two things stop it:

1. **It returns early unless `validation_success`.** The stub B3 writes does not compile, so on the
   file the editor had just written, the gutter could not have been drawn whatever else was true.
   Fixing B3 may be all this needs — but it is not all that is wrong.
2. **The functions list is empty for any script in a module.** `_validate`
   (`src/verse_script_language.cpp:493`) asks `runtime->class_members(p_path.get_file().get_basename())`,
   and every `ClassNameUtf8` in the ABI is module-qualified — `gameplay/player`, not `player`. A
   script under a `.vmodule` marker therefore has no method outline either, which is the same bug
   seen from the other side.

Fix both, then re-check by hand: the gutter is drawn by the editor and no headless run sees it.

## B5. "No suitable template", on a dialog that then writes a template · **fixed**

The Attach Script dialog reports *"No suitable template."* and disables the menu, and then creates
the file from the template anyway.

Both halves are correct behaviour given what the bridge answers. `_get_built_in_templates`
(`src/verse_script_language.cpp:613`) returns an empty array; `ScriptCreateDialog` finds zero
templates in all three locations and sets that message
(`editor/script/script_create_dialog.cpp:691`). `_make_template` is called regardless, with an empty
`p_template` — which the bridge ignores, building the source from scratch.

**What closes it** is doing what GDScript does, which is the other way round. Return the templates
from `_get_built_in_templates`, keyed on the base class they inherit, with content carrying `_BASE_`
and `_CLASS_` placeholders; then have `_make_template` substitute into `p_template` instead of
ignoring it (`GDScriptLanguage::make_template`, `modules/gdscript/gdscript_editor.cpp:107`).
`_BASE_` still goes through `verse_base_class_for`, since the dialog hands over a *Godot* class name
and the template needs the mirrored one.

**And a second row, found when the first fix was checked by hand: unchecking the dialog's Template
checkbox still produced the template.** The checkbox does not mean "pass no content".
`ScriptCreateDialog::_get_current_template` looks through the list for a built-in named exactly
**`"Empty"`** and uses *its* content, and falls back to a default-constructed `ScriptTemplate` —
content `""` — when there is none. So with one row registered, unchecking the box asked for a
template that did not exist, got `""`, and `_make_template`'s fallback answered with the full
default. Two changes: an `"Empty"` row whose content is a blank file, and no fallback in
`_make_template` — an empty template is an empty file.

Blank means blank. A `.verse` with no class named after itself is a **library file** (R-LANG-6),
which is how a project's shared code is written and is supported on purpose; and the same dialog
opens from the FileSystem dock's right-click, where there is no node in the picture at all.

## B6. The template becomes a direct translation of GDScript's · **done**

Decided in the session rather than found: the four-line `<suspends>`/`spawn` block and the two-line
`<transacts>` block go, and the file becomes GDScript's own template with GDScript's own comments.
It must compile as generated, which GDScript's does and today's does not — `{}` is Verse's `pass`:

    using { /Godot.org/Godot }

    _CLASS_ := class(_BASE_):

    	# Called when the node enters the scene tree for the first time.
    	_Ready<override>():void =
    		{} # Replace with function body.

    	# Called every frame. `Delta` is the elapsed time since the previous frame.
    	_Process<override>(Delta:float):void =
    		{}

Tabs are not a preference here and the existing comment saying so should survive the rewrite: Godot's
editor writes tabs and Verse rejects a file that mixes them, so a space-indented template breaks on
the author's first line.

**This unticks two checklist entries** — Phase 4.5's "a new script's template carries the warning"
and Phase 5's "a new script's template says how to wait" both assert text that will no longer be
there. Rewrite them to assert the new template, or delete them; do not leave them asserting the old.
What those two lines were *for* does not go away with them, and `dodge-the-creeps.md` wall 8 is still
standing.

## B7. `explain_effect_errors` goes; the other two appenders stay · **done**

The rule settled in the session: **the bridge annotates a diagnostic only where the bridge is what
the author is confused by.** That keeps `explain_skipped_members`
(`src/verse_script_language.cpp:2436`), whose subject is a Godot API member the mirror does not
carry, and `note_missing_imports` (`:2530`), whose subject is a module — a `.vmodule` marker being a
bridge invention Verse's own diagnostic cannot know about. It drops `explain_effect_errors`
(`:2480`) entirely: an effect that does not fit its context is plain Verse, and the compiler's own
sentence is enough.

The session produced a second, harder argument for dropping it. A fixture written for B10 asked a
`<suspends>` body from the wrong caller and got:

> This invocation calls a function (`(/Godot.org/Godot/signal:)Await`) that has the
> 'suspends' effect, which is not allowed by its context. **`Await` is one of Godot's own and does
> change the scene, so it is this function that has to widen rather than that one: write
> `<transacts>` on the function containing this line.**

The bolded half is the bridge's, and it is **wrong** — `<transacts>` is precisely what an awaiting
body may not carry. The matcher keys on glitch 3512 and the callee's package alone and never looks
at *which* effect was refused, so every `suspends` refusal from a Godot signal takes the
`transacts` branch. This is the failure mode an appended sentence has and a compiler diagnostic does
not: it is confident, it is about a rule the appender does not actually check, and it survived a
phase.

## B8. A `@tool` script only takes effect after an editor restart · **fixed; half of it testable**

Adding `@tool` to a script already attached to a node, and editing the body of a `@tool` script
already running in the editor, both do nothing until the editor is restarted. Build Verse does not
help, and neither does Play.

`_reload_tool_script` and `_reload_scripts` (`src/verse_script_language.cpp:593` and `:600`) each do
one thing: `script->compile()`. Nothing re-instantiates. In the editor a non-`@tool` script's node
holds a *placeholder* instance (`_can_instantiate` is `is_compiled() && (_is_tool() || !editor_hint)`,
`src/verse_script.cpp:392`), and a placeholder does not become a real instance because the source
changed — something has to replace it.

GDScript's `reload_scripts` is the shape to copy (`modules/gdscript/gdscript.cpp`, around 2540–2590):
for every object holding the script it saves the exported property values, calls **`obj->set_script(scr)`**
— which is what re-runs instance creation and so swaps a placeholder for a real tool instance, or
back — and then restores the saved values onto whichever kind of instance it got.

The bridge cannot do that yet, because **`VerseScript` does not track its owners**: it keeps
`placeholders` as opaque `void *` and knows nothing about live instances. So this is two pieces of
work — record the owning object in `_placeholder_instance_create` and in `VerseScriptInstance`
first, then the save / `set_script` / restore pass over them.

**Checked by hand afterwards, and one half of it is still open.** Editing the body of a `@tool`
script that is already running in the editor now takes effect on save, and so does editing an
ordinary one. **Adding `@tool` to a script that did not have it still needs the scene reloaded** —
the node keeps the placeholder it was given until then.

That is the transition `reload_instances` was written to cover and the one no automated layer can
reach, because a placeholder only exists under `is_editor_hint()`. What is known: the re-attach
itself works, since the integration case proves a reload replaces a real instance and carries its
exported values; and `_can_instantiate` is `is_compiled() && (_is_tool() || !editor_hint)`, with
`_is_tool` read from the text, so by the time the saver calls `_reload` the answer should already
have flipped. What is not known is whether `_reload` is reached at all on that save, or whether
`set_script(Variant())` / `set_script(self)` declines somewhere in between. Left open deliberately;
the workaround is one scene reload. **Where to look next:** a print in `reload_instances` and one
windowed session says which of the two it is.

## B9. A runtime error's `ERROR:` line runs four fields together · **fixed**

From the B10 raise fixture, the line Godot prints:

    ERROR: ErrRuntime_NativeInternal: An internal runtime error occurred … already freed. Test
    IsInstanceValid[...] before reaching through a reference the scene may have dropped.)
    (/Godot.org/Godot/node:)GetNameGodotClasses.native.verse28893

The message, the function, the file and the line are concatenated with no separators at all —
`GetName` + `GodotClasses.native.verse` + `28893`. The multi-line `LogVerseRuntime` block printed
just above it is well-formed; it is only the single-line `ERROR:` push that is malformed. A format
string, and the smallest fix on this page.

## B10. `_HasPoint` *is* reachable headless, and G19 is half retired · **automated**

`phase-4-gaps.md` G19 records `_HasPoint` and `_CanDropData` as having no public caller, so no
headless run can make the engine ask. That is right about the *public API* and wrong about
`_HasPoint`, which the engine asks on its own as soon as something injects a click:

    var ev := InputEventMouseButton.new()
    ev.button_index = MOUSE_BUTTON_LEFT
    ev.pressed = true
    ev.position = Vector2(50, 50)
    Input.parse_input_event(ev)

Two Controls on the same rect, the one on top a Verse class overriding `_HasPoint`, both printing
from `_GuiInput`. Run both ways under `--headless --fixed-fps 60`:

| `_HasPoint` answers | which control's `_GuiInput` ran |
| --- | --- |
| `false` | the one **underneath** — the click fell through |
| `true` | the one **on top** |

That is the checklist's requirement, met by the engine's own picking path rather than by calling the
method. **It belongs in `tests/integration`**, where it would have caught a wrong default without a
person; the by-hand entry should keep only `_CanDropData`.

## B11. `_CanDropData` stays on the by-hand list, and this is why · **still owed**

The same approach does not reach it, and two workarounds were tried and failed.

`Control.force_drag("payload", null)` starts the drag — `get_viewport().gui_is_dragging()` answers
true — and a released button ends it. In between, `_CanDropData` is never asked and `_DropData`
never runs, at any target position tried, including one placed under the origin in case the pinned
mouse position was the whole story.

The reason is upstream of the drop. `Viewport::_gui_input_event` sets
`gui.drag_mouse_over = section_root->gui.target_control` and only then calls `_gui_drop(..., true)`,
which is the call that reaches `can_drop_data` (`scene/main/viewport.cpp:2210` and `:1912`).
`target_control` is set by `Viewport::_update_mouse_over` (`:3525`), and for a native window that
function returns without doing anything unless `gui.windowmanager_window_over` is set — which comes
from the display server's window-enter event, and the dummy driver never sends one. Setting
`gui_embed_subwindows` on the root, which takes the other branch of that same test, changed nothing
from `_ready` or as a project setting.

And there is no way round it by calling directly: `_can_drop_data` has a `GDVIRTUAL_BIND` and no
ClassDB method (`scene/gui/control.cpp:5302`), so nothing outside the engine can invoke it. G19's
claim holds for this one exactly as written.

## B12. A bare `logic` in an `if` clause list is evaluated and thrown away · **recorded**

Found by B10's own test, which passed for the wrong reason and then counted two clicks where one
had been injected. The fixture read

    if (Button := input_event_mouse_button[Event], Button.IsPressed()):

and ran its body on the release as well as the press.

`if (X)` on a bare `logic` is refused outright — *"Expected an expression that can fail in the 'if'
condition clause"*, glitch 3513 — and a clause list with no failable clause in it is refused the
same way. But as soon as **one** clause can fail, which the cast above does, a `logic`-valued clause
beside it is accepted, evaluated, and its value discarded. Put to the compiler on its own:

    Truth()<transacts>:logic = false
    Fallible()<decides><transacts>:int = 1

    if (X := Fallible[], Truth()):   # prints
    if (X := Fallible[], Truth()?):  # correctly skipped

The first branch runs. `?` is what tests a `logic`, and nothing says so at the point it is missing.
This is the same family as the constraint about a continuation line beginning with an operator: it
compiles, it runs, and it answers something other than what was written. It is in `CLAUDE.md` beside
that one now.

Nothing in the repository had it. Every `logic`-returning call in a condition across `host/Verse`,
`dodge-the-creeps`, `demo` and `tests/` was either `?`-suffixed — `Keys.IsActionPressed("move_right")?`
in the yardstick, four times — or a comparison operator, which is genuinely `<decides>`. The one
occurrence was the fixture written three minutes earlier.

**Since recorded, the exposure is gone at the source.** The mirror's 568 predicates are
`<decides>:void` rather than `logic`-returning, so `Keys.IsActionPressed["move_right"]` is the only
spelling there is and those four `?`s no longer exist. The Verse fact above is unchanged and still
worth reading: a `logic` from anywhere else — a `var` of your own, a virtual's return, an accessor
with a `set_` twin — sits in an `if` clause list exactly as quietly as it did. What changed is that
Godot's own predicates stopped handing you one.

---

## B13. The hang is gone; the override list arrives ~1.4 s after the bare names · **fixed**

Seen in a later windowed session, after the first three editor-performance commits. Two halves, and
only one of them was still a defect.

**The hang is gone.** B1's fix made override completion work; what this session saw is that the
editor no longer stops while it is being typed at. That is `dcd517e` and `40d72f4` — every read
keyed by a class name answers from the last analysis' snapshot rather than joining the analysis
thread, and completion stopped running an analysis of its own and stopped waiting for one.

**What was left is a gap, not a stall.** At a member-declaration position the first answer carried
the class's own members, the class names and the keywords; the inherited methods spelled as whole
`<override>` declarations — the thing an author is actually reaching for inside a class body —
only appeared when the completion-shaped analysis landed behind it, about 1.4 s later on the first
`.verse` of a session and ~0.7 s after, and again after every method added, because each is a new
buffer. Nothing was wrong with the list; it was late.

`1469dc1` closes it by putting the candidates in the snapshot: each analysis records, per user
class, the inherited members a subclass could still declare with `<override>`, described by the same
`CollectClassAndSupers` + `DescribeCompletion` that answers scope completion, with the class itself
as the access scope — so an item from `vh_class_override_candidates` formats identically to one from
the refined answer and the full list replaces the partial one without anything moving. 0.8 ms per
class and 0.2% of the analysis it rides on. **To re-check it by hand:** open a script, type a bare
identifier on its own line inside the class body, and the `_Ready`/`_Process` declarations must be
in the *first* popup rather than appearing in it a second later.

---

## B14. R-DIST-10: `dodge-the-creeps` exported, run outside the repo, 30 checks green · **done**

Phase 7b §9, and the one check that cannot be automated from inside the build machine: an exported
game has to be run somewhere the toolchain that built it is not.

**What was done.** `dodge-the-creeps` exported release, headless, from Godot 4.7 to
`C:\Temp\dtc-export\` — outside the repository — and run from a shell with:

- `UE_ROOT`, `VERSE_HOST_DLL` and `VERSE_COOKER` **unset**;
- `PATH` cut to `C:\Windows\system32;C:\Windows`, so no entry points at the Unreal checkout, at
  Godot, or at the repo's `bin/`;
- the working directory the export's own, not the repo's.

`dodge-the-creeps.exe --headless --fixed-fps 60 -- --verse-check` printed **30 ok, 0 FAIL** and
exited **0**. The autoload that drives it is `export_check.gd`, which does nothing at all without
the flag.

**What that proves, and what it cannot.** It proves the exported game finds its host, its engine
directory and its cooked container by *where it is running* and reads no setting and no environment
variable — which is D8, and is the whole of R-DIST-10. **It is not a clean machine**, and the finding
says so rather than implying otherwise: this box has the Visual C++ runtime, a UE checkout on another
drive letter, and whatever else a development machine accumulates. A machine-wide dependency such as
a VC redistributable would not be caught by any amount of scrubbing here, and catching it needs a
machine that has never built anything — which is Phase 8's to arrange, not this one's.

**Two defects it found**, both fixed before the green run:

- **The autoload was processing in every ordinary play of the game.** Declaring `_process` is what
  enables it in Godot, so `set_process(true)` at the end of `_begin` was redundant and the absent
  `set_process(false)` meant `_process` ran from the first frame against a null `checks` — a script
  error per frame in any run without `--verse-check`, which is every run a player makes. The same
  bug was in `tests/integration/export_check.gd`, where it was what made the editor-side run stop
  early.
- **A check was asking the wrong question.** "The node references are gone from the inspector" read
  `main.get("Player")` and expected null. An instance answers a read of *any* member it declares,
  exported or not, so what that read actually tested was an incidental difference between a compiled
  host and a cooked one — it passed in the editor and failed in the export. It asks
  `get_property_list()` now, which is what the sentence always meant, and the two agree.

**To run it again:** export release to a directory outside the repo, then run it with those three
variables unset and `PATH` cut. The flag is `-- --verse-check`; without it the game just plays.

---

## B15. Exporting anywhere but inside the project fails, and the host's working directory is why · **fixed**

Reported from a by-hand export of `dodge-the-creeps` to the Desktop. What it looked like:

```
Verse: verse_cook: compiling
...
Verse: verse_cook: generation 1, 9 package(s), 4 source(s) -> ...
  ERROR: Prepare Template: The given export path doesn't exist.
```

The cook succeeded and the export died straight after it, which reads like a Verse failure. It was
one, but not that one.

**Godot stores an export path relative to the project.** `EditorExportPreset::set_export_path`
converts any absolute path the file dialog returns into a project-relative one
(`editor_export_preset.cpp:380-383`), the dialog hands that relative string straight to
`export_project` (`project_export.cpp:1540`), and `EditorExportPlatformPC::prepare_template` tests it
with `DirAccess::exists` (`editor_export_platform_pc.cpp:156`), which resolves against the **process
working directory**. Exporting to the Desktop therefore stores `../../../Desktop/dtc/...` and the
check is only correct while the editor's working directory is the project.

**And loading the Verse host moves it.** Measured with `tests/cooked_probe`, which prints the
directory around each step: it moves at `LoadLibraryExW` — a static initializer in the monolithic
host — and again inside `vh_init`, where UE's `PreInit` sets it deliberately. Both land on
`<engine>/Engine/Binaries/Win64`. The export plugin builds the project in `_export_begin`, which
Godot calls *before* `prepare_template`, so by the time the check ran the working directory was
inside the Unreal checkout and `../../../Desktop/dtc` resolved to nothing.

Traced in place rather than reasoned about, which is what settled it — the first fix covered only
the `LoadLibraryExW` half and the export still failed:

```
[TRACE] cwd at _export_begin = C:\...\godot-verse\dodge-the-creeps
[TRACE] cwd at end           = C:\...\UnrealEngine\Engine\Binaries\Win64
[TRACE] dir_exists           = false
```

`VerseRuntime::load_host_internal` now restores the directory around the whole load
(`FScopedWorkingDirectory`), which covers both moves. Restoring is safe: UE derives its own paths
from `FPlatformProcess::BaseDir()`, not from the working directory, and every path this bridge hands
the host is absolute. After the fix the same trace reads `dir_exists = true`, the export lands on the
Desktop with its data directory, and the game passes all 30 checks run sandboxed.

**What else this was silently breaking:** every relative path the editor resolved after the first
Verse build, not just the export check. Nothing else in the suite noticed, because `run_tests.py`
always passes absolute paths.

**Two things it is not.** Godot will not create the destination directory either — that is the same
error message for a different reason, and it is Godot's. And `is_tool()` reading a stripped source in
an export (7b §13.9) is unrelated, despite both surfacing in the same session.

---

## B16. The Shipping runtime host works, and D12's remaining half is only a name · **measured, not adopted**

7a's D12 said "Development for the debug template, Shipping for release", and that it was "not true
yet" because the `.gdextension` names one file per platform, so whichever configuration was built
last is the one that ships. Nobody had ever *run* a game on the Shipping host; the 72.7 MiB came
from a link, not from a boot. Run by hand, both configurations end to end:

| | Development | Shipping |
| --- | --- | --- |
| `verse_host_runtime.dll` | 112.5 MiB | **72.7 MiB** |
| a whole `dodge-the-creeps` export | 226 MB | **187 MB** |
| `run_tests.py`'s export layer | 308 / 0 / 9 | **308 / 0 / 9** |
| dtc exported and run sandboxed | 30 / 30 | **30 / 30** |
| startup to the first Verse `_Ready` | 0.54 s | **0.45 s** |

**Shipping is strictly better and nothing behaves differently.** 39 MB smaller, a little faster, and
not one case moves. So the remaining half of D12 is a naming problem and not a real one: what it
needs is for the `.gdextension` to name two files and `tools/build_host.py` to stage under two names.

**Not adopted here**, because which configuration a game ships is a decision rather than a
measurement, and the tree is left on Development — which is what `spec.md` R-DIST-11 and
`phase-7b-design.md` §13.11 describe. The measurement is the point of this entry: whoever builds the
split can start from "it works" instead of from "nobody knows".

One thing the session kept tripping over and is worth saying out loud: **the build stamp keys on
`HEAD`, so every commit invalidates all three host binaries.** That is D6 working, and a cook from
before a commit is refused by a host from after it. Rebuild all three after committing, or the next
export refuses to start with a sentence naming both commits.

---

## B17. A stale host shipped, and the game that got it launched anyway · **both fixed**

Reported from an ordinary export-and-play: the game started, nothing responded, and the log said

```
This game's Verse data was cooked by a different build of godot-verse
(cooked 8002/d909c14, host 8002/eb57bf4). Export the project again.
```

Two defects, one behind the other.

**The host was stale because two tools stage the same directory and neither knew it.** `scons`
copies `demo/addons/godot-verse` into the repo root and into `dodge-the-creeps`;
`tools/build_host.py` writes the runtime host into `demo/`'s copy alone. Run `scons` last and the
host is current everywhere; run `build_host.py` last -- which is what a host rebuild after a commit
looks like -- and `dodge-the-creeps` keeps yesterday's host and exports it. `build_host.py` now
refreshes every copy that exists, and says so per destination.

That is also the session's most repeated friction, and it is worth stating plainly: **the build
stamp keys on `HEAD`, so every commit invalidates all three host binaries.** The stamp is D6 doing
its job -- a cook and a host from different commits is exactly what it exists to catch -- but it
caught a packaging mistake rather than an author's, four times in one session.

**And the refusal was only half-built.** D6 says a game that cannot load its Verse "refuses to start,
with one sentence"; what it did was report and carry on, so every script in the game was dead and the
window opened anyway. Nothing in it responds, because nothing in it is running -- which is a worse
answer than no window. `VerseRuntime::refuse_to_start` now shows the sentence with `OS::alert` and
calls `SceneTree::quit(1)`, in an exported build only (`template`, the tag no editor carries): an
editor with a broken host is still an editor, and the author is the person who can fix it. It covers
all three ways the host can fail to come up -- the library missing, the wrong host kind, and vh_init
refusing -- and the sentence it shows is the host's own, captured from `OnDiagnostic`, because
`vh_init` answers only a status code.

Measured, on an export with its `verse_data/Cooked` removed: the game prints *"the cooked Verse
directory ... is not there. Export the project again."*, adds *"The game cannot run without it and
will close."*, and exits **1** instead of opening. A sound export is unaffected -- `dodge-the-creeps`
still passes 30/30 and the export layer 308/0/9.

---

## B18. An `@export` of a script class **in a module** names a class ClassDB has never heard of · **fixed**

Reported from an ordinary editor session, on the first try, against a project of the reporter's own:
picking a value for a Verse-typed resource slot in the inspector printed

```
ERROR: Cannot get class 'Gameplay/myResouce'.
```

**Two spellings of one name, produced by two code paths, and nothing had made them agree.** A
Verse class under a `.vmodule` is `gameplay/my_resource` to the bridge, because every
`ClassNameUtf8` in the ABI is module-qualified — that is the rule, and the export descriptor's
`HintString` follows it. But **ClassDB is one flat namespace and a module is deliberately not part
of it**: `@global_class` registers the *file stem*, PascalCased, and nothing else. So the registry
held `MyResource` while the inspector slot was filtered by `verse_pascal_case("gameplay/my_resource")`
— `Gameplay/my_resource`, a name nobody had registered — and the first thing that asked ClassDB to
resolve it said so.

The fix is one line in `filter_class_from_hint`: take the **leaf** of the qualified name before
PascalCasing it. The host keeps sending what it is required to send; the consumer, which is the side
that owns the Verse→Godot naming transform already, does the other half of it too.

**Nothing had caught it because nothing had tried the combination.** Module fixtures existed and
resource exports existed; no fixture was both. `tests/integration/widgets/left/palette.verse` is now
a `@global_class` Resource inside a module and `widget.verse` beside it exports a reference to one,
which fails the old code with `Left/palette` in exactly the reporter's shape. The assertion worth
having is not the literal string but the invariant that broke: **the name the slot filters by and
the name the class registers under are the same string**, checked against `Script.get_global_name()`
rather than against a constant.

**And a second text-derived answer, found beside it.** `Script.get_global_name()` is read out of
the source text too, so it answers **nothing** in an exported game — the same shape as
`get_instance_base_type` an hour earlier, and harmless for the same reason the base type was not:
the registry an export uses was baked into `project.godot` when the export was made, and nothing at
runtime asks the script. Recorded rather than fixed, and the one integration case that compares
against it is skipped in an export with that reason printed.

**The second half of B19 is at parity with GDScript, and the rest of it is a plan rather than a
patch.** `@global_class` on a class that is *not* the one named after its file registers nothing at
all — Godot collects one global class per script path — so a member typed as one had no name to
filter its slot by, which is the same error arriving by a second route (`Cannot get class
'MyResource'`). What GDScript does in that position was then measured rather than assumed, and it
turns out to lose the class on save entirely. **`property-export.md` §"A second class in one file"
is the whole of it**: the three planes an inner class lives on, why the registration cannot be
granted, and Stages A/B/C.

Stages A and B are done. The slot is now drawn filtered by the class's nearest mirrored ancestor,
which is GDScript's own fallback (A1), and the write that picker admits is refused by class at the
ABI — on the handle path and the instance path alike, verified rather than assumed (A2). **The
refusal is what makes the wide picker honest**, and for `?stowaway` it refuses everything, because
no Godot object can carry a class that is not the one named after its file.

B closed the other half: `@global_class` on a class that is not the file's now says so, at the
attribute's line in the editor and once per build in the log. It used to be accepted and ignored
without a word, which is the part that was the bridge's own fault rather than Godot's.

**C — serialisation — is settled as C1**, on a spike rather than on the recommendation. A second
class's member saves as an *empty* sub-resource and reads back empty, while a mirrored member beside
it round-trips intact; the reason is that a Verse object's members live in the VM and only a
*script* bridges them to Godot, which by R-LANG-6 only the file-named class can be. So the rule --
a class to be authored or persisted lives in its own `.verse` -- has a mechanism behind it, and
Stage B's warning already says it at the attribute. C2 -- addressing a second class as
`res://x.verse::second` -- was spiked twice and **killed**: routing it is a `_recognize_path`
override away, but `::` is Godot's own marker for "internal to a file", so the saver inlines such a
resource and the reload re-homes it into the container. It can be loaded and never referenced.

It is also the case `phase-4b-design.md` §5 put out of scope — "a resource that holds another
resource as an exported member … worth a case but not worth blocking the stage". It was worth
blocking the stage.

---

## B20. The comment above a member stopped reaching its hover · **fixed, two causes**

Reported as "hovering a Verse member that has a comment on it doesn't show the comment — I feel
like this used to work". It did, and the two reasons are unrelated to each other.

**The one that was a regression is a thread, not a line.** A hover on a member of the project's own
class answers `LOOKUP_RESULT_CLASS_PROPERTY` naming that class — which is exactly what GDScript
does (`gdscript_editor.cpp`'s `DataType::SCRIPT` arm) — and Godot then reads the description out of
the script's *registered documentation* rather than off the lookup result, because `description` is
read for the two local results alone. So the tooltip is only ever as good as what
`_get_documentation` answered. **Godot asks for that once per session, on a loader thread of its
own** (`EditorHelp::_regen_script_doc_thread`, which loads every script and asks each to describe
itself), and **every ABI entry point is refused off the game thread** — so `vh_class_members`
answered nothing, every Verse class registered with no members at all, and after that nothing asks
again except a *save*. Saving the file fixed it, which is why it read as intermittent.

It used to work because the read **waited**: before `dcd517e` some twenty-two ABI reads began with
a join on the analysis thread, and that commit's own message names this caller — "the doc pass for
the members". Taking the waits out was right; this pass was the one consumer that had been relying
on one, from a thread that could not have it.

The fix is to decline rather than to answer badly. `_get_documentation` now says *nothing* when it
is off the Verse thread, has no host, or the snapshot cannot yet describe the class — registering
no documentation is better than registering a class with no members — and sets a flag;
`VerseScriptLanguage::_frame` spends it by re-registering **every** loaded script's documentation
through `ScriptEditor::clear_docs_from_script`/`update_docs_from_script`. Every script rather than
the open one, because a class' documentation is read by a hover in any file. The retry is armed
once and re-armed by a landed analysis and by a published generation, which are the two things that
change the answer: waiting for an analysis alone is not enough, because an editor opened and left
alone starts none, and retrying per frame is too much.

A `.verse` that declares no class of its own is separated out first and never re-asked about
(R-LANG-6) — it has nothing to document, which is a different thing from not being able to say so.

**The second cause was never a regression and had been wrong since the feature shipped: an
attribute between the comment and the declaration ended the walk.** `verse_doc_comment_above` reads
upward from a definition's line until it stops finding comment delimiters, and `@global_class`
is not one. Which line it starts from decides whether that matters, and the two callers differ —
the host reports a *member* at its first attribute line, so `@export` members were unaffected, but
`verse_scan_class_decl` reports a *class* at the `:= class` row itself. So `mover`'s whole comment
was dropped where `spinner`'s, carrying no attribute, survived. An attribute line is now stepped
over rather than ending the walk, which makes the answer independent of which line the caller had.

**What it was found with.** `EditorHelp` writes the registered script documentation to
`.godot/editor/editor_script_doc_cache.res` at editor exit, and that file can be read back headless
— `ResourceLoader.load(..., CACHE_MODE_IGNORE)` and `get_meta("classes")`. Before: every Verse class
`props=0 methods=0` with the GDScript beside them intact. After: `mover props=6 methods=2`, each
carrying the comment above its declaration. **A headless editor cannot be used for this** —
`cmdline_mode` is set from `DisplayServer::get_name() == "headless"`, and both the doc-cache
regeneration and `EditorFileSystem::_update_script_documentation` are skipped under it — so the run
that produces the cache has to be a windowed one. Nothing else here can see any of it.

---

## B21. Asking Godot for the `IP` singleton segfaults the process at exit · **open**

Found while writing the fixture for the singleton-class fix, and it is **not** that fix's doing: it
reproduced on master, where the accessor merely *failed*. What the accessor does with the answer has
never mattered — it calls `vh_singleton` first, and that is enough. Since the 39 core accessors
became total (spec R-TYPE-4) `GetIP()` raises instead of failing, and the segfault is the same one.

**The repro is ten seconds.** A `.verse` in `tests/integration/scripts` whose only body is
`GetIP().GetClass()`, a `SceneTree` script that calls it and quits, and:

    godot --headless --path tests/integration --script res://<driver>.gd

The cases all print, the suite's own summary prints, `vh_shutdown` returns, the extension's
terminator runs to its last line — and *then* the process dies with `0xC0000005`. Exit 139 through
Git Bash. Godot's crash handler prints nothing because `Main::cleanup` has already disabled it, and
lldb does not stop on it either.

**What was ruled out, each by its own run.** The mirrored class is not it: building the wrapper as
`object` instead of `ip` still crashes, and realising `ip`'s `UClass` while wrapping the *OS*
handle does not. The wrapper is not it either: passing no fallback class, so the handle crosses as
a bare `vh_object` exactly as it did before the fix, still crashes. `OS`, `DisplayServer`,
`NavigationServer2D` and `Engine` are all fine, so it is neither "a singleton" nor "a late-deleted
one" — `OS` is deleted later than `IP` is. And GDScript's own `Engine.get_singleton("IP")` in the
same process, with the host loaded, exits 0.

What is left is the one thing that only the bridge does: `api_get_singleton` reaches `IP` through
**godot-cpp**, which mints an instance binding on the engine object and registers a free callback
that lives in `godot-verse.dll`. Godot deletes `IP` in `unregister_driver_types()`, after
`deinitialize_extensions()` has already torn our side down. That is a theory with the shape of the
evidence and not a diagnosis; the next session should confirm it before fixing it, and the fix it
implies is dropping the bindings we made at the terminator rather than leaving them for Godot.

**Why it matters more than an exit code:** every layer of `run_tests.py` reads a process's exit
status, so one case touching `IP` turns a green suite red with nothing in the log to say why. That
is how this was found.

---

## What shipped

Every entry is closed. In the order they were done, which is the order the entry above them argued
for:

| | what changed |
| --- | --- |
| **B1** | `method_mapping` grew an `is_virtual` column, generated from the same `is_virtual` in `extension_api.json` that decides how the member is emitted. `completes_as_override` asks the table what a member *is* instead of where it lives. |
| **B4** | `_validate`, `refresh_script_warnings` and the signal pass ask the host by `qualified_class_name(p_path)`. All three used the bare stem, so a script in a module had no method outline, no export warnings and no signal warnings. |
| **B3** | The stub ends `{} # Replace with function body.` |
| **B5**, **B6** | `_get_built_in_templates` answers one template against `Object`; `_make_template` fills in `p_template` when it is given one. The template is GDScript's, and compiles. |
| **B7** | `explain_effect_errors` and its two helpers deleted. `run_tests.py` asserts the compiler's own text for both shapes. |
| **B9** | `_err_print_error`, not `UtilityFunctions::push_error` — the latter is GDScript's variadic global, which concatenates. |
| **B8** | `VerseScript` tracks its owners; `_reload` re-attaches, carrying exported values across. **Read B26 before trusting that last clause** — the carrying across is what erased them, and the re-attach is now conditional on the compile having succeeded. |
| **B10** | `tests/integration/scripts/has_point_probe.verse` and two cases, both answers exercised. |
| **B12** | `CLAUDE.md`, beside the dropped-continuation-line constraint. |
| **B13** | `vh_class_override_candidates` (ABI 7.1) hands the snapshot's per-class override candidates over without waiting; the language folds them into the first answer through the same `completes_as_override` the refined path uses. |

---

## B22. Four of the five script-level hooks were never offered as overrides · **fixed**

Found while testing the `<decides>` virtuals: typing `_Set` in a class body completed nothing. It
had never completed anything, and neither had `_Get`, `_GetPropertyList` or `_ValidateProperty`.

`completes_as_override` admits a member of the native root only when the generated method map finds
it *and* the row says virtual:

    if (const verse_api::method_mapping *mirrored = godot_method_for(owner, p_item["name"])) {
        return mirrored->is_virtual;
    }
    return verse_godot_class_for(owner) == nullptr && owner != String("vh_object");

`_Set` is on `vh_object` and had no row, so it fell to the second line and was rejected by
`owner != "vh_object"`. The map had exactly one hand-written row -- B1 added the `is_virtual` column
and gave `_Notification` one, because `_Notification` was the only hook that existed then. The other
four arrived with R-NODE-10 and nobody added theirs beside it.

The same table answers hover, so those four had no documentation link either. `_Notification`
worked throughout, which is what made the gap look like working behaviour.

**What closes it.** Four rows in `LIFECYCLE_METHODS` (`tools/gen_verse_api.py`), which is the whole
fix -- `verse_api_classes.h` is the only generated file that changes and no host rebuild is needed.
The four names also go into `BASE_MEMBER_NAMES`, so a Godot release that starts describing `_get`
generates nothing that shadows the hand-written one.

The units layer now checks the **set** of five rather than `_Notification` alone, since checking one
row is precisely what let four siblings go missing. What it still cannot check is whether the editor
draws the option: this needed a by-hand session to find and needs one to confirm.

---

## B23. `variant` and `[]char` were drawn as plain text, and `variant` hovered as a local · **fixed**

Reported from an editor session: `[]char` and `variant` are not coloured, and hovering either one
says "Local Constant". Both are true and they are the visible corner of one defect — the editor
learned what a type is from **three** lists, and between them they missed most of the mirror.

    verse_api::classes[]        the 1036 mirrored classes, plus three value types hand-added to it
    verse_api::enums[]          the 793 mirrored enums -- read by the hover and by nothing else
    native_type_names[]         two names in verse_syntax_highlighter.cpp

`variant` was in the third list and was deleted from it by `3f24348`, which took `<public>` off the
type; `e2e4dcf` made it public again for `typed_array`'s constructor and nothing put it back. It
has been a type a script writes in `_Get` and `_Set` ever since, drawn as an unknown word.

The three value types in the first list are `VALUE_TYPE_CLASSES`, added so that "the editor calls
`vector2` a local constant" would stop being true. It stayed true for the other thirteen, and for
`rid`. The 793 enums were in no list the highlighter read at all, so `node_internal_mode` hovered
into Godot's own documentation and coloured as prose.

**Four surfaces, not one.** The same tables answer the hover, the syntax highlighter, the class
names completion offers when the host has offered nothing (`mirrored_class_names`), and the Godot
type → Verse type mapping. A name in none of them fails all four, which is why this reads as
several unrelated complaints.

**What closes it.** One generated table, `verse_api::types[]`, for every exported type name that is
not a mirrored class or enum, each with the Godot page that documents what it carries (empty for
`signal(t)` and `connection`, which stand for nothing of Godot's and keep the mirror's own comment
instead). `VALUE_TYPE_CLASSES` becomes all sixteen math types and `RID`. The highlighter reads all
three tables, and what is left hand-written is the three ABI natives a script must never be offered
and Verse's own type names -- `char`, `event`, `cancelable` and the rest, which are not reserved
words and so are not lexed as keywords. Which ones those are was asked of the compiler rather than
recalled: `tests/verse_probe/stdlib_types_probe.verse`, where `awaitable` and `task` want
`using { /Verse.org/Concurrency }` and `agent` does not exist here at all.

`char` answers Godot's **String** page, because `string` *is* `[]char` — one type the compiler
prints two ways, and the tooltip should not disagree with itself about which.

**What is tested and what is not.** The units layer checks the type table against the two
hand-written mirror files, in both directions: a public type with no row fails, and a row for a name
nothing declares fails. That is the check `variant` needed and did not have. The integration layer
checks the tooltips through `probe_hover` — `vector2i`, `rid`, `variant`, `char` and `typed_array`
each have a case. **The colouring itself still has no test and cannot have one from a headless run**:
`VerseSyntaxHighlighter` is registered at `MODULE_INITIALIZATION_LEVEL_EDITOR`, which `--script`
never reaches, so no GDScript can construct one. It is in the same bucket as the rest of the editor
UI, and the steps are below.

### The same walk, one table over: the functions

Fixing the types left `tools/probe_hover.py` reporting 290 mislabelled names over `tests/integration`
and 16 over `dodge-the-creeps`, every one of them a *function*. The largest group was the one an
author meets first: `Length`, `Normalized`, `Rotated` and `Clamp` on a `vector2` — the yardstick's
own code — each drawing "Local Constant" over its signature, though Godot documents all of them.

Three causes, one shape. **An extension method's owner is the file it is written in**: `(V:vector2)
.Length()` declares `operator'.Length'` at module level, so `godot_method_for(owner, name)` had a
path where it wanted `vector2`, and the receiver appears nowhere but the signature. **Nothing
recorded the pairs**: the generator's math pass walked every builtin method to record the ones
GodotMath does *not* write, and threw away the ones it does. **And the globals table was two rows
hand-written in `verse_script_language.cpp`** — `Print` and `IsInstanceValid` — so the other
forty-eight, all of GodotMath's scalar half among them, had nothing to name.

What closes it: the skip walk records a row for a written method as well as a skip for an absent one
(159 methods, plus three `GetEnd`s that are Godot *members* rather than methods), the globals table
is generated from what the hand-written files declare (50 rows, matched to a Godot utility by name,
then ignoring case, then through `UTILITY_VERSE_SPELLINGS` — which is the only way to reach
`type_string`, whose Verse spelling is `VariantTypeName`), and the consumer reads the receiver off
the first parameter of the declared type. That last part is also the disambiguation: `Snapped` is
`Vector2.snapped` on a vector and `@GlobalScope.snapped` on a float, and only the receiver says
which.

Two smaller things fell out of it. `GodotMath.native.verse` **was not in `is_godot_package_global`'s
list of files** — it postdates the list — so every global in the file was excluded from the path it
needed before any of the above could run. And `UTILITY_VERSE_SPELLINGS` **bound `fmod` twice**; the
second binding won, so R-SCN-2's sentence for `fmod` named Verse's integer `Mod[X, Y]` rather than
GodotMath's `FMod(A, B)`. Both sentences are true, which is why it read as correct.

**Where this stops, and why that is a decision rather than an omission.** 250 rows remain over
`tests/integration` and 12 over the yardstick, in four families: a global the bridge invented
(`MakeVariant`, `AsInt`), a wrapper class's method (`godot_array.GetInt`, `signal(t).Await`),
Verse's own (`event`, `Sqrt`), and the project's own second classes and their members. **None of
them mirrors a Godot function.** The wrappers are the tempting ones — `godot_array.Length` is
plainly `Array.size` — but their bodies call `VhRefSize` and the ABI's own reference primitives, not
a named Godot method, so a table pointing them at one would be judged rather than read. The mirror's
comment above each declaration is what the tooltip draws instead, and it is a better description of
a *typed* accessor than Godot's page for the untyped one. `probe_hover.py`'s H1 rule now names the
four families, so what it reports reads as a line rather than as a backlog.

**And the instrument was measuring a stale build.** `probe_hover.py` runs Godot in a project and the
project holds whatever library was last staged there, so two measurements of a fix that had already
landed came back unchanged and read as the fix doing nothing. It stages the built extension itself
now, the way `run_tests.py` does before each of its Godot layers.

---

## B24. A fifth of every completion popup was a name no author could write · **fixed**

A `.` in a Verse script offered a few hundred options, and `tools/probe_complete.py` says what they
were: over `dodge-the-creeps`, **2816 of 13114 options across 60 positions -- 21.5% -- were class
var accessors**. `AngularDampGetter(…)`, `AngularDampSetter(…)`, `AutoTranslateModeGetter(…)`, two
per Godot property, sorted alphabetically into the middle of the list an author was reading.

**What they are.** `gen_verse_api.py` turns each of Godot's 3312 properties into a Verse `var` plus
the accessor overloads the compiler requires for one -- `var Position<public><getter(PositionGetter)>
<setter(PositionSetter)>` -- and the generator's own comment says the rest: *"Only the compiler ever
names them, at the point it rewrites a read or a write of the public var above."* They are
`<epic_internal>`, they take an `accessor` parameter nothing in a script can construct, and there
are **7344 of them, about 4380 distinct names**. Not one is spellable.

**Why they were offered.** `DescribeCompletion` describes any `CFunction` that fits the filter, and
one line above the accessor test it already refuses the compiler-generated *constructor* for exactly
this reason. The fact that separates them was in hand and used for something else:
`CFunction::_bIsAccessorOfSomeClassVar`, which `IsOverridable` reads so an accessor is not offered
as an `<override>` candidate. Hoisted into `IsClassVarAccessor` and tested first, it refuses the
item outright -- which is also cheaper than describing one, since `SpellSignature` spells a whole
function type per item and that is where the walk spends its time.

**The two paths, and why one fix covers both.** `vh_class_members` reaches completion as the
snapshot answer `_complete_code` draws first, and `ClassMembersLive` passes **no access scope**, so
`CollectScope`'s `IsAccessibleFrom` guard never runs there and `<epic_internal>` is no barrier at
all. Refusing in `DescribeCompletion` is below both paths. The `nullptr` stays, with the reason
recorded beside it: **every `<epic_internal>` name in all four of `host/Verse`'s files is an
accessor**, counted, so with these gone it admits nothing. Adding a non-accessor one means giving
that call an access scope, and then deciding whose -- its callers are a completion at a cursor, a
script's own documentation and the method outline, and they would not all answer the same.

**What the instrument found on the way.** `probe_complete`'s first run reported four archetype
positions answering nothing, in `hud.verse`'s `option{Typed}`. That was the *probe* being wrong, not
the bridge: `option` is a reserved word, `archetype_class_end` declines a `{` behind one, and
`array{}`, `map{}` and `spawn{}` are the same shape. The picker reads the reserved words out of
`src/verse_keywords.h` now, which is generated from the compiler's own list, so the two cannot
disagree. The one genuine archetype position in the yardstick answers with its two fields.

---

## B25. Verse's own library hovered with a type and an empty box · **fixed**

`Sqrt`, `event`, `Concatenate` and everything else `/Verse.org/Verse` declares drew their type and
nothing under it. So did `signal(t).Await` and `typed_array(t).ToArray`. Three separate causes,
found one behind another.

**Verse documents itself with an attribute, not with a comment.** `Engine/Plugins/Verse/Verse/
Source/Verse/Verse/Verse/` carries **132 `@doc("...")` attributes** across 24 files, and that is
the whole of its prose. Every description this bridge draws is read out of the *source text above
the declaration* (`verse_doc_comment_above`), and above `Sqrt` there is an attribute line. No
amount of reading better finds it, and nothing else on the consumer's side exposes an attribute's
text — so the host reads it with `GetAttributeTextValue` against `CSemanticProgram::_doc_attribute`
and hands it over, which is what **ABI 11.0** is for.

The route this was expected to take turned out not to be the one. `DigestGenerator.cpp:1962`
rewrites `@doc` into `#` line comments when it writes a digest, *"regardless of whether it includes
epic_internal definitions"*, and dumping the 32,481-byte digest the trace reports for `Verse/Verse`
shows them there in full. But the definitions the host actually holds are not from that digest: a
probe printing `GetMappedVstNode()->Whence()` for `Sqrt` reports
`.../Verse/Verse/Verse/Math.native.verse row=37`, which is the real file at the exact row of its
`@vm_no_effect_token` — so the attribute is still on the definition and the digest's comments are
never reached. Both facts are worth keeping: the attribute is the route, and a digest is not
automatically what a package is read from just because it has one.

**A parametric class is a `CFunction`.** `signal(t) := class(...)` is a function answering a type,
so `RecordMirrorScope`'s rule — walk into anything that is a scope *unless* it is a function, since
a function's parameters and locals are reachable from nowhere a cursor can be — walked straight
past every member of `signal(t)`, `typed_array(t)`, `typed_dictionary(k,v)` and `event(t)`. After
the first build the mirror is read from its own digest, which is one synthetic snippet at a path no
file is written to, so those members had no recorded location and the consumer had nothing to read
a comment from. `ParametricClassOf` unwraps the `CTypeType` the signature answers and the walk
recurses into the class: **33,047 definitions became 33,067**.

**And an instantiated definition is not the one that was written.** `typed_array(node)` mints a
fresh `CDefinition` per member, so the `ToArray` a cursor resolves to is not the `ToArray` the
table recorded, and its key missed. uLang states the rule where it *ensures* against
`GetAttributes` on one — *"which inherits its attributes from its prototype definition"*
(`uLang/Semantics/Definition.h:222`) — and that ensure firing in the integration log is what found
this. `PrototypeOf` is now asked before the key is built, before the location is read and before
the prose is: an ordinary definition is its own prototype, so nothing else moves.

**What is left is correct.** `VariantBool` still hovers with no description, because
`GodotClasses.native.verse` has no comment above it — the generator does not write one for the
per-lane variant builders. That is a generator question and not a plumbing one.

**The rule the consumer follows** is that its own reading of the file wins and the host's answer is
the fallback. Reading the file is current with an *unsaved* edit where the host describes the text
the last analysis saw, and the parser hangs a comment off whichever node begins a construct, so for
a member behind four lines of `@editable` the host's answer is empty where re-reading is not.

---

## B26. Saving a `.verse` erased its own exported values, and B8's re-attach is what did it · **fixed, by-hand check owed**

Reported rather than watched: an `@export`'s value in the inspector kept going to null while a
script was mid-edit and not compiling. Traced through the engine sources rather than in a session,
so the fix below is argued and built and **has not yet been seen to work in a window** — the steps
are at the end of this entry.

**The code meant to preserve the values is what destroyed them.** Every save of a `.verse` goes
through `VerseResourceFormatSaver::_save`, which calls `_reload`, which called `reload_instances`
unconditionally. That reads the script's exported values off each owning object, swaps the script
off and back on to force a fresh instance, and writes them back — B8's work, and correct as far as
it goes. What it did not account for is that in the editor a non-tool script's owner holds a
`PlaceHolderScriptInstance`, whose `values` map is the **only** copy those values have anywhere:
`Object::set_script(Variant())` is a `memdelete` (`core/object/object.cpp:150`), so the copy is gone
the moment the swap starts and everything rests on the write-back landing. It did not land, for two
reasons that are independent of each other.

**`PlaceHolderScriptInstance::set` refuses any name the script will not report a default for**
(`core/object/script_language.cpp:597`), and for a GDExtension script that is `_has_property_default_value`
(`core/object/script_language_extension.h:165`). This bridge answered it with *"is the default
non-nil"*, which tied two unrelated things to it. A nil default is what an object-, node- or
resource-typed export **always** has — the host records the entry and `ReadDefaultFieldOf` hands
back an invalid value — and it is what **every** export has while the last build failed, because
`_get_property_default_value` short-circuits on `has_own_class`, which is `valid && has_class(...)`
and `valid` is false when *any* file in the project carries a diagnostic. So the fresh placeholder
declined every value handed to it. `Object::set` does fall through to `property_set_fallback`, but
that no-ops unless `is_placeholder_fallback_enabled()`, and `refresh_exports` keys the fallback on
*this file's* diagnostics — so when the broken file was a different one, nothing caught the value
at all and it was simply dropped.

**And a placeholder created during the failure was given no property list.**
`_placeholder_instance_create` calls `update_placeholders`, which returned early whenever the
fallback was on. That early return is right for a placeholder already holding a good list and wrong
for one created a microsecond earlier holding nothing: no inspector rows, and nothing stored for the
node the next time the scene was saved. `reload_instances` creates exactly such a placeholder.

**Three changes, all in `verse_script.cpp`, none touching the host or the ABI.** `_reload`
re-attaches only when `compile()` answered `OK`, because a failed compile publishes no generation
and the swap is loss with no gain — which is also the rule the rest of the bridge already follows,
that a failed build leaves the last good generation running. `_has_property_default_value` answers
Godot's actual question, *is this one of my exported members*, off the export list rather than off
the default's type; GDScript answers the same question the same way, its
`member_default_values_cache` holding an entry for `@export var target: Node2D` whose value is null,
so the name is known and the default is not. And `update_placeholders` hands the last good list over
**with no defaults** when the fallback is on, rather than returning: `PlaceHolderScriptInstance::update`
erases only the values whose names are absent from the list it is given
(`core/object/script_language.cpp:723`), and an empty values dictionary overwrites none of them, so
every placeholder that already holds that list is left exactly as it was and a new one gets a shape.

**Nothing automated can see any of this**, for B8's reason: a placeholder only exists under
`is_editor_hint()`. All four layers stay green, which says only that nothing else moved.

**To check it:** attach a script with an `int` export and a `node2d` export to a node, set both in
the inspector, and save the scene. Then (1) break the script — delete a closing paren — save it, and
confirm both values still show; (2) fix the script, save, and confirm both survived and the node
runs the new code; (3) repeat (1) with the break in a *different* `.verse` file, which is the case
where the fallback never engages and the value used to be dropped outright. Step (3) failed before
this with a perfectly valid script in front of you, and the `node2d` export in step (1) failed on
every save whether or not anything was broken.

---

## B27. The argument hint went missing after a failable call, and after a race · **fixed, two causes, by-hand check owed**

Reported rather than watched: the parameter tooltip that stands above the caret while a call is
open *sometimes* does not appear after completing a function. Two independent causes, one
deterministic and one a race, and the second is why the first read as intermittent. Both are argued
from the sources the way B26 was and neither has been seen in a window; the steps are in
§"What is still open".

**Godot asks for the hint only when the inserted text ends in a trigger character.**
`CodeEdit::confirm_code_completion` ends with `if (code_completion_prefixes.has(
caret_last_completion_char)) { request_code_completion(); }` (`scene/gui/code_edit.cpp:2712`), and
that re-request is the whole of how a hint appears the moment a call is completed: it lands in
`_complete_code`, which fills `call_hint`, and `ScriptTextEditor::_code_complete_script` hands the
result to `set_code_hint`. The table is Godot's own eight characters (`editor/gui/code_editor.cpp`
:2101) plus the `{` and `?` `widen_completion_prefixes` adds for B24's two popups.

Verse spells a `<decides>` call with brackets, so `completion_option_for` inserts `GetNode[` — and
**every failable call in the mirror, which is every object-returning method and all 568 predicates,
landed on a character that asked for nothing**. Nothing was wrong with the answer: `call_hint_for`
had already been asked, and `call_opened_with_bracket` exists precisely to spell the hint with the
brackets the author wrote. `[` joins the table. What disguised this as intermittent rather than
absolute is that the hint *does* appear as soon as the first argument character is typed, which
reaches `request_code_completion` through its `!is_symbol` arm instead.

**And the analysis the hint was waiting for could be displaced by the validate behind it.** When
the host does not already describe the buffer, `vh_signature_at` answers `VH_ERR_STATE`, and
`_complete_code` queues that completion buffer and draws nothing; `refresh_completion_if_current`
is what re-asks once it lands. That recovery fires only when the analysis that landed *was* the
completion one (`in_flight_is_completion`). `request_check` kept **one** pending slot, newest
buffer wins — and confirming a completion changes the text, so the editor's idle timer runs
`_validate` a moment later and queued the author's real buffer into that same slot. If the host
happened to be busy when the hint was asked for, which is the common case because the previous
keystroke's analysis is usually still running, the completion request was gone before it ever
started and nothing re-asked. No hint until the next keystroke.

The fix is a second slot, one per kind, with `start_pending_check` preferring the completion one: a
popup and a hint are blocked on it and drawing nothing, where the author's own buffer feeds a
gutter still showing the last analysis' diagnostics. Each slot holds only its newest buffer, so
preferring one delays the other by a single analysis and can never queue a third. Which of the two
won the one slot was a race against how busy the host was, and that is the whole of why this was
reported as *sometimes*.

**The third cause is not a defect.** `enclosing_call_callee_end` answers -1 at a newline it meets
at bracket depth 0, so a call whose arguments continue on the next line gets no hint. That is
deliberate and the comment there says why.

---

## B28. `Cannot get class 'Vector3'.`, once per keystroke · **fixed, measured**

Reported as a burst of errors in the Output panel while editing a `.verse` file, with nothing else
apparently wrong. It is `ClassDB::get_parent_class` failing its `ERR_FAIL`
(`core/object/class_db.cpp:357`), and three call sites in `src/verse_script_language.cpp` could
reach it with a name ClassDB has never heard of.

**`verse_api::classes` is not only classes.** The generated table carries the sixteen math types
and `rid` beside the 1036 mirrored classes, and its own header comment says why: they are Godot
types with Godot names and Godot documentation pages, which is what every reader of it but
`_make_template` is asking about. So `verse_godot_class_for("vector3")` answers `"Vector3"` — and
`Vector3` is a Variant type, not a registered class, so anything that walks `get_parent_class` from
what that function answers fails the check and walks nothing.

**`skipped_member_for` already had the guard and it had gone stale.** Its `godot_name == nullptr`
branch is commented *"A math type. ClassDB has never heard of Vector2"* and searches the skip rows
by Verse name instead — written in `db6505d`, correct then, and dead from the moment the value
types joined that table. A math type took the walking branch instead, printed the error, and found
no skip row.

`godot_classdb_class_for` is the one test — `verse_godot_class_for` plus `ClassDB::class_exists` —
and the three sites take it: `member_bearing_chain`, `skipped_member_for` and
`collect_signal_names`, the last of which would have said the same thing through
`class_get_signal_list` with double quotes instead. A value type has no ancestry to walk in any
case: its members are all its own.

**Where it comes from, and why the two reports arrived together.** `member_bearing_chain` is
reached from `receiver_classes_from_text`, which runs *only* in the not-ready branch of member
completion — the partial answer drawn while the analysis that would answer properly is still
running. So a burst of these is a direct readout that the host is declining to describe the buffer,
which is the same condition B27's second cause turns into a missing hint. They are not the same
defect and they share a trigger.

**Measured both ways.** `tools/complete_probe.gd` driven by hand against `dodge-the-creeps`, at a
`.` on a `var ScreenSize:vector2` member added for the run: before, `ERROR: Cannot get class
'Vector2'.` and 34 options; after, no error and the same 34. `probe_complete.py` cannot see this
and its silence means nothing — it captures the subprocess' output and discards it on success, so
the errors never reach a terminal. Driving the driver directly is what reads them.


---

## B29. Ctrl+click on a binding scrolled the open file to its own top · **fixed, measured**

Ctrl+click on `main_script` in `demo/scripts/mover.verse` -- the binding for `res://main.gd`'s
`class_name MainScript` -- moved the caret to the top of `mover.verse` instead of opening `main.gd`.
The answer the bridge gave was right, and `probe_hover` said so: `CLASS`, `MainScript`,
`res://main.gd`, location 0. The editor was reading a *different field*.

**Godot renamed it between 4.7 and 4.8.** The lookup result carries `Ref<Script> script` in 4.7 and
`String script_path` in 4.8 (`editor/script/editor_language.h`, where the struct also moved). This
machine develops against a 4.8-dev checkout of Godot's source and *runs* 4.7.2, so a field read out
of the source is not necessarily the field the editor will read.

The consequence is not a missing jump, which is what makes it hard to recognise. A result with a
location and no script beside it is a legitimate answer meaning **a line in the file being edited**,
so `ScriptTextEditor` did exactly that: `goto_line_centered(location - 1)`, and location 0 is line
-1, which clamps to the top. A jump into another file and a jump within this one differ by that one
field, so filling half of it turns one into the other silently.

The jump at the end of `_lookup_code` had set both spellings since it was written. The binding arms
added beside it set one. **A result that fills either must fill both**, and
`probe_hover.py`'s **H9** is now the automated half: it reports a row with a location that names the
script one way and not the other. It was blind to this before, because the row it built carried
`script_path` and nothing about `script` -- so the corpus showed a complete-looking answer for the
click that did not work.

---

## B30. `Error loading resource: 'res://main.gd'` at every editor startup · **fixed, measured**

One red line in the Output panel a second into every session, naming a GDScript the project is
built around and saying nothing about what went wrong with it:

    E 0:00:00:895   load: Error loading resource: 'res://main.gd'.
      <C++ Source>  core/core_bind.cpp:82 @ load()

It appears whether or not any Verse file names that script, and it stops after the first build --
which is what made it read as "a GDScript naming a Verse class cannot parse until Verse has built",
and that is not what it is.

**It is a cyclic load, and the error is the shape of one.** Godot answers `ERR_BUSY` and a null
`Ref` to a load of something already being loaded further up the *same thread's* stack, silently
(`core/io/resource_loader.cpp:1049-1056`); the only thing printed is the caller's own
`ERR_FAIL_COND_V_MSG`, which is what `core_bind.cpp:82` is. So the message names the resource the
caller asked for and never the one it collided with. The stack is:

    main.gd                       Godot loads it, and the analyzer meets `@export var mover: Mover`
      mover.verse                 make_script_meta_type(ResourceLoader::load(path, "Script"))
        VerseScript::compile()    ensure_project_built()
          refresh_bindings()      R-INT-7 generates a binding per `class_name`
            main.gd               already being loaded on this thread -> ERR_BUSY

Two things are worth reading off that. The absent GDScript diagnostic is a *fact*, not a missing
clue: `ResourceFormatLoaderGDScript::load` prints its own sentence only `if (err && scr.is_valid())`
(`gdscript_resource_format.cpp:47-50`), and a load that never reached GDScript has neither. And the
direction of the reference is the opposite of the one suspected -- it is the GDScript naming a Verse
class that closes the loop, so the error appears in a project where no `.verse` file has ever heard
of `main.gd`.

**The fix is to know when the generator is on that stack, and which one script must wait.**
`VerseResourceFormatLoader` keeps a `thread_local` depth around `_load` -- per thread, because a load
on another thread is not this thread's cycle -- and on that stack the generator holds back the
scripts whose *text* names one of the project's Verse classes, because only such a script can close
the loop. Each is still declared: the global class list records what every script extends, followed
through the list where that is another script class, so nothing about the type is lost and the
members arrive with the next frame's generation, which `bindings_incomplete` already asks for.
Measured on a fixture of `demo`: the line is gone, and `bindings.verse` carries the same members.

Text, because there is nothing better to ask. `ResourceFormatLoaderGDScript::get_dependencies`
forwards `GDScriptParser::get_dependencies`, which returns an empty list under a `// TODO: Keep track
of deps.` (`gdscript_parser.h:1699-1702`); `ResourceLoader.load_threaded_get_status` answers only for
a load *it* was asked to start; and a GDScript mid-load is not in `ResourceCache`, because
`GDScriptCache::get_shallow_script` uses `set_path_cache`, which deliberately does not register it,
and `set_path` comes only at the end of `get_full_script` (`gdscript_cache.cpp:396-400`). So nothing
Godot exposes can be asked "is this path being loaded right now".

**Two fixes were tried and measured before this one**, and both are worth not repeating. Skipping
*every* script load on that stack took the integration layer down: `mob.gd` names no Verse class, so
its load was never cyclic, and holding it back left the first build with a memberless `mob` and a
Verse file that calls `Hit` on one. And loading through `ResourceLoader`'s threaded pair -- which
drops the error where `load()` reports it (`core_bind.cpp:72-76` against `:82`) -- **deadlocks**: the
request spawns a worker to load the script, that worker needs the `.verse` this thread is holding,
and the two wait on each other. The run had to be stopped by PID.

**The same interaction from the other end, fixed after it.** A Verse file that *calls* a binding's
method -- not merely names the type -- failed the build this stack triggers, with `Unknown member` at
that call, because that generation had no members to describe. The members landed a frame later and
nothing rebuilt, so the error sat in the panel until the author built or played.

**A build against a held-back roster is provisional, and a failed one now says nothing.** Its
diagnostics are a sentence about a member that exists by the time anyone reads it, and the output log
has no way to retract a line -- the same reason `check_buffer` keeps analysis diagnostics out of it.
The script editor's own list is replaced wholesale on the next validate, so `record_diagnostics`
still runs and the gutter is unaffected; what is withheld is `log_build_diagnostics` and the "did not
build" warning. `_frame` then builds once more, directly after the refresh that completes the roster,
and that build reports.

Three things bound it. It is withheld **once per session**, so a roster that can never complete costs
one silent verdict rather than a silent session. The corrective build fires **whether or not the
roster completed**, for the same reason. And the project is still marked built, with the failure
kept as its status -- because `refresh_from_analysis` asks `ensure_project_built` again from inside
`compile()`, and a build that re-entered itself there would print the very diagnostics being
withheld.

Measured on a fixture of `demo` in all three shapes: a Verse file calling a held-back binding's
method builds silently; a genuine `Unknown identifier` beside it is still reported, once; and a
GDScript with a syntax error -- which can never be described, so the roster never completes -- has
its verdict reported too.

**`tests/binding_cycle` is the automated half, and it is the first this area has had.** A third
headless project, because the cycle has to fire during startup and a project that reproduces it on
purpose would change what every other case in `tests/integration` runs against. Its assertions are
*refutations* -- `run_tests.py` grew a `refute_all` for it -- since both symptoms are things Godot
prints and nothing returns. Confirmed to fail against both previous libraries: reverting `src/` to
before B30's fix brings back `Error loading resource`, and reverting only the provisional-build
change brings back `Unknown member`.

---

## B31. A type the project declares hovered with a colon and nothing after it · **fixed**

Reported from an editor session: `mover_direction` and `something`, both declared in the file
being read, say *"Local Constant"*. Two things are true of that box and only one of them is a
defect.

**The label is a decision, and it predates the report.** Godot's `LookupResultType` is a script
location, eight `CLASS_*` members and two locals; there is no result meaning "a type", so a type
the project declares and Godot does not document has nowhere else to land. `LOOKUP_RESULT_CLASS`
is reachable but worse: only the class a file is **named after** registers a script doc
(`_get_doc_class_name`), so for a second class in a file the tooltip would draw an empty
documentation box where the local result carries the comment written above the declaration.
`e36629d` took that decision for a class and `d62abd0` for an enum, and nothing since has
touched either -- what changed the week this was reported is `a77f8c0`, which started
*colouring* a second class as a type, so the tooltip calling it a local became visible.

**What was a gap is the line under the label: `something:` and then nothing.**
`GodotVerse::LookupSymbol` fills `TypeUtf8` for a `CDataDefinition` or a `CFunction`, and a type
is neither -- so every class, struct, interface, enum and module the project declares reached the
editor with a blank type. The enum arm had already patched exactly this with the hard-coded word
`enum`, which is why one of the two names in the report drew *something* and the other drew
nothing.

**The word can only come from the declaration.** The ABI cannot supply it: a class, a struct and
an interface all arrive as `VH_LOOKUP_CLASS` and nothing else in `vh_lookup_desc` separates them,
so "class" would be wrong for two of the three, and both of those are in `tests/integration`
already. `verse_scan_type_keyword` reads it out of the source the way `verse_scan_class_decl`
reads a class's base -- the first `:=` on a top-level line, because what sits before it is an
access specifier (`StaticsProbeStatics<public> := module:`) or a parametric class's own
parameters (`box(t:type) := class:`), and a parametric class's name lexes as a *function* rather
than as an identifier, which is what the first version of this got wrong.

It reads the file only when the project owns it, and that guard is the whole of what keeps it off
the common path: a mirrored class has a blank type too, and its declaration is 2 MB of generated
Verse in the engine tree -- read on every hover over `node2d`, to produce a word that the Godot
documentation page found two lines later replaces.

**Measured.** `tools/probe_hover.py` over `tests/integration` reported **H4 -- a tooltip with
neither a type nor a description in it -- 12 rows before and 0 after**. H1 rises by the eight rows
the new fixture shapes add, which is the label above rather than this. Four unit cases cover the
scanner with no Godot at all, and four integration cases cover what the editor draws: the second
class, the struct that must not be called a class, the project's own enum -- the case the
hard-coded word used to carry alone -- and the label staying a local, so the decision above fails
a build if it is quietly reversed.

---

## B32. Every object parameter reached Godot as `Object` · **fixed**

Reported from an editor session: a Verse method declared `(Clock:timer)` shows its parameter as
`Object` wherever Godot describes the method, though the declaration names a class.

**What Godot has to go on is a `PropertyInfo`, and its `class_name` was always empty.**
`typed_argument` filled a name and a `Variant::Type` and left the class a default `StringName`,
because `VerseMethodInfo::Param` had nothing else in it, because `vh_param_desc` had no class
field. Everything below that is honest: `Variant::OBJECT` with no class *is* what Godot draws as
`Object`.

**It could not be read off anything the ABI already carried.** The decorated name the method list
hands over does spell the parameter's type -- `(/user@localhost/exports:)AddInts(:int,:int)` --
but that is the VM's mangling, and parsing it is guessing where the host can be asked. So
`vh_param_desc` grew the class and the package that declares it and `vh_method_desc` the same pair
for its result, and the bump is a **major** for the reason recorded twice before it: these
descriptors are handed over as *arrays*, so a field at the end changes the stride the consumer
indexes by and the mismatch reads as corruption rather than as a refusal.

**A signal argument is a `vh_param_desc`, so it came along**, which is what the Node panel's
connect dialog reads to say what a handler will receive.

There are three answers and the third is the one worth knowing. A mirrored class is Godot's own
name; a script class Godot has registered is the PascalCase of its file's stem; and a script class
it has **not** registered is reported as its nearest mirrored ancestor, because a name Godot
cannot resolve is worse than a less specific one it can. That is GDScript's own
`_find_narrowest_native_or_global_class`, and the rule `vh_export_desc` already followed for an
inspector slot -- so the two now answer the same question the same way.

**One assumption was wrong and the measurement caught it.** `?timer` was expected to cross as a
variant, with the class dropped for want of an object slot to filter. It crosses as an *object*:
null is the empty case and it is the one value every object slot can hold, so an optional
reference is named exactly as a plain one is. The case that asserts it says so.

Five cases in the ABI layer and nine in the integration layer, the latter split deliberately
across both descriptions Godot asks for -- a node answers `get_method_list` through the raw
GDExtension vtable and a script answers `_get_script_method_list` with a Dictionary, and only one
of them was ever wrong at a time when the other was right. The one *positive* script-class case
has to be there rather than in the ABI layer: no class in `tests/host_smoke` is registered, since
`exports_probe` carries `@global_class` and is not the class its file is named after, which is
B19's pair from the other side.

---

## What is still open

The checklist itself is gone — every entry on it was watched happen, and a list of twenty-two ticks
is not worth keeping. Eleven things stand open, all of them things no automated layer can reach.
Phase 6's session has since been run and is recorded below with what it found, because the steps
are worth keeping: its half of the debugger has no other test.

### The script editor's colours, after B23 and the three type tiers

**To check it:** open a `.verse` file in Godot's script editor with a line naming one of each --
`Cell:vector2i`, `Nothing:variant`, `Mode:node_internal_mode`, `Greeting:[]char`, `Hit:event(int)`
-- and read the colours. None of them may be the plain text colour a local gets. The names to
watch are the ones from each of the four groups: a mirrored class, a mirrored enum, a value type,
an exported type, and one of Verse's own.

**Then read the three tiers against each other**, which is GDScript's arrangement and is what a
Godot author's eye is already trained on: `node2d` and `node_internal_mode` in the engine-type
colour, `vector2i` and `variant` in the base-type colour, and the project's own class -- plus any
binding generated for a GDScript `class_name` -- in the user-type colour. A binding for a class a
**GDExtension** registered belongs with `node2d`, and checking that one takes a project with a real
addon in it; nothing in this repository has one.

**And a second class in the file**, which is the one that was missed: only the class a file is
named after reaches `script_class_names`, so `test := class(main_script)` beside `mover := class
(node2d)` drew as plain text while its own base drew as a type. Declare a second class, a `struct`
and an `enum` at top level and read all three; the name being declared is user-coloured on the
declaration line itself as well as wherever it is used below.

Nothing automated sees this. The tables behind it are checked in the units layer, which is the part
that drifted; that the highlighter reads them and the editor draws the result is what the eye is
for.

### The completion popup, where Godot decides whether to open one · after B24

**To check it:** in the script editor, type `vector2{` and stop. The popup must open by itself, with
`X` and `Y` in it and nothing else. Then type `Input.IsActionPressed("ui_accept", ?` and stop: the
popup must open with the callee's named parameters. Both were answering correctly before and being
closed before they drew, so what is being read here is the *trigger*, and nothing headless can reach
it — `CodeEdit` decides it from a table no answer from the language passes through.

Then check the same `.` popup an ordinary member completion opens and read the list for a name
ending in `Getter` or `Setter`. There must be none; `probe_complete.py`'s C1 rule is the automated
half, and it counts them, but only the eye sees what the list actually looks like to someone
reading it.

**And the argument hint, which B27 owes.** Complete a call with parentheses — `Input.
IsActionPressed(` — and the hint must be standing above the caret the instant the option lands,
with the first parameter between the markers. Then complete a *failable* one, `GetNode[`, and watch
for the same thing: that is B27's first cause, and `[` in the prefix table is the whole of the fix,
so a hint that appears only once an argument character is typed means it did not take. Then press
**Play**, let the game come up, come back and complete another call straight away: that is the
window where the host is refusing positions and the queued analysis is what puts the hint there, so
the hint may be a beat late but it must arrive without a further keystroke. Do that last one twice
with a save in between, which is what puts a `_validate` behind the completion and is B27's second
cause.

### The Node panel, for a signal that is not `<public>`

`@export_signal` registers a member whatever its access level — the access check is gone, and
R-SIG-1 says why. The integration layer asserts the registration, the connection and the delivery in
both directions, which is everything a headless run can see. What it cannot see is the panel.

**To check it:** put a script on a node with `Own<private>:event(int)` and `@export_signal` above it,
open the **Node** dock, and connect `Own` to a method through the dialog. It must appear in the list
beside the `<public>` ones, with the same payload row, and the connection must save into the scene
and fire at runtime. There is nothing in the drawing path that reads an access level, which is
exactly why this is an eye check rather than a suspicion.

### `_CanDropData` has never been exercised

The one entry that was never ticked, and §B11 above is the measurement that says why no headless run
can reach it. It is a `Control` virtual Godot asks only from the drag path.

**To check it:** give a Control a `_CanDropData<override>(AtPosition:vector2, Data:variant)<decides>:void`
whose body succeeds and a `_DropData<override>` that prints, put it in a windowed scene beside another
Control, start a drag with `force_drag` and drop it on the first. The cursor must accept the drop
and `_DropData` must run. A wrong default is a script that works and an engine that behaves
differently, with nothing printed. `phase-4-gaps.md` G19 carries the same note against the gap it
came from.

### Adding `@tool` to an existing script needs the scene reloaded

Editing a live `@tool` script takes effect on save; giving a script `@tool` for the first time does
not, because the node is holding a *placeholder* and the swap to a real instance does not happen.
§B8 has what is ruled out and where to look. Small enough to live with — the workaround is one
scene reload — and invisible to every automated layer, because a placeholder only exists under
`is_editor_hint()`.

### R-EXP-6's editor half: the custom Resource round-trip · **owed**

Phase 4b's stage 3 built and tested the *runtime* half — a `Resource` with a Verse script attached,
its exported values written and read, saved to `.tres`, loaded back with its values and its methods
intact, in the editor run and in an exported game both. What that cannot reach is the editor's own
UI, which is where the roadmap's exit clause for 4b actually lives, so these five steps stay here
rather than being claimed.

`tests/integration/scripts/settings_resource.verse` is the fixture — a `@global_class` on a
`class(resource)` with three `@export` members — and
`tests/integration/resources/shipped_settings.tres` is one saved from it.

**To check it**, in a *copy* of `tests/integration` — opening it in the editor rewrites its
committed, editor-owned `project.godot`, and the session is for clicking around in:

1. **FileSystem dock → Create New → Resource.** The dialog lists the global class registry; typing
   `SettingsResource` must find it and must file it under `Resource`. This is what
   `_get_global_class_name` and its `base_type` are for, and the one step that says the registry
   half works.
2. **Save it as a `.tres`.** The three exported members must be in the inspector with their declared
   defaults — `untitled`, `3`, `1.5`.
3. **Edit all three and save again.** A non-`@tool` script is a **placeholder** in the editor, and
   deliberately: GDScript does exactly the same (`ScriptServer::is_scripting_enabled()` is false
   under the editor, which is what `_can_instantiate`'s `is_editor_hint()` stands in for). The
   values must survive the save, which is the placeholder's own storage being written out.
4. **Reopen the file.** The edited values must come back, and the `.tres` on disk must carry
   `script_class="SettingsResource"` beside them.
5. **Play the scene with it loaded.** Outside the editor it is a real instance, so `Describe()` must
   answer the edited values rather than the declared ones.

**What a failure would look like, and where to look.** An empty inspector is the export list not
reaching the placeholder (`VerseScript::update_placeholders`). Values that revert on save are
`_get_property_default_value` answering the edited value rather than the declared one. A class the
dialog cannot find is `_get_global_class_name`'s `base_type`, which comes from `base_types_for`.

### R-EXP-8's `@icon`, and the five inspector hints · **owed**

Stage 7 built both and neither is fully visible from a headless run, for two different reasons.

**The five hints** (`@export_file`, `@export_dir`, `@export_multiline`, `@export_flags`,
`@export_node_path`) are asserted as far as they can be: `get_script_property_list()` reports each
one's `hint` and `hint_string`, and twelve cases check that every one is Godot's own constant with
Godot's own spelling beside it. What that does *not* say is that the editor draws the right
control -- a `PROPERTY_HINT_FILE` on a member Godot will not draw looks identical from a script.

**`@icon` is worse**: `Script::get_class_icon_path` is a pure virtual with no ClassDB entry, so
GDScript cannot call it at all. The units layer asserts `verse_scan_class_decl`, which is where
the attribute is read, and nothing above that is reachable.

`tests/integration/scripts/hints.verse` is the fixture -- six exports, five hinted, one
deliberately mispaired -- and it carries `@icon("res://icon.svg")`.

**To check it**, in a *copy* of `tests/integration`, with an `icon.svg` beside `project.godot`:

1. **Select a node carrying `hints.verse`.** `Portrait` must be a file field with a browse button,
   and the dialog it opens must filter to `.png` and `.jpg`.
2. **`SaveFolder`** must browse to a directory rather than a file.
3. **`Notes`** must be a multi-line box that grows, not a one-line field.
4. **`Elements`** must be three checkboxes named Fire, Water and Earth, and ticking Fire then
   Earth must store 5.
5. **`Target`** must offer a node picker that refuses anything that is not a Node2D.
6. **`Mismatched` must be absent**, with the bridge's sentence in the warnings panel and on its
   line in the gutter -- the one case here whose *text* the integration layer already asserts, so
   what is being checked is that the gutter draws it.
7. **The scene tree and the create-node dialog must show `icon.svg`** for the class, rather than
   Node2D's own icon. Then delete the file and reopen: Godot must fall back rather than draw
   nothing, because an `@icon` naming a file that is not there is an author's typo and not a
   thing this bridge validates.

**What a failure would look like, and where to look.** A plain field where a picker belongs is the
hint not arriving -- print `get_script_property_list()` first, because that separates the host's
half from the editor's. A picker with the wrong filter is `hint_string`, which passes through
untranslated and so is exactly what the attribute said. No icon at all is `verse_scan_class_decl`,
which the units layer already covers, or `_get_class_icon_path` not being asked -- Godot asks it of
the *base* script when a scene node has none of its own.

### R-EXP-9's other half: an RPC that arrives at a second peer · **owed**

Phase 4b's stage 6 built and tested everything a single process can see. `Script.get_rpc_config()`
is bound in ClassDB, so the whole *receiving* configuration is assertable from GDScript: which
methods are keys, what each one's `rpc_mode`, `call_local`, `transfer_mode` and `channel` are, that
Godot's defaults are applied to a partial `@rpc`, and that a refused one is absent rather than
half-registered. `tests/integration/scripts/rpcs.verse` is the fixture and there are fifteen cases
on it, in the editor run and in an exported game both.

**What no single-process run can see is the call arriving.** The sending half leaves Verse and comes
back as one of Godot's Error ordinals -- which is asserted -- but *which* ordinal differs between the
two runs for reasons that are Godot's rather than this bridge's: the editor-side driver's SceneTree
has no MultiplayerAPI at all and stops at `Node::rpcp`, while an exported game has one whose default
offline peer reports itself connected, so the call reaches `SceneRPCInterface`, finds the method in
the config, and sends it to nobody. Neither says anything about whether a peer would have run it.

**To check it**, two processes against a copy of `tests/integration`:

1. **Host.** A scene with a `rpcs.verse` node, a GDScript autoload that makes an
   `ENetMultiplayerPeer`, calls `create_server(port)`, and assigns it to
   `get_tree().get_multiplayer().multiplayer_peer`.
2. **Client.** The same scene, `create_client("127.0.0.1", port)`, and the *same node path* -- the
   RPC is addressed by path, so a node at a different path is the commonest way for this to look
   broken when it is not.
3. **From the client, call `SendTakeDamage(5)`.** `TakeDamage` is `@rpc("authority")`, so this must
   be *refused*: only the node's authority may call it, and the client is not. That refusal is the
   permission field doing its job and is worth seeing before the success.
4. **From the host, call it.** `ReadDamage()` on the *client* must answer 5, and on the host 0 --
   `authority` does not imply `call_local`.
5. **From either, call `Nudge(5)`**, which is `@rpc("unreliable_ordered any_peer call_local 3")`.
   Both sides' `ReadDamage()` must move, because `call_local` is what makes the caller run it too.
6. **Check `Ordinary()` is not callable remotely at all** -- it carries no `@rpc`, so it must be
   absent from the config and refused with Godot's own "not marked for RPCs in the local script".

**What a failure would look like, and where to look.** A method Godot says is not marked is
`_get_rpc_config` answering without it: check `vh_class_rpc_list` first in the editor, where
`Script.get_rpc_config()` can be printed, and then in an export, where the config comes from the
**sidecar** rather than from an analysis -- that half was absent for a version and every `@rpc` in a
shipped game was silently not one. A call that arrives but runs on the wrong side is `call_local`.
A call refused for permission when it should not be is the mode, which is the one field whose
default is not zero.

### Phase 6's editor session has been run · **one defect, fixed**

Run, and it found one thing: **a `vector2` reached the inspector as the *text* of one** rather than
as a `Vector2` slot. D7 listed what a local crosses as and the mirrored math structs were not on it,
so every one of them fell through to `VValue::ToString` — honest, and unusable. They belong in the
typed list because they are the one shape a value can name itself: `VNamedType::GetBaseName()` is
the key the generated layout table is keyed by, so the debugger can build the tuple with no
declaration to consult. `GodotVerse::ReadMathStruct` is that, and `tests/host_smoke` now asserts a
`vector2` member arrives tagged `VH_VARIANT_VECTOR2` with its two components.

Worth keeping from the fix: **`tests/host_smoke/debug_probe.verse`'s line numbers are part of the
test.** The new member went in at the top the first time and moved every armed breakpoint under it,
failing six cases. It lives at the bottom of the class now, with a note saying why.

Everything else in the session — the breakpoint gutter, both arming paths, the Debugger panel's
stack and locals and members, stepping, *Skip Breakpoints*, toggling a breakpoint mid-run, and the
profiler panel — behaved.

### What the session covered, and what stays uncovered

Everything from the ABI inward is covered end to end by `tests/host_smoke` — a breakpoint stops
once rather than once per op, the stack's depth and name and path and line, locals and members
(including the `vector2`), stepping, attach and detach, and the profiler's counts and self time and
signature shape. Everything from `EngineDebugger` inward is not, and cannot be: `ScriptLanguage`
exposes nothing a script can ask, and Godot's debugger UI is the only caller of the virtuals that
half consists of. **So this session is what has to be repeated whenever that half changes**, and
the steps are kept for that rather than as an outstanding task.

**To repeat it** — `phase-6-design.md` §2's S-6, verbatim:

1. Open a `.verse` script in Godot's script editor and click the breakpoint gutter.
   `ScriptTextEditor` is language-agnostic on paper — `_breakpoint_toggled` sends
   `edited_res->get_path()` and the row — and this is the confirmation that it is in fact.
2. Run the project. The editor passes the current list as `--breakpoints` at launch *and* sends
   each one again on connect; confirm both paths arm.
3. Confirm the Debugger panel populates: stack frames, the locals list, the members list. `self`
   appears under **members**, not as its own row, and that is permanent (D8).
4. Step in, step over, step out, continue; toggle *Skip Breakpoints*.
5. Toggle a breakpoint **while the game is running** and confirm it arms.

Plus a profiler session: turn it on in the Debugger panel, run `dodge-the-creeps`, and confirm
Verse rows appear beside Godot's own with plausible numbers. Watch that one especially — the array
Godot hands `_profiling_get_accumulated_data` is laid out differently from what godot-cpp declares
(`phase-6-design.md` §13.3), and the stride the bridge uses instead is reasoned from the engine's
version rather than measured. More than one row appearing, with sane signatures, is the evidence
that the reasoning was right.

**Recorded and not taken:** Godot's `LocalDebugger` is drivable headless —
`godot --headless --debug --breakpoints res://scripts/x.verse:N` reads `bt`, `lv`, `mv`, `c` from
stdin and prints frames, locals and members — which `run_tests.py` could pipe and assert on in the
same shape as `tests/coverage_diagnostic`. It is written down so that if this check proves too
costly to repeat, the automated route is a known quantity rather than a rediscovery.

### `refresh_script_warnings` reaches the log now · **taken**

Left here as the record, because the gap was real and the fix is a pattern worth reusing. **Nothing
`refresh_script_warnings` produced was asserted anywhere** — the export rejections (R-EXP-2), the
signal rejections (R-SIG-1) and B19 Stage C's "cannot be saved" all reached the editor alone,
through `_validate`, which hands a warning to Godot's own C++ for the gutter and the warnings panel
and reaches no log.

`VerseScriptLanguage::log_script_warnings` is the second reporter, called once per build beside
`report_name_collisions`, and `run_tests.py`'s integration layer now asserts one sentence per
category. **The pass refreshes the map before reading it** rather than reading it as it stands: in a
session that has only ever built, nothing has called `_validate` and the map is empty.

Two properties measured on the way, both of which were wrong on first writing and neither of which
announces itself:

- the pass runs from the **analysis-completion path**, which the editor drives per keystroke. It
  does not run in a headless `--script` session at all, so "it did not print" there proves nothing
  about the logic — which is why the build-time copy is what a test reads.
- a member's declared type arrives from `vh_class_members` spelled as Verse source, and a `var`
  member's is **`^?stowaway`** — a reference around an option. Stage C's test stripped the `?` and
  not the `^`, and the warning was silent with no other symptom.

**What is still owed is the gutter**, which no automated layer can reach: that each of these appears
at its own member's line, in the warnings panel, and clears as the author fixes it. The build copy
proves the sentence and the line number; it cannot prove the editor draws either.

### Stage B's warning in the script editor, as opposed to in the log · **owed**

B19's Stage B writes one sentence through two reporters, and only one of them is testable.
`report_name_collisions` prints it once per build, which is what `tests/coverage_diagnostic`
asserts; `_validate` returns it to the editor's own C++ for the gutter and the warnings panel, and
**a `_validate` warning reaches no log**, so nothing headless can see it. That half is the half an
author actually meets.

**To check it:** open a `.verse` file declaring a class named after the file plus a second
top-level class, and write `@global_class` above the second one.
`tests/coverage_diagnostic/scripts/inert_global.verse` is exactly that file, and
`tests/integration/scripts/settings_resource.verse` is the same shape with the member to go with it.
The warning must appear in the warnings panel **on the attribute's row, not the class's**, and it
must clear as the attribute is deleted and come back as it is retyped — that liveness is the whole
reason it is read from the buffer rather than from the last analysis. Check it on a file whose own
class carries `@global_class` too, where exactly one of the two attributes should be flagged.

A wrong answer here is quiet in the usual way: the sentence still reaches the log once per build, so
the request is not silently ignored, and only the line the editor points at is wrong.

### R-EXP-7's two editor-side halves · **owed**

Stage 4 is done in a running game and asserted there. Neither of these can be:

**The autoload dialog refusing a non-Node class.** `_create_autoload` tests
`ClassDB::is_parent_class(get_instance_base_type(), "Node")`
(`editor_autoload_settings.cpp:354-355`) and refuses with its own message. The integration suite
asserts the *predicate* -- `settings_resource` reports `Resource`, which the test rejects -- because
naming a bad autoload in `project.godot` would stop the project rather than test it. What is owed is
the dialog itself.

**To check it:** Project > Project Settings > Globals, add `res://scripts/settings_resource.verse`.
Godot must refuse it with its own sentence and no crash. Then add
`res://scripts/game_state.verse`, which must be accepted.

**A `@tool` autoload in the editor.** `in_editor` is `scr.is_valid() && scr->is_tool()`
(`:390`, and again at `:527` and `:590`), so a `@tool` Verse autoload is instantiated in the editor
too and a plain one is not. `is_editor_hint()` is false in every headless run, so no automated layer
can see either case.

**To check it:** give `game_state.verse` `@tool`, reopen the project, and confirm a `@tool`
autoload answers from a `@tool` script in the editor. The bargain it makes is the one `@tool`
already documents -- it runs the **last built** generation -- so an editor session that has never
built runs an autoload with no class behind it, and the honest behaviour there is a script that
reports and carries on rather than a silent no-op. That is the thing to watch for.

### The named-argument popup, and when it opens · **owed**

A `?` at the head of an argument completes to the callee's named parameters —
`Input.IsActionPressed("jump", ?ExactMatch := true)`. The half that can be asserted is asserted:
`host_smoke` proves `vh_signature_at` recovers the `?` off the function type, and that the buffer
the editor sends the instant a `?` is typed — the argument replaced by the placeholder, which past a
`?` reads as an option *type* — does not cost the call its signature. What no headless run can read
is the popup itself, for the reason every completion case here cannot: `ScriptLanguage` exposes
nothing a script can ask.

**To check it:** in the script editor, type `Input.IsActionPressed("ui_accept", ?E`. The popup must
offer `ExactMatch:logic`, accepting it must leave `?ExactMatch := ` with the `?` the author typed
still there, and the hint above the caret must read `IsActionPressed(Action:string,
?ExactMatch:logic):logic` with the `?`. Then check the two spellings of `?` that must **not** open
it: `if (Target?` and a member declared `:?node2d`, both of which are ordinary code and neither of
which has a set of names to offer.

**The popup opens on the bare `?` now, and the first diagnosis of why it did not was wrong.** There
is no `_get_code_completion_prefixes` on `ScriptLanguageExtension` — the trigger characters are the
*editor's*, hard-coded in `CodeTextEditor`'s constructor as `.`, `,`, `(`, `=`, `$`, `@`, `"` and
`'` (`editor/gui/code_editor.cpp:2100`), and `@` and `.` were in that list all along. Only `?` was
outside it. A caret with nothing typed behind a character outside the list is cancelled by
`CodeEdit::_filter_code_completion_candidates`, and `force` does not exempt it: the one branch
`code_completion_forced` reaches is the one for `(`.

What the list is, though, is a per-`CodeEdit` **property** with a bound setter, so a language can
widen it for its own editor and leave every other tab alone.
`VerseEditorPlugin::widen_completion_prefixes` adds `?` and `{` on `editor_script_changed`.
Only those two: a type after `:` and a
specifier after `<` decline an empty prefix inside `_complete_code` itself, so putting them in the
list would raise a popup with nothing to draw.

### And when one of these is looked at again

Record what you saw the way every other correction in this repository is recorded: in
[`phase-4-gaps.md`](phase-4-gaps.md), or in the relevant design document's "where this design was
wrong" section, or here. Then decide whether it is worth automating — `_HasPoint` in §B10 is the
case for asking that question rather than assuming the answer.
