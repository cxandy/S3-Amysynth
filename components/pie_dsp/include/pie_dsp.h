// ESP32-S3 PIE (128-bit SIMD) kernels for the AMY render path.
//
// Scope note - why this component is deliberately narrow:
//
// The S3's PIE unit multiplies/multiply-accumulates on 8- and 16-bit lanes only
// (EE.VMULAS.S16 -> 40-bit QACC/ACCX). 32-bit lanes get add/sub/shift/min/max/compare
// and nothing else; there is no gather and no float SIMD. AMY's fixed-point build uses
// SAMPLE = s8.23 = int32, so its hot-path multiplies (MUL8_SS, SMULR6, top16SMUL) are
// 32x32 products that PIE cannot vectorize at all.
//
// That leaves the multiply-free, dependency-free, contiguous operations - which is
// exactly what lives here. The rest of AMY's render path is either gather-indexed
// (the LUT oscillators walk a wavetable by phase accumulator) or recurrence-bound
// (biquads, EQ, echo/chorus/reverb all carry IIR feedback state), and neither is
// vectorizable on any SIMD unit. esp-dsp independently agrees: its own ESP32-S3
// biquad (dsps_biquad_f32_aes3.S) contains zero PIE instructions.
//
// Off the S3, every entry point falls back to portable C / libc so AMY still builds
// for desktop and other targets.

#ifndef PIE_DSP_H
#define PIE_DSP_H

#include <stddef.h>
#include <stdint.h>
#include <strings.h>
#include <string.h>

#include "pie_dsp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PIE_DSP_S3_ENABLED

// Bulk block move / fill, 128-bit (EE.VLD.128 / EE.VST.128) inner loops.
// Both handle aligned and unaligned buffers internally - no alignment precondition.
// Vendored from esp-dsp; see pie_memset_s3.S / pie_memcpy_s3.S.
void *pie_memset_s3(void *dest, uint8_t val, size_t nbytes);
void *pie_memcpy_s3(void *dest, const void *src, size_t nbytes);

#define PIE_BZERO(p, nbytes)         pie_memset_s3((p), 0, (nbytes))
// NB: bcopy(src, dst, n) has the opposite argument order to memcpy(dst, src, n).
#define PIE_BCOPY(src, dst, nbytes)  pie_memcpy_s3((dst), (src), (nbytes))

// --- Raw kernels (pie_kernels_s3.S). ---------------------------------------
// PRECONDITION: p is 16-byte aligned and n4 counts groups of FOUR int32.
// PIE forces the low address bits to zero, so an unaligned p silently accesses the
// wrong address instead of faulting. Call the safe wrappers below, not these.
int32_t pie_absmax_s32(const int32_t *p, int n4);
int32_t pie_shl_absmax_s32(int32_t *p, int n4, int bits);
int32_t pie_shr_absmax_s32(int32_t *p, int n4, int bits);

// --- Safe wrappers (pie_dsp.c). --------------------------------------------
// Any pointer, any length. Scalar head/tail around the vector body.

// max(|p[i]|) over len samples. Drop-in for AMY's scan_max().
int32_t pie_scan_absmax_s32(const int32_t *p, int len);

// In-place p[i] <<= bits (or >>= -bits when negative), returns max(|p[i]|) of the
// shifted values. Bit-identical to AMY's block_norm(): SHIFTL/SHIFTR are plain
// arithmetic shifts in the fixed-point build, which EE.VSL.32/EE.VSR.32 match.
int32_t pie_block_norm_s32(int32_t *p, int len, int bits);

#else

#define PIE_BZERO(p, nbytes)         bzero((p), (nbytes))
#define PIE_BCOPY(src, dst, nbytes)  bcopy((src), (dst), (nbytes))

#endif // PIE_DSP_S3_ENABLED

#ifdef __cplusplus
}
#endif

#endif // PIE_DSP_H
