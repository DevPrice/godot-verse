using { /Godot.org/Godot }
using { /Godot.org/Bindings }

# verse_host_unit's fixture: one data member per kind ClassifyDeclaredType answers, named for what
# HostUnitMain.cpp expects of it. Nothing here is instantiated, which is why most members carry no
# initializer -- an archetype would have to supply them, and none is ever written.

unit_mode := enum:
	Idle
	Busy
	Done

unit_report := struct:
	Count<public>:int = 0
	Label<public>:string = ""

unit_helper := class(node2d):
	Tag:int = 0

unit_plain := class:
	Value:int = 0

unit_probe := class(node2d):
	# How many there are.
	AnInt:int
	ABounded:type{_X:int where 0 <= _X, _X <= 10}
	ALogic:logic
	var AFloat:float = 0.0
	AChar:char
	AString:string
	AnEnum:unit_mode
	AnArray:[]int
	AStringArray:[]string
	AMap:[int]string
	ATuple:tuple(int, float)
	AnOption:?int
	AnEnumOption:?unit_mode
	ANested:??int
	AVariant:variant
	ARid:rid
	AVector:vector2
	AColor:color
	ARect:rect2
	AReport:unit_report
	ANode:node2d
	AMaybeNode:?node2d
	AHelper:?unit_helper
	ABinding:?unit_binding
	AGodotArray:godot_array
	ADictionary:dictionary
	ACallable:callable
	ASignalRef:signal_ref
	ANodeArray:typed_array(node)
	APlain:unit_plain
	AnEvent:event(int)

	@export_signal
	Pinged<public>:event(int) = event(int){}

	Take<public>(Count:int, Where:vector2, Who:?node2d):string = ""

(Probe:unit_probe).ToString()<transacts>:string = "unit_probe"

# One constant that crosses and one that has no lane, for what GetClassStatics says about each.
@statics("unit_probe")
unit_probe_statics<public> := module:
	Limit<public>:int = 7
	Names<public>:[int]string = map{1 => "one"}
