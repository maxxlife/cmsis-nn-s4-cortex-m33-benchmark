/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Experiment v0.4:
 *   1. cmsis_u16_or: stock arm_nn_vec_mat_mult_t_s4 with ONLY read_and_pad_s4
 *      replaced by read_and_pad_s4_u16_or (4-line upstream helper patch).
 *   2. tile4_u16_or_nospill: 4-row tiling with row_bytes offset addressing and
 *      pointer-end loop bound to eliminate stack loads in the hot loop.
 */
#include <string.h>
#include "bench.h"
#include "arm_nnsupportfunctions.h"
#ifdef BENCH_HOST_TEST
#include "portable_dsp.h"
#else
#if !defined(ARM_MATH_DSP) || defined(ARM_MATH_MVEI)
#error "Expected the Cortex-M DSP path, without MVE"
#endif

__STATIC_FORCEINLINE void read_and_pad_s4_u16_mask(const int8_t *source,
                                                   int32_t *out1,
                                                   int32_t *out2)
{
    uint16_t packed;
    memcpy(&packed, source, sizeof(packed));
    int32_t inA = ((int32_t)packed & 0x00FF) | (((int32_t)packed & 0xFF00) << 8);

    *out1 = SXTB16_RORn(__sxtb16(inA << 4), 4);
    *out2 = SXTB16_RORn(__sxtb16(inA), 4);
}

__STATIC_FORCEINLINE void read_and_pad_s4_u16_or(const int8_t *source,
                                                 int32_t *out1,
                                                 int32_t *out2)
{
    uint16_t packed;
    memcpy(&packed, source, sizeof(packed));
    uint32_t inA = (uint32_t)packed | ((uint32_t)packed << 8);

    *out1 = SXTB16_RORn(__sxtb16((int32_t)(inA << 4)), 4);
    *out2 = SXTB16_RORn(__sxtb16((int32_t)inA), 4);
}
#endif

int bench_check_unpack_all_u16(uint32_t *bad_input)
{
    for (uint32_t v = 0; v < 65536u; ++v) {
        volatile uint16_t hv = (uint16_t)v;
        const uint16_t cur = hv;
        int8_t bytes[2];
        memcpy(bytes, &cur, sizeof(bytes));

        int32_t a1, a2, b1, b2, c1, c2;
        read_and_pad_s4(bytes, &a1, &a2);
        read_and_pad_s4_u16_mask(bytes, &b1, &b2);
        read_and_pad_s4_u16_or(bytes, &c1, &c2);

        if (a1 != b1 || a2 != b2 || a1 != c1 || a2 != c2) {
            if (bad_input) {
                *bad_input = v;
            }
            return 0;
        }
    }
    return 1;
}

static inline int8_t finish(int32_t acc, const struct bench_args *p)
{
    acc = arm_nn_requantize(acc, p->multiplier, p->shift);
    acc += p->output_offset;
    acc = MAX(acc, p->activation_min);
    acc = MIN(acc, p->activation_max);
    return (int8_t)acc;
}

static void scalar_rows(const struct bench_args *p, int32_t first)
{
    for (int32_t r = first; r < p->rows; ++r) {
        int32_t acc = p->bias ? p->bias[r] : 0;
        for (int32_t c = 0; c < p->cols; ++c) {
            const int32_t index = r * p->cols + c;
            const uint32_t byte = (uint8_t)p->weights[index / 2];
            const int32_t nibble = (byte >> (4 * (index & 1))) & 15;
            const int32_t weight = nibble < 8 ? nibble : nibble - 16;
            acc += ((int32_t)p->x[c] + p->input_offset) * weight;
        }
        p->out[r] = finish(acc, p);
    }
}

void bench_reference(const struct bench_args *p)
{
    scalar_rows(p, 0);
}

void bench_cmsis(const struct bench_args *p)
{
    (void)arm_nn_vec_mat_mult_t_s4(
        p->x, p->weights, p->bias, p->out, p->input_offset,
        p->output_offset, p->multiplier, p->shift, p->cols, p->rows,
        p->activation_min, p->activation_max);
}

