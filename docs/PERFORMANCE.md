# Performance

How fast the relay's code is, how that was measured, and what the optimisation pass after 0.4.10 changed. The short version: the colour work of an output tick is about
**1.8 times faster** on the board, the LED output is **byte for byte the same**, and the cost is 6 KB of flash and 1.2 KB of RAM. The relay was never CPU bound (it
spends most of its time waiting for the network), so the practical effect is headroom: more targets, longer strips and the new finishing stage cost less.

## How it was measured

Nothing here is estimated from the PC; the figures that matter come from the ESP32-S3 itself.

* **Per-function benchmark on the board** (`bench/`): the same cases are built for the PC (`python bench/run_host.py`) and for the board (`pio run -e atoms3-bench -t upload`,
  then `python bench/run_board.py`). The benchmark firmware runs the cases instead of the relay (no Wi-Fi, no other task, so nothing disturbs the timing) and prints the
  result over USB. Every case does the operation many times on realistic fixed input (real Sync Box frames, a 266 LED strip); the figure is the **fastest of 9 repetitions
  of about 40 ms**, taken as the best of 3 passes, so interrupts do not count. The board runs at 240 MHz. Differences of a few percent between runs of an unchanged function
  (see the unchanged `ddp_header` and `black_frame` below, which differ by 9 and 16 %) come from where the linker happens to place the code; treat anything under 10 % as noise.
* **The relay's real tick on your real setup** (`bench/tick_stats.py`): `pio run -e atoms3-tickstat -t upload` builds the normal relay plus a counter around the colour work
  of every output tick of every target (mixing, easing, finishing, packing; not the network send), printed over USB every five seconds. It was run on the author's board
  with its real three targets (a 210 LED strip with blur and saturation 1.8, a 266 LED strip, a Govee light with 42 segments and vivid blending) and the real Sync Box
  feeding it, 12 windows of 5 seconds per condition, for the baseline commit (`bench-baseline`, the same counter patched in) and the optimised one.
* **Same output** (`tools/output_fingerprint.py`): sends 18 fixed pictures to the relay and keeps the complete LED frame it sends back for each, once with the finishing
  stage off and once with all three parts on. The baseline firmware and the optimised one gave **identical frames for all 36 pictures**.
* **Same results in the tests**: `test/legacy_core.h` holds the original functions, cut out of the baseline commit by `tools/freeze_legacy.py`. `test/test_core.cpp` runs every
  optimised function against its original on random inputs (400 random tables, plain and vivid, 20,000 Reaction boost calls, 6,000 valid and damaged frames, 60 finishing-stage
  sequences of 80 frames, ...) and requires **bit-identical** results; only `adjustZones` (the same sums in another order) is compared to within 0.002 of a colour level. The
  tests are built with fused multiply-adds switched off (`-ffp-contract=off`): `core.cpp` is forced to `-O3` by a pragma and GCC fuses `a * b + c` into one rounding there but not
  in the `-O1` test file, which made CI show one-bit differences between code that computes the same thing. (The board's compiler does fuse, in both the old and the new code;
  that is what the complete-output check above covers.) A
  deliberately injected error of 0.1 % in one branch is caught by these tests (that is how a hole in the first version of the test was found: random tables from the layout
  builders never exercised the "smooth beyond the TV" blend, because for them it adds nothing; tables with arbitrary entries were added).

## Results on the board

Microseconds per operation, ESP32-S3 at 240 MHz:

