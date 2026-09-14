# The by-hand checklist

**Status:** 2026-09-13 · **run, once, and all but one entry is ticked.** What the session found is
[`by-hand-findings.md`](by-hand-findings.md) — nine things that were wrong, numbered B1–B9, plus
B10 and B11 on the list itself. Read that before acting on anything here; several of the ticks
below are "watched happen, and it was broken", not "watched happen and it was fine".

The one entry still open is **`_CanDropData`**, and it is the only one measured to be unreachable
any other way. Two others were expected to be — `_make_function`, whose ClassDB entry does not
exist and whose `Script.get_language()` is not public API, and `_HasPoint` — but `_HasPoint` turned
out to be reachable headless after all (B10), and belongs in `tools/run_tests.py` rather than here.

Every automated layer in this repository drives Godot with `--headless`, and the editor is exactly
what `--headless` does not start. So the flows below have never been exercised by anything: they
need a person, a window, and about twenty minutes. Phase 4's design asked for the checklist as a
written artefact now and for automation only if one of these regresses twice — "the checklist
becomes the manual's raw material".

Each line is one thing to do and one thing to see. Tick nothing you have not watched happen.

    godot --path demo             # the sandbox project
    godot --path dodge-the-creeps # the yardstick, with a window

---

## Owed since Phase 3

- [x] **A windowed run of the yardstick.** `godot --path dodge-the-creeps`, press Start, play a
      round, die. The 30 headless checks assert on state; this is the one that asks whether it
      *looks* right — the sprite animates and faces the way it moves, the mobs come in from the
      edges, the score ticks, the message sequence reads at a human pace.
- [x] **An editor session exercising Play, Build and `@tool`.** Open `demo`, edit a `.verse`, see
      the diagnostic appear per keystroke; press Play and see the edit take effect (a build happens
      on Play, not on save — `VerseEditorPlugin::_build`); use Project ▸ Tools ▸ Build Verse and see
      a failed build refuse the run and leave the last good generation running.

      > adding/updating @tool scripts only seems to take affect on editor restart. not sure if that is intended or not

## Phase 4

- [x] **Connect a Verse-declared signal through the Node panel.** Select the Player node in
      `dodge-the-creeps/player.tscn`. The Node dock's Signals list must show **`Hit`** under the
      script's own section, with no arguments, beside Godot's `body_entered`.

- [x] **Let `_make_function` write the handler.** Connect `Hit` to another node and leave "Make
      Function" checked. The editor must write a handler into that node's `.verse` with the right
      name and no parameters, and the file must still compile.

      **This is the only way to check it at all**, which is why it is worth doing carefully:
      `_make_function` is a `ScriptLanguageExtension` virtual with no ClassDB entry, and
      `Script.get_language()` is not in the public API, so GDScript can reach neither. The editor's
      own C++ is its only caller — a headless test answers *"Nonexistent function"*, measured.

      Exactly what to expect, tabs included, since Verse rejects mixed tabs and spaces and Godot's
      editor writes tabs:

      ```
      	OnHit<public>()<transacts>:void =
      		# TODO
      ```

      The `<transacts>` is load-bearing, not decoration: `Subscribe` fixes its callback at that
      effect, and a specifier-less function carries the wider default set that a `<transacts>`
      context may not call. A stub without it is `dodge-the-creeps.md` wall 8 on the author's first
      generated line. For a signal with arguments, each parameter takes the Verse spelling of its
      type — `Damage:int`, `By:string`, `Body:node2d` — and a struct payload gives them the field's
      own names rather than `Arg0`.

      > This works, but when the editor inserts the function in the script for you, it looks like below, which doesn't compile in Verse (Dangling `=` assignment with no expressions or empty braced block `{}` on its right hand side.) Also, connected signal methods don't have an icon in the gutter indicating they have connection(s) like those in GDScript.
```
OnPlayerHit<public>()<transacts>:void =
	# TODO
```

- [x] **A signal with arguments names them.** Do the same for the HUD's `StartGame`, and for a
      signal declared `godot_signal(tuple(int, string))` — the connect dialog must show two
      arguments, `Arg0` and `Arg1`, and the generated stub must take two parameters. (Verse tuples
      cannot name their elements. A *struct* payload does give real names — see the next line.)
- [x] **A struct payload names its arguments in the connect dialog.** `signals.verse` declares
      `Reported:godot_signal(strike_report)` over a three-field struct. The dialog must show
      **`Damage`, `By`, `Point`** — the fields' own names, not `Arg0`/`Arg1`/`Arg2` — and typed
      `int`, `String`, `Vector2`. This is the whole reason the struct row exists, and it is the one
      half of it a headless run cannot check: the integration suite proves the *arguments* arrive,
      not that a designer sees the names.
