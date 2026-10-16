/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <string.h>
#include "bench.h"

#ifdef BENCH_HOST_TEST
#include <stdio.h>
#define PRINT printf
#else
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <cmsis_core.h>
#include "arm_nnsupportfunctions.h"
#include "build_info.h"
#define PRINT printk
#endif

#define MAX_ROWS 256
#define MAX_COLS 512
#define GUARD 4
#define SAMPLES 31
#define KERNEL_COUNT 10

static int8_t x_storage[MAX_COLS + 4] __attribute__((aligned(4)));
static int8_t weight_storage[MAX_ROWS * MAX_COLS / 2 + 4] __attribute__((aligned(4)));
static int32_t biases[MAX_ROWS];
static int8_t out_storage[MAX_ROWS + 2 * GUARD];
static int8_t expected[MAX_ROWS];
static uint32_t samples[KERNEL_COUNT][SAMPLES];
static volatile uint32_t output_sink;

static const struct {
    const char *name;
    bench_kernel_fn run;
} kernels[KERNEL_COUNT] = {
    {"cmsis_stock", bench_cmsis},
    {"cmsis_u16_or", bench_cmsis_u16_or},
    {"tile1", bench_tile1},
    {"tile2", bench_tile2},
    {"tile2_u16_mask", bench_tile2_u16_mask},
    {"tile2_u16_or", bench_tile2_u16_or},
    {"tile4", bench_tile4},
    {"tile4_u16_mask", bench_tile4_u16_mask},
    {"tile4_u16_or", bench_tile4_u16_or},
    {"tile4_u16_or_nospill", bench_tile4_u16_or_nospill},
};

static uint32_t random_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static struct bench_args make_args(int rows, int cols)
{
    const struct bench_args p = {
        .x = x_storage, .weights = weight_storage, .bias = biases,
        .out = out_storage + GUARD, .rows = rows, .cols = cols,
        .input_offset = 0, .output_offset = 0,
        .multiplier = 0x40000000, .shift = -8,
        .activation_min = -128, .activation_max = 127,
    };
    return p;
}

static void fill_inputs(struct bench_args *p, uint32_t seed, int pattern, int misaligned)
{
    p->x = x_storage + misaligned;
    p->weights = weight_storage + misaligned;
    int8_t *x = x_storage + misaligned;
    int8_t *w = weight_storage + misaligned;
    uint32_t state = seed ? seed : 1;
    for (int i = 0; i < p->cols; ++i) {
        x[i] = pattern == 1 ? (i & 1 ? 127 : -128) :
            pattern == 2 ? 0 : (int8_t)(random_next(&state) >> 24);
    }
    for (int i = 0; i < (p->rows * p->cols + 1) / 2; ++i) {
        w[i] = pattern == 1 ? 0x78 : pattern == 2 ? 0 :
            (int8_t)(random_next(&state) >> 24);
    }
    for (int i = 0; i < p->rows; ++i) {
        biases[i] = (int32_t)(random_next(&state) % 65537) - 32768;
    }
}

static void prepare_expected(const struct bench_args *p)
{
    struct bench_args reference = *p;
    reference.out = expected;
    bench_reference(&reference);
}

static int verify(const struct bench_args *p, int which)
{
    for (int i = 0; i < p->rows; ++i) {
        if (p->out[i] != expected[i]) {
            PRINT("FAIL,kernel=%s,rows=%d,cols=%d,offset=%d,row=%d,got=%d,expected=%d\n",
                  kernels[which].name, (int)p->rows, (int)p->cols,
                  (int)p->input_offset, i, p->out[i], expected[i]);
            return 0;
        }
    }
    for (int i = 0; i < GUARD; ++i) {
        if ((uint8_t)out_storage[i] != 0x5a ||
            (uint8_t)out_storage[GUARD + p->rows + i] != 0x5a) {
            PRINT("FAIL,output_guard,kernel=%s\n", kernels[which].name);
            return 0;
        }
    }
    return 1;
}