| case | what | before (us) | after (us) | speed-up |
|---|---|---:|---:|---:|
| `classify_frame_88` | decode one 88 byte Sync Box packet | 8.18 | 7.41 | 1.10x |
| `classify_frame_91` | decode one 91 byte Sync Box packet | 7.79 | 7.49 | 1.04x |
| `black_frame` | TV-off tracking, per packet | 0.21 | 0.18 | 1.16x |
| `lead_boost_72ch` | Reaction boost, 72 channels, one call per channel (the old way) | 65.56 | 63.19 | 1.04x |
| `lead_boost_packet` | Reaction boost, 72 channels, factor worked out once (the new way) | 65.56 | 8.07 | 8.13x |
| `adjust_zones_default` | brightness only, per packet | 25.93 | 5.71 | 4.54x |
| `adjust_zones_gamma` | gamma 1.2 and saturation 1.3, per packet | 221.93 | 198.08 | 1.12x |
| `motion_push_at` | Motion smoothing, per picture | 50.69 | 50.66 | 1.00x |
| `speed_push` | box speed detector, per packet | 14.82 | 14.79 | 1.00x |
| `apply_table_linear` | mix 266 LEDs from the 24 zones (linear layout) | 183.07 | 97.89 | 1.87x |
| `apply_table_loop` | mix 266 LEDs (loop layout) | 151.58 | 66.64 | 2.27x |
| `apply_table_vivid` | mix 266 LEDs, vivid blending | 471.18 | 232.72 | 2.02x |
| `saturate_266` | per-target saturation, 266 LEDs | 86.97 | 75.85 | 1.15x |
| `blur_266` | blur over the TV part, 266 LEDs | 216.97 | 106.95 | 2.03x |
| `dark_outside_266` | dark outside the TV part, 266 LEDs | 20.36 | 10.34 | 1.97x |
| `ease_266` | one easing step, 266 LEDs | 73.90 | 43.77 | 1.69x |
| `finish_all_266` | black cut-off + hold steady + dither, 266 LEDs | 467.67 | 272.61 | 1.72x |
| `pack_rgb_266` | round to bytes, RGB, 266 LEDs | 22.64 | 22.61 | 1.00x |
| `pack_rgbw_266` | round to bytes, RGBW, 266 LEDs | 35.54 | 35.50 | 1.00x |
| `tick_minimal_266` | a whole tick: plain strip | 279.41 | 164.06 | 1.70x |
| `tick_rich_266` | a whole tick: vivid, saturation, blur, dark outside | 866.04 | 466.27 | 1.86x |
| `tick_everything_266` | a whole tick: the above plus the finishing stage | 1233.56 | 676.14 | 1.82x |
| `govee_json_50` | one Govee frame (50 segments) | 12.25 | 12.23 | 1.00x |
| `ddp_header` | one DDP header | 0.25 | 0.23 | 1.09x |

A whole output tick of one 266 LED target went from 279 to 164 us (plain strip), 866 to 466 us (vivid, saturation, blur, dark outside) and 1234 to 676 us (those plus the
finishing stage).

### The relay's real tick, on the real setup

Average colour work per output tick of a target (all three real targets together), 12 windows of 5 seconds each:

| condition | before | after | speed-up | ticks | ticks that reused the previous wanted colours |
|---|---:|---:|---:|---:|---:|
| as configured (Smoothing over time 0) | 392 us | 216 us | 1.81x | 3680 / 3681 | 0 % / 38 % |
| Smoothing over time 0.5 | 473 us | 247 us | 1.91x | 4470 / 4514 | 0 % / 52 % |

That is about 24.0 ms of colour work per second before and 13.3 ms after on the real setup (2.4 % and 1.3 % of one core). The relay was never close to its limit, and
this does not change what the lights do; it leaves more room. (The worst single tick in both builds is occasionally one to six milliseconds, probably from other tasks and Wi-Fi
interrupts rather than the colour work; that did not change.)

## What changed

* **Mixing the LEDs (`applyTable`, 1.9 to 2.3x).** A table of the 24 zones' weights replaces a division per LED; the strip is processed in three parts (before the TV, along
  it, after it) so the part along the TV, which is most of it, has no per-LED tail logic; LEDs that are exactly one zone are copied instead of blended. The colour wheel
  conversions for vivid blending no longer call the library's `floorf` / `fmodf` (slow software routines on this chip) but give the same numbers.
* **Blur (2.0x).** One pass over the three colour channels together instead of three separate passes, with the clamped edge handling kept out of the middle of the strip.
* **Easing step (1.7x).** The largest remaining distance is tracked with two running maxima instead of one chain of compares, and the factor is worked out once.
* **Finishing stage (1.7x).** The branches are decided once, not for every channel of every LED (one specialised loop for each combination of the three parts); `floorf` is a
  plain truncation where the value cannot be negative.
* **Saturation (1.15x).** Two LEDs at a time, so the floating point unit works on both chains at once; the arithmetic is unchanged.
* **Dark outside the TV (2.0x).** Two ranges are cleared instead of testing every LED.
* **Reaction boost (8x for the packet).** The factor depends only on the settings and the gap since the last packet, so it is worked out once per packet instead of for each of
  the 72 channels (`leadFactor` / `leadApply`).
