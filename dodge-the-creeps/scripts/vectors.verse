using { /Godot.org/Godot }

# A library file: it declares no class named after itself, so Godot refuses to attach it to a node
# and every sibling script sees these names with nothing written to import them (R-LANG-6's third
# clause). It exists because the port needs arithmetic that Godot's own Vector2 already has.
#
# Godot's math types are data in Verse -- sixteen structs with fields and no methods -- so
# `velocity.normalized() * speed` has no spelling yet, and neither does `PI`. Both are waiting on
# the same answer: a free function and a value-type method each have no object handle to ride a
# call on, which is OQ-11, and R-SCN-3 behind it. Until that lands a port writes this file, which
# is the single most-felt absence in the whole attempt -- see docs/dodge-the-creeps.md.
#
# Every function here says `<transacts>`, and that is load-bearing rather than decorative. A
# module-scope function with no effect specifier is `no_rollback`, and a `no_rollback` function
# cannot be called from inside a transaction -- which is where every Godot callback runs, because
# the host opens one around Ready, Process and every signal handler. So a helper that forgets the
# specifier compiles on its own and fails at its first call site, with a message about an effect the
# author never wrote down.
Pi<public>:float = 3.141592653589793

# The two fractions of PI the mob spawner needs, as constants rather than as `Pi / 2.0`: float
# division is failable, and a spawn is not the place to be handling that.
HalfPi<public>:float = 1.5707963267948966
QuarterPi<public>:float = 0.7853981633974483

V2Add<public>(A:vector2, B:vector2)<transacts>:vector2 = vector2{X := A.X + B.X, Y := A.Y + B.Y}

V2Scale<public>(A:vector2, Scalar:float)<transacts>:vector2 = vector2{X := A.X * Scalar, Y := A.Y * Scalar}

V2Length<public>(A:vector2)<transacts>:float = Sqrt(A.X * A.X + A.Y * A.Y)

# Godot's `Vector2.normalized()`, which returns the zero vector for a zero-length input. This fails
# instead, because that is what the caller is asking anyway: the branch that normalizes is the one
# that has already established the player is moving.
V2Normalized<public>(A:vector2)<decides><transacts>:vector2 =
	Length := V2Length(A)
	Length > 0.0
	vector2{X := A.X / Length, Y := A.Y / Length}

V2Clamp<public>(A:vector2, Low:vector2, High:vector2)<transacts>:vector2 =
	vector2{X := Clamp(A.X, Low.X, High.X), Y := Clamp(A.Y, Low.Y, High.Y)}

V2Rotated<public>(A:vector2, Angle:float)<transacts>:vector2 =
	C := Cos(Angle)
	S := Sin(Angle)
	vector2{X := A.X * C - A.Y * S, Y := A.X * S + A.Y * C}
