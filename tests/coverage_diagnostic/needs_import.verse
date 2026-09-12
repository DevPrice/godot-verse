using { /Godot.org/Godot }

# Deliberately missing its import. `SoloHelper` is declared in the `solo` module and nothing here
# brings it into scope, so the compiler reports an unknown identifier and the bridge appends the
# exact `using` line that resolves it.
needs_import := class(node2d):
    Call<public>()<transacts>:int = SoloHelper()
