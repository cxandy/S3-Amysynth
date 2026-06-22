#pragma once

#include <stdint.h>
/**
 * @file seq_clamp.h
 * @brief Type-safe, double-evaluation-safe boundary clamping utilities.
 *
 * @section DESIGN_BENEFITS
 * - Eliminates double-evaluation bugs (e.g., passing 'step++' is completely safe).
 * - Accepts signed inputs for unsigned destinations, allowing negative intermediate 
 * sequencer math (e.g., current + negative_delta) to safely underflow before clamping.
 *
 * @section CRITICAL_LIMITATIONS
 * 1. NOT FOR COMPILE-TIME CONSTANTS: Because these wrappers rely on static inline 
 * functions, they cannot be used in global/static array initializers or struct 
 * definitions. Use raw ternary macros for static compilation targets.

 * * 3. EXTREME UNSIGNED OVERFLOW: Passing a pre-existing uint32_t greater than INT32_MAX 
 * (> 2,147,483,647) into the u8/u16/int helpers will cause a signed rollover, 
 * resulting in a false underflow clamp to min_value. Ensure massive upstream values 
 * are validated before down-casting.
 */
/* Type-specific helpers avoid macro double-evaluation pitfalls. */
/* Using 'int' for the input 'value' in unsigned helpers allows negative results 
   from (current + delta) to be safely evaluated before returning the clamped type. */
static inline int seq_clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static inline uint8_t seq_clamp_u8(int value, uint8_t min_value, uint8_t max_value)
{
    if (value < (int)min_value) return min_value;
    if (value > (int)max_value) return max_value;
    return (uint8_t)value;
}

static inline uint16_t seq_clamp_u16(int value, uint16_t min_value, uint16_t max_value)
{
    if (value < (int)min_value) return min_value;
    if (value > (int)max_value) return max_value;
    return (uint16_t)value;
}

static inline uint32_t seq_clamp_u32(int64_t value, uint32_t min_value, uint32_t max_value)
{
    if (value < (int64_t)min_value) return min_value;
    if (value > (int64_t)max_value) return max_value;
    return (uint32_t)value;
}

/*static inline float seq_clamp_f32(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}*/
static inline float seq_clamp_f32(float value, float min_value, float max_value)
{
    // If value is NaN, both comparisons fail. 
    // By structures, it falls through to explicitly return a known valid boundary.
    if (value > min_value && value < max_value) return value;
    if (value >= max_value) return max_value;
    
    // Fallback captures both true underflows AND any NaN corruptions
    return min_value; 
}

#ifndef SEQ_CLAMP_INT
#define SEQ_CLAMP_INT(value, min_value, max_value) \
    seq_clamp_int((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_U8
#define SEQ_CLAMP_U8(value, min_value, max_value) \
    seq_clamp_u8((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_U16
#define SEQ_CLAMP_U16(value, min_value, max_value) \
    seq_clamp_u16((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_U32
#define SEQ_CLAMP_U32(value, min_value, max_value) \
    seq_clamp_u32((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP_F32
#define SEQ_CLAMP_F32(value, min_value, max_value) \
    seq_clamp_f32((value), (min_value), (max_value))
#endif

#ifndef SEQ_CLAMP
#define SEQ_CLAMP(value, min_val, max_val) _Generic((value), \
    uint8_t:  seq_clamp_u8,  \
    uint16_t: seq_clamp_u16, \
    uint32_t: seq_clamp_u32, \
    float:    seq_clamp_f32, \
    default:  seq_clamp_int  \
)((value), (min_val), (max_val))
#endif