#define ROW_MAC_A(pointer, accumulator) do { \
    int32_t weight_even, weight_odd; \
    read_and_pad_s4((pointer), &weight_even, &weight_odd); \
    (pointer) += 2; \
    (accumulator) = SMLAD(x_even, weight_even, (accumulator)); \
    (accumulator) = SMLAD(x_odd, weight_odd, (accumulator)); \
} while (0)

#define ROW_MAC_B(pointer, accumulator) do { \
    int32_t weight_even, weight_odd; \
    read_and_pad_s4_u16_mask((pointer), &weight_even, &weight_odd); \
    (pointer) += 2; \
    (accumulator) = SMLAD(x_even, weight_even, (accumulator)); \
    (accumulator) = SMLAD(x_odd, weight_odd, (accumulator)); \
} while (0)

#define ROW_MAC_C(pointer, accumulator) do { \
    int32_t weight_even, weight_odd; \
    read_and_pad_s4_u16_or((pointer), &weight_even, &weight_odd); \
    (pointer) += 2; \
    (accumulator) = SMLAD(x_even, weight_even, (accumulator)); \
    (accumulator) = SMLAD(x_odd, weight_odd, (accumulator)); \
} while (0)

#define LOAD_X() \
    int32_t x_word = arm_nn_read_s8x4_ia(&xp); \
    int32_t x_even = SXTAB16(offsets, x_word); \
    int32_t x_odd = SXTAB16_RORn(offsets, x_word, 8)

static inline uint32_t pack_offset(int32_t offset)
{
    const uint32_t half = (uint16_t)offset;
    return half | (half << 16);
}

void bench_tile1(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    for (int32_t r = 0; r < p->rows; ++r) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_A(w0, acc0);
        }
        p->out[r] = finish(acc0, p);
    }
}

void bench_tile2(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    int32_t r = 0;
    for (; r + 2 <= p->rows; r += 2) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        const int8_t *w1 = w0 + p->cols / 2;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_A(w0, acc0);
            ROW_MAC_A(w1, acc1);
        }
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
    }
    scalar_rows(p, r);
}

void bench_tile2_u16_mask(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    int32_t r = 0;
    for (; r + 2 <= p->rows; r += 2) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        const int8_t *w1 = w0 + p->cols / 2;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_B(w0, acc0);
            ROW_MAC_B(w1, acc1);
        }
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
    }
    scalar_rows(p, r);
}

void bench_tile2_u16_or(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    int32_t r = 0;
    for (; r + 2 <= p->rows; r += 2) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        const int8_t *w1 = w0 + p->cols / 2;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_C(w0, acc0);
            ROW_MAC_C(w1, acc1);
        }
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
    }
    scalar_rows(p, r);
}

void bench_tile4(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    int32_t r = 0;
    for (; r + 4 <= p->rows; r += 4) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        const int8_t *w1 = w0 + p->cols / 2;
        const int8_t *w2 = w1 + p->cols / 2;
        const int8_t *w3 = w2 + p->cols / 2;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        int32_t acc2 = p->bias ? p->bias[r + 2] : 0;
        int32_t acc3 = p->bias ? p->bias[r + 3] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_A(w0, acc0);
            ROW_MAC_A(w1, acc1);
            ROW_MAC_A(w2, acc2);
            ROW_MAC_A(w3, acc3);
        }
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
        p->out[r + 2] = finish(acc2, p);
        p->out[r + 3] = finish(acc3, p);
    }
    scalar_rows(p, r);
}

void bench_tile4_u16_mask(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    int32_t r = 0;
    for (; r + 4 <= p->rows; r += 4) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        const int8_t *w1 = w0 + p->cols / 2;
        const int8_t *w2 = w1 + p->cols / 2;
        const int8_t *w3 = w2 + p->cols / 2;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        int32_t acc2 = p->bias ? p->bias[r + 2] : 0;
        int32_t acc3 = p->bias ? p->bias[r + 3] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_B(w0, acc0);
            ROW_MAC_B(w1, acc1);
            ROW_MAC_B(w2, acc2);
            ROW_MAC_B(w3, acc3);
        }
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
        p->out[r + 2] = finish(acc2, p);
        p->out[r + 3] = finish(acc3, p);
    }
    scalar_rows(p, r);
}

