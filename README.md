# C64 RND "easter egg" seed search

The fake easter egg from 8-Bit Show and Tell (TPUG newsletter, Fall 2015):

```
10 A=125708:GOSUB 20:A=33435700:GOSUB20:A=17059266:GOSUB 20:END
20 A=RND(-A)
30 A=INT(RND(A)*22):IF A THEN PRINT CHR$(A+64);:GOTO30
40 PRINT:RETURN
```

prints `BILL` / `GATES` / `SUCKS`. `RND(-n)` reseeds the generator, and each
following `RND(1)` scaled to 0..21 gives a letter (0 ends the word). This
project finds seeds for other words, starting with a deliberately simple,
bit-exact reference implementation that faster versions can be tested
against.

## Layout

| path | what |
|---|---|
| `src/c64float.[ch]` | Bit-exact model of the BASIC float routines RND needs (FMULT, FADD, normalise, round, pack/unpack, QINT), one function per ROM routine |
| `src/c64rnd.[ch]` | `RND()` itself, plus the two ways the programs scale it to an integer |
| `src/seedsearch.c` | The reference search: `build/seedsearch [--assign] WORD [FIRST [LAST]]` |
| `test/cpu6502.c`, `test/c64machine.c` | Test oracle: a small 6502 emulator that runs the **real** BASIC/KERNAL ROMs (from VICE) |
| `test/runbasic.c` | Runs a tokenised BASIC program on the oracle and prints its output |
| `test/test_rnd.c` | Compares the C model with the ROM routines, seed by seed and on random operands |
| `test/compare_basic.sh` | Runs a BASIC search program on the ROMs and checks `seedsearch` finds the same seeds |
| `test/vice_crosscheck.sh` | Checks the oracle against VICE itself |
| `basic/` | The easter egg, Robin's search, an INT() variant of it, and the cross-check program |
| `tools/dis6502.py` | Disassembler used to read the ROM routines |

```
make            # build
make test       # quick checks (~30 s)
make test-long  # millions of seeds against the ROM (~10 min)
make test-vice  # oracle vs. VICE (~30 s)
```

## How RND works ($E097)

* `RND(x)`, x < 0: x itself becomes the seed.
* `RND(x)`, x > 0: `seed = seed * 11879546 + 3.927677739E-8`.
* Both paths then "scramble": reverse the 4 mantissa bytes, make the value
  positive, use the old exponent as the rounding byte, set the exponent to $80,
  normalise, and store the rounded value in RNDX ($8B-$8F). That value is the
  result.

Being bit-exact means modelling the 40-bit FAC (32-bit mantissa plus the
rounding byte at $70) and when it does and doesn't get rounded.

### Quirks the model has to reproduce

Each of these was found by reading the ROM and confirmed against it. The
tests fail if either of the first two is removed.

1. **FMULT zero-byte bug.** FMULT skips a zero multiplier byte by calling
   FADD's shift routine ($B983). That routine expects carry set and leaves it
   clear, so a zero byte that follows another zero byte shifts the partial
   product **9** bits instead of 8. This happens in RND's own multiply when the
   seed has two zero mantissa bytes in a row.
2. **FADD equal-exponent +1.** When both exponents are equal, a leftover carry
   adds 1 to the rounding byte.
3. **`A%=X` rounds, `INT(X)` doesn't.** The egg uses `A=INT(RND(A)*22)`, which
   truncates the 40-bit product. Robin's search uses `A%=RND(A)*T`, and LET
   rounds to 32 bits first. Across every seed 1..99,999,999 this changes exactly
   one result: seed **33721604**, whose first draw is RND ≈ 0.545454545412,
   gives `INT` 11 ('K') but `A%` 12 ('L'). `seedsearch` defaults to the egg's
   semantics; `--assign` matches Robin's program.
4. Operand order matters to FMULT's exact bits: in `RND(A)*22` the RND value is
   pushed (and rounded) and ends up in FAC2, while 22 in FAC1 is the multiplier.

FMULT's shifter also fills from $68 rather than 0, but only QINT ever makes
$68 non-zero, and it clears it again before returning.

## Validation

* `test_rnd`: 2,000,000 seeds × 10 draws (seeds 1..1M plus 1M random seeds up
  to 4×10⁹), including both scale-to-integer modes. Also 10M random FMULT/FADD
  operand pairs biased towards zero bytes and equal exponents. The C model and
  the ROM agree on every bit.
* `compare_basic.sh`: Robin's program, run verbatim on the ROMs, finds the same
  seeds as `seedsearch` (e.g. all 13 seeds for "AN" in 1..200000).
* `vice_crosscheck.sh`: the egg, the seed-33721604 divergence, raw RND values,
  and a Robin search all print the same in VICE as on the oracle.

## Baseline performance

`seedsearch BILL 1 99999999` (Robin's full range) takes **7.9 s** on one M2
Ultra core: 26 seeds, including the egg's 125708.

## Notes for the optimised versions

* Only letters A–U are possible (`CHR$(64 + 1..21)`).
* A word of n letters matches about 1 seed in 22^(n+1), so a 5-letter word
  turns up about once per 113M seeds and Robin's 10⁸ range often has none.
  Seeds don't have to be integers below 10⁸, though. Any number BASIC can parse
  works, so the real search space is the ~2³¹ distinct RNDX states. Mapping a
  wanted state back to a literal that `FIN` parses to exactly that float is its
  own problem.
* Every seed is independent, so the search is embarrassingly parallel.
