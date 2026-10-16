/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only model of the DSP operations; never used in the Zephyr build.
 * Tests loop/indexing correctness. It does NOT validate Arm machine code. */
#ifndef PORTABLE_DSP_H
#define PORTABLE_DSP_H
#include <stdint.h>
#include <string.h>

static inline int32_t pack_halves(int32_t lo, int32_t hi)
{
    return (int32_t)((uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16));
}
static inline int32_t host_sxtb16(uint32_t word)
{
    return pack_halves((int8_t)(word & 0xFFu), (int8_t)((word >> 16) & 0xFFu));
}
static inline int32_t host_sxtb16_ror(uint32_t word, unsigned n)
{
    return host_sxtb16((word >> n) | (word << (32 - n)));
}
static inline int32_t host_sxtab16(uint32_t offsets, uint32_t word)
{
    return pack_halves((int16_t)offsets + (int8_t)word,
                       (int16_t)(offsets >> 16) + (int8_t)(word >> 16));
}
static inline int32_t host_sxtab16_ror(uint32_t offsets, uint32_t word, unsigned n)
{
    return host_sxtab16(offsets, (word >> n) | (word << (32 - n)));
}
static inline int32_t host_smlad(uint32_t a, uint32_t b, int32_t sum)
{
    return sum + (int16_t)a * (int16_t)b +
        (int16_t)(a >> 16) * (int16_t)(b >> 16);
}
static inline int32_t signed_nibble(unsigned x)
{
    x &= 15;
    return x < 8 ? (int32_t)x : (int32_t)x - 16;
}
static inline void read_and_pad_s4(const int8_t *w, int32_t *even, int32_t *odd)
{
    const uint8_t b0 = (uint8_t)w[0], b1 = (uint8_t)w[1];
    *even = pack_halves(signed_nibble(b0), signed_nibble(b1));
    *odd = pack_halves(signed_nibble(b0 >> 4), signed_nibble(b1 >> 4));
}
static inline void read_and_pad_s4_u16_mask(const int8_t *source, int32_t *out1, int32_t *out2)
{
    uint16_t packed;
    memcpy(&packed, source, sizeof(packed));
    uint32_t inA = ((uint32_t)packed & 0x00FFu) | (((uint32_t)packed & 0xFF00u) << 8);
    *out1 = host_sxtb16_ror((uint32_t)host_sxtb16(inA << 4), 4);
    *out2 = host_sxtb16_ror((uint32_t)host_sxtb16(inA), 4);
}
static inline void read_and_pad_s4_u16_or(const int8_t *source, int32_t *out1, int32_t *out2)
{
    uint16_t packed;
    memcpy(&packed, source, sizeof(packed));
    uint32_t inA = (uint32_t)packed | ((uint32_t)packed << 8);
    *out1 = host_sxtb16_ror((uint32_t)host_sxtb16(inA << 4), 4);
    *out2 = host_sxtb16_ror((uint32_t)host_sxtb16(inA), 4);
}
#define SXTAB16 host_sxtab16
#define SXTAB16_RORn host_sxtab16_ror
#define SMLAD host_smlad
#endif
