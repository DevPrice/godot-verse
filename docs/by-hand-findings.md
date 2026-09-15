# What the by-hand session found

**Status:** 2026-09-14 · the record of the windowed passes over
what was `docs/by-hand-checklist.md`, and of the work that closed what they found. **The checklist is
deleted**: all twenty-two of its entries were watched happen, and what is worth keeping is what they
found rather than the list. §"What is still open" at the bottom carries the two things that
outlived it. **Every entry below is fixed**, except the two that are not defects: B11 is a
measurement, and B8 is fixed for the half a headless run can reach and re-checkable by hand for the
other. `tools/run_tests.py` is 2/2 with **317** integration cases.

Four kinds of entry are below. **B1–B9** are what the session saw go wrong, each traced to the code
that causes it rather than left as a symptom. **B10–B11** are what the session learned about the
checklist itself: one entry did not need to be on it, and one belongs there permanently. **B12** is a
Verse fact found while writing B10's test, which the test had got wrong. **B13** is from a later
session, after the editor-performance commits, and is the one entry here that is about latency
rather than behaviour.

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
`dodge-the-creeps`, `demo` and `tests/` is either `?`-suffixed — `Keys.IsActionPressed("move_right")?`
in the yardstick, four times — or a comparison operator, which is genuinely `<decides>`. The one
occurrence was the fixture written three minutes earlier.

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
| **B8** | `VerseScript` tracks its owners; `_reload` re-attaches, carrying exported values across. |
| **B10** | `tests/integration/scripts/has_point_probe.verse` and two cases, both answers exercised. |
| **B12** | `CLAUDE.md`, beside the dropped-continuation-line constraint. |
| **B13** | `vh_class_override_candidates` (ABI 7.1) hands the snapshot's per-class override candidates over without waiting; the language folds them into the first answer through the same `completes_as_override` the refined path uses. |

---

## What is still open

The checklist itself is gone — every entry on it was watched happen, and a list of twenty-two ticks
is not worth keeping. Two things stand open, both of them things no automated layer can reach.
Phase 6's session has since been run and is recorded below with what it found, because the steps
are worth keeping: its half of the debugger has no other test.

### `_CanDropData` has never been exercised

The one entry that was never ticked, and §B11 above is the measurement that says why no headless run
can reach it. It is a `Control` virtual Godot asks only from the drag path.

**To check it:** give a Control a `_CanDropData<override>(AtPosition:vector2, Data:variant):logic`
returning `true` and a `_DropData<override>` that prints, put it in a windowed scene beside another
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

### And when one of these is looked at again

Record what you saw the way every other correction in this repository is recorded: in
[`phase-4-gaps.md`](phase-4-gaps.md), or in the relevant design document's "where this design was
wrong" section, or here. Then decide whether it is worth automating — `_HasPoint` in §B10 is the
case for asking that question rather than assuming the answer.
