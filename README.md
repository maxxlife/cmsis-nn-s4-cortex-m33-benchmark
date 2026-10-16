# CMSIS-NN INT4 (`s4`) Unpacking & Row-Tiling Benchmark on Cortex-M33

Reproducible Zephyr / nRF Connect SDK benchmark and correctness suite evaluating INT4 (`s4`) weight unpacking (`read_and_pad_s4`) and row tiling (`arm_nn_vec_mat_mult_t_s4`) on **Arm Cortex-M33 with DSP extensions** (`ARM_MATH_DSP=1`, no MVE).

## Key Findings (Nordic nRF54LM20B, Cortex-M33 @ 128 MHz, GCC 14.3.0 `-Ofast`)

1. **Eliminating `BFI` and register locking in `read_and_pad_s4`:**
   In stock CMSIS-NN, `read_and_pad_s4()` reads 2 packed bytes via `arm_nn_read_s8x2()`, which uses `memcpy` into an uninitialized 32-bit `int32_t val`. Under GCC 14 (`-O2` / `-Ofast`), this partial write causes the compiler to emit `bfi` (Bit Field Insert) instructions into dedicated live registers across loop iterations (`sl`/`ip`, `r6`/`r7`), increasing register pressure and forcing `rhs_offset` and loop bounds onto the stack in `arm_nn_vec_mat_mult_t_s4()`.
2. **Eliminating redundant byte masks before `SXTB16`:**
   By architectural definition, `SXTB16` only inspects bits `[7:0]` and `[23:16]` of its input register and ignores bits `[15:8]` and `[31:24]`. Reading directly into `uint16_t in` via `memcpy` and computing `int32_t inA = in | (in << 8);` produces **bit-for-bit identical** outputs across all `65,536` possible 16-bit inputs while collapsing 5 instructions (`bfi`, `lsl #8`, `uxtb`, `and #0xff0000`, `orrs`) into a single `orr.w Rd, Rn, Rn, lsl #8`.
3. **Measured Hardware Speedup:**
   - **3-line change in `read_and_pad_s4()` alone (`cmsis_u16_or`):** speeds up stock `arm_nn_vec_mat_mult_t_s4()` by **1.335× to 1.385× (−25.1% to −27.8% CPU cycles)**.
   - **Combined with a spill-free 4-row loop (`tile4_u16_or_nospill`):** reaches **1.357× to 1.408× (−26.3% to −28.9% CPU cycles)**.

```c
__STATIC_FORCEINLINE void read_and_pad_s4(const int8_t *source, int32_t *out1, int32_t *out2)
{
    uint16_t in;
    memcpy(&in, source, sizeof(in));
    int32_t inA = in | (in << 8);

    *out1 = SXTB16_RORn(__sxtb16(inA << 4), 4);
    *out2 = SXTB16_RORn(__sxtb16(inA), 4);
}
Hardware Benchmark Summary (DWT->CYCCNT median of 31 warmed runs, SRAM, IRQs masked)
M × K	Offset	cmsis_stock	cmsis_u16_or	tile2_u16_or	tile4_u16_or	tile4_u16_or_nospill	stock / cmsis_u16_or	stock / tile4_nospill
64 × 64	0	25,064	18,772	20,623	19,320	18,473	1.335×	1.357×
64 × 64	37	25,062	18,772	20,623	19,320	18,473	1.335×	1.357×
128 × 256	0	180,506	131,076	145,647	135,352	129,049	1.377×	1.399×
128 × 256	37	180,503	131,076	145,647	135,352	129,049	1.377×	1.399×
128 × 512	0	354,591	256,004	284,911	264,376	251,929	1.385×	1.408×
128 × 512	37	354,573	256,004	284,911	264,376	251,929	1.385×	1.407×
256 × 256	0	360,852	261,988	291,247	270,648	258,041	1.377×	1.398×
256 × 256	37	360,837	261,988	291,247	270,648	258,041	1.377×	1.398×
256 × 512	0	709,006	511,844	569,775	528,696	503,801	1.385×	1.407×
256 × 512	37	708,974	511,844	569,775	528,696	503,801	1.385×	1.407×
Full raw UART logs (RAW + RESULT lines) and compiler metadata are available in results/.
Correctness Verification
Every run executes:
UNPACK_U16 (65,536 inputs): Exhaustive on-target check of all 65,536 16-bit values (0x0000..0xFFFF), verifying that stock read_and_pad_s4, u16_mask (Variant B), and u16_or (Variant C) return identical *out1 and *out2 32-bit words.
CORRECTNESS (1,152 matrix cases × 10 kernels): Tests 16 matrix shapes (including odd dimensions and tail rows), 4 input_offset values (0, -127, 128, 37), null/non-null bias, 3 requantization multiplier/shift/clamp configurations, 3 data patterns, and byte-misaligned input/weight pointers against an independent scalar reference.
Building & Running
1. Host AddressSanitizer Test (Linux GCC)
code
Bash
python3 scripts/host_test.py --cmsis-nn /path/to/cmsis-nn
2. Target Build & Flash (nRF Connect SDK v3.5.0-preview3 / Zephyr)
code
Bash
cd /path/to/ncs/zephyr
west build -p always -b nrf54lm20dk/nrf54lm20b/cpuapp /path/to/this/repo
python3 /path/to/this/repo/scripts/collect_build.py build --out ./evidence
west flash
3. Parse UART Log into Markdown Summary
code
Bash
python3 scripts/summarize.py results/board_v04_nrf54lm20b.log
