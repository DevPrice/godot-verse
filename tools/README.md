# tools

- `build_host.py` — stages `host/` into the UE engine tree and builds `verse_host.dll` with UBT: `python tools/build_host.py`
- `build_smoke.py` — compiles the ABI smoke test with MSVC: `python tools/build_smoke.py`
- `probe_hover.py` — every tooltip the script editor could draw over a project's `.verse` files, and what is wrong with each: `python tools/probe_hover.py`. An instrument, not a test. `hover_probe.gd` is the driver it copies into the project.
