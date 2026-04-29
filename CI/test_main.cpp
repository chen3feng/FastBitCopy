// Copyright (c) chen3feng. All Rights Reserved.
//
// Standalone CI test: compares FastBitCopy (the optimized version)
// against OriginalAppBitsCpy (the stock UE reference), on thousands
// of random (DestBit, SrcBit, BitCount) combinations, and prints a mini
// micro-benchmark. No Unreal Engine required.

#include "ue_shim.h"
#include "FastBitCopy.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// Anti-DCE / anti-hoist barriers for the micro-benchmark loops.
//
// Without these, an optimizing compiler is free to notice that each iteration
// of the bench loop writes to the same destination buffer with the same
// arguments and keep only a single invocation (or worse, elide the writes
// entirely because the buffer is never read afterwards). That used to show
// up in our sweep as a flat `Fast_ns ~ 4.4` column across payload sizes on
// Linux/g++ builds.
//
// Design:
//   FBC_CLOBBER()     - memory barrier, tells the compiler that all memory
//                       may have changed so prior stores must be emitted.
//   FBC_DONOTOPT(x)   - make `x` observably escape, so `x` (and everything
//                       feeding it) cannot be dead-code-eliminated.
//
// GCC/Clang: use the standard `asm volatile("" ::: "memory")` idiom which
// costs zero instructions at runtime but prevents reordering/DCE.
//
// MSVC: has no equivalent inline asm on x64. We keep FBC_CLOBBER() empty and
// instead rely on (a) rolling the Src/Dst offset every iteration so distinct
// stores cannot be coalesced, plus (b) a volatile-sink write at loop exit so
// the final buffer state is observably escaped. The sink write costs ~1 ns
// but happens only once per bench run, not per iteration.
#if defined(__GNUC__) || defined(__clang__)
#define FBC_CLOBBER() asm volatile("" ::: "memory")
#define FBC_DONOTOPT(x) asm volatile("" : "+r,m"(x) : : "memory")
#else
namespace
{
    volatile int FBC_Sink = 0;
}
#define FBC_CLOBBER() ((void)0)
#define FBC_DONOTOPT(x)                                                     \
    do                                                                      \
    {                                                                       \
        FBC_Sink = static_cast<int>(reinterpret_cast<std::intptr_t>(&(x))); \
    } while (0)
#endif

namespace
{
    // Buffer must be big enough + a healthy tail padding, because
    // BitsCopyFastUnaligned does 64-bit loads/stores one past the last
    // fully-used word. Give it 128 bytes of slack so no implementation-
    // internal over-read can touch the heap red-zone.
    constexpr int kTailPadding = 128;
    constexpr int kPayloadBytes = 4096;
    constexpr int kBufferBytes = kPayloadBytes + kTailPadding;

    struct Buffers
    {
        std::vector<uint8> Src;
        std::vector<uint8> DstFast;
        std::vector<uint8> DstRef;

        Buffers()
            : Src(kBufferBytes), DstFast(kBufferBytes), DstRef(kBufferBytes)
        {
        }
    };

    // Return true if the `BitCount` bits starting at `Bit` match between A and B.
    bool BitsEqual(const uint8 *A, const uint8 *B, int Bit, int BitCount)
    {
        for (int i = 0; i < BitCount; ++i)
        {
            int ByteIdx = (Bit + i) / 8;
            int BitIdx = (Bit + i) % 8;
            int BitA = (A[ByteIdx] >> BitIdx) & 1;
            int BitB = (B[ByteIdx] >> BitIdx) & 1;
            if (BitA != BitB)
                return false;
        }
        return true;
    }

