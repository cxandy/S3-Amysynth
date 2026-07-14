// ESP32-S3 PIE (128-bit SIMD) kernels for the AMY render path.
//
// Scope note - why this component is only a block memset and a block memcpy:
//
// The S3's PIE unit multiplies/multiply-accumulates on 8- and 16-bit lanes only
// (EE.VMULAS.S16 -> 40-bit QACC/ACCX). 32-bit lanes get add/sub/shift/min/max/compare
// and nothing else; there is no gather and no float SIMD. AMY's fixed-point build uses
// SAMPLE = s8.23 = int32, so its hot-path multiplies (MUL8_SS, SMULR6, top16SMUL) are
// 32x32 products that PIE cannot vectorize at all.
//
// That leaves the multiply-free, dependency-free, contiguous operations. The rest of
// AMY's render path is either gather-indexed (the LUT oscillators walk a wavetable by
// phase accumulator) or recurrence-bound (biquads, EQ, echo/chorus/reverb all carry IIR
// feedback state), and neither is vectorizable on any SIMD unit. esp-dsp independently
// agrees: its own ESP32-S3 biquad (dsps_biquad_f32_aes3.S) contains zero PIE
// instructions.
//
// An earlier revision also vectorized AMY's scan_max() and block_norm(), which are
// multiply-free reductions and so looked eligible. On-target A/B said otherwise: they
// bought nothing on any scene and cost up to ~1% on filter-heavy ones. Nearly every
// call is on a tiny buffer - scan_max(w, 4), scan_max(w, 6) for LPF24, and scan_max /
// block_norm over the 8-entry filter_delay - where the vector setup costs more than the
// scalar loop it replaces, and those buffers live inside synthinfo so they are not
// aligned either. Multiply-free turns out to be necessary but not sufficient: the
// operation also has to be long enough to amortise the vector setup, and in AMY only
// the bulk block clears and copies are.
//
// Off the S3, every entry point falls back to libc memset/memcpy so AMY still builds
// for desktop and other targets.

#ifndef PIE_DSP_H
#define PIE_DSP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pie_dsp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PIE_DSP_S3_ENABLED

// Bulk block move / fill, 128-bit (EE.VLD.128 / EE.VST.128) inner loops.
// Both handle aligned and unaligned buffers internally - no alignment precondition,
// though an aligned base skips the scalar head, which is what malloc_caps_block() in
// amy.c exists to provide.
// Vendored from esp-dsp; see pie_memset_s3.S / pie_memcpy_s3.S.
void *pie_memset_s3(void *dest, uint8_t val, size_t nbytes);
void *pie_memcpy_s3(void *dest, const void *src, size_t nbytes);

#define PIE_BZERO(p, nbytes)         pie_memset_s3((p), 0, (nbytes))
// NB: bcopy(src, dst, n) has the opposite argument order to memcpy(dst, src, n).
#define PIE_BCOPY(src, dst, nbytes)  pie_memcpy_s3((dst), (src), (nbytes))

#else

// Plain libc, not bzero/bcopy: <strings.h> is POSIX-only (absent on MSVC), and
// memcpy matches the S3 kernel's no-overlap semantics where bcopy is allowed to
// behave like memmove. Neither macro supports overlapping buffers on any target.
#define PIE_BZERO(p, nbytes)         memset((p), 0, (nbytes))
#define PIE_BCOPY(src, dst, nbytes)  memcpy((dst), (src), (nbytes))

#endif // PIE_DSP_S3_ENABLED

#ifdef __cplusplus
}
#endif

#endif // PIE_DSP_H
