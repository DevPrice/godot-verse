# What the by-hand session found

**Status:** 2026-09-13 · the record of the first windowed pass over
[`by-hand-checklist.md`](by-hand-checklist.md), which had never been run — and of the work that
closed what it found. **Every entry below is fixed**, except the two that are not defects: B11 is a
measurement, and B8 is fixed for the half a headless run can reach and re-checkable by hand for the
other. `tools/run_tests.py` is 2/2 with **316** integration cases, up from 311.

Three kinds of entry are below. **B1–B9** are what the session saw go wrong, each traced to the code
that causes it rather than left as a symptom. **B10–B11** are what the session learned about the
checklist itself: one entry did not need to be on it, and one belongs there permanently. **B12** is a
Verse fact found while writing B10's test, which the test had got wrong.

Three of these were checked with the two tools this repository already has for the purpose rather
than by reasoning about them — `tests/verse_probe` for B2 and B3, a scratch Godot project driven
headless for B9, B10 and B11. §0 of [`phase-4-gaps.md`](phase-4-gaps.md) is why, and it held again
here: B4's obvious diagnosis is not its whole cause, and B11's first two plausible workarounds both
failed.

**Companion to:** [`by-hand-checklist.md`](by-hand-checklist.md) (the list, now ticked),
[`phase-4-gaps.md`](phase-4-gaps.md) (G19, which B10 partly retires),
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

**What closes it** is doing what GDScript does, which is the other way round. Return one template
from `_get_built_in_templates`, keyed on the base class it inherits, with the content carrying
`_BASE_` and `_CLASS_` placeholders; then have `_make_template` substitute into `p_template` when it
is non-empty instead of ignoring it (`GDScriptLanguage::make_template`,
`modules/gdscript/gdscript_editor.cpp:107`). `_BASE_` still goes through `verse_base_class_for`,
since the dialog hands over a *Godot* class name and the template needs the mirrored one.

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

> This invocation calls a function (`(/Godot.org/Godot/godot_signal:)Await`) that has the
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

**Two things are still owed by hand**, and both are named on the checklist rather than here: the
gutter icon B4 should have restored, which only the editor draws, and the half of B8 that turns a
placeholder into a real instance, which only happens under `is_editor_hint()`. Plus `_CanDropData`,
which was never going to be automated.
