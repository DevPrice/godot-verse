using { /Godot.org/Godot }

# spec/modules.md §5: module-scope data is a constant computed once; what it may be.

ModulesFirstData:int = 41
ModulesSecondData:int = ModulesFirstData + 1

modules_computes_holder := class<computes>:
	Held:int = 0
	Label:string = "held"

ModulesGlobalHolder:modules_computes_holder = modules_computes_holder{Held := 9}

modules_basic := class(object):

	ReadModuleData<public>():void =
		Print("FirstData={ModulesFirstData} SecondData={ModulesSecondData}")

	ReadComputesObjectData<public>():void =
		Print("Held={ModulesGlobalHolder.Held} Label={ModulesGlobalHolder.Label}")

	ModuleDataIsStableAcrossReads<public>():void =
		A := ModulesSecondData
		B := ModulesSecondData
		Print("stable: {if (A = B) then "yes" else "no"}")
