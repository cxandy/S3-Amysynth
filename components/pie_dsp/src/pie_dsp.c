// C wrappers around the ESP32-S3 PIE kernels.
//
// The kernels require a 16-byte-aligned base pointer and a length that is a multiple
// of four int32 (PIE loads/stores force the low address bits to zero, so an unaligned
// pointer silently reads the wrong address rather than faulting). These wrappers make
// that safe for arbitrary pointers and lengths by handling a scalar head up to the
// alignment boundary and a scalar tail for the leftover 1-3 samples.
//
// The head is empty for the big render buffers (see amy_pie_aligned_alloc), so the
// common case is one aligned vector pass with no scalar fixup.

#include "pie_dsp.h"

#if PIE_DSP_S3_ENABLED

#include <stdint.h>

static inline int32_t s_abs(int32_t v) { return v < 0 ? -v : v; }

int32_t pie_scan_absmax_s32(const int32_t *p, int len) {
    int32_t max = 0;

    // Scalar head: advance to a 16-byte boundary.
    while (len > 0 && ((uintptr_t)p & 15u)) {
        int32_t a = s_abs(*p++);
        if (a > max) max = a;
        --len;
    }

    const int n4 = len >> 2;
    if (n4 > 0) {
        const int32_t m = pie_absmax_s32(p, n4);
        if (m > max) max = m;
        p   += (n4 << 2);
        len -= (n4 << 2);
    }

    // Scalar tail: the 0-3 samples that do not fill a vector.
    while (len-- > 0) {
        int32_t a = s_abs(*p++);
        if (a > max) max = a;
    }
    return max;
}

int32_t pie_block_norm_s32(int32_t *p, int len, int bits) {
    const int left = (bits >= 0);
    const int sh   = left ? bits : -bits;
    int32_t max = 0;

    while (len > 0 && ((uintptr_t)p & 15u)) {
        *p = left ? (*p << sh) : (*p >> sh);
        int32_t a = s_abs(*p++);
        if (a > max) max = a;
        --len;
    }

    const int n4 = len >> 2;
    if (n4 > 0) {
        const int32_t m = left ? pie_shl_absmax_s32(p, n4, sh)
                               : pie_shr_absmax_s32(p, n4, sh);
        if (m > max) max = m;
        p   += (n4 << 2);
        len -= (n4 << 2);
    }

    while (len-- > 0) {
        *p = left ? (*p << sh) : (*p >> sh);
        int32_t a = s_abs(*p++);
        if (a > max) max = a;
    }
    return max;
}

#endif // PIE_DSP_S3_ENABLED
