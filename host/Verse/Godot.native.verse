using {/Verse.org/Native}

# The Phase 2 digest of Godot's API: free functions over instance-id handles.
# Real class hierarchies come later; see the plan.

Print<public><native>(Message:string)<transacts>:void

GetNode<public><native>(Path:string)<decides><transacts>:int
IsValid<public><native>(Handle:int)<transacts>:logic
GetChildren<public><native>(Handle:int)<transacts>:[]int

GetFloat<public><native>(Handle:int, Property:string)<decides><transacts>:float
SetFloat<public><native>(Handle:int, Property:string, Value:float)<transacts>:void
GetInt<public><native>(Handle:int, Property:string)<decides><transacts>:int
SetInt<public><native>(Handle:int, Property:string, Value:int)<transacts>:void
GetLogic<public><native>(Handle:int, Property:string)<decides><transacts>:logic
SetLogic<public><native>(Handle:int, Property:string, Value:logic)<transacts>:void
GetText<public><native>(Handle:int, Property:string)<decides><transacts>:string
SetText<public><native>(Handle:int, Property:string, Value:string)<transacts>:void

GetVector2<public><native>(Handle:int, Property:string)<decides><transacts>:tuple(float, float)
SetVector2<public><native>(Handle:int, Property:string, X:float, Y:float)<transacts>:void

CallMethod<public><native>(Handle:int, Method:string, Args:[]float)<transacts>:void
GetMeta<public><native>(Handle:int)<transacts>:[string]string
FindChar<public><native>(Value:string, Index:int)<decides><transacts>:char
