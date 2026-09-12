using { /Godot.org/Godot }

# R-LANG-1, R-LANG-2, R-LANG-3, all in one script because they share a subject: the Verse language
# itself, used the way a project would use it, rather than the bridge's marshalling.
#
# The interface, the struct, the enum and the parametric class are declared here rather than in their
# own files for the reason a script's class is named after its file: one flat scope and no shadowing,
# so every top-level name in the project has to be unique, and a file is only obliged to carry the
# class Godot attaches. What else it declares beside that is its own business.

# R-LANG-1: an interface, and a class that implements it.
named := interface:
    Name<public>()<transacts>:string

# R-LANG-2: a struct in user code, with a nested one, which is the shape the mirror's math types
# have and the shape a `var` cannot hold.
stats := struct:
    Attack<public>:int = 0
    Defence<public>:int = 0

loadout := struct:
    Base<public>:stats = stats{}
    Bonus<public>:stats = stats{}

# R-LANG-2: an enum in user code. Distinct from Godot's enums, which are generated -- this is one a
# project declares for itself, and R-EXP-1 exports it as a dropdown.
stance := enum:
    Defensive
    Neutral
    Aggressive

# R-LANG-3: a parametric class, and a generic function over it. Neither has anything to do with
# Godot; what is being asked is whether Verse's own generics survive being compiled by the host.
box(t:type) := class:
    Value<public>:t

Unbox<public>(t:type, B:box(t))<transacts>:t = B.Value

@global_class
derived_entity := class(base_entity, named):

    @export
    var Stance<public>:stance = stance.Neutral

    # R-LANG-1: overriding a method declared by another *script's* class.
    Describe<override>()<transacts>:string = "derived"

    # R-LANG-1: implementing an interface method.
    Name<override>()<transacts>:string = "derived_entity"

    # R-LANG-1: the base's own code, reached through the derived instance and reading the base's
    # exported member.
    BaseDoubled<public>()<transacts>:int = DoubledHealth()

    # R-LANG-2: a struct built, nested, and read back.
    TotalAttack<public>(A:int, B:int)<transacts>:int =
        Made := loadout{Base := stats{Attack := A}, Bonus := stats{Attack := B}}
        Made.Base.Attack + Made.Bonus.Attack

    StanceName<public>()<transacts>:string =
        case (Stance):
            stance.Defensive => "defensive"
            stance.Neutral => "neutral"
            stance.Aggressive => "aggressive"

    # R-LANG-3: the parametric class instantiated at two different types in one script, and read
    # back through the generic function.
    BoxedInt<public>(Value:int)<transacts>:int = Unbox(int, box(int){Value := Value})

    BoxedString<public>(Value:string)<transacts>:string = Unbox(string, box(string){Value := Value})

    # R-LANG-1 again, through the interface rather than through the class: the value is typed as the
    # interface, so the call dispatches dynamically.
    NameThroughInterface<public>()<transacts>:string =
        AsNamed:named = Self
        AsNamed.Name()