    int RunCorrectness(std::mt19937 &Rng)
    {
        Buffers B;
        std::uniform_int_distribution<int> ByteDist(0, 255);

        // Fill src with random bytes; dst buffers with a fixed sentinel so we
        // can verify that non-target bits are preserved.
        for (auto &V : B.Src)
            V = static_cast<uint8>(ByteDist(Rng));

        int failures = 0;

        // --- Deterministic edge cases -----------------------------------
        // These target the post-prelude `DestBit == 0` path in
        // CopyBitsSrcAligned<uint64>, which used to rely on `Word >> 64`
        // (UB: MSVC folded it to 0, Clang/GCC did not).
        struct Case
        {
            int DestBit;
            int SrcBit;
            int BitCount;
            const char *Tag;
        };
        const Case kEdge[] = {
            // (OverlappedBits + DestBit) % 8 == 0 ⇒ post-prelude DestBit == 0.
            {5, 3, 64, "prelude-zero-dest-small"},
            {5, 3, 256, "prelude-zero-dest-mid"},
            {5, 3, 1024, "prelude-zero-dest-big"},
            {7, 1, 63, "one-bit-src-prelude"},
            {1, 7, 63, "seven-bit-src-prelude"},
            {4, 2, 17, "odd-tail"},
            {0, 0, 0, "zero-length"},
            {0, 0, 1, "single-bit-aligned"},
            {3, 3, 1, "single-bit-mid"},
            {0, 1, 128, "word-dest-bit-src"},
            {1, 0, 128, "bit-dest-word-src"},
        };
        for (const auto &C : kEdge)
        {
            std::memset(B.DstFast.data(), 0xA5, B.DstFast.size());
            std::memset(B.DstRef.data(), 0xA5, B.DstRef.size());
            FastBitCopy(B.DstFast.data(), C.DestBit, B.Src.data(), C.SrcBit, C.BitCount);
            OriginalAppBitsCpy(B.DstRef.data(), C.DestBit, B.Src.data(), C.SrcBit, C.BitCount);
            if (!BitsEqual(B.DstFast.data(), B.DstRef.data(), C.DestBit, C.BitCount))
            {
                ++failures;
                std::fprintf(stderr,
                             "[FAIL] edge '%s' DestBit=%d SrcBit=%d BitCount=%d\n",
                             C.Tag, C.DestBit, C.SrcBit, C.BitCount);
                int FirstByte = C.DestBit / 8;
                int LastByte = (C.DestBit + C.BitCount + 7) / 8;
                int Diffs = 0;
                for (int i = FirstByte; i < LastByte && Diffs < 4; ++i)
                {
                    if (B.DstFast[i] != B.DstRef[i])
                    {
                        std::fprintf(stderr,
                                     "       byte[%d]: fast=0x%02X ref=0x%02X src[%d]=0x%02X src[%d]=0x%02X\n",
                                     i, B.DstFast[i], B.DstRef[i],
                                     i, B.Src[i], i + 1, B.Src[i + 1]);
                        ++Diffs;
                    }
                }
            }
        }
        if (failures == 0)
        {
            std::printf("[ OK ] edge cases: %zu passed\n",
                        sizeof(kEdge) / sizeof(kEdge[0]));
        }

        const int kTrials = 20000;
        for (int t = 0; t < kTrials; ++t)
        {
            // Reset both destination buffers to the same known pattern.
            std::memset(B.DstFast.data(), 0xA5, B.DstFast.size());
            std::memset(B.DstRef.data(), 0xA5, B.DstRef.size());

            // Pick random parameters that stay inside the *payload* region.
            // The tail padding is reserved for the algorithm's over-read.
            const int kMaxBits = kPayloadBytes * 8;
            std::uniform_int_distribution<int> BitDist(0, kMaxBits);

            int SrcBit = BitDist(Rng);
            int DestBit = BitDist(Rng);
            int MaxCount = std::min(kMaxBits - SrcBit, kMaxBits - DestBit);
            std::uniform_int_distribution<int> CountDist(0, MaxCount);
            int BitCount = CountDist(Rng);

            FastBitCopy(B.DstFast.data(), DestBit, B.Src.data(), SrcBit, BitCount);
            OriginalAppBitsCpy(B.DstRef.data(), DestBit, B.Src.data(), SrcBit, BitCount);

            // Target bits must match.
            if (!BitsEqual(B.DstFast.data(), B.DstRef.data(), DestBit, BitCount))
            {
                if (++failures <= 3)
                {
                    std::fprintf(stderr,
                                 "[FAIL] trial=%d DestBit=%d SrcBit=%d BitCount=%d\n",
                                 t, DestBit, SrcBit, BitCount);
                    // Dump first differing byte so we have a minimal repro.
                    int FirstByte = DestBit / 8;
                    int LastByte = (DestBit + BitCount + 7) / 8;
                    for (int i = FirstByte; i < LastByte && i < (int)B.DstFast.size(); ++i)
                    {
                        if (B.DstFast[i] != B.DstRef[i])
                        {
                            std::fprintf(stderr,
                                         "       byte[%d]: fast=0x%02X ref=0x%02X (xor=0x%02X)\n",
                                         i, B.DstFast[i], B.DstRef[i],
                                         (unsigned)(B.DstFast[i] ^ B.DstRef[i]));
                            break;
                        }
                    }
                }
            }
        }

        if (failures == 0)
        {
            std::printf("[ OK ] correctness: %d random trials passed\n", kTrials);
            return 0;
        }
        std::fprintf(stderr, "[FAIL] correctness: %d trials failed\n", failures);
        return 1;
    }

void RunBench(std::mt19937& Rng)
{
    using clock = std::chrono::steady_clock;
    Buffers B;
    std::uniform_int_distribution<int> ByteDist(0, 255);
    for (auto& V : B.Src) V = static_cast<uint8>(ByteDist(Rng));

    const int kIters     = 20000;
    const int kCopyBytes = 1024;
    const int kCopyBits  = kCopyBytes * 8;

    // Roll the Src/Dst pointer each iteration in 8-byte steps so
    //   (a) the compiler cannot coalesce successive stores (each targets a
    //       different address), and
    //   (b) SrcBit/DestBit alignment is preserved because off % 8 == 0.
    // Range: buffer is 4224 B, payload is 1024 B. We cap the offset so
    // off + payload + slack <= buffer, and round down to a power of two for
    // a cheap `& mask`.
    const int kBenchSlack = 16;
    int BenchMaxOff = kBufferBytes - kCopyBytes - kBenchSlack;
    int BenchRollMask = 8;
    while ((BenchRollMask << 1) <= BenchMaxOff)
        BenchRollMask <<= 1;
    BenchRollMask = (BenchRollMask - 1) & ~7;

    auto Bench = [&](const char* Label, void(*Fn)(uint8*, int32, uint8*, int32, int32), int DestBit, int SrcBit)
    {
        // Warm up.
        for (int i = 0; i < 64; ++i)
        {
            const int off = (i * 8) & BenchRollMask;
            Fn(B.DstFast.data() + off, DestBit, B.Src.data() + off, SrcBit, kCopyBits);
        }

        auto t0 = clock::now();
        for (int i = 0; i < kIters; ++i)
        {
            const int off = (i * 8) & BenchRollMask;
            Fn(B.DstFast.data() + off, DestBit, B.Src.data() + off, SrcBit, kCopyBits);
            FBC_CLOBBER();
        }
        auto t1 = clock::now();
        FBC_DONOTOPT(B.DstFast[0]);
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
        std::printf("  %-24s  %8.1f ns/op\n", Label, ns);
    };

    std::printf("[bench] aligned (SrcBit=DestBit=3), %d bits/call, %d iters\n", kCopyBits, kIters);
    Bench("Original appBitsCpy", &OriginalAppBitsCpy, 3, 3);
    Bench("FastBitCopy", &FastBitCopy, 3, 3);

    std::printf("[bench] unaligned (SrcBit=1 DestBit=5), %d bits/call, %d iters\n", kCopyBits, kIters);
    Bench("Original appBitsCpy", &OriginalAppBitsCpy, 5, 1);
    Bench("FastBitCopy", &FastBitCopy, 5, 1);
}

} // namespace