- [x] **A refused signal says why, at its own line.** Open `tests/integration/scripts/signal_rejects.verse`
      in the script editor. Four members must each carry a warning on their own line — `Reassignable`
      (a `var`), `Unseen` (not `<public>`), `Nested` (a struct field that is itself a struct) and
      `Maybe` (a payload with no Godot type) — and `Fine` must carry none. Attach the script to a
      node and confirm the Node dock's Signals list shows **only `Fine`**. Then fix one (drop the
      `var`) and watch its warning clear on the next keystroke, without a build.
- [x] **A `@tool` script's configuration warning shows on the node.** Give a `@tool` script a
      `_GetConfigurationWarnings<override>():[]string` that returns one string. The node must show
      the warning triangle in the Scene dock with that text in its tooltip, and it must clear when
      the condition does.

      > while testing this, I notice that function autocomplete no longer works in class bodies (autocompleting full override function signatures). this is a regression from something.
- [x] **An `_Input` handler receives a key.** Override `_Input<override>(Event:input_event):void` on
      a node in `demo`, print the event, and press a key with the game window focused. Nothing
      headless can press a key.

      > this doesn't work:
      Error at (13, 22): The data (/user@localhost/spinner/_Input:)event in package GodotScripts_2 is ambiguous with these definitions:
          function (/Verse.org/Verse:)event(:t) in package Verse/Verse
          function (/Verse.org/Verse:)event() in package Verse/Verse
- [x] **`_HasPoint` gates what the engine picks.** Give a Control a `_HasPoint<override>` returning
      `false` over its whole rect and confirm clicks fall through to what is behind it.

      > Done headless, and it did not need a person: `Input.parse_input_event` with an
      > `InputEventMouseButton` makes the engine's own picking path ask. `false` sends the click to
      > the control underneath, `true` keeps it on top. `phase-4-gaps.md` G19 is wrong about this
      > half — see `by-hand-findings.md` B10, which asks for it in `tests/integration` instead.

- [ ] **`_CanDropData` gates what the engine will drop on.** A `Control` virtual Godot reaches only
      from the drag path — it has no public caller, and unlike `_HasPoint` above it cannot be
      reached by injecting input either (`by-hand-findings.md` B11: the drag-hover path needs
      `gui.target_control`, which `Viewport::_update_mouse_over` leaves unset for a native window
      because the dummy display server never sends a window-enter event). Give a Control a
      `_CanDropData<override>` returning `true` and confirm the drag cursor accepts a drop. A wrong
      default is a script that works and an engine that behaves differently, with nothing printed.

      **This is the last entry on this list**, and the only one measured to need a window.
- [x] **The inspector no longer shows the yardstick's node slots.** Open `dodge-the-creeps/main.tscn`
      and select Main: the only exported property must be `MobScene`. The nine that were there
      before Phase 4 are lookups in `_Ready` now, and a stale `node_paths` entry left in a `.tscn`
      would show up here.
- [x] **A statics module shows in the editor, and a mistyped one says so.** With `@statics("...")`
      applied, the script's constants must appear where Godot shows a script's constant map. Then
      mistype the class name: the editor must report that the module names a class no script
      declares, at the module's own line. Add a *second* module claiming the same class and it must
      say that too. Those two diagnostics are the entire argument for the attribute over a naming
      convention (`phase-4-gaps.md` G10), so a silent failure here means the attribute bought
      nothing.
- [x] **A missing math method explains itself.** Write `SomeBasis.GetEuler()` in a `.verse` — one of
      the 410 still unwritten, and deliberately so. The diagnostic must not stop at "Unknown member
      `GetEuler` in `basis`": it must append that the math types are ordinary Verse, this one has not
      been written yet, and `host/Verse/GodotMath.native.verse` is where it goes (`phase-4-gaps.md`
      G12); before, every one of them was silence. Try an operator too — `SomeVector2 * 2` with an
      *integer* right-hand side, which is unwritten where the float one is — and it must take the
      other of the two sentences.
- [x] **A utility explains itself too.** Write `floor(X)` in a `.verse`. The diagnostic must name the
      Verse spelling — `FloorF(X)` — rather than saying the utility was skipped. All 114 of Godot's
      utilities answer one of three ways now and none is unexplained (`phase-4-gaps.md` G11), so try
      `hash(X)` as well: that one must say its Variant parameter is unspellable rather than offering
      an alternative.

