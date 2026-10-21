# CMSIS-NN INT4 (`s4`) Unpacking, Row-Tiling & RRAM Cache Benchmark on Cortex-M33

Reproducible Zephyr / nRF Connect SDK benchmark and verification suite evaluating INT4 (`s4`) weight unpacking (`read_and_pad_s4`), row tiling (`arm_nn_vec_mat_mult_t_s4`), and **SRAM vs. RRAM (C-AHB 2-way cache) weight streaming** on **Nordic nRF54LM20B (Arm Cortex-M33 @ 128 MHz, `ARM_MATH_DSP=1`, no MVE)**.

- **Upstream Arm CMSIS-NN PR (Merged):** [ARM-software/CMSIS-NN#250](https://github.com/ARM-software/CMSIS-NN/pull/250) (Merged in [`7b515b9ded98`](https://github.com/ARM-software/CMSIS-NN/commit/7b515b9ded985bc429173525ac24cc757a961c7f))
- **Nordic DevZone Discussion:** [DevZone #129438 — CMSIS-NN INT4 (`s4`) speedup on nRF54LM20B & RRAM benchmarking](https://devzone.nordicsemi.com/f/nordic-q-a/129438/cmsis-nn-int4-s4-1-385x-speedup-on-nrf54lm20b-ncs-v3-5-0-preview3-upstream-pr-rram-benchmarking-question)

---

## 1. Summary of Findings

### A. Eliminating `BFI` Register Locking & Redundant Masks in `read_and_pad_s4`

[Merged PR #250](https://github.com/ARM-software/CMSIS-NN/pull/250)

1. **Partial-register `BFI` trap:** In stock CMSIS-NN, `read_and_pad_s4()` calls `arm_nn_read_s8x2()`, which copies 2 bytes via `memcpy` into an uninitialized 32-bit `int32_t val`. Under GCC 14 (`-O2` / `-Ofast`), this partial write causes the compiler to emit `bfi` (Bit Field Insert) instructions into dedicated live registers across loop iterations (`sl`/`ip`, `r6`/`r7`), exhausting the 14 general-purpose registers on Cortex-M33 and forcing `rhs_offset` and loop bounds onto the stack in `arm_nn_vec_mat_mult_t_s4()`.

2. **Redundant byte masks before `SXTB16`:** By architectural definition, `SXTB16` only inspects bits `[7:0]` and `[23:16]` of its input operand and ignores bits `[15:8]` and `[31:24]`. Reading directly into `uint16_t in` via `memcpy` and computing `int32_t inA = in | (in << 8);` produces **100% bit-exact outputs** across all `65,536` possible 16-bit inputs while collapsing 5 instructions (`bfi`, `lsl #8`, `uxtb`, `and #0xff0000`, `orrs`) into a single `orr.w Rd, Rn, Rn, lsl #8`.

```c
__STATIC_FORCEINLINE void read_and_pad_s4(const int8_t *source, int32_t *out1, int32_t *out2)
{
    uint16_t in;
    memcpy(&in, source, sizeof(in));
    int32_t inA = in | (in << 8);

    *out1 = SXTB16_RORn(__sxtb16(inA << 4), 4);
    *out2 = SXTB16_RORn(__sxtb16(inA), 4);
}
```

### B. RRAM (NVM) Streaming & C-AHB 2-Way Set-Associative Cache Aliasing on nRF54LM20B

On nRF54LM20B, constant weights in RRAM (`0x0000e3c0`) are fetched over the C-AHB bus through `NRF_ICACHE` (a 2-way set-associative cache).

Stock `arm_nn_vec_mat_mult_t_s4()` pairs row `i` with row `i + rhs_rows/2` (`rhs_offset = 32,768 bytes = 0x8000` for a `256 × 512` matrix). Because `0x8000` is a large power-of-2 stride, the two concurrent weight streams alias to the same cache sets / RRAM pages and incur `+135k..+148k` extra RRAM wait cycles compared to SRAM.

Visiting adjacent rows (`tile2_u16_or` and `tile4_u16_or_nospill`, stride = 256 bytes) maps the active rows into distinct cache sets, cutting the RRAM streaming penalty in half (`+59k..+80k` cycles) and achieving a **1.468× total speedup** (`−31.9% CPU cycles`, saving `273,273` cycles) when executing directly from RRAM.

---

## 2. Hardware Benchmark Results

**nRF54LM20B @ 128 MHz, GCC 14.3.0 `-Ofast`**

### Table 1: Weights in RRAM (`0x0000e3c0`, C-AHB ICACHE=1) vs. SRAM (`0x20001104`, S-AHB)

| M × K | Weights Size | SRAM stock | RRAM stock | RRAM cmsis_u16_or (PR #250) | RRAM tile2_C (adj. 2-row) | RRAM tile4_C_nospill (adj. 4-row) | RRAM stock / cmsis_u16_or | RRAM stock / tile4_C_nospill |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 × 64 | 2 KB | 25,062 | 26,132 | 19,873 | 20,633 | 19,245 | 1.315× | 1.358× |
| 128 × 256 | 16 KB | 180,503 | 217,829 | 167,073 | 160,438 | 149,243 | 1.304× | 1.460× |
| 256 × 256 | 32 KB | 360,837 | 435,242 | 333,753 | 320,786 | 298,363 | 1.304× | 1.459× |
| 128 × 512 | 32 KB | 354,573 | 429,033 | 324,045 | 314,450 | 292,251 | 1.324× | 1.468× |
| 256 × 512 | 64 KB | 708,974 | 857,652 | 647,697 | 628,810 | 584,379 | 1.324× | 1.468× |

Tuned `NRF_RRAMC->POWER.LOWPOWERCONFIG = 3 / STANDBYCONFIG = 0` vs. default `LOWPOWERCONFIG = 0x20` yields identical cycle counts during continuous matrix streaming; full logs in `results/board_v05_sram_and_rram_nrf54lm20b.log`.

### Table 2: Weights in SRAM — Ablation Study

DWT->CYCCNT medians of 31 warmed runs.

| M × K | Offset | cmsis_stock | cmsis_u16_or (PR #250) | tile2 (A) | tile2_B (mask) | tile2_C (or) | tile4 (A) | tile4_B (mask) | tile4_C (or) | tile4_C_nospill | stock / cmsis_u16_or | stock / tile4_C_nospill |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 × 64 | 0 | 25,064 | 18,774 | 25,267 | 23,025 | 20,623 | 25,878 | 21,254 | 19,320 | 18,473 | 1.335× | 1.357× |
| 128 × 256 | 0 | 180,506 | 131,078 | 182,579 | 165,809 | 145,647 | 185,334 | 151,510 | 135,352 | 129,049 | 1.377× | 1.399× |
| 128 × 512 | 0 | 354,591 | 256,006 | 358,707 | 325,553 | 284,911 | 363,510 | 296,918 | 264,376 | 251,929 | 1.385× | 1.408× |
| 256 × 256 | 0 | 360,852 | 261,990 | 365,107 | 331,569 | 291,247 | 370,614 | 302,966 | 270,648 | 258,041 | 1.377× | 1.398× |
| 256 × 512 | 0 | 709,006 | 511,846 | 717,363 | 651,057 | 569,775 | 726,966 | 593,782 | 528,696 | 503,801 | 1.385× | 1.407× |

---

## 3. Correctness Verification

Every run executes:

- **UNPACK_U16 (65,536 inputs):** Exhaustive on-target check of all 65,536 16-bit values (`0x0000..0xFFFF`), verifying that stock `read_and_pad_s4`, `u16_mask` (Variant B), and `u16_or` (Variant C) return identical `*out1` and `*out2` 32-bit words.

- **CORRECTNESS (1,152 matrix cases × 10 kernels):** Tests 16 matrix shapes (including odd dimensions and tail rows), 4 `input_offset` values (`0`, `-127`, `128`, `37`), null/non-null bias, 3 requantization multiplier/shift/clamp configurations, 3 data patterns, and byte-misaligned input/weight pointers against an independent scalar reference.

---

## 4. Building & Running

### Host AddressSanitizer Test (Linux GCC)

```bash
python3 scripts/host_test.py --cmsis-nn /path/to/cmsis-nn
```

### Target Build & Flash (nRF Connect SDK v3.5.0-preview3 / Zephyr)

```bash
cd /path/to/ncs/zephyr

west build -p always \
  -b nrf54lm20dk/nrf54lm20b/cpuapp \
  /path/to/this/repo

python3 /path/to/this/repo/scripts/collect_build.py build --out ./evidence

west flash
```