#if FASTBITCOPY_EXPOSE_INTERNALS

// Forward declarations for the internal wrappers exposed by FastBitCopy.cpp
// when FASTBITCOPY_EXPOSE_INTERNALS is defined.
// NOTE: these must be declared at global scope (outside any anonymous namespace)
// to match the definitions in FastBitCopy.cpp.
void FastBitCopy_Internal_Aligned(uint8 *Dest, uint8 *Src, int BitOffset, int BitCount);
void FastBitCopy_Internal_Unaligned(uint8 *Dest, int DestBit, uint8 *Src, int SrcBit, int BitCount);
void FastBitCopy_Internal_Original(uint8 *Dest, int32 DestBit, uint8 *Src, int32 SrcBit, int32 BitCount);

namespace
{

    // Simulate FastBitCopy with a custom small-size threshold (in bits).
    // Below the threshold: forward to OriginalAppBitsCpyImpl (via the exposed wrapper).
    // At or above: use the fast aligned/unaligned path directly.
    static void FastBitCopyWithThreshold(uint8 *Dest, int32 DestBit, uint8 *Src, int32 SrcBit, int32 BitCount,
                                         int32 SmallBitsThreshold)
    {
        if (BitCount <= SmallBitsThreshold)
        {
            FastBitCopy_Internal_Original(Dest, DestBit, Src, SrcBit, BitCount);
            return;
        }
        // Normalise to byte boundary (mirrors FastBitCopy's own prelude).
        Dest += DestBit / 8;
        DestBit %= 8;
        Src += SrcBit / 8;
        SrcBit %= 8;
        if (SrcBit == DestBit)
            FastBitCopy_Internal_Aligned(Dest, Src, SrcBit, BitCount);
        else
            FastBitCopy_Internal_Unaligned(Dest, DestBit, Src, SrcBit, BitCount);
    }