void bench_tile4_u16_or(const struct bench_args *p)
{
    if (p->cols % 4) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    int32_t r = 0;
    for (; r + 4 <= p->rows; r += 4) {
        const int8_t *xp = p->x;
        const int8_t *w0 = p->weights + r * (p->cols / 2);
        const int8_t *w1 = w0 + p->cols / 2;
        const int8_t *w2 = w1 + p->cols / 2;
        const int8_t *w3 = w2 + p->cols / 2;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        int32_t acc2 = p->bias ? p->bias[r + 2] : 0;
        int32_t acc3 = p->bias ? p->bias[r + 3] : 0;
        for (int32_t c = 0; c < p->cols; c += 4) {
            LOAD_X();
            ROW_MAC_C(w0, acc0);
            ROW_MAC_C(w1, acc1);
            ROW_MAC_C(w2, acc2);
            ROW_MAC_C(w3, acc3);
        }
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
        p->out[r + 2] = finish(acc2, p);
        p->out[r + 3] = finish(acc3, p);
    }
    scalar_rows(p, r);
}

void bench_tile4_u16_or_nospill(const struct bench_args *p)
{
    if (p->cols % 4 || p->cols <= 0) {
        scalar_rows(p, 0);
        return;
    }
    const uint32_t offsets = pack_offset(p->input_offset);
    const int32_t row_bytes = p->cols >> 1;
    int32_t r = 0;
    for (; r + 4 <= p->rows; r += 4) {
        const int8_t *xp = p->x;
        const int8_t *const xp_end = xp + p->cols;
        const int8_t *w0 = p->weights + r * row_bytes;
        const int8_t *w2 = w0 + 2 * row_bytes;
        int32_t acc0 = p->bias ? p->bias[r] : 0;
        int32_t acc1 = p->bias ? p->bias[r + 1] : 0;
        int32_t acc2 = p->bias ? p->bias[r + 2] : 0;
        int32_t acc3 = p->bias ? p->bias[r + 3] : 0;
        do {
            LOAD_X();
            int32_t we, wo;
            read_and_pad_s4_u16_or(w0, &we, &wo);
            acc0 = SMLAD(x_even, we, acc0);
            acc0 = SMLAD(x_odd, wo, acc0);

            read_and_pad_s4_u16_or(w0 + row_bytes, &we, &wo);
            w0 += 2;
            acc1 = SMLAD(x_even, we, acc1);
            acc1 = SMLAD(x_odd, wo, acc1);

            read_and_pad_s4_u16_or(w2, &we, &wo);
            acc2 = SMLAD(x_even, we, acc2);
            acc2 = SMLAD(x_odd, wo, acc2);

            read_and_pad_s4_u16_or(w2 + row_bytes, &we, &wo);
            w2 += 2;
            acc3 = SMLAD(x_even, we, acc3);
            acc3 = SMLAD(x_odd, wo, acc3);
        } while (xp < xp_end);
        p->out[r] = finish(acc0, p);
        p->out[r + 1] = finish(acc1, p);
        p->out[r + 2] = finish(acc2, p);
        p->out[r + 3] = finish(acc3, p);
    }
    scalar_rows(p, r);
}

#define read_and_pad_s4 read_and_pad_s4_u16_or
#define arm_nn_vec_mat_mult_t_s4 arm_nn_vec_mat_mult_t_s4_u16_or
#include "arm_nn_vec_mat_mult_t_s4.c"
#undef arm_nn_vec_mat_mult_t_s4
#undef read_and_pad_s4

void bench_cmsis_u16_or(const struct bench_args *p)
{
    (void)arm_nn_vec_mat_mult_t_s4_u16_or(
        p->x, p->weights, p->bias, p->out, p->input_offset,
        p->output_offset, p->multiplier, p->shift, p->cols, p->rows,
        p->activation_min, p->activation_max);
}
