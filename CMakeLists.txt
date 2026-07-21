# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 4.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(S3-Amysynth)

# GCC LTO: cross-TU inlining is the primary DSP gain, so it stays on for the
# shipping toolchain. cmake_utilities' gcc.cmake keys solely on
# CONFIG_CU_GCC_LTO_ENABLE and never inspects the compiler, so it would inject
# -flto-compression-level=9 / -ffat-lto-objects into a clang build as well --
# flags clang rejects outright. Gate the call on the toolchain instead.
# (gcc.cmake only defines cu_gcc_lto_set when the Kconfig symbol is set, so the
# symbol has to be part of the condition or the call becomes an unknown command.)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CONFIG_CU_GCC_LTO_ENABLE)
    include(gcc)
    cu_gcc_lto_set(COMPONENTS main synth_core display u8g2 amy)
endif()

# esp-clang 20.1.1 crashes in instruction selection on patches_load_patch
# ("Cannot select: v4i1 = BUILD_VECTOR") when the loop vectorizer fires.
# Scoped to the single affected file so the rest of the DSP keeps vectorising;
# patch loading is not on the render path, so the cost is nil.
# -fno-slp-vectorize alone is not sufficient -- it is the loop vectorizer.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    idf_component_get_property(amy_lib amy COMPONENT_LIB)
    set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/components/amy/src/patches.c"
                 TARGET_DIRECTORY ${amy_lib}
                 APPEND PROPERTY COMPILE_OPTIONS -fno-vectorize -fno-slp-vectorize)
endif()