# Standalone GCC LTO for selected IDF components.
#
# Replaces espressif/cmake_utilities' cu_gcc_lto_set: that macro gates on
# check_ipo_supported(), which does not reliably evaluate true against the
# ESP-IDF cross toolchain and can abort configure with FATAL_ERROR, and it
# redirects CMAKE_AR/CMAKE_RANLIB to gcc-ar/gcc-ranlib wrappers that are not
# guaranteed present. This version applies the same compile/link flags with no
# probe, no archiver override, and no Kconfig dependency.
#
# Usage (after project()):
#   include(${CMAKE_CURRENT_LIST_DIR}/cmake/gcc_lto.cmake)
#   if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND AMYSYNTH_LTO)
#       gcc_lto_enable(COMPONENTS main synth_core ...)
#   endif()
#
# A/B builds: idf.py build -DAMYSYNTH_LTO=OFF (full clean between flips —
# incremental builds lie under LTO).
#
# GCC only: clang rejects -ffat-lto-objects / -flto-compression-level, so the
# caller must gate on CMAKE_C_COMPILER_ID.

option(AMYSYNTH_LTO "Compile the hot components with GCC LTO" ON)

# -ffat-lto-objects keeps full native code alongside the LTO IR so non-LTO
# consumers of the archives (and tools like objdump) still work.
# -flto-partition=max at link maximizes dead-symbol removal.
set(GCC_LTO_COMPILE_OPTIONS -flto=auto -ffat-lto-objects -flto-compression-level=9)
set(GCC_LTO_LINK_OPTIONS    -flto -fuse-linker-plugin -ffat-lto-objects -flto-partition=max)

function(gcc_lto_enable)
    cmake_parse_arguments(LTO "" "" "COMPONENTS" ${ARGN})
    message(STATUS "GCC LTO for components: ${LTO_COMPONENTS}")
    foreach(c ${LTO_COMPONENTS})
        idf_component_get_property(t ${c} COMPONENT_LIB)
        target_compile_options(${t} PRIVATE ${GCC_LTO_COMPILE_OPTIONS})
    endforeach()
    target_link_libraries(${project_elf} PRIVATE ${GCC_LTO_LINK_OPTIONS})
endfunction()
