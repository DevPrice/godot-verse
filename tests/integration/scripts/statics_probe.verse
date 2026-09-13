using { /Godot.org/Godot }

# R-NODE-4 and R-NODE-5: a script's own statics, and an abstract base.
#
# Verse has no `static` keyword. What it has is a module, and a module inside a script file is
# ordinary Verse -- `StaticsProbeStatics.MaxSpeed` works with nothing from the bridge at all. What
# `@statics` buys is the *link*, which Godot needs to answer `get_script_constant_map()` and
# `has_static_method()`, and which a naming convention could not: a typo in a convention makes a
# silently empty statics module where a module naming a class that does not exist can be told.
@statics("statics_probe")
StaticsProbeStatics<public> := module:
    MaxSpeed<public>:float = 400.0
    Label<public>:string = "probe"
    Lives<public>:int = 3

    Describe<public>()<transacts>:string = "the probe"

@global_class
statics_probe := class(node2d):

    # Read from ordinary Verse, which is the half that needed no bridge.
    ReadMaxSpeed<public>()<transacts>:float = StaticsProbeStatics.MaxSpeed
    ReadDescription<public>()<transacts>:string = StaticsProbeStatics.Describe()
