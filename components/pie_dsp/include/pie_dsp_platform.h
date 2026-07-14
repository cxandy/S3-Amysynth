// Build gate for the ESP32-S3 PIE (128-bit SIMD) kernels.
//
// The kernels are hand-written Xtensa assembly using the S3's PIE extension
// (EE.VLD.128 / EE.VST.128 / EE.VMAX.S32 / ...). They only assemble for a target
// whose core-isa actually has the extension, so everything in this component is
// gated behind PIE_DSP_S3_ENABLED and the C API falls back to libc elsewhere.
//
// Shared by the .S sources (preprocessed) and pie_dsp.h.

#ifndef PIE_DSP_PLATFORM_H
#define PIE_DSP_PLATFORM_H

#include "sdkconfig.h"

#if defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3)
#define PIE_DSP_S3_ENABLED 1
#else
#define PIE_DSP_S3_ENABLED 0
#endif

#endif // PIE_DSP_PLATFORM_H
