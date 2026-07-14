// LOCAL EDIT (S3-Amysynth): ESP32-S3 PIE (128-bit SIMD) hooks for AMY's render path.
//
// Thin shim over the local `pie_dsp` component. The reasoning for why so little of AMY
// is PIE-eligible lives in components/pie_dsp/include/pie_dsp.h - short version: SAMPLE
// is int32 (s8.23) and PIE has no 32-bit vector multiply, the LUT oscillators are
// gather-indexed, and the filters/delays are all recurrence-bound. What is left, once
// the block scans turned out to be too short to amortise the vector setup, is the bulk
// block clears and copies - which is what this covers, and all it covers.
//
// Kept as a separate header (rather than edits to amy.h) so an AMY re-vendor only has to
// re-apply the call sites, not untangle a modified upstream header.
//
// IMPORTANT: amy.h (which defines AMY_USE_FIXEDPOINT) must be included before this.

#ifndef AMY_SIMD_H
#define AMY_SIMD_H

#include "pie_dsp.h"

// The int32 kernels are only correct when SAMPLE is the s8.23 fixed-point type. In a
// float build SAMPLE is `float`, so the vector paths must compile out entirely.
#if PIE_DSP_S3_ENABLED && defined(AMY_USE_FIXEDPOINT)
#define AMY_PIE_SIMD 1
#else
#define AMY_PIE_SIMD 0
#endif

// PIE-accelerated on ESP32-S3, libc memset/memcpy everywhere else. Safe in float builds
// too: a byte-wise clear/copy does not care what the samples mean.
#define AMY_BLOCK_BZERO(p, nbytes)          PIE_BZERO((p), (nbytes))
#define AMY_BLOCK_BCOPY(src, dst, nbytes)   PIE_BCOPY((src), (dst), (nbytes))

#endif // AMY_SIMD_H
