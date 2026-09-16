using { /Godot.org/Godot }

# R-NODE-10's remaining four hooks, asked of the compiler rather than of SemanticAnalyzer.cpp:
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/hooks_probe.verse --class hooks_probe
#
# The question each answers is "does the declaration on the native root admit the override an
# author would write", and none of the four was obvious from reading the root:
#
#   - `_Get` answers `variant`, which a script can now name in a signature. `variant{}` is the whole
#     of "not mine"; `VariantFrom<GodotType>` is the rest.
#   - `_Set` takes one and reads it with the failable `As<GodotType>` family, so its body is a
#     failure context -- which is why an `if` is the spelling and a cast is not.
#   - `_GetPropertyList` answers a `godot_array` the override has to *build*: `MakeArray` plus
#     `MakeDictionary`, and Godot's own property keys as strings.
#   - `_ValidateProperty` mutates its argument, which is a reference, so there is nothing to return.
#
# Four things this file measured that reading the root would not have told anyone:
#
#   1. **The base class is load-bearing.** Written `hooks_probe := class:`, all four are glitch 3523
#      -- "could not find a parent function to override" -- because a plain Verse class does not
#      derive from `object` and so has none of these. The message blames the access specifiers.
#   2. **`TagInt` and its 38 siblings are not a script's to write.** They carry no access specifier,
#      so `Entry.SetInt("type", TagInt)` is glitch 3593 talking about control scopes. What *is*
#      public is the generated `ToInt(:variant_type)`, which is the spelling below -- and it is the
#      closer analogue of what a GDScript author writes, which is `TYPE_INT`.
#   3. **The hooks carry `no_rollback`, so narrowed code may not call them.** A `<transacts>` helper
#      calling its own `_Get` is glitch 3512 at the call. That is the specifier-less declaration
#      working as intended -- Godot is the caller, not the script -- and it is why the three probe
#      helpers below have no specifier either.
#   4. **An override may call a specifier-less helper.** `Bump` has no specifier and `_Set` calls it.
#      Under a `<transacts>` declaration on the root that would have been 3512 at the call site,
#      one file at a time, which is the cascade `docs/dodge-the-creeps.md` wall 8 is about.

hooks_probe := class(object):

    var Extra:int = 0

    var Taken:string = ""

    _Get<override>(Property:string):variant =
        if (Property = "Extra"):
            VariantFromInt(Extra)
        else if (Property = "Label"):
            VariantFromString("from _Get")
        else:
            variant{}

    _Set<override>(Property:string, Value:variant):logic =
        if (Property = "Extra", Number := Value.AsInt[]):
            set Extra = Number
            Bump()
            true
        else if (Property = "Taken", Text := Value.AsString[]):
            set Taken = Text
            true
        else:
            false

    _GetPropertyList<override>():godot_array =
        List := MakeArray()
        Entry := MakeDictionary()
        Entry.SetString("name", "Extra")
        Entry.SetInt("type", ToInt(variant_type.TypeInt))
        Entry.SetInt("usage", 6)
        List.AddDictionary(Entry)
        List

    _ValidateProperty<override>(Property:dictionary):void =
        if (Name := Property.GetString["name"], Name = "Extra"):
            Property.SetInt("usage", 0)

    # Specifier-less, and called from _Set. Finding 4.
    Bump():void =
        set Extra = Extra + 0

    # What the probe can call: zero-argument, so the answers are readable without a Godot. No
    # specifier, for finding 3.
    AskGet<public>():int =
        V := _Get("Extra")
        if (N := V.AsInt[]) then N else -1

    AskMissing<public>():int =
        V := _Get("NoSuchThing")
        case (VariantKind(V)):
            variant_type.TypeNil => 0
            _ => 1

    ListLength<public>():int = _GetPropertyList().Length()
