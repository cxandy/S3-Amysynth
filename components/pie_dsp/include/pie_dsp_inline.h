// Header-only alternative to the vendored .S kernels: the same 128-bit block
// clear/copy as gated inline asm, specialized to what AMY actually does.
//
// NOT used by this component's shipping path - pie_dsp.h and the .S kernels are
// what the firmware builds and what was A/B-benched. This header is the shape to
// offer upstream if platform-specific source files (or vendored Apache-2.0
// assembly) are unwanted in their tree: it is original code (no lines copied,
// though credit to esp-dsp's dsps_memset_aes3.S / dsps_memcpy_aes3.S, whose
// aligned main loops are the reference this reproduces), lives entirely in one
// header, and compiles to the same inner loop as the kernels' aligned path.
// Codegen verified with xtensa-esp-elf-gcc esp-15.2.0 at -O2: the clear is
// ee.xorq + a single-instruction loopnez ZOL of ee.vst.128.ip, the copy a
// two-instruction ee.vld.128.ip / ee.vst.128.ip ZOL body, and a constant-size
// call folds to an alignment test plus the loop. Not yet A/B-benched on target,
// though with an instruction-identical inner loop the numbers should match.
//
// The specialization that makes it small: the vector path is taken only when
// the base pointer(s) and nbytes are all 16-byte multiples - which AMY's block
// buffers satisfy by construction (AMY_BLOCK_SIZE * sizeof(SAMPLE), allocated
// via malloc_caps_block) - and everything else falls back to plain libc. That
// drops the unaligned head/tail handling that makes up most of the .S kernels;
// the trade-off is that an unaligned buffer gets no speedup at all instead of
// a scalar-head-then-vector one, so the aligned allocator stops being optional.
// No overlapping buffers, same as the kernels.

#ifndef PIE_DSP_INLINE_H
#define PIE_DSP_INLINE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pie_dsp_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#if PIE_DSP_S3_ENABLED

// bits = OR of every base pointer involved; the vector loop needs all of them
// 16-byte aligned and a nonzero nbytes that is a multiple of 16.
static inline int pie_inline_eligible_(uintptr_t bits, size_t nbytes) {
    return ((bits | (uintptr_t)nbytes) & 15u) == 0 && nbytes != 0;
}

static inline void pie_block_bzero(void *p, size_t nbytes) {
    if (pie_inline_eligible_((uintptr_t)p, nbytes)) {
        void *pp = p;  // ee.vst.128.ip post-increments the address register
        __asm__ volatile(
            "ee.xorq q0, q0, q0\n\t"
            "loopnez %1, .Lpie_bz%=\n\t"
            "ee.vst.128.ip q0, %0, 16\n"
            ".Lpie_bz%=:"
            : "+&r"(pp)
            : "r"(nbytes / 16)
            : "memory");
    } else {
        memset(p, 0, nbytes);
    }
}

// bcopy argument order (src, dst) - the reverse of memcpy - matching PIE_BCOPY.
static inline void pie_block_bcopy(const void *src, void *dst, size_t nbytes) {
    if (pie_inline_eligible_((uintptr_t)src | (uintptr_t)dst, nbytes)) {
        const void *s = src;
        void *d = dst;
        __asm__ volatile(
            "loopnez %2, .Lpie_bc%=\n\t"
            "ee.vld.128.ip q0, %1, 16\n\t"
            "ee.vst.128.ip q0, %0, 16\n"
            ".Lpie_bc%=:"
            : "+&r"(d), "+&r"(s)
            : "r"(nbytes / 16)
            : "memory");
    } else {
        memcpy(dst, src, nbytes);
    }
}

#else

static inline void pie_block_bzero(void *p, size_t nbytes) {
    memset(p, 0, nbytes);
}

static inline void pie_block_bcopy(const void *src, void *dst, size_t nbytes) {
    memcpy(dst, src, nbytes);
}

#endif // PIE_DSP_S3_ENABLED

#ifdef __cplusplus
}
#endif

#endif // PIE_DSP_INLINE_H
