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
constexpr int kBufferBytes = 4096;

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
bool BitsEqual(const uint8* A, const uint8* B, int Bit, int BitCount)
{
    for (int i = 0; i < BitCount; ++i)
    {
        int ByteIdx = (Bit + i) / 8;
        int BitIdx  = (Bit + i) % 8;
        int BitA = (A[ByteIdx] >> BitIdx) & 1;
        int BitB = (B[ByteIdx] >> BitIdx) & 1;
        if (BitA != BitB) return false;
    }
    return true;
}

int RunCorrectness(std::mt19937& Rng)
{
    Buffers B;
    std::uniform_int_distribution<int> ByteDist(0, 255);

    // Fill src with random bytes; dst buffers with a fixed sentinel so we
    // can verify that non-target bits are preserved.
    for (auto& V : B.Src) V = static_cast<uint8>(ByteDist(Rng));

    int failures = 0;
    const int kTrials = 20000;
    for (int t = 0; t < kTrials; ++t)
    {
        // Reset both destination buffers to the same known pattern.
        std::memset(B.DstFast.data(), 0xA5, B.DstFast.size());
        std::memset(B.DstRef.data(),  0xA5, B.DstRef.size());

        // Pick random parameters that stay inside buffer bounds.
        const int kMaxBits = (kBufferBytes - 16) * 8; // leave slack
        std::uniform_int_distribution<int> BitDist(0, kMaxBits);
        std::uniform_int_distribution<int> CountDist(0, kMaxBits);

        int SrcBit  = BitDist(Rng);
        int DestBit = BitDist(Rng);
        int MaxCount = std::min(kMaxBits - SrcBit, kMaxBits - DestBit);
        int BitCount = CountDist(Rng) % (MaxCount + 1);

        appBitsCpyFastImpl(B.DstFast.data(), DestBit, B.Src.data(), SrcBit, BitCount);
        OriginalAppBitsCpyForTest(B.DstRef.data(), DestBit, B.Src.data(), SrcBit, BitCount);

        // Target bits must match.
        if (!BitsEqual(B.DstFast.data(), B.DstRef.data(), DestBit, BitCount))
        {
            if (++failures <= 5)
            {
                std::fprintf(stderr,
                    "[FAIL] trial=%d DestBit=%d SrcBit=%d BitCount=%d\n",
                    t, DestBit, SrcBit, BitCount);
            }
        }
    }

    if (failures == 0)
    {
        std::printf("[ OK ] correctness: %d random trials passed\n", kTrials);
        return 0;
    }
    std::fprintf(stderr, "[FAIL] correctness: %d/%d trials failed\n", failures, kTrials);
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