* **Brightness, gamma and saturation of the zones (4.5x in the usual case).** With neutral saturation and gamma it is one multiply per channel. The gamma path still needs
  `powf` for every channel and is only 12 % faster.
* **Frame decoding (about 10 %).** The key index is stepped, not worked out with a modulo for every byte.
* **Not recomputing what has not changed (relay).** While a target eases toward a picture that has not changed (several output ticks per Sync Box packet, or a still scene), the
  wanted colours (mixing, saturation, blur, dark outside) are kept from the previous tick. On the real setup 38 % of ticks (52 % with smoothing) skipped that work. It is keyed on
  the picture itself, the settings and the table, so a change of any of them recomputes at once.
* **Three helpers moved into the core** (`adjustZones`, `easeColours`, `packColours`) so they can be tested and measured; they were `static` in `relay.cpp`.

## What did not help

* **`packColours` (rounding to bytes) is unchanged.** Two rewrites that looked better (an unsigned range test, pointer output) measured 20 to 90 % **slower** on the board in the
  same file, although they were faster or equal on the PC; the original ternary clamps compile to conditional moves. The original is back, so it is unchanged.
* **A float-clamp variant of `pack`** was 2.7x slower on the board. For saturation, pre-multiplying `y * (1 - sat)` gained less (79 us, against 65 us for two LEDs at a time with
  the original arithmetic); two LEDs at a time with the pre-multiplied form reached 62 us but changes the arithmetic slightly, so the exact form was kept.
* **The PC is not the board.** On the PC (`bench/results/host-*.json`) `apply_table_*` is only 1.4 to 1.7x faster, `dark_outside_266` is slower (0.6x) and `saturate_266` is not
  faster, because its hardware divider and out-of-order core change what is cheap. Every decision was made on the board's figures.
* **Unchanged on purpose:** `motion_push_at`, `speed_push`, `govee_json_50` and `ddp_header` (a few microseconds a packet or less, and they are not on the per-tick path).
  The gamma path of `adjustZones` stays on `powf` because a lookup table with interpolation is inaccurate near black for gamma below 1, which is exactly where the
  Dark scenes settings work.

## Cost

* Flash: the normal firmware is about 6 KB larger (1,156,841 to 1,163,077 bytes; the specialised loops and a 1 KB weight table). Both images stay well under the 90 % limit.
* RAM: about 1.2 KB more (each target keeps the 24 zone colours its wanted colours were computed from). The finishing stage's buffers are still only allocated while it is
  on; with it on, each target now also keeps one output buffer (12 bytes per LED).

## Dead code

Found with the linker's list of discarded sections, a search for names that occur once, `vulture` on the Python and a check of the page's script and styles. Removed:
`kBoxLead`, `kCols`, `kRows` (constants nothing used), `tableHasStored()`, `wifiSsid()` and an empty `webLoop()` (never called), a helper in `relay.cpp`, an unused function in
`tools/make_golden.py`, and a stale sentence in `docs/RESILIENCE.md` that still pointed at `kBoxLead`. Left in on purpose: `vividBlend()` and `motionReset()`, which only the tests
call (they are the tested form of what the relay does inline). Nothing was found in the page's script or styles.

## One observation

The *Smooth beyond the TV* blend (`smoothOut`) adds nothing for layouts built from a position along the TV (linear or loop): the LEDs beyond the TV already show the corner zone
that the blend settles on. It only has an effect for tables with entries that blend other zones, such as phone-calibrated ones. It was left as it is.

## Reproducing

```
python bench/run_host.py --label after --runs 3 --out bench/results/host-after.json      # the PC (any C++ compiler; zig works)
pio run -e atoms3-bench -t upload                                                         # the board: flash the benchmark firmware ...
python bench/run_board.py --label after --cycles 3 --out bench/results/board-after.json   # ... and read it (then flash the normal firmware again)
python bench/compare.py bench/results/board-before.json bench/results/board-after.json
pio run -e atoms3-tickstat -t upload                                                      # the relay with the tick counter
python bench/tick_stats.py --label after --windows 12 [--smoothing 0.5]
python tools/output_fingerprint.py fancy2wled.local --out after.json                      # then --compare before.json after.json
```

The "before" files are the commit tagged `bench-baseline` (the 0.4.10 code with the helpers moved into the core and the benchmark harness added, nothing optimised).
