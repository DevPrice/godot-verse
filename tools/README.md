# tools

- `build_host.py` — stages `host/` into the UE engine tree and builds `verse_host.dll` with UBT: `python tools/build_host.py`
- `build_smoke.py` — compiles the ABI smoke test with MSVC: `python tools/build_smoke.py`
- `probe_hover.py` — every tooltip the script editor could draw over a project's `.verse` files, and what is wrong with each: `python tools/probe_hover.py`. An instrument, not a test. `hover_probe.gd` is the driver it copies into the project.
- `probe_complete.py` — the same for the completion popup: what `_complete_code` offers at each caret, and what in it no author could write. `python tools/probe_complete.py`. Also an instrument; `complete_probe.gd` is its driver. A position costs an analysis where a hover costs none, so it takes a `--limit` and picks the carets rather than walking every column. It measures the *answer*: whether Godot raises the popup at all is `CodeEdit`'s own, and that half is by hand.
