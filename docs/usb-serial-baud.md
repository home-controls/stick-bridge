# USB serial baud rates: CH340/CH341 and FTDI

How the bridge turns a baud rate into the values the CH340/CH341 and FTDI USB serial chips take. This is the
specification `main/serial_baud.c` is written from: it describes what the chips do, from the makers' documents
and measurement, not any other driver's code. Licence: MIT, like the rest of the project.

## CH340 / CH341 (WCH)

### What the chip takes

The rate is set by writing one 16-bit word to the register pair 0x13 (high byte) / 0x12 (low byte), with the
chip's vendor "write register" request (bRequest 0x9A, wValue 0x1312, wIndex = the word).

- **Low byte, the clock select**
  - bits 0–1: `PS`, 0 to 3
  - bit 2: `FACT`, 0 or 1
  - bits 3–6: zero
  - bit 7: set for chips whose version (vendor request 0x5F) is greater than 0x27. It tells newer chips to pass
    received bytes on at once instead of waiting to fill a packet. Clear for version 0x27 and older.
- **High byte:** `256 − N`, where `N` is the divisor, 2 to 255.

The chip's UART clock is 48 MHz divided by a prescaler `P`, then by `N`:

```
rate = 48,000,000 / (P × N)        P = 2^(12 − 3·PS − FACT)
```

| PS | FACT | P |
| --- | --- | --- |
| 0 | 0 | 4096 |
| 0 | 1 | 2048 |
| 1 | 0 | 512 |
| 1 | 1 | 256 |
| 2 | 0 | 64 |
| 2 | 1 | 32 |
| 3 | 0 | 8 |
| 3 | 1 | 4 |

### Choosing the word

`sb_ch34x_word(baud, chip_version)` returns the word as a non-negative int, or −1:

1. `baud == 0` → −1.
2. Clamp `baud` to the range 46 … 3,000,000.
3. Among every `(PS, FACT, N)` with `N` in 2 … 255, and **`N` ≥ 9 whenever `FACT` = 1** (small divisors
   are only used with the slower clock, to keep the receiver's sampling tolerant), pick the one whose exact rate
   48,000,000 / (P × N) (a fraction, not rounded) is closest to `baud`.
4. Several choices can give exactly the same rate, because only `P × N` matters: `FACT` = 1 with an even `N`
   equals `FACT` = 0 with `N/2`. Prefer, in this order: `FACT` = 0 (the slower, more tolerant clock); then the
   larger `N` (finer steps, the same rate). If two different rates are equally close, take the higher rate.
5. Set bit 7 if `chip_version > 0x27`.

`sb_ch34x_rate(word)` gives the rate a word really produces, from the formula above (integer division, rounded
down). Only the low 16 bits of the word count. It returns 0 for a negative word, or when the high byte is 0
(which would mean N = 256, never produced).

### Expected values (from the formula; the tests check them)

- 115200 → `PS`=3, `FACT`=0, `N`=52: 48e6/(8×52) = 115384 (+0.16 %); word `0xCC03` (`0xCC83` with bit 7)
- 9600 → `PS`=2, `FACT`=0, `N`=78: 48e6/(64×78) = 9615 (+0.16 %); word `0xB202`
- 921600 → `PS`=3, `FACT`=1, `N`=13: 48e6/(4×13) = 923076 (+0.16 %); word `0xF307`
- 300 → `PS`=0, `FACT`=0, `N`=39: 300.48 (+0.16 %), the closest the chip gets; word `0xD900`. 3,000,000 → `0xFE03`; 1,000,000 → `0xFA03`
- Every standard rate from 300 to 921600 comes within 2 %; 1,000,000 and 2,000,000 and 3,000,000 are exact.

## FTDI FT232R / FT-X ("BM" type)

From FTDI application note AN232B-05, *Configuring FT232R, FT2232 and FT232B Baud Rates*.

### What the chip takes

A 17-bit value, sent with the SET_BAUD_RATE request (bRequest 0x03): bits 0–15 in wValue, bit 16 in bit 0 of
wIndex. The chip divides 3 MHz (48 MHz / 16) by a divisor `D`, written as a whole part plus eighths:

- bits 0–13: the whole part of `D`
- bits 14–16: the fraction, as a 3-bit code (the app note's table):

| Fraction | Code |
| --- | --- |
| 0 | 0 |
| 1/8 | 3 |
| 2/8 (1/4) | 2 |
| 3/8 | 4 |
| 4/8 (1/2) | 1 |
| 5/8 | 5 |
| 6/8 (3/4) | 6 |
| 7/8 | 7 |

Two special values: **0** means `D` = 1 (3,000,000 baud), **1** means `D` = 1.5 (2,000,000 baud). Other
divisors between 1 and 2 don't exist (the app note: sub-integer divisors below 2 aren't allowed), so a value
with whole part 1 and a nonzero fraction is never produced. Bits above 16 are ignored by `sb_ftdi_rate`.

### Choosing the value

`sb_ftdi_value(baud)`:

1. `baud == 0` counts as 1.
2. The divisor in eighths is `24,000,000 / baud`, rounded to the nearest integer (halves round up).
3. Up to 12 eighths (`D` ≤ 1.5): take whichever of 8 (`D` = 1, value 0) and 12 (`D` = 1.5, value 1) is
   closer to the computed number, the higher rate on a tie. From 13 to 15 eighths, use 16 (`D` = 2).
4. Otherwise: whole part = eighths / 8, fraction code from the table for eighths % 8. If that divisor is above
   16383 7/8 (the largest the 14-bit whole part and the fraction can hold), use exactly 16383 7/8 (whole part
   16383, code 7): the slowest rate the chip makes, about 183 baud.

`sb_ftdi_rate(value)` gives the real rate: 0 → 3,000,000; 1 → 2,000,000; otherwise
24,000,000 / (whole × 8 + eighths), rounded to the nearest integer.

### Expected values (from the app note's formula)

- 115200 → 24e6/115200 = 208.33 → 208 eighths = 26 + 0 → value `0x001A`
- 460800 → 52.08 → 52 eighths = 6 + 4/8 → code 1 → `0x4006`
- 921600 → 26.04 → 26 eighths = 3 + 2/8 → code 2 → `0x8003`
- 9600 → 2500 eighths = 312 + 4/8 → `0x4138`
- 3,000,000 → 0; 2,000,000 → 1
