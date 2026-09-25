# VM facts: T1.2

Measured, not argued, per `docs/phase-7.5-design.md` §6.1 and the task list's T1.2. Every reading
below comes from one run of the fixture `tests/verse_probe/vm_facts_probe.verse` against
`verse_host.dll` (built 2026-09-18, `../UnrealEngine` at `203d764`) with:

```
bin/verse_probe.exe C:/Users/Devin/projects/UnrealEngine/Engine/Binaries/Win64/verse_host.dll \
    C:/Users/Devin/projects/UnrealEngine/Engine \
    C:/Users/Devin/projects/godot-verse/tests/verse_probe/vm_facts_probe.verse --class vm_facts_probe
```

Run from an absolute path outside `bin/`, not `cd`'d into it: `verse_probe.exe` still resolved
`tbbmalloc.dll` from its own directory (ordinary Windows DLL search order, directory-of-the-exe),
so **the "run it from bin/" rule in `CLAUDE.md` is about where `tbbmalloc.dll` has to sit, not
about the working directory the probe is invoked from.** No `LoadLibrary` 126 was seen.

Compile: `status 0, generation 1, 0 error(s)` on the version of the fixture below. Two lines were
tried and removed because they do not compile at all (recorded under "Refused at compile time").

The probe driver calls every public, zero-argument, non-suspending method on the class
automatically, in declaration order, ticking between calls — so a runtime error in one call cannot
take a later call down with it (`tests/verse_probe/verse_probe.cpp`). What it *can* take down is
every `Print` inside its **own** call: a `<transacts>` failure rolls back the whole call's deferred
writes, so a call that raises shows no output at all, not a truncated prefix (see "Adjacent
finding: a raise erases the whole call's Prints" below). That is why the fixture spreads the risky
lines (§3's int-overflow printing) into their own zero-argument methods rather than appending them
to a method that already printed something worth keeping.

## 1. NaN under all six comparisons — the disagreement, settled

**Both sources were half right.** Epic's source is right that `NaN <= NaN` and `NaN >= NaN`
succeed; `CLAUDE.md`'s claim that NaN "fails `<=` and `>=` against everything, itself included" is
wrong about *itself* — against a *different* value (`1.0`) it is right. The shape that explains
both readings at once: `<=` and `>=` look reflexive here only because Verse's `=` is reflexive for
NaN (measured previously in `tests/verse_probe/math_facts.verse`), and `<=`/`>=` are true whenever
`<`/`>` is true *or* `=` is true — so `NaN <= NaN` reduces to `false or true = true` without needing
any NaN-specific rule in `<=` itself. Against `1.0`, `=` is false, so `<=`/`>=` fall back to pure
ordering and NaN is unordered there, exactly as `CLAUDE.md` says.

| Operator | `NaN` vs `NaN` | `NaN` vs `1.0` |
| --- | --- | --- |
| `<=` | succeeds | fails |
| `>=` | succeeds | fails |
| `<` | fails | fails |
| `>` | fails | fails |
| `=` | succeeds | fails |
| `<>` | fails | succeeds |

`CLAUDE.md`'s "NaN fails `<=` and `>=` against itself" line should read: NaN fails `<=`/`>=`
against anything it is not *equal to* (by Verse's reflexive `=`), itself included when `=` is false
— it is never true because NaN is "small"; it is true against itself only because `=` already is.

## 2. Integer `Mod[]` / `Quotient[]` — floor was a coincidence of testing only positive divisors

`docs/phase-7.5-design.md` §6.1 asked "does `Mod[]` floor or truncate". It does **neither** in
general — it is **Euclidean**: the remainder is always in `[0, |B|)`, and the quotient is whatever
makes `A = Quotient*B + Mod` hold with that remainder. That is indistinguishable from flooring
division when `B > 0` (which is the only sign `math_facts.verse`'s `Quotient[-3, 2] = -2` and
`CLAUDE.md`'s "Quotient floors" tested), and it visibly diverges from flooring when `B < 0`:
flooring `7 / -2` is `-4` with remainder `-1`; Verse answers quotient `-3`, remainder `1`.

| `A` | `B` | `Mod[A, B]` | `Quotient[A, B]` | Flooring division would give |
| --- | --- | --- | --- | --- |
| 7 | 2 | 1 | 3 | 3, rem 1 (agrees) |
| -7 | 2 | 1 | -4 | -4, rem 1 (agrees) |
| 7 | -2 | 1 | -3 | -4, rem -1 (**disagrees**) |
| -7 | -2 | 1 | 4 | 3, rem -1 (**disagrees**) |
| 7 | 0 | declines (`<decides>` fails) | declines | — |

So: **`Mod[]`/`Quotient[]` implement Euclidean division** (remainder always non-negative), which
equals floor division for a positive divisor and does not for a negative one. `Mod`/`Quotient` by
zero both decline rather than raising or crashing. `docs/web-vm/spec/values.md` should say
"Euclidean", not "floors", or it will mislead the one case (`B < 0`) where it matters.

## 3. `ToString(float)` vs `"{X}"` interpolation

`docs/phase-7.5-design.md` §6.1's third question — `1.000000` or shortest round-trip — is settled:
**fixed six-decimal notation, the same shape as C's default `%f`, for every magnitude including
`1.0e20`.** `float` has **no `.ToString()` extension method** (`X.ToString()` on a `float` is
refused at compile time: *"error 3506: Unknown member `ToString` in `float`"*) — unlike a user
class, where `tostring_probe.verse` found the extension-method spelling is what a script writes.
The free function `ToString(X)` is what exists for a primitive, and it and `"{X}"` interpolation
print byte-for-byte identically in every row below — so for a primitive, **interpolation does not
diverge from `ToString`**, unlike the object case in `tostring_probe.verse` (round 4: `.ToString()`
reaches the extension method, `"{Self}"` reaches Godot's `to_string()`). No divergence means there
was nothing to chase into Epic's sources for this question.

| Input | `ToString(X)` | `"{X}"` |
| --- | --- | --- |
| `1.0` | `1.000000` | `1.000000` |
| `0.1` | `0.100000` | `0.100000` |
| `1.5` | `1.500000` | `1.500000` |
| `-0.0` | `0.000000` | `0.000000` |
| `1.0e20` | `100000000000000000000.000000` | `100000000000000000000.000000` |
| `123456789.125` | `123456789.125000` | `123456789.125000` |
| `1.0 / 3.0` | `0.333333` | `0.333333` |
| `1.0 / 0.0` (Inf) | `Inf` | `Inf` |
| `-1.0 / 0.0` (-Inf) | `-Inf` | `-Inf` |
| `0.0 / 0.0` (NaN) | `NaN` | `NaN` |

Two more facts fell out of this table: **the sign of `-0.0` is not preserved in the printed
string** (prints `0.000000`, not `-0.000000`), and non-finite values print as the bare words `Inf`,
`-Inf`, `NaN` rather than as decimal notation.

## 4. Int printing, and arithmetic crossing 2^63

| Input | `ToString(X)` | `"{X}"` |
| --- | --- | --- |
| `0` | `0` | `0` |
| `-5` | `-5` | `-5` |
| `4611686018427387904` (2^62) | `4611686018427387904` | `4611686018427387904` |
| `9223372036854775807` (2^63-1, int64 max) | `9223372036854775807` | `9223372036854775807` |

**A 2^70 literal (`1180591620717411303424`) is refused at compile time**, not accepted and
truncated: *"error 3555: Integer literal must be in the range -9223372036854775808 to
9223372036854775807."* An int **literal** is capped to the int64 range regardless of what int
**arithmetic** does at that boundary, which is the next two rows:

| Expression | `> int64 max` | `< 0` | `= int64 max` |
| --- | --- | --- | --- |
| `9223372036854775807 + 1` | succeeds | fails | fails |
| `9223372036854775807 + 9223372036854775807` | succeeds | fails | fails |

So **addition across 2^63 does not wrap to negative and does not clamp** — the value compares as
genuinely greater than int64 max, i.e. Verse's `int` is arbitrary-precision at the value level even
though its literal syntax is not. But **printing that value raises**: both `ToString(Overflowed)`
and bare `"{Overflowed}"` interpolation fail identically —

```
LogVerseRuntime: Error: ErrRuntime_GeneratedNativeInternal: An internal runtime error occurred in
(generated) native code that was called from Verse. There is no other information available.
(Value exceeds the range of a 64 bit integer.)

Callstack follows:
    [native] (/Verse.org/Verse:)ToString(:int)
```

— which means the *value* can exceed 64 bits, but the native `ToString(:int)` that both spellings
call cannot. This is the same native either way, which answers the "which function does
interpolation call" question for `int` the same way §3 answered it for `float`: there is only one.

## 5. Adjacent facts

**`Round[]` is round-half-to-even (banker's rounding), not round-half-up or round-half-away-from-zero:**

| Input | `Round[]` | `Floor[]` | `Ceil[]` |
| --- | --- | --- | --- |
| `2.5` | `2` | — | — |
| `-2.5` | `-2` | — | — |
| `0.5` | `0` | — | — |
| `-0.5` | `0` | -1 | 0 |
| `0.5` | (above) | 0 | 1 |

All four `Round[]` results round to the nearest *even* integer (2, -2, 0, 0), never away from zero
and never consistently up or down — the decisive case is `Round[-2.5] = -2` (round-half-up would
give `-3`, round-half-away-from-zero would give `-3`, round-half-to-even gives `-2`, which is what
was measured).

**`-0.0` compares equal to `0.0`, and is not "less than" it:**

| Comparison | Result |
| --- | --- |
| `-0.0 = 0.0` | succeeds |
| `-0.0 <= 0.0` | succeeds |
| `-0.0 < 0.0` | fails |

## Adjacent finding: a raise erases the whole call's `Print`s, not just what follows it

Not asked for, but load-bearing for anyone writing a fixture like this one: the first version of
§4's `ReportIntToString` printed four safe lines (`0`, `-5`, `2^62`, int64 max) and then one that
raised on the int-overflow `ToString`. **None of the four safe lines appeared in the transcript.**
`CLAUDE.md`'s "a failure at any depth drops the deferred writes" already says this about state, and
it turns out to include `Print` — a Godot-side effect that, per the same document, "defer[s] to
commit". A `<transacts>` call that raises anywhere in its body commits nothing it printed, even
lines that ran and would otherwise have succeeded. The fixture works around this by giving every
risky line its own zero-argument method, since the driver ticks (and so starts a fresh call) between
top-level calls but not between statements inside one.

## Fixture

`tests/verse_probe/vm_facts_probe.verse`, kept in the repo. Compiles clean (`status 0, generation 1,
0 error(s)`) as of this measurement. Re-run with the command at the top of this file; `bin/`
already has `tbbmalloc.dll` and `verse_probe.exe` (built 2026-09-18), and
`../UnrealEngine/Engine/Binaries/Win64/verse_host.dll` (built 2026-09-18) was used unmodified — it
was not rebuilt for this task.
