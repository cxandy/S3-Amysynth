file(GLOB AMY_SRC_FILES LIST_DIRECTORIES OFF "${CMAKE_CURRENT_LIST_DIR}/src/*.c")

# Exclude standalone examples and non-ESP drivers
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "amy-example\\.c$")
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "libminiaudio-audio\\.c$")
list(FILTER AMY_SRC_FILES EXCLUDE REGEX "usb\\.c$")

if(NOT AMY_SRC_FILES)
    message(FATAL_ERROR "No AMY sources found in ${CMAKE_CURRENT_LIST_DIR}/src. Make sure the `src` directory exists within the component.")
endif()

idf_component_register(
    SRCS ${AMY_SRC_FILES}
    INCLUDE_DIRS "src"
    REQUIRES esp_timer esp_driver_uart esp_driver_i2s esp_driver_i2c esp_driver_gpio espressif__tinyusb
)

# Keep the same compile flag for amy.c (relax strict-aliasing warnings)
set(AMY_CORE_C "${CMAKE_CURRENT_LIST_DIR}/src/amy.c")
if(EXISTS ${AMY_CORE_C})
    set_source_files_properties(${AMY_CORE_C} PROPERTIES COMPILE_FLAGS "-Wno-strict-aliasing")
endif()

# PERF (2026-06): drop assert() from the pure-DSP hot-path files only. The build
# enables assertions globally (CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE),
# which emits runtime assert checks inside the per-sample render/filter loops
# (e.g. filters.c dsps_biquad_*_split_fb assert(FILTER_SCALEUP_BITS==0)). These
# files contain no structural / cross-core state, so NDEBUG here removes the
# checks without weakening the safety asserts in amy.c (osc alloc/free, delta
# queue, render lock) or anywhere else in the project.
set(AMY_DSP_HOT_FILES
    "${CMAKE_CURRENT_LIST_DIR}/src/filters.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/oscillators.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/envelope.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/delay.c"
    "${CMAKE_CURRENT_LIST_DIR}/src/log2_exp2.c"
)
foreach(_dsp_file ${AMY_DSP_HOT_FILES})
    if(EXISTS ${_dsp_file})
        set_property(SOURCE ${_dsp_file} APPEND_STRING PROPERTY COMPILE_FLAGS " -DNDEBUG")
    endif()
endforeach()
