using { /Godot.org/Godot }

Main(Args:[]string, Env:[string]string)<suspends>:void=
    Print("Hello from Verse, running inside a DLL.")
    if (Handle := VhGetNode["/root/Main/Mover"]):
        Print("found a node")
