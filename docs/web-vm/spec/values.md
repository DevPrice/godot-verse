# Values

Status: reviewed by the lead 2026-09-24; `ops.md` §15.1 overrides this file where they disagree. Room: dirty. Sources read: `docs/phase-7.5-design.md`,
`docs/web-vm/format.md`, `docs/web-vm/ops.json`, `docs/web-vm/facts.md`; VerseVM runtime headers and
sources under `Engine/Source/Runtime/CoreUObject/{Public,Private}/VerseVM` (value encoding, float,
int, heap int, rational, array, mutable array, map, option, false/true, value object, native struct,
union variant, int/float/tuple types, intrinsics, float printing, value printing, native
converter, runtime-error table); `Engine/Source/Runtime/CorePreciseFP` (float); the bytecode
generator `Engine/Plugins/VerseVM/Source/VerseVMCodeGen/Private/VVMCodeGenerator.cpp` (which ops a
construct compiles to); `Engine/Source/Runtime/VerseCompiler/.../Desugarer.cpp` (interpolation) and
`SemanticProgram.cpp` (the built-in operator signatures); the Verse library files
`Math.native.verse`, `float_util.native.verse`, `int_util.native.verse`, `String.native.verse`,
`Array.native.verse` and their C++ (`Verse.cpp`, `VerseMath.cpp`, `VerseStringLib.cpp`).
Probes: `tests/verse_probe/vm_values_probe.verse`, `vm_values_reject.verse`,
`vm_values_false_probe.verse`, `vm_values_native_struct_probe.verse`, and T1.2's
`vm_facts_probe.verse` (readings in `docs/web-vm/facts.md`).

Every claim below is either measured — the fixture and method are named beside it, as
`probe:Method` for `vm_values_probe.verse`, `reject:Method` for `vm_values_reject.verse`,
`facts §n` for `docs/web-vm/facts.md` — or marked **(source, unprobed)**. Where a measurement and
the source disagree, the measurement is what this file states.

This file owns what a value *is* and what the value-level operations answer. Who unifies a result
into a register and what happens when an operand is an unbound placeholder is `unification.md`'s;
what failing and raising do to a frame, a transaction and the log is `failure.md`'s; the exact
operand layout of each op is `ops.md`'s; the natives named here (`ToString`, `Floor`, `Mod`, ...) are
`natives.md`'s, which this file cross-references for their value behaviour rather than restating.

## 1. Value kinds

| Kind | Script type | Held as | Notes |
| --- | --- | --- | --- |
| integer | `int` (and its subranges) | an immediate small integer, or a `heap int` cell | arbitrary precision (§2) |
| rational | `rational` | a `rational` cell | only from dividing two ints (§2.4) |
| float | `float` | an immediate IEEE 754 binary64 | Verse-specific equality and ordering (§3) |
| char | `char` | an immediate, one UTF-8 code unit 0–255 | §4 |
| char32 | `char32` | an immediate, one code point | §4 |
| logic | `logic` | the `false` cell or the `true` cell | `true` is itself an option (§9) |
| option | `?t` | an `option` cell, or the `false` cell for the empty option | §9 |
| array | `[]t`, `string` | an `array` cell, or a `mutable array` cell while held by a `var` | a `string` is `[]char` (§5, §6) |
| tuple | `tuple(...)` | an `array` cell | there is no separate tuple kind (§10) |
| map | `[k]v` | a `map` cell, or a `mutable map` cell while held by a `var` | ordered (§8) |
| struct instance | a `struct` | a `value object` whose class is a struct | compared field by field (§11) |
| class instance | a `class` | a native object (every class in this project derives from the native root) or a `value object` | `objects.md` |
| enumerator | an `enum` | an `enumerator` cell | compared by identity |
| function | a function type | a `function` / `native procedure` cell | not comparable |
| type | `type`, `subtype(t)`, ... | an `int type`, `float type`, `tuple type`, `map type`, `simple type`, `class`, `false`, `true` or `option` cell | §13 |

