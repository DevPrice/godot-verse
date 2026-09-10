using {/Verse.org/Native}

# Free functions over instance-id handles. These are the whole native surface: the generated
# class API in GodotApi.verse is ordinary Verse that bottoms out here.

Print<public><native>(Message:string)<transacts>:void

VhGetNode<public><native>(Path:string)<decides><transacts>:int
VhIsValid<public><native>(Handle:int)<transacts>:logic
VhGetChildren<public><native>(Handle:int)<transacts>:[]int

VhGetFloat<public><native>(Handle:int, Property:string)<decides><transacts>:float
VhSetFloat<public><native>(Handle:int, Property:string, Value:float)<transacts>:void
VhGetInt<public><native>(Handle:int, Property:string)<decides><transacts>:int
VhSetInt<public><native>(Handle:int, Property:string, Value:int)<transacts>:void
VhGetLogic<public><native>(Handle:int, Property:string)<decides><transacts>:logic
VhSetLogic<public><native>(Handle:int, Property:string, Value:logic)<transacts>:void
VhGetText<public><native>(Handle:int, Property:string)<decides><transacts>:string
VhSetText<public><native>(Handle:int, Property:string, Value:string)<transacts>:void

VhGetVector2<public><native>(Handle:int, Property:string)<decides><transacts>:tuple(float, float)
VhSetVector2<public><native>(Handle:int, Property:string, X:float, Y:float)<transacts>:void

VhCallMethod<public><native>(Handle:int, Method:string, Args:[]float)<transacts>:void
VhGetMeta<public><native>(Handle:int)<transacts>:[string]string
VhFindChar<public><native>(Value:string, Index:int)<decides><transacts>:char

# One Godot Variant on the wire, as (Tag, Ints, Floats, Texts). Tag is Godot's own Variant::Type
# and decides which payload slots are read: an int in Ints(0), a Vector2 as two Floats, an object
# as its instance id in Ints(0), a PackedStringArray as the whole of Texts. Keeping the tag means
# a two-float payload is a Vector2 rather than guessed at, which is what the untagged Phase 2
# encoding could not express.
variant<public> := tuple(int, []int, []float, []string)

# Reads and calls happen now; they observe a Godot the transaction cannot roll back.
VhCallValue<public><native>(Handle:int, Method:string, Args:[]variant)<decides><transacts>:variant
VhGetValue<public><native>(Handle:int, Property:string)<decides><transacts>:variant

# Mutations defer to transaction commit, so a failed expression leaves the scene untouched.
VhCallVoid<public><native>(Handle:int, Method:string, Args:[]variant)<transacts>:void
VhSetValue<public><native>(Handle:int, Property:string, Value:variant)<transacts>:void

# ClassDB::instantiate. The caller owns the result until it is parented or freed.
VhInstantiate<public><native>(ClassName:string)<decides><transacts>:int
VhSingleton<public><native>(Name:string)<decides><transacts>:int

# The one native class in the API. Everything in GodotApi.native.verse derives from it as
# ordinary Verse, which is what keeps a thousand mirrored classes from needing a thousand C++
# shadows: the VM dispatches through the object's own shape, not through anything native.
#
# It is native because a script's class has to reach C++ somehow -- the host instantiates a user
# subclass and calls into it through a UObject, and only a native ancestor gives the instance a
# UObject representation to be reached by.
object<public><native> := class:
    # Godot instance id. The host writes it at construction; it never changes afterwards, and it
    # keeps its value after Godot frees the object, which is why every accessor is <decides>.
    Handle<public><native>:int = 0

    # Godot's _ready, _process and _physics_process. Defined here so a script overrides rather
    # than declares them, and so a script that wants only one of the three still compiles.
    Ready<public>():void = {}
    Update<public>(Delta:float):void = {}
    PhysicsUpdate<public>(Delta:float):void = {}