static int correctness_suite(void)
{
    uint32_t bad_u16 = 0;
    if (!bench_check_unpack_all_u16(&bad_u16)) {
        PRINT("FAIL,unpack_u16,input=0x%04x\n", (unsigned)bad_u16);
        return 0;
    }
    PRINT("UNPACK_U16,PASS,inputs=65536\n");

    static const int shapes[][2] = {
        {1, 1}, {2, 3}, {3, 2}, {4, 4}, {5, 7}, {7, 8},
        {8, 15}, {9, 16}, {17, 31}, {17, 32}, {64, 64},
        {128, 256}, {256, 256}, {128, 512}, {256, 512}, {255, 257},
    };
    static const int offsets[] = {0, -127, 128, 37};
    unsigned cases = 0;
    for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); ++s) {
        for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
            for (int use_bias = 0; use_bias < 2; ++use_bias) {
                for (int q = 0; q < 3; ++q) {
                    for (int pattern = 0; pattern < 3; ++pattern) {
                        struct bench_args p = make_args(shapes[s][0], shapes[s][1]);
                        fill_inputs(&p, 0x142857u + cases * 7919u, pattern, cases & 1);
                        p.input_offset = offsets[o];
                        p.bias = use_bias ? biases : NULL;
                        if (q == 1) {
                            p.multiplier = 0x60000000;
                            p.shift = -3;
                            p.output_offset = -9;
                            p.activation_min = -17;
                            p.activation_max = 23;
                        } else if (q == 2) {
                            p.shift = 1;
                            p.output_offset = 17;
                        }
                        prepare_expected(&p);
                        for (int k = 0; k < KERNEL_COUNT; ++k) {
                            memset(out_storage, 0x5a, sizeof(out_storage));
                            kernels[k].run(&p);
                            if (!verify(&p, k)) {
                                PRINT("FAIL,case=%u,pattern=%d,quant=%d,bias=%d\n",
                                      cases, pattern, q, use_bias);
                                return 0;
                            }
                        }
                        ++cases;
                    }
                }
            }
        }
        PRINT("CHECK,rows=%d,cols=%d,PASS\n", shapes[s][0], shapes[s][1]);
    }
    PRINT("CORRECTNESS,PASS,cases=%u,kernels=%d\n", cases, KERNEL_COUNT);
    return 1;
}

#ifndef BENCH_HOST_TEST
static int enable_cycle_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    __DSB();
    __ISB();
    if (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) {
        return 0;
    }
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    __DSB();
    __ISB();
    const uint32_t start = DWT->CYCCNT;
    for (volatile unsigned i = 0; i < 100; ++i) {
        __NOP();
    }
    return DWT->CYCCNT != start;
}

static uint32_t time_kernel(bench_kernel_fn run, const struct bench_args *p)
{
    unsigned int key = irq_lock();
    __DSB();
    __ISB();
    __asm__ volatile("" ::: "memory");
    const uint32_t begin = DWT->CYCCNT;
    __asm__ volatile("" ::: "memory");
    run(p);
    __asm__ volatile("" ::: "memory");
    __DSB();
    const uint32_t end = DWT->CYCCNT;
    __asm__ volatile("" ::: "memory");
    irq_unlock(key);
    return end - begin;
}

static void empty_kernel(const struct bench_args *p)
{
    (void)p;
    __asm__ volatile("" ::: "memory");
}

static void sort_samples(uint32_t *v)
{
    for (unsigned i = 1; i < SAMPLES; ++i) {
        const uint32_t x = v[i];
        unsigned j = i;
        while (j && v[j - 1] > x) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = x;
    }
}

static uint32_t checksum(const struct bench_args *p)
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < p->rows; ++i) {
        hash = (hash ^ (uint8_t)p->out[i]) * 16777619u;
    }
    return hash;
}

