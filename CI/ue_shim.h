// Copyright (c) chen3feng. All Rights Reserved.
//
// Minimal UE-compat shim that lets BitCopyFast.cpp compile outside of UE.
// It is ONLY used by the standalone CI build in CI/CMakeLists.txt, never
// by the real plugin build done through UBT.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

// ---- UE integer type aliases ----
using uint8  = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using int8   = std::int8_t;
using int16  = std::int16_t;
using int32  = std::int32_t;
using int64  = std::int64_t;

// ---- Platform flags (assume modern little-endian x64/arm64) ----
#ifndef PLATFORM_LITTLE_ENDIAN
#define PLATFORM_LITTLE_ENDIAN 1
#endif
#ifndef PLATFORM_SUPPORTS_UNALIGNED_LOADS
#define PLATFORM_SUPPORTS_UNALIGNED_LOADS 1
#endif

// ---- Attribute macros ----
#ifndef FORCEINLINE
#if defined(_MSC_VER)
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE inline __attribute__((always_inline))
#endif
#endif

#ifndef CORE_API
#define CORE_API
#endif

#ifndef FASTBITCOPY_API
#define FASTBITCOPY_API
#endif

// ---- CoreMinimal.h / Math / Memory shims ----
// BitCopyFast.cpp includes "CoreMinimal.h" and "Math/UnrealMathUtility.h";
// we provide empty headers under CI/compat/ so the #includes resolve.
// The real symbols it needs are FMath and FMemory, shimmed below.

struct FMath
{
    // NOTE: return by value, NOT by the deduced type of a conditional
    // expression.  `decltype(a < b ? a : b)` on two same-typed lvalues
    // yields an lvalue reference, which would make Min/Max return a
    // dangling reference to a by-value parameter -- observed as an
    // AddressSanitizer "stack-use-after-return" at -O2 on GCC/Clang
    // (the -O0 CI jobs happened to not reuse the stack slot so the
    // bug was masked there).  Keep the signature close to UE's real
    // FMath::Min<T>(T,T) which also returns by value.
    template <class T>
    static constexpr T Min(T a, T b)
    {
        return a < b ? a : b;
    }
    template <class T>
    static constexpr T Max(T a, T b)
    {
        return a > b ? a : b;
    }
};

struct FMemory
{
    static void Memcpy(void* Dest, const void* Src, size_t Count)
    {
        std::memcpy(Dest, Src, Count);
    }
};