    // Threshold sweep benchmark.
    //
    // For each candidate threshold value, measures the time to copy payloads of
    // exactly (threshold - 1), threshold, and (threshold + 1) bits under both
    // aligned and unaligned conditions, comparing:
    //   - "Orig"  : always use OriginalAppBitsCpyImpl
    //   - "Fast"  : always use the fast path (no threshold guard, threshold=0)
    //   - "Gated" : use FastBitCopyWithThreshold(threshold)
    //
    // The output table lets you pick the crossover point where "Gated" stops
    // being slower than "Orig" for both aligned and unaligned cases.
    void RunBenchSweep(std::mt19937 &Rng)
    {
        using clock = std::chrono::steady_clock;

        Buffers B;
        std::uniform_int_distribution<int> ByteDist(0, 255);
        for (auto &V : B.Src)
            V = static_cast<uint8>(ByteDist(Rng));

        // Candidate thresholds to sweep (in bits).
        //
        // 8..256 covers the "small payload" crossover region where the
        // per-call overhead of the fast path might not pay off yet.
        //
        // 384..2048 covers the "medium / large" region we also care about
        // for UE networking: Bunches are split at ~1024 bytes on send, so
        // anything beyond ~2048 bits (256 B) is already well inside the
        // fast path's comfort zone and doesn't add information.
        //
        // At these larger sizes the "Gated" column degenerates into the
        // "Orig" column (payload <= thresh always takes the original
        // path), so the useful signal there is the Orig_ns vs Fast_ns
        // comparison, i.e. when does the unaligned fast path start to
        // dominate the scalar original implementation.
        const int kThresholds[] = {8, 16, 32, 48, 64, 96, 128, 192, 256,
                                   384, 512, 768, 1024, 2048};
        // Iterations per measurement.
        const int kIters = 100000;

        // Measure ns/op for a given (DestBit, SrcBit, BitCount) combination.
        //
        // Rolling offset + FBC_CLOBBER() + FBC_DONOTOPT() keep the optimizer
        // honest: without them, Clang/GCC at -O2/-O3 happily observe that
        // every iteration writes the exact same bits to the exact same
        // location and delete all but one invocation, producing a flat
        // ~4 ns/op "Fast" column that has nothing to do with real cost.
        auto MeasureNs = [&](auto fn, int DestBit, int SrcBit, int BitCount) -> double
        {
            // Compute a safe rolling window for this payload. We need room
            // for (BitCount bits, rounded up to bytes) + one 8-byte slack
            // (the fast path over-reads up to one uint64 past the end).
            const int PayloadBytes = (BitCount + 7) / 8 + 8;
            const int MaxOff = kBufferBytes - PayloadBytes;
            int RollMask = 8;
            while ((RollMask << 1) <= MaxOff)
                RollMask <<= 1;
            RollMask = (RollMask - 1) & ~7; // 8-byte steps preserve bit alignment

            // Warm up.
            for (int i = 0; i < 256; ++i)
            {
                const int off = (i * 8) & RollMask;
                fn(B.DstFast.data() + off, DestBit, B.Src.data() + off, SrcBit, BitCount);
            }
            auto t0 = clock::now();
            for (int i = 0; i < kIters; ++i)
            {
                const int off = (i * 8) & RollMask;
                fn(B.DstFast.data() + off, DestBit, B.Src.data() + off, SrcBit, BitCount);
                FBC_CLOBBER();
            }
            auto t1 = clock::now();
            FBC_DONOTOPT(B.DstFast[0]);
            return std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
        };

        std::printf("\n[sweep] Small-size threshold sweep (%d iters each)\n", kIters);
        std::printf("  %-9s  %-12s  %-8s  %8s  %8s  %9s  %10s  %10s\n",
                    "Thresh", "PayloadBits", "Align", "Orig_ns", "Fast_ns", "Gated_ns", "Orig/Gated", "Fast/Gated");
        std::printf("  %.*s\n", 85, "---------------------"
                                    "---------------------"
                                    "---------------------"
                                    "---------------------"
                                    "-----");

        for (int thresh : kThresholds)
        {
            // Test three payload sizes around the threshold.
            const int payloads[] = {thresh - 1, thresh, thresh + 1};
            for (int bits : payloads)
            {
                if (bits <= 0)
                    continue;

                // Aligned: DestBit == SrcBit == 3
                {
                    int db = 3, sb = 3;
                    double origNs = MeasureNs([](uint8 *d, int db2, uint8 *s, int sb2, int bc)
                                              { FastBitCopy_Internal_Original(d, db2, s, sb2, bc); }, db, sb, bits);
                    double fastNs = MeasureNs([&](uint8 *d, int db2, uint8 *s, int sb2, int bc)
                                              { FastBitCopyWithThreshold(d, db2, s, sb2, bc, 0); }, db, sb, bits);
                    double gatedNs = MeasureNs([&](uint8 *d, int db2, uint8 *s, int sb2, int bc)
                                               { FastBitCopyWithThreshold(d, db2, s, sb2, bc, thresh); }, db, sb, bits);
                    std::printf("  %-9d  %-12d  %-8s  %8.1f  %8.1f  %9.1f  %10.2fx  %10.2fx\n",
                                thresh, bits, "aligned",
                                origNs, fastNs, gatedNs,
                                origNs / gatedNs, fastNs / gatedNs);
                }
                // Unaligned: DestBit=5, SrcBit=1
                {
                    int db = 5, sb = 1;
                    double origNs = MeasureNs([](uint8 *d, int db2, uint8 *s, int sb2, int bc)
                                              { FastBitCopy_Internal_Original(d, db2, s, sb2, bc); }, db, sb, bits);
                    double fastNs = MeasureNs([&](uint8 *d, int db2, uint8 *s, int sb2, int bc)
                                              { FastBitCopyWithThreshold(d, db2, s, sb2, bc, 0); }, db, sb, bits);
                    double gatedNs = MeasureNs([&](uint8 *d, int db2, uint8 *s, int sb2, int bc)
                                               { FastBitCopyWithThreshold(d, db2, s, sb2, bc, thresh); }, db, sb, bits);
                    std::printf("  %-9d  %-12d  %-8s  %8.1f  %8.1f  %9.1f  %10.2fx  %10.2fx\n",
                                thresh, bits, "unalign",
                                origNs, fastNs, gatedNs,
                                origNs / gatedNs, fastNs / gatedNs);
                }
            }
        }
        std::printf("\n  Interpretation:\n");
        std::printf("    Orig/Gated > 1.0 => gated is faster than always-original (good)\n");
        std::printf("    Fast/Gated > 1.0 => gated is faster than always-fast (expected for small payloads)\n");
        std::printf("    Ideal threshold: smallest value where Orig/Gated >= 1.0 for both aligned and unaligned.\n\n");
    }

} // namespace

