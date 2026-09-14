# The by-hand checklist

**Status:** 2026-09-13 · **run, once, and all but one entry is ticked.** What the session found is
[`by-hand-findings.md`](by-hand-findings.md) — nine things that were wrong (B1–B9), two about this
list (B10, B11), and one Verse fact (B12). **All of them are closed.** Read that document before
acting on anything here: most of the ticks below are "watched happen, and it was broken", and the
entries have been rewritten to describe the behaviour that replaced what was watched.

The one entry still open is **`_CanDropData`**, the only one measured to be unreachable any other
way. Two others were expected to be — `_make_function`, whose ClassDB entry does not exist and whose
`Script.get_language()` is not public API, and `_HasPoint` — but `_HasPoint` turned out to be
reachable headless after all (B10) and is a case in `tests/integration` now.

**Those four have since been looked at by hand.** Override completion and the connection gutter are
confirmed working; the Attach Script dialog needed one more fix, and got it (its entry says which).
One thing stayed broken and is documented rather than repaired: **adding `@tool` to a script that
did not have it still needs the scene reloaded.** That is the only known-open behaviour on this
page besides `_CanDropData`.

Every automated layer in this repository drives Godot with `--headless`, and the editor is exactly
what `--headless` does not start. Phase 4's design asked for the checklist as a written artefact
first and for automation only if one of these regresses twice — "the checklist becomes the manual's
raw material".

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

      > Not intended. Fixed and re-checked by hand for the part that matters most: **editing a
      > live `@tool` script now takes effect on save**, because a reload re-attaches the script to
      > every object holding it (`by-hand-findings.md` B8, and a case in `tests/integration`).
      >
      > **Still open, and known:** *adding* `@tool` to a script that did not have it needs the
      > scene reloaded before the node picks it up. That transition turns a placeholder into a real
      > instance, only happens under `is_editor_hint()`, and so has no headless test. Small enough
      > to live with — the workaround is one scene reload — and B8 says where to look.

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
      		{} # Replace with function body.
      ```

      The `<transacts>` is load-bearing, not decoration: `Subscribe` fixes its callback at that
      effect, and a specifier-less function carries the wider default set that a `<transacts>`
      context may not call. A stub without it is `dodge-the-creeps.md` wall 8 on the author's first
      generated line. For a signal with arguments, each parameter takes the Verse spelling of its
      type — `Damage:int`, `By:string`, `Body:node2d` — and a struct payload gives them the field's
      own names rather than `Arg0`.

      > The stub it wrote did not compile — a `# TODO` comment is not an expression, so the body was
      > empty. Fixed: it ends `{} # Replace with function body.` now, which is the text above.
      > **Re-check by hand, and check the gutter with it**: a connected handler got no Slot icon,
      > which had two causes and one of them was this (`by-hand-findings.md` B3, B4).

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

      > While testing this: override completion inside a class body offered nothing. It was a
      > regression, from Phase 4 rather than from anything nearby — the guard that skipped the
      > mirror was written when Godot's virtuals lived on the native root, and Phase 4 moved all
      > 1413 of them onto the mirrored classes. Fixed (`by-hand-findings.md` B1); **re-check by
      > hand**, since nothing automated draws a completion popup.
- [x] **An `_Input` handler receives a key.** Override `_Input<override>(Event:input_event):void` on
      a node in `demo`, print the event, and press a key with the game window focused. Nothing
      headless can press a key.

      > Reported as broken; it is not. The parameter was written `event`, which collides with
      > `/Verse.org/Verse`'s own `event`. `Event` — the mirror's spelling — compiles and binds to
      > Godot's `_input`, both confirmed with `tests/verse_probe` (`by-hand-findings.md` B2). The
      > reason it was typed by hand at all is B1 above.
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

Both of these were *sentences in the editor* that no longer exist. They were watched, disliked, and
removed — `by-hand-findings.md` B6 and B7 — so what is written here now is the behaviour that
replaced them, and both were re-checked against it.

- [x] **The `<transacts>` trap says nothing but what the compiler says.** In `demo`, write a helper
      with no effect specifier and call it from a failure context:

          Helper():int = 7
          Uses()<transacts>:int = Helper()

      The script editor's error list must show the compiler's sentence and **only** that sentence:
      *"This invocation calls a function (`…Helper`) that has the 'no_rollback' effect, which is not
      allowed by its context."* No appended advice about where the fix goes. Then write
      `QueueFree()` inside a `()<reads>:void` function and confirm the same — the compiler naming
      the `transacts` effect, and nothing after it. Both must appear per keystroke and clear when
      the fix is typed.

      > The appended sentences are gone. What decided it beyond taste: the appender keyed on glitch
      > 3512 and the callee's package alone and never on *which* effect had been refused, so a
      > `suspends` refusal from a Godot signal took the `transacts` branch and told the author to
      > write the one word an awaiting body may not carry. `tests/coverage_diagnostic` now asserts
      > the compiler's own text.

- [x] **A new script's template is GDScript's, and compiles.** Attach a new Verse script to a node
      through the editor's Attach Script dialog. The dialog must offer **"Object: Default"** rather
      than reporting "No suitable template.", and the generated file must be a direct translation of
      GDScript's — the two comments, `_Ready` and `_Process`, `{}` bodies — and must compile as
      generated with no edit.

      > Both halves were wrong before. `_get_built_in_templates` returned nothing, which is what
      > produced "No suitable template." over a dialog that then wrote one; and the template carried
      > six lines of `<transacts>` and `spawn` caveat that no longer earn their place ahead of the
      > first line of code. `{}` is Verse's `pass`.
      >
      > Re-checked by hand, which turned up a third: **unchecking the Template checkbox still wrote
      > the template.** The checkbox does not clear the content — Godot looks for a built-in named
      > exactly "Empty" and uses that one. There is one now, and it is a blank file. Check it from
      > the FileSystem dock's right-click too, which opens the same dialog with no node involved.

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