## Phase 4.5

Both of these are *sentences in the editor*, and the automated layers can only see them in the
output log — `tests/coverage_diagnostic` asserts the text, never where it is drawn.

- [x] **The `<transacts>` trap says where the fix goes.** In `demo`, write a helper with no effect
      specifier and call it from a failure context:

          Helper():int = 7
          Uses()<transacts>:int = Helper()

      The script editor's error list must show the compiler's own sentence *followed by* "Write
      `<transacts>` on `Helper`'s own declaration, which is where the fix goes even though the error
      is reported here". Then write `QueueFree()` inside a `()<reads>:void` function: the same
      diagnostic must take the *other* sentence, the one that says this function is what has to
      widen. Both must appear per keystroke, and both must clear when the fix is typed.

      > I don't like the additional error message context; the built-in message is enough on its own

- [x] **A new script's template carries the warning.** Attach a new Verse script to a node through
      the editor's Attach Script dialog. The generated file must carry the two comment lines above
      the class saying a helper of your own wants `<transacts>`. The template is never read by any
      automated layer, so this is the only thing that sees it. (It does not compile as generated and
      is not meant to: `_Process`'s body is left empty for the author's cursor.)

      > this mostly works, but the attach node dialog shows "No suitable template." even though it does render a template on creation. like the above I don't like the extra context. let's just directly translate GDScripts template and comments, e.g.:

```
extends Node2D

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.

# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	pass

```

## Phase 5

Tasks are the one part of the bridge whose *feel* cannot be asserted on: a headless run at
`--fixed-fps 60` proves a sequence resumes on the right frame, and says nothing about whether a
second of waiting looks like a second.

- [x] **The game-over sequence looks right with a window.** `godot --path dodge-the-creeps`, play a
      round and die. "Game Over" must hold for about a second, then the title, then about another
      second, then the Start button — with no flicker where the message hides and the title appears
      in the same frame. This is the wall-3 sequence the port now writes as two `Await`s, and the
      headless check only knows it happened on frames 275 and 345.
- [x] **A task on a node you free does not take the frame with it.** In `demo`, write a script that
      spawns a body awaiting `GetTree[].CreateTimer[5.0]`'s timeout, attach it to a node, run, and
      free the node before the timer fires (a second node with a Timer and `QueueFree` on the first
      will do). Nothing must be printed, nothing must warn, and the game must keep running — a
      cancelled task that still held its instance would show as a crash here and nowhere else.

      > Done headless. A node spawning a body that awaits `CreateTimer[5.0]`, freed at frame 30 of
      > a 420-frame run: the timeout print never came, no warning, no error, and a third node's
      > heartbeat ran to the last frame. The control run — the same scene with nothing freeing the
      > waiter — prints the timeout, so the silence is cancellation and not a dead fixture.
- [x] **The frame-budget monitors are readable.** Run `demo` from the editor with the profiler open
      and the Monitors tab showing. `verse/queued_jobs`, `verse/pump_ms` and `verse/sleeping_tasks`
      must all be listed and must draw — and spawning a body that `Sleep`s must move
      `sleeping_tasks` off zero. R-ASYNC-6 asks for the budget to be observable, and this is the
      only place the observation is drawn rather than printed.
- [x] **A new script's template says how to wait.** The same Attach Script dialog as Phase 4.5's
      entry below, read again: the generated file must carry the four comment lines saying a
      `<suspends>` method started with `spawn{...}` is how you wait, and that neither `<suspends>`
      on the override nor an effect specifier on the waiting method will compile. This is the only
      thing that ever reads the template.
- [x] **A raise in one script leaves the others running, visibly.** Two scripted nodes in `demo`,
      both printing from `_Process`, one of them reaching through a freed reference every frame.
      The error must appear once per frame *and the other node must keep printing* — before Phase 5
      the whole project went quiet for the rest of each frame, which is invisible to a test that
      only calls one script.

      > Done headless. One error per frame from the raising node, and `steady: frame N` on every
      > frame from 1 with no gap where a raise landed. The `ERROR:` line Godot prints is malformed
      > — `by-hand-findings.md` B9.

## What a failure here means

These are the flows a *person* meets first and the automated layers never reach. A failure is not a
regression in something tested elsewhere; it is a gap that has never been covered. Record what you
saw in [`phase-4-gaps.md`](phase-4-gaps.md) — or the relevant design document's "where this design
was wrong" section, for an older phase — the way every other correction in this repository is
recorded, and only then decide whether it is worth automating.
