using { /Godot.org/Godot }

# R-SCN-6: the failable cast, and the object identity it rests on.
#
# `GetNode` is declared to return `node`, so every case here turns on the host having built the
# object at the class Godot says the handle *is* rather than at the class the signature named. The
# spelling that used to be needed instead was an `@export` slot per child and an `?option` unwrap
# around it, which is what docs/dodge-the-creeps.md measured as the most expensive absence.
@global_class
casting := class(node2d):

    # A node Godot hands back, cast to the mirror class it actually is.
    SpriteName<public>(Path:string)<transacts>:string =
        if (Found := GetNode[Path], Sprite := sprite2d[Found]):
            Sprite.GetName()
        else:
            "no"

    # The same node asked for a class it is not. A miss rather than a raise, which is the half that
    # makes a cast usable as a question rather than as an assertion.
    TimerName<public>(Path:string)<transacts>:string =
        if (Found := GetNode[Path], Clock := timer[Found]):
            Clock.GetName()
        else:
            "no"

    # The identity half: a node carrying a Verse script crosses as *that script's own object*, so
    # it casts to the script's class and the method that runs is the script's own override. A fresh
    # mirror wrapper would be a `node2d` and this could never succeed.
    Describe<public>(Path:string)<transacts>:string =
        if (Found := GetNode[Path], Entity := base_entity[Found]):
            Entity.Describe()
        else:
            "no"

    # The same cast on a node handed over as an argument rather than looked up, which is the other
    # way a script meets a Godot object -- a signal payload takes this path.
    DescribeGiven<public>(Target:node2d)<transacts>:string =
        if (Entity := base_entity[Target]):
            Entity.Describe()
        else:
            "no"
