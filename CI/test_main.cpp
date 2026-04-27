// Copyright (c) chen3feng. All Rights Reserved.
//
// Standalone CI test: compares appBitsCpyFastImpl (the optimized version)
// against OriginalAppBitsCpyForTest (the stock UE reference), on thousands
// of random (DestBit, SrcBit, BitCount) combinations, and prints a mini
// micro-benchmark. No Unreal Engine required.

#include "ue_shim.h"
#include "BitCopyFast.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

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
            appBitsCpyFastImpl(B.DstFast.data(), C.DestBit, B.Src.data(), C.SrcBit, C.BitCount);
            OriginalAppBitsCpyForTest(B.DstRef.data(), C.DestBit, B.Src.data(), C.SrcBit, C.BitCount);
            if (!BitsEqual(B.DstFast.data(), B.DstRef.data(), C.DestBit, C.BitCount))
            {
                ++failures;
                std::fprintf(stderr,
                             "[FAIL] edge '%s' DestBit=%d SrcBit=%d BitCount=%d\n",
                             C.Tag, C.DestBit, C.SrcBit, C.BitCount);
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

            appBitsCpyFastImpl(B.DstFast.data(), DestBit, B.Src.data(), SrcBit, BitCount);
            OriginalAppBitsCpyForTest(B.DstRef.data(), DestBit, B.Src.data(), SrcBit, BitCount);

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

    auto Bench = [&](const char* Label, void(*Fn)(uint8*, int32, uint8*, int32, int32), int DestBit, int SrcBit)
    {
        // Warm up.
        for (int i = 0; i < 64; ++i)
            Fn(B.DstFast.data(), DestBit, B.Src.data(), SrcBit, kCopyBits);

        auto t0 = clock::now();
        for (int i = 0; i < kIters; ++i)
            Fn(B.DstFast.data(), DestBit, B.Src.data(), SrcBit, kCopyBits);
        auto t1 = clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
        std::printf("  %-24s  %8.1f ns/op\n", Label, ns);
    };

    std::printf("[bench] aligned (SrcBit=DestBit=3), %d bits/call, %d iters\n", kCopyBits, kIters);
    Bench("Original appBitsCpy",   &OriginalAppBitsCpyForTest, 3, 3);
    Bench("appBitsCpyFastImpl",    &appBitsCpyFastImpl,        3, 3);

    std::printf("[bench] unaligned (SrcBit=1 DestBit=5), %d bits/call, %d iters\n", kCopyBits, kIters);
    Bench("Original appBitsCpy",   &OriginalAppBitsCpyForTest, 5, 1);
    Bench("appBitsCpyFastImpl",    &appBitsCpyFastImpl,        5, 1);
}

} // namespace

int main()
{
    std::mt19937 Rng(0xC0FFEEu);
    int rc = RunCorrectness(Rng);
    RunBench(Rng);
    return rc;
}
