using { /Godot.org/Godot }

# The editor layer's placeholder-and-save fixture (docs/editor-test-audit.md step 3): an `int` export
# and a node export, which are the two B26 lost -- the int while a compile failed, the node on every
# save -- and the one B39 wrote as `= null` in a scene that instanced this one. Nothing in the
# integration layer uses it; the editor layer breaks, fixes and restores it in a throwaway copy.
#
# `Answer` is the line the editor layer rewrites: its body is what "the node runs the new code" is
# read off once the game is played, and deleting a paren from it is how the file is broken.
save_probe := class(node2d):

	@export
	var Count<public>:int = 7

	@export
	var Buddy<public>:?node2d = false

	Answer<public>()<transacts>:int = 1

	_Ready<override>():void =
		Print("save_probe ready: answer {Answer()}, count {Count}")

	# B8's observable, and the only safe one: a placeholder ignores a notification where a real
	# instance runs this, and calling a method on a placeholder is an error rather than an answer.
	_Notification<override>(What:int):void =
		if (What = 9001):
			set Count = What
