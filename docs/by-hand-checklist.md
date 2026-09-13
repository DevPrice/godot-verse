# The by-hand checklist

**Status:** 2026-09-12 · **nothing on it has been run.** It is the list of things no headless run
can see, which is why they are here and not in `tools/run_tests.py`.

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

- [ ] **A windowed run of the yardstick.** `godot --path dodge-the-creeps`, press Start, play a
      round, die. The 30 headless checks assert on state; this is the one that asks whether it
      *looks* right — the sprite animates and faces the way it moves, the mobs come in from the
      edges, the score ticks, the message sequence reads at a human pace.
- [ ] **An editor session exercising Play, Build and `@tool`.** Open `demo`, edit a `.verse`, see
      the diagnostic appear per keystroke; press Play and see the edit take effect (a build happens
      on Play, not on save — `VerseEditorPlugin::_build`); use Project ▸ Tools ▸ Build Verse and see
      a failed build refuse the run and leave the last good generation running.

## Phase 4

- [ ] **Connect a Verse-declared signal through the Node panel.** Select the Player node in
      `dodge-the-creeps/player.tscn`. The Node dock's Signals list must show **`Hit`** under the
      script's own section, with no arguments, beside Godot's `body_entered`.
- [ ] **Let `_make_function` write the handler.** Connect `Hit` to another node and leave "Make
      Function" checked. The editor must write a handler into that node's `.verse` with the right
      name and no parameters, and the file must still compile.
- [ ] **A signal with arguments names them.** Do the same for the HUD's `StartGame`, and for a
      signal declared `godot_signal(tuple(int, string))` — the connect dialog must show two
      arguments, `Arg0` and `Arg1`, and the generated stub must take two parameters. (Verse tuples
      cannot name their elements. A *struct* payload does give real names — see the next line.)
- [ ] **A struct payload names its arguments in the connect dialog.** `signals.verse` declares
      `Reported:godot_signal(strike_report)` over a three-field struct. The dialog must show
      **`Damage`, `By`, `Point`** — the fields' own names, not `Arg0`/`Arg1`/`Arg2` — and typed
      `int`, `String`, `Vector2`. This is the whole reason the struct row exists, and it is the one
      half of it a headless run cannot check: the integration suite proves the *arguments* arrive,
      not that a designer sees the names.
- [ ] **A refused signal says why, at its own line.** Open `tests/integration/scripts/signal_rejects.verse`
      in the script editor. Four members must each carry a warning on their own line — `Reassignable`
      (a `var`), `Unseen` (not `<public>`), `Nested` (a struct field that is itself a struct) and
      `Maybe` (a payload with no Godot type) — and `Fine` must carry none. Attach the script to a
      node and confirm the Node dock's Signals list shows **only `Fine`**. Then fix one (drop the
      `var`) and watch its warning clear on the next keystroke, without a build.
- [ ] **A `@tool` script's configuration warning shows on the node.** Give a `@tool` script a
      `_GetConfigurationWarnings<override>():[]string` that returns one string. The node must show
      the warning triangle in the Scene dock with that text in its tooltip, and it must clear when
      the condition does.
- [ ] **An `_Input` handler receives a key.** Override `_Input<override>(Event:input_event):void` on
      a node in `demo`, print the event, and press a key with the game window focused. Nothing
      headless can press a key.
- [ ] **The inspector no longer shows the yardstick's node slots.** Open `dodge-the-creeps/main.tscn`
      and select Main: the only exported property must be `MobScene`. The nine that were there
      before Phase 4 are lookups in `_Ready` now, and a stale `node_paths` entry left in a `.tscn`
      would show up here.
- [ ] **A statics module shows in the editor.** With `@statics("...")` applied, the script's
      constants must appear where Godot shows a script's constant map. A module naming a class
      that does not exist **will not** say so — that diagnostic is unbuilt, `phase-4-gaps.md` G10 —
      so this checks the constants only.
- [ ] **A missing math method explains itself.** Write `Position.Snapped(vector2{X := 8.0, Y := 8.0})`
      in a `.verse`. The diagnostic must not stop at "Unknown member `Snapped` in `vector2`": it must
      append that the math types are ordinary Verse, this one has not been written yet, and
      `host/Verse/GodotMath.native.verse` is where it goes. 585 members answer this way now
      (`phase-4-gaps.md` G12); before, every one of them was silence. Try an operator too —
      `SomeVector / OtherVector` — which takes the other of the two sentences.

## What a failure here means

These are the flows a *person* meets first and the automated layers never reach. A failure is not a
regression in something tested elsewhere; it is a gap that has never been covered. Record what you
saw in [`phase-4-gaps.md`](phase-4-gaps.md) — or the relevant design document's "where this design
was wrong" section, for an older phase — the way every other correction in this repository is
recorded, and only then decide whether it is worth automating.
