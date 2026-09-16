using { /Godot.org/Godot }

# R-NODE-10's four property hooks, as a script Godot attaches to a node and asks.
#
# `_Get`, `_Set`, `_GetPropertyList` and `_ValidateProperty` are the half of R-NODE-10 that lets a
# class serve a property nothing declares. All four are hand-written on the native root with empty
# bodies -- none of them is in extension_api.json, because Godot offers them to *scripts* rather
# than registering them in ClassDB -- so an `<override>` is the spelling, and a script that writes
# none of them is never asked about any of them.
#
# The one design question inside the stage is how they compose with `@export`, and the answer is
# Godot's own: `_Get` and `_Set` are consulted only for a name the property list did not already
# carry. `Declared` below is what asserts it -- an export and a hook that both claim the same name,
# where the export must win and the hook must never see it.
#
# Two names this class deliberately does not use: `Hidden`, because `canvas_item` mirrors Godot's
# `hidden` signal as a member of that name and a script may not shadow one; and `Property` for the
# helper, because the three hooks each have a parameter of that name.
@global_class
hooks := class(node2d):

    # The export half of the composition case. `_Get` and `_Set` below both claim to serve
    # "Declared" as well, and neither is ever asked for it.
    @export
    var Declared<public>:int = 11

    # What the hooks actually serve. Ordinary members with no `@export`, so Godot's own property
    # lookup misses them and the hooks are what make them reachable at all.
    var Virtual<public>:int = 7
    var Label<public>:string = "from _Get"

    # How many times `_Set` was asked, which is what proves an `@export`ed member never reaches it.
    var SetCalls<public>:int = 0

    # How many times `_ValidateProperty` matched, so a case can tell "it ran and changed nothing"
    # from "it never ran at all".
    var Validated<public>:int = 0

    _Get<override>(Property:string):variant =
        if (Property = "Virtual"):
            VariantFromInt(Virtual)
        else if (Property = "Label"):
            VariantFromString(Label)
        else if (Property = "Declared"):
            # Never reached: the export answers first. A wrong number rather than the member's own,
            # so a case that did reach it says so instead of agreeing by accident.
            VariantFromInt(-999)
        else:
            # Godot's spelling of "not mine", and Verse's: a variant holding nothing.
            variant{}

    _Set<override>(Property:string, Value:variant):logic =
        set SetCalls = SetCalls + 1
        if (Property = "Virtual", Number := AsInt[Value]):
            set Virtual = Number
            true
        else if (Property = "Label", Text := AsString[Value]):
            set Label = Text
            true
        else:
            # Anything else -- including "Declared", which never arrives here at all.
            false

    # The two properties this object serves beyond its exports, as Godot's own property
    # dictionaries. `ToInt` is how a script spells a Variant::Type: the `Tag...` constants are
    # module-scoped to the mirror, so writing one is glitch 3593 rather than a name to reach for.
    _GetPropertyList<override>():godot_array =
        List := MakeArray()
        List.AddDictionary(MakeProperty("Virtual", variant_type.TypeInt))
        List.AddDictionary(MakeProperty("Label", variant_type.TypeString))
        List

    # Godot asks this of every ClassDB property of the object, one at a time, and the dictionary is
    # a *reference* -- so there is nothing to return, and what this writes is what Godot reads back.
    _ValidateProperty<override>(Property:dictionary):void =
        if (Name := Property.GetString["name"], Name = "rotation"):
            set Validated = Validated + 1
            Property.SetInt("usage", 0)

    MakeProperty(Name:string, Kind:variant_type)<transacts>:dictionary =
        Entry := MakeDictionary()
        Entry.SetString("name", Name)
        Entry.SetInt("type", ToInt(Kind))
        # STORAGE | EDITOR, which is what Godot's PROPERTY_USAGE_DEFAULT is.
        Entry.SetInt("usage", 6)
        Entry

    # Readers, so a case can check what a hook wrote without going back through the hook.
    ReadVirtual<public>()<transacts>:int = Virtual
    ReadLabel<public>()<transacts>:string = Label
    ReadDeclared<public>()<transacts>:int = Declared
    ReadSetCalls<public>()<transacts>:int = SetCalls
    ReadValidated<public>()<transacts>:int = Validated
