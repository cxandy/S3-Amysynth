# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 4.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
# Provable infinite recursion is already a -Wall warning on this toolchain
# (GCC >= 12); promote it to an error so it stops the build instead of
# scrolling past. It only fires when every path through a function recurses,
# so there are no false positives to live with.
idf_build_set_property(COMPILE_OPTIONS "-Werror=infinite-recursion" APPEND)
project(S3-Amysynth)

# GCC LTO: cross-TU inlining is the primary DSP gain, so it stays on for the
# shipping toolchain (clang rejects several of the flags, hence the compiler
# gate). Standalone recipe in cmake/gcc_lto.cmake; disable for A/B builds with
# -DAMYSYNTH_LTO=OFF.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/gcc_lto.cmake)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND AMYSYNTH_LTO)
    gcc_lto_enable(COMPONENTS main synth_core display u8g2 amy)
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