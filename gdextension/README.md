# The Godot API this extension is built against

`extension_api.json` and `gdextension_interface.json` are dumped from the **Godot 4.7 stable**
editor. godot-cpp's own `gdextension/` carries 4.6, and its remote has no `godot-4.7-stable` tag
(the 4.7 dump landed on `master` under the newer `extension_api-4-7.json` name, which the pinned
submodule commit cannot resolve), so the dump lives here instead of overwriting the submodule's
copy. A modified submodule would be reverted, silently and to a *different Godot version*, by the
`git submodule update --init --recursive` that `SConstruct` itself tells people to run.

`SConstruct` points godot-cpp at this directory with `gdextension_dir`; `tools/gen_verse_api.py`
and `tools/audit_const_overrides.py` read `extension_api.json` from here.

To move to a newer Godot, run its editor:

    godot --headless --dump-extension-api --dump-gdextension-interface-json

and replace both files (the `.h` it also writes is not used — godot-cpp generates its own from the
JSON). Then `python tools/gen_verse_api.py`, bump `compatibility_minimum` in
`godot-verse.gdextension.in`, and rebuild both DLLs.
