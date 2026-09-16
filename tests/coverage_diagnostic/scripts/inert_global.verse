using { /Godot.org/Godot }

# Stage B of B19: `@global_class` on a class that is not the one named after its file registers
# nothing at all, because Godot collects one global class per script *path* -- and until this
# warning existed the bridge accepted the attribute and ignored it in silence.
#
# The file's own class is deliberately left plain, so the warning below cannot be confused with
# anything about this one. `sidecar` is the request that cannot be served.
inert_global := class(node2d):
    Which<public>()<transacts>:string = "inert"

@global_class
sidecar := class(resource):
    Value<public>:int = 0
