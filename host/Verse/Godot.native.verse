using {/Verse.org/Native}

# The Phase 2 digest of Godot's API: free functions over instance-id handles.
# Real class hierarchies come later; see the plan.

Print<public><native>(Message:string):void

GetNode<public><native>(Path:string)<decides>:int
IsValid<public><native>(Handle:int):logic
GetChildren<public><native>(Handle:int):[]int

GetFloat<public><native>(Handle:int, Property:string)<decides>:float
SetFloat<public><native>(Handle:int, Property:string, Value:float):void
GetInt<public><native>(Handle:int, Property:string)<decides>:int
SetInt<public><native>(Handle:int, Property:string, Value:int):void
GetLogic<public><native>(Handle:int, Property:string)<decides>:logic
SetLogic<public><native>(Handle:int, Property:string, Value:logic):void
GetText<public><native>(Handle:int, Property:string)<decides>:string
SetText<public><native>(Handle:int, Property:string, Value:string):void

GetVector2<public><native>(Handle:int, Property:string)<decides>:tuple(float, float)
SetVector2<public><native>(Handle:int, Property:string, X:float, Y:float):void

CallMethod<public><native>(Handle:int, Method:string, Args:[]float):void
GetMeta<public><native>(Handle:int):[string]string
FindChar<public><native>(Value:string, Index:int)<decides>:char
