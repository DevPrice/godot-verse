using { /Godot.org/Godot }

# A call site for the symbol lookup, and deliberately never called. The 39 core singleton accessors
# are total and raise through `Err` when the cast finds nothing, and nothing answers GetSingleton
# behind this ABI -- so running this would take `Main` with it, and `vh_run_main` wants one that
# returns.
ReachSingleton<public>()<reads>:string = GetEngine().GetClass()

Main(Args:[]string, Env:[string]string)<suspends>:void=
    Print("Hello from Verse, running inside a DLL.")
