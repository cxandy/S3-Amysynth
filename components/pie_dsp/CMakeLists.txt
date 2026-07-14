# ESP32-S3 PIE (128-bit SIMD) kernels used by the AMY render path.
#
# The .S sources are gated internally on PIE_DSP_S3_ENABLED (see pie_dsp_platform.h),
# so they assemble to nothing on other targets and the C API falls back to libc.
# Listing them unconditionally keeps the component buildable for any target.

idf_component_register(
    SRCS "src/pie_memset_s3.S"
         "src/pie_memcpy_s3.S"
         "src/pie_kernels_s3.S"
         "src/pie_dsp.c"
    INCLUDE_DIRS "include"
)