Representation is free wherever it is not observable. The `.vbc` container fixes how a *constant*
is written (`format.md` §3–§4); what the interpreter does with it in memory is its own choice, and
the design's tagged 64-bit word (`phase-7.5-design.md` §7.3) is compatible with everything here. In
particular: whether an integer is immediate or a `heap int` cell, and which element storage an
array uses (`format.md`'s element kinds 0–4), are **never observable** to a program (§2.1, §6.1).

## 2. Integers

### 2.1 Range and representation

An integer is an exact mathematical integer with no upper or lower bound. Arithmetic never wraps and
never saturates: `9223372036854775807 + 1` is greater than `9223372036854775807` and not less than 0
(`facts §4`); `4294967296 * 4294967296` compares greater than 2^63−1 and divides back exactly to
2^32; −(−2^63) exceeds 2^63−1; `(2^63−1)+1−1` prints `9223372036854775807` (`probe:IntHeapRoundTrip`).

Whether a value is held immediately or in a `heap int` cell is invisible: equality, ordering,
hashing, map-key identity and printing depend only on the mathematical value. A heap int used as a
map key is found again by an equal value computed separately (`probe:MapKeys`, "heap int key").
An implementation may choose any threshold between its immediate and boxed forms; `.vbc` writes
integers that fit 32 bits signed as immediates and all others as `heap int` cells (`format.md` §3).

**Literals are narrower than values.** An integer literal must lie in
[−9223372036854775808, 9223372036854775807]; outside it the compiler refuses the file with *error
3555: Integer literal must be in the range -9223372036854775808 to 9223372036854775807.* (`facts §4`).
`-9223372036854775808` itself is accepted and prints as itself (`probe:AcceptedIntLiteralMin`).

### 2.2 `Add`, `Sub`, `Mul`, `Neg` on integers

Exact integer sum, difference, product and negation. No failure, no error, for any magnitude.

Mixed operands: `Mul` alone accepts an integer with a float, in either order, and answers a float
(§2.7). `Add`, `Sub` and `Div` with one int and one float are refused at compile time with *error
3509: No overload of the function `operator'+'` matches the provided arguments
(:type{1},:type{1.000000})* (and the `-`, `/` equivalents) (`reject:MixedAdd`, `reject:MixedSub`,
`reject:MixedDiv`), so the interpreter never sees them.

### 2.3 `Div` on integers

`Div` of two integers produces a **rational** (§2.4), never an integer and never a float — even when
the division is exact (`6 / 3` is the rational 2/1). A zero divisor makes `Div` **fail** (the
failure of `failure.md`, not a runtime error): `7 / 0` and `0 / 0` both fail (`probe:IntRational`).
Integer division is `<decides>` in the language for exactly this reason.

Script code turns the rational into an integer with `Floor` or `Ceil` (§2.4).

### 2.4 Rationals

A rational is created only by `Div` (§2.3) or by `Div`/`Add`/`Sub`/`Mul`/`Neg` of an existing
rational. It is always held **in lowest terms with a positive denominator**: the sign lives on the
numerator, and numerator and denominator are divided by their greatest common divisor. So `−1 / −2`
equals `1 / 2`, and `14 / 4` equals `7 / 2` (`probe:AcceptedRationalEquality`).

What a script may do with one, measured against the compiler:

| Operation | Allowed? | Answer | Evidence |
| --- | --- | --- | --- |
| `Floor(R)` | yes | the greatest integer ≤ R | `probe:IntRational` |
| `Ceil(R)` | yes | the least integer ≥ R | `probe:IntRational` |
| `R = S`, `R <> S` (rational with rational) | yes | equal iff same lowest terms | `probe:AcceptedRationalEquality` |
| `R = N` (rational with int) | yes | equal iff the denominator is 1 and the numerator equals N | `probe:AcceptedRationalEquality` |
| `R < S`, `R > S`, ... | **no** — *error 3509: No overload of the function `operator'>'` matches the provided arguments (:rational,:rational)* | — | `reject:RationalOrder` |
| `R + 1`, any arithmetic | **no** — error 3509 | — | `reject:RationalAdd` |
| `ToString(R)`, `"{R}"` | **no** — *error 3509: No overload of the function `ToString` matches the provided arguments (:rational)* | — | `reject:RationalPrint` |

`Floor` and `Ceil` also accept a plain integer and return it unchanged: `Floor(5) = 5`,
`Ceil(-5) = -5` (`probe:IntRational`).

| R | `Floor(R)` | `Ceil(R)` |
| --- | --- | --- |
| 7 / 2 | 3 | 4 |
| −7 / 2 | −4 | −3 |
| 7 / −2 | −4 | −3 |
| −7 / −2 | 3 | 4 |
| 6 / 3 | 2 | 2 |
| 0 / 5 | 0 | 0 |
| 2^63 / 3 | 3074457345618258602 | — |

(`probe:IntRational`). Neither `Floor` nor `Ceil` on a rational has a range limit; the result is an
integer of any size.

At the op level (none of this is reachable from a script, because the compiler refuses the
spellings above, but the ops define it): `Add`, `Sub`, `Mul`, `Div` accept a rational on either side
with a rational or an integer on the other, the integer standing for n/1, and answer a rational in
lowest terms; `Div` by a zero rational fails; `Neg` negates the numerator; `Lt`, `Lte`, `Gt`, `Gte`
and their `FastFail` forms order two rationals, or a rational and an integer, exactly. A rational
beside a float in any of these ops is outside the contract (§15) **(source, unprobed)**.

### 2.5 `Mod`, and the library's `Mod[]` and `Quotient[]`

The `Mod` op is never emitted at this commit (`ops.json`: `emitted: false`); an implementation need
not support it. The script functions `Mod[X, Y]` and `Quotient[X, Y]` are natives
(`natives.md`), and their measured behaviour is **Euclidean** division — the remainder is always in
[0, |Y|) — not flooring and not truncating (`facts §2`). Both fail when Y = 0. Both take their
arguments through the 64-bit conversion of §2.6, so an argument outside the int64 range raises that
section's error (`probe:IntModHeap`), and the one int64 quotient that overflows raises a different
one:

| Call | Result | Evidence |
| --- | --- | --- |
| `Mod[2^63, 7]` | runtime error `ErrRuntime_GeneratedNativeInternal`, message `Value exceeds the range of a 64 bit integer.` | `probe:IntModHeap` |
| `Mod[−2^63, −1]` (and `Quotient[−2^63, −1]`, source) | runtime error `ErrRuntime_IntegerOverflow`, description `Integer overflow encountered.`, message `Integer overflow encountered.` | `probe:IntModMinNegOne` |

### 2.6 Where an integer meets 64 bits

The value range is unbounded, but every native that takes an `int` receives a signed 64-bit integer.
Passing a value outside [−2^63, 2^63−1] to any such native raises a runtime error — it does not fail,
wrap or clamp:

- diagnostic: `ErrRuntime_GeneratedNativeInternal`
- description: `An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available.`
- message: `Value exceeds the range of a 64 bit integer.`
- the callstack's first frame is the native that was called.

The line the reference prints is

    ErrRuntime_GeneratedNativeInternal: An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available. (Value exceeds the range of a 64 bit integer.)

with the native named in the callstack beneath it (`facts §4`, `probe:IntNegMinToString`,
`probe:IntBelowMinToString`, `probe:IntModHeap`). How that line and the callstack are assembled and
reported is `failure.md`'s; the three strings above are the values this section fixes.

`ToString(:int)` is such a native, and string interpolation of an int calls it (§5.4), so **printing
an integer outside the int64 range raises this error**: `ToString(2^63)` and `"{-(−2^63)}"` and
`"{−2^63 − 1}"` all raise it. The boundary values themselves print: `−9223372036854775808` and
`9223372036854775807` (`probe:IntHeapRoundTrip`, `facts §4`).

**`ToString(:int)` output** for a value in range: an optional `-`, then the decimal digits with no
leading zeros (`0` for zero), nothing else — no `+`, no grouping (`facts §4`).

### 2.7 Integer to float

The only int-to-float conversion a script has is multiplying by a float (`X * 1.0`), which `Mul`
performs by converting the integer to the nearest binary64 value — **round to nearest, ties to
even** — and then multiplying (§3.1). An integer whose magnitude is too large for binary64 converts
to ±Inf:

| Expression | Prints | Evidence |
| --- | --- | --- |
| `3 * 1.5`, `1.5 * 3` | `4.500000` | `probe:IntTimesFloat` |
| `9007199254740993 * 1.0` (2^53+1) | `9007199254740992.000000` | `probe:IntTimesFloat` |
| `2^63 * 1.0` | `9223372036854775808.000000` | `probe:IntTimesFloat` |
| `2^1071 * 1.0` | `Inf` | `probe:IntTimesFloat` |
| `0 * -1.0` | `0.000000` | `probe:IntTimesFloat` |

Whether an integer just below 2^1024 that rounds up to 2^1024 gives `Inf` is §16 Q3.

## 3. Floats

### 3.1 Arithmetic

A float is an IEEE 754 binary64 value. `Add`, `Sub`, `Mul` and `Div` on two floats are the IEEE
operations in round-to-nearest-even, with one exception: **a divisor of −0 is treated as +0**, so
`1.0 / −0.0` is `Inf`, `−1.0 / −0.0` is `−Inf` and `0.0 / −0.0` is `NaN` (`probe:FloatSpecials`:
`1/-Zero = Inf`, `-1/-Zero = -Inf`, `1/-0.0 literal = Inf`). `Neg` flips the sign. Float division
never fails: division by zero answers ±Inf or NaN (CLAUDE.md, "Float division is total"). `Inf −
Inf`, `Inf * 0.0` are `NaN` (`probe:FloatSpecials`).

`Abs` of a float is its magnitude; `Abs(−0.0)` prints `0.000000` (`probe:FloatSpecials`; the
intrinsic itself is `natives.md`'s).

### 3.2 Signed zero

A −0 may exist in a register — `Neg` of `0.0` produces one, as can a multiplication — but **no
program can tell it from +0**. It compares equal to +0 and not less than it (`facts §5`), divides as
+0 (§3.1), prints as `0.000000` (`facts §3`), keys the same map entry (§8.5), and every native
receiving a float receives +0 **(source; the printing and division halves are measured)**. An
implementation may therefore canonicalise −0 to +0 whenever a float is produced; nothing a script
can observe changes.

### 3.3 NaN and infinities

There is one observable NaN. Every NaN, whatever its sign or payload, is equal to every other NaN
and to nothing else; NaN is **unordered** against everything including itself for `<` and `>`; for
`<=` and `>=`, NaN against NaN succeeds and NaN against any non-NaN fails (`facts §1`):

| Operator | NaN vs NaN | NaN vs 1.0 | 1.0 vs NaN (source) |
| --- | --- | --- | --- |
| `=` | succeeds | fails | fails |
| `<>` | fails | succeeds | succeeds |
| `<` | fails | fails | fails |
| `>` | fails | fails | fails |
| `<=` | succeeds | fails | fails |
| `>=` | succeeds | fails | fails |

Stated as rules: `a = b` holds iff both are NaN, or neither is and they are IEEE-equal. `a < b` is
IEEE less-than (false whenever a NaN is involved). `a <= b` holds iff both are NaN, or neither is and
`a` is IEEE-less-than-or-equal to `b`. `a > b` is `b < a`; `a >= b` is `b <= a`. `NaN > Inf` fails
(`probe:FloatSpecials`). `Inf = Inf` succeeds; `−Inf < −1e308` succeeds (`probe:FloatSpecials`).

Because NaN = NaN, a struct holding NaN in a field equals another holding NaN there
(`probe:Identity`), and NaN is a usable map key (§8.5).

The literal constants are `Inf` and `NaN` from `/Verse.org/Verse`. A float literal must be below
1.7976931348623158e+308 in magnitude: *error 3554: Float literal must be smaller than
1.7976931348623158e+308.* (`reject:FloatLiteralRange`). A decimal literal is rounded to the nearest
binary64, ties to even: `9007199254740993.0` prints `9007199254740992.000000`; the smallest subnormal
literal `4.9406564584124654e-324` is accepted (`probe:FloatPrintB`).

### 3.4 `ToString(:float)` — the printed form a game sees

`ToString(X)` for a float, and `"{X}"` interpolation of a float (which calls it, §5.4), produce:

1. `NaN` for any NaN; `Inf` for +Inf; `-Inf` for −Inf.
2. Otherwise, with −0 first replaced by +0: the **exact** decimal value of the binary64 number,
   rounded to **six digits after the decimal point**, written as
   - `-` if and only if the value is negative (strictly less than zero) — **including when every
     printed digit is zero**, so −0.0000001 prints `-0.000000`;
   - the integer part in decimal, no leading zeros, `0` if it is zero, no grouping, no exponent —
     for the largest finite value that is 309 digits;
   - `.` (always a full stop, independent of any locale);
   - exactly six fractional digits.
3. Rounding to six places is **round half to even applied to the exact binary value**. A tie is only
   possible when the value is an odd multiple of 2^−7 (1/128); every other value is rounded to the
   nearer of its two six-place neighbours by its exact expansion, so a decimal literal that is not
   exactly representable rounds according to the binary value it became, not the text that was
   written.

This is the output of C's `%f` conversion as performed by a correctly rounding `printf` in the
default rounding mode (both glibc and the Windows UCRT behave so; the reference is the latter). An
implementation may use any routine that reproduces the table below byte for byte; it must not use a
shortest-round-trip formatter here.

| Value | Printed | Evidence |
| --- | --- | --- |
| `1.0` | `1.000000` | `facts §3` |
| `0.1` | `0.100000` | `facts §3` |
| `1.5` | `1.500000` | `facts §3` |
| `0.5` | `0.500000` | `probe:FloatPrintA` |
| `2.5` | `2.500000` | `probe:FloatPrintA` |
| `-0.0` | `0.000000` | `facts §3` |
| `1.0 / 3.0` | `0.333333` | `facts §3` |
| `0.1 + 0.2` | `0.300000` | `probe:FloatPrintA` |
| `1.0 / 128.0` (0.0078125, a tie) | `0.007812` | `probe:FloatPrintA` |
| `3.0 / 128.0` (0.0234375, a tie) | `0.023438` | `probe:FloatPrintA` |
| `5.0 / 128.0` (0.0390625, a tie) | `0.039062` | `probe:FloatPrintA` |
| `0.0000005` (binary value just below 5e−7) | `0.000000` | `probe:FloatPrintA` |
| `0.0000015` (binary value just above 1.5e−6) | `0.000002` | `probe:FloatPrintA` |
| `-0.0000001` | `-0.000000` | `probe:FloatPrintA` |
| `-0.0000004` | `-0.000000` | `probe:FloatPrintA` |
| `123.4567895` | `123.456789` | `probe:FloatPrintA` |
| `999999.9999995` | `999999.999999` | `probe:FloatPrintA` |
| `1.0e15 + 0.3` | `1000000000000000.250000` | `probe:FloatPrintA` |
| `123456789.125` | `123456789.125000` | `facts §3` |
| `1.0e20` | `100000000000000000000.000000` | `facts §3` |
| `1.0e22` | `10000000000000000000000.000000` | `probe:FloatPrintB` |
| `1.0e23` | `99999999999999991611392.000000` | `probe:FloatPrintB` |
| `1.0e300` | `1000000000000000052504760255204420248704468581108159154915854115511802457988908195786371375080447864043704443832883878176942523235360430575644792184786706982848387200926575803737830233794788090059368953234970799945081119038967640880074652742780142494579258788820056842838115669472196386865459400540160.000000` | `probe:FloatPrintB` |
| largest finite (`1.7976931348623157e308`) | `179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000` | `probe:FloatPrintB` |
| its negation | the same with a leading `-` | `probe:FloatPrintB` |
| smallest normal (2^−1022) | `0.000000` | `probe:FloatPrintB` |
| smallest subnormal (2^−1074) | `0.000000` | `probe:FloatPrintB` |
| `Inf`, `-Inf`, `NaN` | `Inf`, `-Inf`, `NaN` | `facts §3` |
| `-NaN` | `NaN` | `probe:FloatSpecials` |

There is no `X.ToString()` for a float: *error 3506: Unknown member `ToString` in
`type{1.500000}`* (`reject:FloatMethodToString`; `facts §3`).

### 3.5 The shortest round-trip form

The reference has a second float formatter — the shortest decimal that reads back to the same
binary64, with `.0` appended when the result would otherwise look like an integer, and exponent
notation when that is shorter. It is used only where the VM renders a value for a human: the
debugger's rendering of values that cannot cross typed, internal diagnostics, and the printed form
of a `float type` (§13). **A Godot game running on the interpreter cannot observe it**: script
printing goes through §3.4, and the step debugger is out of scope in an exported game by decision
(`CLAUDE.md`, "Out by decision"). An implementation need not provide it; if its own diagnostics
render floats, any readable form will do **(source, unprobed)**.

### 3.6 Float to integer

The natives `Floor[X]`, `Ceil[X]`, `Round[X]` and `Int[X]` (`natives.md`) round a float to an
integer — toward −∞, toward +∞, to nearest with **ties to even** (`facts §5`), and toward zero
respectively. Each **fails** when X is not finite (`Floor[Inf]`, `Floor[NaN]` fail,
`probe:FloatToIntEdges`). When the rounded value lies outside [−2^63, 2^63), each **raises**:

- diagnostic `ErrRuntime_IntegerBoundsExceeded`, description `A value does not fall inside the representable range of a Verse integer.`
- message `The value <V> cannot be converted to an integer because it does not fall inside the representable range of a Verse integer.`, where `<V>` is the **rounded** value printed by the §3.4 rule — `Floor[1.0e19]` gives `The value 10000000000000000000.000000 cannot ...` (`probe:FloatToIntTooBig`).

`Floor[−9.2233720368547758e18]` (exactly −2^63) succeeds with −9223372036854775808; `Int[−2.7]` is
−2 (`probe:FloatToIntEdges`).

## 4. `char` and `char32`

A `char` is one UTF-8 code unit, 0–255. A `char32` is one Unicode code point. They are distinct
kinds: a char never equals a char32, whatever the numbers **(source, unprobed)**.

| Behaviour | `char` | `char32` | Evidence |
| --- | --- | --- | --- |
| literal | `'a'` (an ASCII character in single quotes) | a non-ASCII character in single quotes, e.g. `'é'` | `probe:Chars`, `probe:AcceptedChar32` |
| `=`, `<>` | by value | by value | `probe:Chars`, `probe:AcceptedChar32` |
| `<`, `<=`, `>`, `>=` | refused: *error 3509: No overload of the function `operator'<'` matches the provided arguments (:char,:char)* | refused likewise (source) | `reject:CharOrder` |
| `ToString(C)` | a one-byte string holding that byte | the code point's UTF-8 encoding, one to four bytes | `probe:Chars`, `probe:AcceptedChar32` |
| `"{C}"` | as `ToString` | as `ToString` | `probe:Chars`, `probe:AcceptedChar32` |

`ToString(:char)` does no validation: a byte of 0x80 or above becomes a one-byte string that is not
valid UTF-8 **(source, unprobed)**. `ToString(:char32)` is declared `<epic_internal>` in the library
but a script in this project reaches it, directly and through interpolation
(`probe:AcceptedChar32`). What it does with a code point that has no UTF-8 encoding (a surrogate,
or above U+10FFFF) is §16 Q4.

## 5. Strings

### 5.1 A string is an array of bytes

`string` is `[]char`: an array whose elements are UTF-8 code units. Everything in §6 applies to it. A
string literal is the array of its UTF-8 bytes; nothing validates that a string is well-formed UTF-8
**(source, unprobed)**.

| Behaviour | Answer | Evidence |
| --- | --- | --- |
| `Length` | the number of **bytes**: `"é".Length = 2`; `"".Length = 0` | `probe:AcceptedChar32`, `probe:Chars` |
| `S[I]` | the byte at I as a `char`; out of range fails (`"abc"[3]` fails) | `probe:Chars` |
| `A + B` | byte concatenation: `"ab" + "cd"` is `abcd` | `probe:Chars` |
| `=`, `<>` | element-wise, so length-sensitive: `"ab" <> "abc"` | `probe:Chars` |
| equality with an array of chars | a string equals any array of the same chars: `array{'a', 'b'} = "ab"` succeeds | `probe:Chars` |
| an array of chars as a string | `array{'x', 'y'}` assigned to a `string` prints `xy` | `probe:Chars` |
| ordering | refused: *error 3509: No overload of the function `operator'<'` matches the provided arguments (:[]char,:[]char)* | `reject:StringOrder` |

### 5.2 `false` as the empty string

`false` is accepted where a `string` (or any array) is expected (`reject:ArrayFromFalse` compiled;
§6.5). Such a string has `Length` 0, interpolates as nothing, and is **equal** to `""`
(`probe:CrossKind`). It is **not** interchangeable with `""` as a map key; §8.5.

### 5.3 A char is not a string

`'a' = "a"` through `comparable` fails: a char and a one-element string are different values
(`probe:CrossKind`).

### 5.4 Interpolation

A string literal with `{...}` interpolants is rewritten by the compiler, before any bytecode exists,
into a left-associated chain of `Add` over its pieces:

- each run of literal text is one string piece;
- each interpolant `{E}` becomes a call `ToString(E)`, resolved by ordinary overload resolution;
- an interpolant that is a single **character literal** is folded into the adjacent literal text
  rather than calling anything (`"{'z'}!"` prints `z!`, `probe:Chars`);
- an interpolant holding only whitespace or comments contributes nothing (`"{}"` is empty,
  `probe:Chars`);
- an interpolated string with exactly one piece is that piece alone — `"{X}"` is exactly
  `ToString(X)`, with no `Add` — and one with no pieces is `""`.

So interpolation and `ToString` cannot disagree for a primitive (`facts §3`, `facts §4`), and the
interpreter needs no interpolation support of its own: it sees `Add` on arrays and calls to
`ToString` natives. The overloads a script reaches are `ToString(:int)` (§2.6), `(:float)` (§3.4),
`(:[]char)` (the identity), `(:char)` and `(:char32)` (§4), and the Godot package's
`ToString(:object)` (`godot-natives.md`). A `logic`, an option, a rational or a tuple has no
`ToString`, and interpolating one is a compile-time error 3509 (`reject:LogicToString`,
`reject:RationalPrint`; an option was refused during `vm_values_probe.verse`'s first compile with
the same error; the tuple, array and map cases are source, unprobed).

## 6. Arrays

### 6.1 Immutable and mutable arrays

An `array` cell is immutable: nothing changes its length or elements after it is built. A
`mutable array` cell is the form an array takes while it is the contents of a `var` (§7) or while a
`for` expression is building its result (§6.4). Scripts never hold a reference to a mutable array
as a value; they see only its frozen copies.

The reference stores elements in one of several specialised layouts (`format.md`'s element kinds:
empty, general values, 32-bit integers, `char`, `char32`). **The choice is not observable**:
equality, hashing, map-key identity, printing and conversion to a native string all behave as if
every array held general values. An array built by a loop equals the literal with the same
elements (`probe:CrossKind`); a map keyed by a string finds the key when looked up with an array of
chars built another way (`probe:MapKeys`, "string key via char array").

### 6.2 Length and indexing

| Op | Operands | Behaviour |
| --- | --- | --- |
| `Length` | an array, mutable array, map, mutable map, or `false` | the element (or entry) count; `false` answers 0 |
| `LengthWithEffects` | as `Length`, or a `var` reference to one | reads through the reference first; the rest is `Length` (`ops.md` for the effect token) |
| `Call` with an array callee and one argument | index | the element at that index, or **failure** |
| `ArrayIndexFastFail` | `Array`, `Index` | the same, in fast-failure form (`failure.md`); additionally a `false` callee fails |

An index selects an element only if it is an integer in [0, Length). A negative index, an index ≥
Length, an index of 2^32 or more, and a heap-int index all **fail** — never a runtime error:
`A[3]`, `A[−1]`, `A[2^32]`, `A[2^63]` of a three-element array all fail (`probe:Arrays`).

`Call` on a `false` callee is **not** a failure in the reference: `false` is also a type (§13), and
the reference treats the call as a type cast it cannot perform and **aborts the process**
(`vm_values_false_probe.verse`, `IndexFalseArrayInDecides`: a fatal assertion, no output). Only
`ArrayIndexFastFail` handles a `false` array. This is reachable from a script — a `[]int` variable
initialised to `false` (§6.5), indexed inside a `<decides>` function body — and is §16 Q1.

### 6.3 Concatenation

`Add` of two arrays (either of which may be mutable) answers a **new immutable array** holding the
left's elements then the right's; neither operand changes. `Add` with `false` on either side answers
the other operand unchanged (`probe:AcceptedEmptyContainers`: `false + array{1, 2}` has Length 2).
Strings concatenate this way (§5.1).

### 6.4 Construction ops

| Op | Behaviour |
| --- | --- |
| `NewArray(Values)` | a new immutable array of the operands in order. Also how every tuple is built (§10) |
| `NewMutableArray(Values)` | a new mutable array of the operands in order. The compiler emits it with no operands to start a `for` result |
| `NewMutableArrayWithCapacity(Size)` | not emitted (`ops.json`). If supported: a new empty mutable array; `Size` is a 32-bit hint and has no observable effect |
| `ArrayAdd(Container, ValueToAdd, bTransactional)` | `Container` must be a mutable array. Appends `ValueToAdd` **as is** (not melted). The destination is unified with `Container` itself. When `bTransactional` is true the append is recorded so that a failure of the enclosing transaction removes it again; when false it is not recorded. The compiler sets it true exactly when the `for` body can suspend, because only then can the append land in a different transaction from the one that created the array; otherwise both happen in one transaction and discarding the array discards the append |
| `InPlaceMakeImmutable(Container)` | `Container` must be a mutable array. The **same cell** becomes an immutable array (identity is kept, nothing is copied); the destination is unified with it. The conversion is recorded so that failure undoes it (`failure.md`). Emitted once at the end of every `for` that builds an array |
| `MutableAdd` | not emitted (`ops.json`). If supported: as `Add` of two arrays, but the result is a mutable array |
| `FastAppendToArray(LeftSource, RightSource)` | `LeftSource` must be a mutable array (the melted contents of a `var`), `RightSource` any array. Appends each element of `RightSource`, **melted** (§7), to `LeftSource` in order, each append recorded for rollback. This is `set A += B` on the fast path |
| `CanFastAppendToArrayFastFail(Ref, MaybeMutableArray)` | the guard for the fast path, in fast-failure form. Succeeds iff `MaybeMutableArray` is a mutable array **and** `Ref` is either absent (the uninitialized constant, `format.md` §3 tag 0) or a `var` reference with no domain. Otherwise fails, and the compiler's slow path does freeze, `Add`, melt, store instead. The two paths are indistinguishable to a script |

What `set A[I] = V` does to an element (`CallSet`) is `ops.md`'s; its value-level contract is §7:
`set A[10] = 1` on a five-element array fails and changes nothing (`probe:ArrayValueSemantics`).

A `for` over a range builds its result with the ops above; `for (I := 1..4) do I * I` has Length 4
and element 3 is 16, and a filter drops elements (`probe:Arrays`).

### 6.5 `false` as an array

`false` is accepted where any array type is expected (`A:[]int = false` compiles,
`reject:ArrayFromFalse`); the reverse is not true of maps — `F:[int]int = false` is refused with
*error 3509: This variable expects to be initialized with a value of type [int]int, but this
initializer is an incompatible value of type true.* (that wording, `true` and all, is the
compiler's; `vm_values_false_probe.verse`). As an array, `false`:

- has `Length` 0; iterating it runs no iterations (`probe:AcceptedEmptyContainers`);
- fails `ArrayIndexFastFail` (`probe:AcceptedEmptyContainers`: `F[0]` in an `if` fails) but aborts
  the reference under `Call` (§6.2);
- is absorbed by `Add` (§6.3);
- equals every empty array and every empty map (§11).

## 7. Value semantics: `Melt` and `Freeze`

Arrays, maps, options and struct instances are **values**: no two variables ever share one, and
changing what one `var` holds never changes another (`probe:ArrayValueSemantics`):

| Script | Afterwards | Evidence |
| --- | --- | --- |
| `var A := array{1,2,3}; B := A; set A[0] = 9` | `B[0] = 1`, `A[0] = 9` | `probe:ArrayValueSemantics` |
| `var M := array{array{1,2}, array{3}}; N := M; set M[0][1] = 7` | `N[0][1] = 2`, `M[0][1] = 7` (the copy is deep) | `probe:ArrayValueSemantics` |
| `set A += array{4,5}` after `B := A` | `A.Length = 5`, `B.Length = 3` | `probe:ArrayValueSemantics` |
| `var C := A; set C += array{6}` | `C.Length = 6`, `A.Length = 5` | `probe:ArrayValueSemantics` |

The ops that implement it are `Melt` and `Freeze`. The compiler melts a value on its way into a
`var` and freezes it on its way out, so the value inside a `var` is private to it.

**`Melt(Value)`** answers a deep mutable copy of anything with value structure, and the operand
itself otherwise:

| Operand | Result |
| --- | --- |
| an array or mutable array | a **new** mutable array whose elements are the melted elements, in order |
| a map or mutable map | a **new** mutable map with the same keys (not melted) in the same order, each value melted |
| an option other than `true` | a **new** option holding the melted content |
| `true` | `true` itself |
| a struct instance | a **new** struct instance whose fields are melted |
| anything else (integers, floats, chars, `false`, class instances, functions, types, enumerators, rationals) | the operand itself |

Class instances are not copied: they are references (`objects.md`). If melting reaches an unbound
placeholder anywhere inside the value, the op waits on it (`unification.md`).

**`Freeze(Value)`** is the inverse on melted values: a mutable array becomes a **new** immutable
array of frozen elements; a mutable map a new immutable map, values frozen, order kept; an option
other than `true` a new option with frozen content; a struct instance a new struct instance; anything
without value structure is answered as is. The compiler applies `Freeze` only to what a `var` holds,
which is always melted; freezing an immutable array or map aborts the reference, and is outside the
contract **(source, unprobed)**. `Freeze` of an accessor reference calls the getter instead
(`ops.md`).

## 8. Maps

### 8.1 Order

A map is an **ordered** collection of distinct keys, each with a value. Its order is observable —
`for (K -> V : M)` visits entries in it, and equality depends on it (§8.4) — and is defined as
follows.

| Construction | Order | Evidence |
| --- | --- | --- |
| `NewMap(Keys, Values)` (a `map{...}` literal) | the textual order of the entries | `probe:Maps`: `map{3=>c, 1=>a, 2=>b}` iterates 3, 1, 2 |
| a literal with a repeated key | the **last** occurrence wins, and the entry takes the **position of the last occurrence**: `map{1=>x, 2=>y, 1=>z}` has Length 2 and iterates `2 => y`, `1 => z` | `probe:Maps` |
| `ConcatenateMaps(L, R)` | as a literal listing L's entries then R's: a key of R already in L moves to R's position with R's value. `ConcatenateMaps(map{1=>a, 2=>b}, map{2=>B, 3=>c})` iterates 1, 2 (B), 3; `ConcatenateMaps(map{1=>a, 2=>b}, map{1=>A})` iterates `2 => b`, `1 => A` | `probe:Maps`, `probe:MapConcatDuplicateOrder` |
| `set M[K] = V` on a `var` map, K present | the value is replaced **in place**; the order is unchanged | `probe:Maps`: after `set W[1] = "A"`, `1 => A` is still first |
| `set M[K] = V`, K absent | appended at the end | `probe:Maps` |
| `Melt`, `Freeze` | order preserved | (source, unprobed) |

`NewMap`'s operands are two equal-length lists; entry *i* is `Keys[i] => Values[i]`. Every key must
be concrete before the map is built; the op waits otherwise (`unification.md`). `ConcatenateMaps`
is a `$BuiltIn` intrinsic (`natives.md`).

The two ways a duplicate can arise disagree on purpose: building a map (literal or concatenation)
moves the entry, updating one through `set` keeps its place. An implementation must reproduce both.

### 8.2 Lookup, length, iteration

- `Call` with a map callee and one argument answers the value for the key **equal** (§11) to the
  argument, or **fails** when there is none (`M[9]` fails, `probe:Maps`). The argument must be
  concrete; the op waits otherwise.
- `Length` answers the entry count (`probe:Maps`).
- `MapKey(Map, Index)` and `MapValue(Map, Index)` answer the key and the value at 0-based position
  `Index` in the order of §8.1. They exist for `for (K -> V : M)`, which the compiler lowers to a
  loop over `Index` from 0 while `Index < Length`, reading both. `Index` is always a small integer
  within range in emitted code; any other operand is outside the contract.

### 8.3 Key types

A key type must be `comparable` (the language's built-in map signatures), and the compiler adds a
restriction: **an array type other than `string` cannot be a key** — *error 3502: Use of '[]int' as
a map key is not yet implemented.* (`vm_values_probe.verse`, recorded in `CrossKind`'s comment).
Measured as usable keys (`probe:MapKeys`): `int` including heap ints, `float`, `string`, tuples,
enumerators, structs, options, and instances of a `<unique>` class (by identity: a different
instance of the same class is not found).

### 8.4 Map equality

Two maps are equal iff they have the same number of entries and, **position by position**, equal
keys and equal values. Order matters: `map{1=>1, 2=>2} = map{2=>2, 1=>1}` fails
(`probe:Maps`). Two empty maps are equal; an empty map also equals an empty array and `false` (§11).

### 8.5 Key identity

A lookup finds an entry iff the probe key is equal to the stored key under §11, with the
consequences a hash table must honour:

| Keys | Same entry? | Evidence |
| --- | --- | --- |
| `0.0` and `−0.0` | yes: `map{0.0=>"pos", −0.0=>"neg"}` has Length 1 and one entry, `0.000000 => neg` | `probe:MapKeys` |
| any two NaNs | yes: a map with a `NaN` key is found by `Inf − Inf` | `probe:MapKeys` |
| a heap int and an equal heap int computed separately | yes | `probe:MapKeys` |
| a string and an equal array of chars | yes | `probe:MapKeys` |
| equal structs, equal tuples, equal options, the same enumerator | yes | `probe:MapKeys` |
| `vector2{X:=0.0, ...}` and `vector2{X:=−0.0, ...}` | yes | `vm_values_native_struct_probe.verse` |
| two distinct `<unique>` instances | no | `probe:MapKeys` |
| **`""` and `false` used as a string** | **no — in either direction**, although `"" = false` succeeds | `probe:CrossKind` |

The last row is a reference defect: the reference hashes `false` by identity and an empty array by
contents, so the two land in different places although equality calls them equal. Whether the
lookup would succeed on a chance collision is §16 Q2. **The lead decided the other way (§16.1):
equal values are one key, so `false`, `""`, an empty array and an empty map are one map key.** No
fixture depends on it.

## 9. Options and logic

| Value | Is |
| --- | --- |
| `false` | the `false` cell. It is the logic `false`, the empty option of every option type, and (§6.5) usable as an empty array |
| `true` | the `true` cell: a single, pre-existing option whose content is `false` |
| `option{V}` | a **new** `option` cell holding V, made by `NewOption(Value)` — including `option{false}`, which is a fresh cell and **not** the `true` cell |
| `option{E}` where E fails | `false`: the compiler evaluates E in a failure context and stores `false` on failure (`probe:Options`, "opt of failure = false") |
| `option{}` | refused: *error 3622: option{} requires an argument; did you mean `false`?* (found during the first compile of `vm_values_probe.verse`) |

`Query(Source)` (`X?`) and `QueryFastFail`:

| Source | Answer |
| --- | --- |
| `false` | fails |
| `true` | succeeds with `false` (its content) |
| any other option | succeeds with its content |
| anything else | outside the contract (the reference aborts), except a native object — §16 Q5 |

Measured (`probe:Options`, `probe:Logic`): `option{5}?` is 5; `false?` fails; `true?` succeeds;
`option{false}?` succeeds and its content is `false`; `option{option{5}}` ≠ `option{false}`.

Option equality (§11): two options are equal iff their contents are; an option never equals a
non-option. `false` = `false` (as the empty option) succeeds; `option{5} = false` fails
(`probe:Options`). Because `true` is compared as a logic value rather than as an option,
**`option{false}` compared with `true` through `comparable` fails, in both orders**, while
`option{false} = option{false}` succeeds (`probe:CrossKind`, `probe:Logic`).

`logic` values print nowhere: `ToString(:logic)` does not exist (`reject:LogicToString`).

## 10. Tuples

A tuple is an immutable array: `(1, 2.5, "s")` is built with `NewArray`, `T(0)` is ordinary array
indexing, and a tuple is assignable to an array type (`Q:[]int = P` for a `tuple(int, int)` gives
Length 2) and **equal** to the array of the same elements (`P = array{3, 4}` succeeds) (`probe:Tuples`).
Tuple equality is array equality: `(1,2) = (1,2)` succeeds, `(1,2) = (1,3)` fails. A tuple can be a
map key (`probe:MapKeys`). A function's several arguments are also passed as a tuple in some call
shapes; that is `calls.md`'s.

## 11. Equality

### 11.1 The four answers

Comparing two values answers exactly one of:

| Answer | Meaning |
| --- | --- |
| **Eq** | the values are equal |
| **Neq** | the values are not equal |
| **Undecidable** | the values are of a kind that has no equality (functions, types, non-unique VM-level class instances) and are not the same value |
| **error** | a runtime error was raised while comparing (a struct field whose read raises); the error has been raised by the time the answer is given |

An unbound placeholder on either side is a fifth situation owned by `unification.md`: the
comparison reports Eq provisionally and records the placeholder so the op can wait on it.

### 11.2 The rules

The first rule that applies decides.

1. **The same value** — the same immediate, or the same cell — is Eq. This covers NaN compared with
   the identical NaN, any cell with itself (a function with itself, an instance with itself), and
   every singleton.
2. **Two floats** follow §3.3: Eq iff both NaN or IEEE-equal (so +0 = −0).
3. **An integer on either side**: Eq iff the other is an integer of the same value, or a rational
   whose denominator is 1 and whose numerator is that value. Anything else — a float, a char, an
   array — is Neq. So `1 = 1.0` through `comparable` fails and `1 <> 1.0` succeeds
   (`probe:CrossKind`).
4. **An empty array or empty map on either side**: Eq iff the other is `false`, an empty array or an
   empty map; otherwise Neq. So `""`, `array{}`, `map{}` and `false` are all equal to one another
   (`probe:AcceptedEmptyContainers`, `probe:CrossKind`), whatever the declared types.
5. **`false` or `true` on either side**: Eq iff both are logic values and the same one; otherwise
   Neq. (This is why `option{false}` is not equal to `true`, §9.)
6. **An enumerator on either side**: Neq (identity was rule 1). Same for a union variant tag.
7. **Two cells**, by the left one's kind:

   | Left | Right | Answer |
   | --- | --- | --- |
   | option | option | the answer for the two contents |
   | option | anything else | Neq |
   | array / mutable array | array / mutable array | Neq if the lengths differ; otherwise element by element in order, the first non-Eq answer, else Eq |
   | array | not an array | Neq |
   | map / mutable map | map / mutable map | Neq if the lengths differ; otherwise position by position, key then value, the first non-Eq answer, else Eq |
   | map | not a map | Neq |
   | rational | rational | Eq iff same lowest terms |
   | rational | not a rational | Neq |
   | struct instance | an object of the same struct class with the same number of fields | each field compared; the first non-Eq answer; a field read that raises gives **error**; else Eq |
   | struct instance | anything else | Neq |
   | VM-level class instance | not a VM-level instance of the same class | Neq |
   | VM-level class instance | a different instance of the same class, class `<unique>` | Neq |
   | VM-level class instance | a different instance of the same class, not unique | **Undecidable** |
   | union variant | union variant | Neq if the tags differ; else the answer for the payloads |
   | native struct | native struct | Eq iff same struct type and the struct's own comparison says equal (`objects.md`) |
   | any other cell (function, type, scope, ...) | anything | **Undecidable** |

8. **Anything else** — two different chars, a char and a char32, two different native objects, a
   native object and a non-object — is **Neq**.

Class instances in this project are native objects (every script class derives from the native
root, `objects.md`), so by rules 1 and 8 **two native-object instances are Eq iff they are the same
object**, and never Undecidable (`probe:Identity`: a `<unique>` instance equals itself, not another).
The compiler refuses `=` on a non-unique class outright — *error 3509: This function parameter
expects a value of type tuple(comparable,comparable), but this argument is an incompatible value of
type tuple(vm_values_reject_plain,vm_values_reject_plain).* (`reject:NonUniqueEqual`) — so
Undecidable reaches only the ops in §11.3 that compare without a comparable type: unification.

Struct equality is field-wise with Verse float equality inside, whether the struct is the script's
own or one the Godot package declares (`vector2`): NaN fields are equal, +0 and −0 fields are equal
(`probe:Identity`, `vm_values_native_struct_probe.verse`).

### 11.3 Which ops compare, and what each does with each answer

| Op | Eq | Neq | Undecidable | error |
| --- | --- | --- | --- | --- |
| `Move`, `MoveTrailed` (unification; `=` outside a fast-failure context compiles to two `Move`s into a fresh register) | proceed | fail | **fatal**: the reference aborts | runtime error |
| `MoveNonComparable` | proceed | fail | **fail** | runtime error |
| `EqFastFail` | succeed, result is `Lhs` | fail | fail | fail (the error is already raised) |
| `NeqFastFail` | fail | succeed, result is `Lhs` | fail | fail (the error is already raised) |
| `Neq` | fail | succeed, result is `LeftSource` | fail | fail (the error is already raised) |
| map lookup (`Call` on a map, `CallSet` on a mutable map) | the entry matches | no match | no match | no match |

`Neq` first waits on any placeholder the comparison met (`unification.md`). The table's "fatal"
entry is a reference abort a program with correct types cannot reach; `unification.md` decides what
the interpreter does instead.

## 12. Ordering: `Lt`, `Lte`, `Gt`, `Gte` and their `FastFail` forms

Defined on: two integers (exact comparison at any size — `2^63 > 2^63−1 > 0` succeeds,
`probe:ComparisonResults`); two floats (§3.3); a rational with a rational or an integer (§2.4, not
reachable from a script). The compiler refuses ordering on every other kind — chars, strings,
rationals, and anything else (`reject:CharOrder`, `reject:StringOrder`, `reject:RationalOrder`) — and
the reference aborts if an op meets one anyway.

**The result of a successful comparison is its left operand**, unified into the destination — for
`Lt`/`Lte`/`Gt`/`Gte`/`Neq` and all six `FastFail` forms alike. So `V := (3 < 5)` binds 3,
`(5 > 3)` gives 5, `(2.5 <= 2.5)` gives `2.500000`, `(4 <> 5)` gives 4, and `(4 = 4)` gives 4
(`probe:ComparisonResults`). A chained comparison `a < b < c` is two comparisons, the second taking
the first's result: `1 < 2 < 2` fails (`probe:ComparisonResults`).

Both operands must be concrete; the non-`FastFail` forms wait on a placeholder (`unification.md`).

## 13. Type cells as values

A type is a value (a type parameter is passed like any other argument). `format.md` §4 lists the
kinds: `int type` (optional lower and upper bound, each an integer), `float type` (bounds as floats),
`tuple type`, `map type`, `simple type`, plus classes, and — because `false`, `true` and options are
types as well as values — the `false`, `true` and `option` cells. Types are not comparable; two
distinct type cells compare Undecidable (§11.2 rule 7).

A type is used as a value by `TypeCastFastFail(Type, Value)`, and by `Call` with a type as the callee
and one argument, which is the same test in the non-fast-fail form. Each succeeds with `Value`
itself when the type **admits** it, and fails otherwise:

| Type | Admits |
| --- | --- |
| the `any` simple type | everything |
| `int type` | an integer — or a rational whose denominator is 1 — at least the lower bound if there is one and at most the upper bound if there is one |
| `float type` | a float that is at least the lower bound and, unless the upper bound is NaN, at most the upper bound. A NaN is admitted only when the lower bound is −Inf **and** the upper bound is NaN (the unbounded `float`); `float type` with an upper bound of +Inf therefore excludes NaN |
| a class or interface | `objects.md` |
| every other type (`tuple type`, `map type`, every other simple type, `false`, `true`, `option`) | outside the contract: the reference aborts |

(source, unprobed — no script spelling reaching a bounded `int type` or `float type` cast was found;
class casts are `objects.md`'s and are probed there.) A `TypeCastFastFail` whose `Type` operand is not
a type aborts the reference.

The printed forms of type cells exist only for diagnostics (`int`, `nat`, `float`, `type{...}`) and
are not observable to a game (§3.5).

## 14. What a script can print

| Kind | `ToString(X)` / `"{X}"` |
| --- | --- |
| `int` | §2.6; raises outside int64 |
| `float` | §3.4 |
| `string` | itself |
| `char`, `char32` | §4 |
| a Godot object, a script class instance | Godot's `to_string()` (`godot-natives.md`) |
| `logic`, option, rational, tuple, array other than `string`, map, struct | no `ToString`: a compile-time error 3509 |

## 15. Outside the contract

The reference **aborts the process** (a fatal assertion, not a Verse runtime error) when an op meets
an operand kind it does not define. The compiler's type checking prevents every case below except
§6.2's `Call` on `false`, so a conforming program never reaches one; an implementation may report
them however it likes, and a raised runtime error naming the op and line is recommended.

- `Add` other than int+int, float+float, rational with int/rational, array+array, or with `false`;
  `Sub`, `Mul`, `Div`, `Neg` on other kinds (including rational with float);
- `Mod` (not emitted) on non-integers;
- ordering on anything but ints, floats and rationals (§12);
- `Length` of anything but an array, a map or `false`;
- `Query` of anything but `false`, `true`, an option, or a native object (§9);
- `MapKey`/`MapValue` on a non-map or with a non-integer index;
- `ArrayAdd`, `InPlaceMakeImmutable` on anything but a mutable array; `FastAppendToArray` with a
  right operand that is not an array;
- `Call` on `false`, on a non-castable type, or on a value that is neither function, array, map,
  type, union variant tag nor reference (§6.2, §13);
- `Freeze` of an immutable array or map (§7);
- unification answering Undecidable (§11.3).

## 16. Open questions

1. **`Call` on `false`.** A `[]int` holding `false` (legal, §6.5), indexed where the compiler emits
   `Call` rather than `ArrayIndexFastFail` — a `<decides>` function body — aborts the reference
   (`vm_values_false_probe.verse`). The differential harness cannot use such a fixture. The
   recommendation is to treat `Call` on `false` with one integer argument as a failure, matching
   `ArrayIndexFastFail`; the lead should confirm, since that is a deliberate divergence from an
   abort.
2. **`false` versus `""` as map keys** (§8.5). The reference hashes them differently, so the lookup
   misses, but a collision in the reference's hash table would make it hit; the reference's answer
   is not deterministic in principle. This file specifies "different keys". A fixture should not
   depend on it.
3. **Integer to float at the top of the range.** An integer in [2^1024 − 2^970, 2^1024) rounds to
   2^1024, which is not a finite binary64. Whether the reference answers `Inf` or something else
   there was not probed; §2.7 states `Inf` only for the measured 2^1071.
4. **`ToString(:char32)` of a code point with no UTF-8 encoding** (a surrogate, or above
   U+10FFFF), and whether such a `char32` can be produced at all. Not probed.
5. **`Query` of a native object.** The reference succeeds without producing a value; what the
   destination then holds, and whether the compiler ever emits it, was not settled. It is likely
   dead; `ops.md` should decide.
6. **Invalid UTF-8 reaching Godot.** A string is unvalidated bytes (§5.1), and `ToString` of a high
   `char` makes one. What happens when such a string crosses to Godot (`Print`, a `String`
   parameter) is `godot-natives.md`'s to settle.
7. **The Mod op.** Not emitted, so its remainder sign convention (the source uses the truncating one,
   unlike the Euclidean library `Mod[]`) was not specified. If a later engine emits it, it needs a
   probe.

### 16.1 The lead's decisions

- **Q1: `Call` on `false` with one integer argument fails**, exactly as `ArrayIndexFastFail` does. This
  is a deliberate divergence from the reference, which aborts the process: no program can depend on
  an abort, and no conformance fixture exercises it.
- **Q2: `false` and `""` are the same map key**, because they are equal (§11) and a map key's
  identity is equality. This is a deliberate divergence from the reference's hashing, whose answer is
  not deterministic; no conformance fixture depends on it.
