using { /Godot.org/Godot }

# A statics module's constants as the cook records them. host_smoke never loads this;
# tools/run_tests.py cooks it and reads verse_classes.json, where each constant's value carried an
# uninitialised variant tag (T5.7c), which a runtime host reads back and hands to the consumer.

@statics("statics_values")
StaticsValuesStatics<public> := module:
	MaxSpeed<public>:float = 400.0
	Label<public>:string = "probe"
	Lives<public>:int = 3

statics_values := class(object):

	ReadLives<public>()<transacts>:int = StaticsValuesStatics.Lives
