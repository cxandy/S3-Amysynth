#pragma once

#include <stdint.h>

/**
 * @file seq_clamp.h
 * @brief Type-safe, double-evaluation-safe boundary clamping utilities.
 *
 * Design notes:
 * - static inline functions (not naked ternary macros) eliminate
 *   double-evaluation bugs: passing 'step++' is completely safe.
 * - Integer helpers take a widened int64_t input so negative intermediate
 *   sequencer math (e.g. current + negative_delta) and out-of-range unsigned
 *   values clamp correctly instead of wrapping through the destination type.
 *
 * Limitations:
 * 1. NOT FOR COMPILE-TIME CONSTANTS: inline functions cannot appear in
 *    global/static initializers or other constant expressions. Use
 *    SEQ_CLAMP_CONST there (and only there — it double-evaluates).
 * 2. uint64_t inputs above INT64_MAX wrap when converted to the int64_t
 *    parameter and false-clamp to min_value. Validate 64-bit unsigned
 *    upstream values before clamping; there is deliberately no u64 helper.
 * 3. SEQ_CLAMP uses C11 _Generic and is unavailable in C++ translation
 *    units; the typed macros work in both languages.
 *
 * Deliberately absent: NaN checks and unset-sentinel tests. The AMY layer
 * already provides those (isnan_c11, AMY_UNSET / AMY_IS_SET / AMY_IS_UNSET
 * in components/amy/src/amy.h) — use them rather than duplicating here.
 */

static inline int seq_clamp_int(int64_t value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return (int)value;
}

static inline uint8_t seq_clamp_u8(int64_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < (int64_t)min_value) return min_value;
    if (value > (int64_t)max_value) return max_value;
    return (uint8_t)value;
}

static inline uint16_t seq_clamp_u16(int64_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < (int64_t)min_value) return min_value;
    if (value > (int64_t)max_value) return max_value;
    return (uint16_t)value;
}

static inline uint32_t seq_clamp_u32(int64_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < (int64_t)min_value) return min_value;
    if (value > (int64_t)max_value) return max_value;
    return (uint32_t)value;
}

static inline float seq_clamp_f32(float value, float min_value, float max_value)
{
    /* NaN fails both comparisons and falls through to min_value, so a
     * corrupted input degrades to a known-valid boundary instead of
     * propagating. */
    if (value > min_value && value < max_value) return value;
    if (value >= max_value) return max_value;
    return min_value;
}

/* Ternary form for static/global initializers ONLY. Evaluates every argument
 * more than once — never pass expressions with side effects. */
#define SEQ_CLAMP_CONST(value, min_value, max_value) \
    ((value) < (min_value) ? (min_value)             \
                           : ((value) > (max_value) ? (max_value) : (value)))

#define SEQ_CLAMP_INT(value, min_value, max_value) \
    seq_clamp_int((value), (min_value), (max_value))

#define SEQ_CLAMP_U8(value, min_value, max_value) \
    seq_clamp_u8((value), (min_value), (max_value))

#define SEQ_CLAMP_U16(value, min_value, max_value) \
    seq_clamp_u16((value), (min_value), (max_value))

#define SEQ_CLAMP_U32(value, min_value, max_value) \
    seq_clamp_u32((value), (min_value), (max_value))

#define SEQ_CLAMP_F32(value, min_value, max_value) \
    seq_clamp_f32((value), (min_value), (max_value))

#ifndef __cplusplus
/* Generic dispatch on the value's type. Notes:
 * - uint32_t is a typedef whose underlying type is toolchain-dependent:
 *   'unsigned long' on xtensa-esp-elf-gcc, 'unsigned int' on esp-clang, for
 *   the same target. Associating on uint32_t itself therefore duplicates
 *   whichever of the two it happens to alias, which is a constraint violation.
 *   'unsigned int' and 'unsigned long' are always distinct types in C even
 *   when they share a width, so naming both is unambiguous everywhere and
 *   covers uint32_t whichever one it resolves to.
 * - 'double' routes to the float helper (arguments convert implicitly):
 *   a stray double promotion must never fall into the int default, where it
 *   would truncate before clamping. */
#define SEQ_CLAMP(value, min_val, max_val) _Generic((value), \
    uint8_t:       seq_clamp_u8,  \
    uint16_t:      seq_clamp_u16, \
    unsigned int:  seq_clamp_u32, \
    unsigned long: seq_clamp_u32, \
    float:         seq_clamp_f32, \
    double:        seq_clamp_f32, \
    default:       seq_clamp_int  \
)((value), (min_val), (max_val))
#endif