static int benchmark_shape(int rows, int cols, int offset)
{
    struct bench_args p = make_args(rows, cols);
    fill_inputs(&p, 0x98765432u + rows * 123u + cols, 0, 0);
    p.input_offset = offset;
    prepare_expected(&p);
    memset(out_storage, 0x5a, sizeof(out_storage));
    for (int k = 0; k < KERNEL_COUNT; ++k) {
        for (int w = 0; w < 5; ++w) {
            kernels[k].run(&p);
        }
    }
    for (int s = 0; s < SAMPLES; ++s) {
        for (int j = 0; j < KERNEL_COUNT; ++j) {
            const int k = (s + j) % KERNEL_COUNT;
            kernels[k].run(&p);
            samples[k][s] = time_kernel(kernels[k].run, &p);
            if (!verify(&p, k)) {
                return 0;
            }
            output_sink ^= checksum(&p);
        }
    }
    for (int k = 0; k < KERNEL_COUNT; ++k) {
        PRINT("RAW,%d,%d,%d,%s", rows, cols, offset, kernels[k].name);
        for (int s = 0; s < SAMPLES; ++s) {
            PRINT(",%u", (unsigned)samples[k][s]);
        }
        PRINT("\n");
        sort_samples(samples[k]);
        PRINT("RESULT,%d,%d,%d,%s,%u,%u,%u,%08x\n", rows, cols, offset,
              kernels[k].name, (unsigned)samples[k][0],
              (unsigned)samples[k][SAMPLES / 2], (unsigned)samples[k][SAMPLES - 1],
              (unsigned)checksum(&p));
    }
    return 1;
}
#endif

int main(void)
{
#ifdef BENCH_HOST_TEST
    PRINT("HOST_TEST: scalar CMSIS-NN + software model of DSP operations; no timing claims\n");
#else
    k_msleep(2000);
    PRINT("\nS4_ROWTILING,v0.4\n");
    PRINT("BOARD,%s\nCMSIS_NN,%s\nCOMPILER,%s\n", BENCH_BOARD, BENCH_CMSIS_REV, __VERSION__);
    PRINT("TIMING,DWT_CYCCNT,raw_cycles,IRQs_masked,warm_code,data_in_SRAM\n");
    PRINT("FEATURES,ARM_MATH_DSP=1,MVE=0\n");
#ifdef CMSIS_NN_USE_SINGLE_ROUNDING
    PRINT("REQUANTIZE,single_rounding\n");
#else
    PRINT("REQUANTIZE,double_rounding\n");
#endif
    PRINT("CORRECTNESS,START\n");
#endif
    if (!correctness_suite()) {
        return 1;
    }
#ifdef BENCH_HOST_TEST
    return 0;
#else
    if (!enable_cycle_counter()) {
        PRINT("ERROR,DWT_CYCCNT_unavailable; no performance results\n");
        return 1;
    }
    struct bench_args dummy = make_args(1, 4);
    uint32_t overhead = UINT32_MAX;
    for (int i = 0; i < SAMPLES; ++i) {
        const uint32_t value = time_kernel(empty_kernel, &dummy);
        overhead = MIN(overhead, value);
    }
    PRINT("TIMER_OVERHEAD,min_cycles=%u,not_subtracted\n", (unsigned)overhead);
    PRINT("COLUMNS,rows,cols,input_offset,kernel,min_cycles,median_cycles,max_cycles,checksum\n");
    const int shapes[][2] = {{64, 64}, {128, 256}, {256, 256}, {128, 512}, {256, 512}};
    for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); ++s) {
        if (!benchmark_shape(shapes[s][0], shapes[s][1], 0) ||
            !benchmark_shape(shapes[s][0], shapes[s][1], 37)) {
            return 1;
        }
    }
    PRINT("DONE,PASS,sink=%08x\n", (unsigned)output_sink);
    return 0;
#endif
}
