# The by-hand checklist

**Status:** 2026-09-13 · **nothing on it has been run**, and it has grown: it is now the *whole* of
what Phase 4 still owes, every numbered gap having been closed or answered. It is the list of things
no headless run can see, which is why they are here and not in `tools/run_tests.py`.

Three of the entries below are not merely untested but **untestable** from a headless run, and that
was measured rather than assumed: `_make_function` has no ClassDB entry and `Script.get_language()`
is not in the public API, so GDScript can reach neither; `_HasPoint` and `_CanDropData` have no
public caller at all. For those three this list is not the cheaper option, it is the only one.

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
- [ ] **`_HasPoint` and `_CanDropData` gate what the engine does.** Both are `Control` virtuals Godot
      reaches only from pointer-input and drag paths — neither has a public caller, so no headless
      run can make the engine ask (`phase-4-gaps.md` G19, where `_GetMinimumSize` covers the shared
      mechanism instead). Give a Control a `_HasPoint<override>` returning `false` over its whole
      rect and confirm clicks fall through to what is behind it; give another a
      `_CanDropData<override>` returning `true` and confirm the drag cursor accepts a drop. A wrong
      default in either is a script that works and an engine that behaves differently, with nothing
      printed.
- [ ] **The inspector no longer shows the yardstick's node slots.** Open `dodge-the-creeps/main.tscn`
      and select Main: the only exported property must be `MobScene`. The nine that were there
      before Phase 4 are lookups in `_Ready` now, and a stale `node_paths` entry left in a `.tscn`
      would show up here.
- [ ] **A statics module shows in the editor, and a mistyped one says so.** With `@statics("...")`
      applied, the script's constants must appear where Godot shows a script's constant map. Then
      mistype the class name: the editor must report that the module names a class no script
      declares, at the module's own line. Add a *second* module claiming the same class and it must
      say that too. Those two diagnostics are the entire argument for the attribute over a naming
      convention (`phase-4-gaps.md` G10), so a silent failure here means the attribute bought
      nothing.
- [ ] **A missing math method explains itself.** Write `SomeBasis.GetEuler()` in a `.verse` — one of
      the 410 still unwritten, and deliberately so. The diagnostic must not stop at "Unknown member
      `GetEuler` in `basis`": it must append that the math types are ordinary Verse, this one has not
      been written yet, and `host/Verse/GodotMath.native.verse` is where it goes (`phase-4-gaps.md`
      G12); before, every one of them was silence. Try an operator too — `SomeVector2 * 2` with an
      *integer* right-hand side, which is unwritten where the float one is — and it must take the
      other of the two sentences.
- [ ] **A utility explains itself too.** Write `floor(X)` in a `.verse`. The diagnostic must name the
      Verse spelling — `FloorF(X)` — rather than saying the utility was skipped. All 114 of Godot's
      utilities answer one of three ways now and none is unexplained (`phase-4-gaps.md` G11), so try
      `hash(X)` as well: that one must say its Variant parameter is unspellable rather than offering
      an alternative.

## What a failure here means

These are the flows a *person* meets first and the automated layers never reach. A failure is not a
regression in something tested elsewhere; it is a gap that has never been covered. Record what you
saw in [`phase-4-gaps.md`](phase-4-gaps.md) — or the relevant design document's "where this design
was wrong" section, for an older phase — the way every other correction in this repository is
recorded, and only then decide whether it is worth automating.
