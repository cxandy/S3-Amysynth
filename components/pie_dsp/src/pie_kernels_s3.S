/*
 * ESP32-S3 PIE (128-bit SIMD) kernels for AMY's fixed-point render path.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * These cover the only shapes in AMY's render path that PIE can actually help with:
 * contiguous, multiply-free, dependency-free passes over an int32 (SAMPLE = s8.23)
 * block. Everything else in AMY is either gather-indexed (the LUT oscillators index a
 * wavetable by phase accumulator) or recurrence-bound (biquads, EQ, echo/chorus/reverb
 * all carry IIR feedback state) - neither vectorizes on any SIMD unit, and PIE has no
 * 32-bit vector multiply anyway.
 *
 * ALIGNMENT CONTRACT (important):
 *   `p` MUST be 16-byte aligned and `n4` counts groups of FOUR int32.
 *   PIE loads/stores force the low address bits to zero - an unaligned pointer does not
 *   fault, it silently accesses the wrong (rounded-down) address. The C wrappers in
 *   pie_dsp.c enforce this with a scalar head/tail; do not call these directly.
 *
 * Saturation note: negation is done with the saturating EE.VSUBS.S32, so |INT32_MIN|
 * yields INT32_MAX rather than wrapping back to INT32_MIN as the scalar C does. That is
 * the more defensible answer for an audio magnitude scan and cannot occur for real
 * s8.23 sample values.
 */

#include "pie_dsp_platform.h"
#if PIE_DSP_S3_ENABLED

    .text

/* ---------------------------------------------------------------------------
 * int32_t pie_absmax_s32(const int32_t *p, int n4);
 *
 * Returns max(|p[i]|) over n4*4 samples.  a2 = p, a3 = n4.  Result in a2.
 * ------------------------------------------------------------------------- */
    .align  4
    .global pie_absmax_s32
    .type   pie_absmax_s32,@function
pie_absmax_s32:
    entry   a1, 32
    ee.zero.q       q6                  // running lane-wise max
    ee.zero.q       q7                  // constant zero, for negation
    beqz            a3, .Labsmax_empty

    loopnez         a3, .Labsmax_end
        ee.vld.128.ip   q0, a2, 16      // q0 = 4 x int32, p += 16
        ee.vsubs.s32    q1, q7, q0      // q1 = 0 - x   (saturating)
        ee.vmax.s32     q1, q0, q1      // q1 = max(x, -x) = |x|
        ee.vmax.s32     q6, q6, q1      // accumulate lane-wise max
.Labsmax_end:

    // horizontal max across the four 32-bit lanes of q6
    ee.movi.32.a    q6, a4, 0
    ee.movi.32.a    q6, a5, 1
    ee.movi.32.a    q6, a6, 2
    ee.movi.32.a    q6, a7, 3
    max             a4, a4, a5
    max             a6, a6, a7
    max             a2, a4, a6
    retw.n

.Labsmax_empty:
    movi.n          a2, 0
    retw.n
    .size   pie_absmax_s32, . - pie_absmax_s32


/* ---------------------------------------------------------------------------
 * int32_t pie_shl_absmax_s32(int32_t *p, int n4, int bits);
 *
 * In-place p[i] <<= bits, returns max(|p[i]|) of the shifted values.
 * a2 = p, a3 = n4, a4 = bits.  Result in a2.
 * ------------------------------------------------------------------------- */
    .align  4
    .global pie_shl_absmax_s32
    .type   pie_shl_absmax_s32,@function
pie_shl_absmax_s32:
    entry   a1, 32
    ee.zero.q       q6
    ee.zero.q       q7
    wsr.sar         a4                  // EE.VSL.32 shifts by SAR[5:0]
    beqz            a3, .Lshl_empty
    mov.n           a5, a2              // separate write cursor (loads post-increment a2)

    loopnez         a3, .Lshl_end
        ee.vld.128.ip   q0, a2, 16
        ee.vsl.32       q0, q0          // q0 <<= SAR
        ee.vst.128.ip   q0, a5, 16
        ee.vsubs.s32    q1, q7, q0
        ee.vmax.s32     q1, q0, q1
        ee.vmax.s32     q6, q6, q1
.Lshl_end:

    ee.movi.32.a    q6, a4, 0
    ee.movi.32.a    q6, a5, 1
    ee.movi.32.a    q6, a6, 2
    ee.movi.32.a    q6, a7, 3
    max             a4, a4, a5
    max             a6, a6, a7
    max             a2, a4, a6
    retw.n

.Lshl_empty:
    movi.n          a2, 0
    retw.n
    .size   pie_shl_absmax_s32, . - pie_shl_absmax_s32


/* ---------------------------------------------------------------------------
 * int32_t pie_shr_absmax_s32(int32_t *p, int n4, int bits);
 *
 * In-place p[i] >>= bits (arithmetic, sign-preserving), returns max(|p[i]|).
 * a2 = p, a3 = n4, a4 = bits.  Result in a2.
 * ------------------------------------------------------------------------- */
    .align  4
    .global pie_shr_absmax_s32
    .type   pie_shr_absmax_s32,@function
pie_shr_absmax_s32:
    entry   a1, 32
    ee.zero.q       q6
    ee.zero.q       q7
    wsr.sar         a4                  // EE.VSR.32 shifts by SAR[5:0]
    beqz            a3, .Lshr_empty
    mov.n           a5, a2

    loopnez         a3, .Lshr_end
        ee.vld.128.ip   q0, a2, 16
        ee.vsr.32       q0, q0          // arithmetic >>, sign-extended
        ee.vst.128.ip   q0, a5, 16
        ee.vsubs.s32    q1, q7, q0
        ee.vmax.s32     q1, q0, q1
        ee.vmax.s32     q6, q6, q1
.Lshr_end:

    ee.movi.32.a    q6, a4, 0
    ee.movi.32.a    q6, a5, 1
    ee.movi.32.a    q6, a6, 2
    ee.movi.32.a    q6, a7, 3
    max             a4, a4, a5
    max             a6, a6, a7
    max             a2, a4, a6
    retw.n

.Lshr_empty:
    movi.n          a2, 0
    retw.n
    .size   pie_shr_absmax_s32, . - pie_shr_absmax_s32

#endif // PIE_DSP_S3_ENABLED
