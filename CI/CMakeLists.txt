cmake_minimum_required(VERSION 3.16)
project(FastBitCopyCI CXX)

# This build is ONLY used by the CI workflow to verify the SIMD / bit-copy
# algorithm outside of Unreal Engine. The real plugin build is done by UBT
# as part of a UE project. See /CI/README.md for details.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(PLUGIN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(fastbitcopy_ci
    test_main.cpp
    "${PLUGIN_ROOT}/Source/FastBitCopy/Private/FastBitCopy.cpp"
)

target_include_directories(fastbitcopy_ci PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"           # ue_shim.h
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"    # CoreMinimal.h, Math/UnrealMathUtility.h
    "${PLUGIN_ROOT}/Source/FastBitCopy/Public"
)

# Expose internal functions (BitsCopyFastAligned/Unaligned, OriginalAppBitsCpyImpl)
# for the threshold sweep benchmark. This flag is ONLY set in the CI standalone
# build; the UE plugin build never sees it.
target_compile_definitions(fastbitcopy_ci PRIVATE FASTBITCOPY_EXPOSE_INTERNALS=1)

# Force-include ue_shim.h ahead of anything so UE types & macros are defined
# before the plugin sources are parsed.
if(MSVC)
    target_compile_options(fastbitcopy_ci PRIVATE /FI"ue_shim.h" /W4 /permissive-)
else()
    # -fno-strict-aliasing matches what UBT passes to GCC/Clang; without it,
    # FastBitCopy.cpp's uint8*/uint64* aliasing is UB and gets DCE'd on -O2.
    target_compile_options(fastbitcopy_ci PRIVATE -include ue_shim.h -Wall -Wextra -fno-strict-aliasing)
endif()

# Optimize hard on Release so bench numbers mean something.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(fastbitcopy_ci PRIVATE /O2)
    else()
        target_compile_options(fastbitcopy_ci PRIVATE -O2)
    endif()
endif()

enable_testing()
add_test(NAME fastbitcopy_ci COMMAND fastbitcopy_ci)
