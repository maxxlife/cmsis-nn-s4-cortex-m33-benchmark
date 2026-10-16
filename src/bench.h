/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S4_ROWTILING_BENCH_H
#define S4_ROWTILING_BENCH_H
#include <stdint.h>

struct bench_args {
    const int8_t *x;
    const int8_t *weights;
    const int32_t *bias;
    int8_t *out;
    int32_t rows;
    int32_t cols;
    int32_t input_offset;
    int32_t output_offset;
    int32_t multiplier;
    int32_t shift;
    int32_t activation_min;
    int32_t activation_max;
};

typedef void (*bench_kernel_fn)(const struct bench_args *);
int bench_check_unpack_all_u16(uint32_t *bad_input);
void bench_reference(const struct bench_args *p);
void bench_cmsis(const struct bench_args *p);
void bench_cmsis_u16_or(const struct bench_args *p);
void bench_tile1(const struct bench_args *p);
void bench_tile2(const struct bench_args *p);
void bench_tile2_u16_mask(const struct bench_args *p);
void bench_tile2_u16_or(const struct bench_args *p);
void bench_tile4(const struct bench_args *p);
void bench_tile4_u16_mask(const struct bench_args *p);
void bench_tile4_u16_or(const struct bench_args *p);
void bench_tile4_u16_or_nospill(const struct bench_args *p);
#endif