#endif // FASTBITCOPY_EXPOSE_INTERNALS
// Forward declaration for the probe defined in FastBitCopy.cpp.
int FastBitCopy_IsOptimizedBuild();

int main()
{
    std::setbuf(stdout, nullptr);
    std::setbuf(stderr, nullptr);

    // Confirm which build path we're on. Also exposes a self-check of the
    // ue_shim platform macros.
    std::printf("[info] FastBitCopy_IsOptimizedBuild = %d\n",
                FastBitCopy_IsOptimizedBuild());
    std::printf("[info] PLATFORM_LITTLE_ENDIAN = %d\n",
                (int)PLATFORM_LITTLE_ENDIAN);

    // Smoke test: byte-aligned 8-bit copy. If the function writes anything
    // at all, tmp[0] must end up 0xFF. A result of 0x00 means the optimized
    // store path has been eliminated by the compiler somewhere.
    {
        uint8 tmp[16] = {0};
        uint8 src[16];
        std::memset(src, 0xFF, sizeof(src));
        FastBitCopy(tmp, 0, src, 0, 8);
        std::printf("[info] smoke(aligned): tmp[0]=0x%02X (expect 0xFF)\n", tmp[0]);
    }
    {
        uint8 tmp[16] = {0};
        uint8 src[16];
        std::memset(src, 0xFF, sizeof(src));
        FastBitCopy(tmp, 5, src, 3, 64);
        std::printf("[info] smoke(unaligned 5/3 64b): tmp[0..4]=%02X %02X %02X %02X %02X\n",
                    tmp[0], tmp[1], tmp[2], tmp[3], tmp[4]);
    }
    // Same unaligned smoke but with the 0xA5 sentinel used by the edge
    // cases and a realistic source pattern (first bytes from the random
    // stream): this is what the main correctness sweep actually feeds to
    // the function. On a healthy build tmp[0] should come out 0x45.
    {
        uint8 tmp[32];
        std::memset(tmp, 0xA5, sizeof(tmp));
        const uint8 src_bytes[] = {0x92, 0x8C, 0xD0, 0x24, 0xED, 0xA6, 0x00, 0x00, 0};
        uint8 src[32];
        std::memcpy(src, src_bytes, sizeof(src_bytes));
        FastBitCopy(tmp, 5, src, 3, 64);
        std::printf("[info] smoke(sentinel 0xA5, real src): tmp[0..4]=%02X %02X %02X %02X %02X (expect 45 32 42 93 ..)\n",
                    tmp[0], tmp[1], tmp[2], tmp[3], tmp[4]);
    }

    // Heap-based repro of the same unaligned smoke. If this reproduces the
    // Linux -O2 "nothing was written" behaviour seen on the stack version,
    // the bug is truly inside FastBitCopy. If it does NOT reproduce,
    // the bug is specifically a stack-escape / no-address-taken analysis
    // issue that only affects short-lived local arrays.
    {
        auto *tmp = new uint8[32];
        std::memset(tmp, 0xA5, 32);
        auto *src = new uint8[32];
        const uint8 src_bytes[] = {0x92, 0x8C, 0xD0, 0x24, 0xED, 0xA6, 0x00, 0x00, 0};
        std::memcpy(src, src_bytes, sizeof(src_bytes));
        FastBitCopy(tmp, 5, src, 3, 64);
        // Also dump via volatile reads to prevent any post-call DCE.
        volatile uint8 v0 = tmp[0], v1 = tmp[1], v2 = tmp[2], v3 = tmp[3], v4 = tmp[4];
        std::printf("[info] smoke(HEAP sentinel): tmp[0..4]=%02X %02X %02X %02X %02X (expect 45 32 42 93 ..)\n",
                    (unsigned)v0, (unsigned)v1, (unsigned)v2, (unsigned)v3, (unsigned)v4);
        delete[] tmp;
        delete[] src;
    }

    std::mt19937 Rng(0xC0FFEEu);
    int rc = RunCorrectness(Rng);
    RunBench(Rng);
#if FASTBITCOPY_EXPOSE_INTERNALS
    RunBenchSweep(Rng);
#endif
    return rc;
}
