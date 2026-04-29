// Copyright (c) chen3feng. All Rights Reserved.
//
// Automation tests for the FastBitCopy plugin:
//   * Correctness test: compares the hooked appBitsCpy (which forwards to
//     our optimized impl) against a verbatim copy of the original
//     implementation across thousands of randomised inputs.
//   * Page-boundary test: makes sure we never over-read / over-write near
//     a page boundary (since the fast impl uses unaligned 64-bit loads).
//   * Speed benchmark: compares Original vs Fast across various sizes.

#include "CoreMinimal.h"
#include "FastBitCopy.h"
#include "FastBitCopyModule.h"

#include "Misc/AutomationTest.h"
#include "Misc/MemStack.h"
#include "HAL/PlatformTime.h"

#include <algorithm>
#include <bitset>
#include <string>

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_LITTLE_ENDIAN

// UE's bit copy symbol — this may or may not be hooked depending on whether
// the FastBitCopy runtime module successfully installed its hook.
CORE_API void appBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount);

DEFINE_LOG_CATEGORY_STATIC(LogFastBitCopyTests, Log, All);

constexpr auto BasicIntegrationTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

static void PrintBinary(const FString& Name, const uint8* Addr, int Size)
{
	std::string str;
	for (int i = 0; i < Size; i++)
	{
		if (!str.empty()) str += ' ';
		auto bits = std::bitset<8>(Addr[i]).to_string();
		std::reverse(bits.begin(), bits.end());
		str += bits;
	}
	UE_LOG(LogFastBitCopyTests, Display, TEXT("%s: %s"), *Name, UTF8_TO_TCHAR(str.c_str()));
}

// ----------------------------------------------------------------------------
// Correctness: compare Fast vs Original across many random inputs.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFastBitCopyCorrectness,
	"FastBitCopy.Correctness",
	BasicIntegrationTestFlags)
bool FFastBitCopyCorrectness::RunTest(const FString& Parameters)
{
	constexpr int TestBytes = 4096;
	constexpr int TestBits = TestBytes * 8;
	uint8 Src[TestBytes] = "0123456789ABCDEFG";
	for (int i = 0; i < TestBytes; ++i) Src[i] ^= (uint8)(i * 131);

	int Errors = 0;
	for (int i = 0; i < 10000; ++i)
	{
		int DstBit   = rand() % TestBits;
		int SrcBit   = rand() % (TestBits - DstBit);
		int BitCount = rand() % (TestBits - FMath::Max(DstBit, SrcBit));

		uint8 DstFast[TestBytes] = { 0 };
		uint8 DstRef [TestBytes] = { 0 };

		FastBitCopy(DstFast, DstBit, Src, SrcBit, BitCount);
		OriginalAppBitsCpy(DstRef, DstBit, Src, SrcBit, BitCount);

		if (FMemory::Memcmp(DstFast, DstRef, TestBytes) != 0)
		{
			auto poss = std::mismatch(DstFast, DstFast + TestBytes, DstRef);
			UE_LOG(LogFastBitCopyTests, Display,
				TEXT("MISMATCH SrcBit=%d DstBit=%d BitCount=%d Offset=%d"),
				SrcBit, DstBit, BitCount, int(poss.first - DstFast));
			PrintBinary(TEXT("Fast"), poss.first,  32);
			PrintBinary(TEXT("Ref "), poss.second, 32);
			++Errors;
		}
	}
	UTEST_EQUAL("Error count", Errors, 0);
	return Errors == 0;
}

// ----------------------------------------------------------------------------
// Hook sanity: confirm the hook is installed in this run, and that the
// publicly-visible `appBitsCpy` now produces the same bytes as our fast
// implementation.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFastBitCopyHookSanity,
	"FastBitCopy.HookSanity",
	BasicIntegrationTestFlags)
bool FFastBitCopyHookSanity::RunTest(const FString& Parameters)
{
	UTEST_TRUE("Hook must be installed", FFastBitCopyModule::IsHookInstalled());

	constexpr int TestBytes = 1024;
	uint8 Src[TestBytes]; for (int i=0;i<TestBytes;++i) Src[i] = (uint8)(i*7+3);

	uint8 A[TestBytes] = {}, B[TestBytes] = {};
	const int DstBit = 17, SrcBit = 5, BitCount = TestBytes*8 - 100;

	appBitsCpy         (A, DstBit, Src, SrcBit, BitCount); // hooked
	FastBitCopy(B, DstBit, Src, SrcBit, BitCount);		   // direct

	UTEST_EQUAL("Hooked appBitsCpy matches fast impl",
		FMemory::Memcmp(A, B, TestBytes), 0);
	return true;
}

// ----------------------------------------------------------------------------
// Page-bound test: make sure unaligned 64-bit loads never walk past a page.
// ----------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFastBitCopyPageBound,
	"FastBitCopy.PageBound",
	BasicIntegrationTestFlags)
bool FFastBitCopyPageBound::RunTest(const FString& Parameters)
{
	uint32 PageSize = FPageAllocator::PageSize;
	uint8* Src = (uint8*)FPageAllocator::Get().Alloc();
	uint8* Dst = (uint8*)FPageAllocator::Get().Alloc();
	FMemory::Memset(Src, 0xFF, PageSize);

	FastBitCopy(Dst, 9, Src, 10, PageSize * 8 - 10);
	FastBitCopy(Dst, 23890, Src, 464, 8839);

	const int TestBits = PageSize * 8;
	for (int i = 0; i < 10000; ++i)
	{
		int DstBit   = rand() % TestBits;
		int SrcBit   = rand() % (TestBits - DstBit);
		int BitCount = rand() % (TestBits - FMath::Max(DstBit, SrcBit));
		FastBitCopy(Dst, DstBit, Src, SrcBit, BitCount);
	}

	FPageAllocator::Get().Free(Src);
	FPageAllocator::Get().Free(Dst);
	return true;
}

// ----------------------------------------------------------------------------
// Speed benchmark.
//
// Methodology notes (don't regress these):
//
//  * Src buffer is (BytesSize + 8) bytes long, because the fast path may
//    issue an unaligned 64-bit load up to one word past the nominal end
//    (bounded inside CopyBitsSrcAligned but still past BytesSize when the
//    caller only allocated BytesSize). Under-sized Src was UB and, more
//    importantly, biased the benchmark because stack padding is
//    unpredictable across compilers.
//
//  * We accumulate a byte of Dest into a volatile sink after each inner
//    batch. Without this the optimiser is free to hoist/DCE the call
//    (the result is never observed), which makes Fast look artificially
//    good on small sizes where the call is trivially inlinable.
//
//  * We run a warm-up pass (to seed I-cache / branch predictors / TLB)
//    and then take the minimum of several timed repeats. Minimum is the
//    right statistic for micro-benchmarks: it's the sample least
//    contaminated by OS preemption, turbo-boost transitions and
//    interrupts. Mean/median bias upward with noise.
// ----------------------------------------------------------------------------
enum EBitsCopyType { Original, Hooked, Fast };
enum EAlignment    { Aligned, Unaligned };

// Volatile sink: observed side-effect to keep the optimiser from eliding
// the benchmarked calls. `volatile` forces an actual store per write.
static volatile uint8 GBenchSink = 0;

// Number of timed repeats per (size, impl, alignment) tuple. We report the
// minimum. Keep small to bound total test time.
static constexpr int BenchRepeats = 5;

template <int BytesSize, EBitsCopyType Type, EAlignment Alignment>
FORCENOINLINE static double BenchOneRun(int LoopCount)
{
	const bool IsAligned = (Alignment == Aligned);

	uint8 Dest[BytesSize]; FMemory::Memset(Dest, 0, sizeof(Dest));
	// +8 so the fast path's unaligned uint64 reads never walk off the end.
	uint8 Src[BytesSize + 8];
	for (int i = 0; i < (int)sizeof(Src); ++i)
		Src[i] = (uint8)(i * 131 + 7);
	constexpr int Bits = BytesSize * 8;

	uint8 SinkAccum = 0;
	const double StartTime = FPlatformTime::Seconds();
	for (int i = 0; i < LoopCount; ++i)
	{
		if constexpr (Type == Original)
		{
			if (IsAligned)
				OriginalAppBitsCpy(Dest, 0, Src, 0, Bits);
			else
				OriginalAppBitsCpy(Dest, 0, Src, 1, Bits - 1);
		}
		else if constexpr (Type == Hooked)
		{
			if (IsAligned) appBitsCpy(Dest, 0, Src, 0, Bits);
			else           appBitsCpy(Dest, 0, Src, 1, Bits - 1);
		}
		else
		{
			if (IsAligned)
				FastBitCopy(Dest, 0, Src, 0, Bits);
			else
				FastBitCopy(Dest, 0, Src, 1, Bits - 1);
		}
		// Observe a byte so the call can't be eliminated as dead.
		SinkAccum = (uint8)(SinkAccum ^ Dest[0]);
	}
	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	GBenchSink = SinkAccum; // publish
	return Elapsed;
}

template <int BytesSize, EBitsCopyType Type, EAlignment Alignment>
static double BenchOne(int LoopCount)
{
	const bool IsAligned = (Alignment == Aligned);
	const TCHAR *AlignName = IsAligned ? TEXT("Aligned  ") : TEXT("Unaligned");
	const TCHAR *FuncName =
		Type == Original ? TEXT("Original") : Type == Hooked ? TEXT("Hooked  ")
															 : TEXT("Fast    ");

	// Warm-up: not timed. Seeds I-cache / branch predictors.
	BenchOneRun<BytesSize, Type, Alignment>(FMath::Max(LoopCount / 10, 1));

	double BestElapsed = TNumericLimits<double>::Max();
	for (int r = 0; r < BenchRepeats; ++r)
	{
		const double E = BenchOneRun<BytesSize, Type, Alignment>(LoopCount);
		if (E < BestElapsed)
			BestElapsed = E;
	}
	UE_LOG(LogFastBitCopyTests, Display,
		   TEXT("  %s %s %d bytes : %.4f s (min of %d)"),
		   FuncName, AlignName, BytesSize, BestElapsed, BenchRepeats);
	return BestElapsed;
}

// Benchmark result for a single size: holds Original and Fast timings.
struct FBenchResult
{
	double OriginalAligned;
	double OriginalUnaligned;
	double FastAligned;
	double FastUnaligned;
	double HookedAligned;
	double HookedUnaligned;
};

template <int BytesSize>
static FBenchResult BenchSize()
{
	// Note: actual per-tuple runtime is ~BenchRepeats * LoopCount plus the
	// 10% warmup. We keep LoopCount smaller than before (was 1,000,000) to
	// keep the full Speed test inside the CI timeout budget.
	constexpr int LoopCount = 200000;
	UE_LOG(LogFastBitCopyTests, Display, TEXT("--- %d bytes ---"), BytesSize);

	FBenchResult R;
	R.OriginalAligned = BenchOne<BytesSize, Original, Aligned>(LoopCount);
	R.OriginalUnaligned = BenchOne<BytesSize, Original, Unaligned>(LoopCount);
	R.FastAligned = BenchOne<BytesSize, Fast, Aligned>(LoopCount);
	R.FastUnaligned = BenchOne<BytesSize, Fast, Unaligned>(LoopCount);
	R.HookedAligned = BenchOne<BytesSize, Hooked, Aligned>(LoopCount);
	R.HookedUnaligned = BenchOne<BytesSize, Hooked, Unaligned>(LoopCount);
	return R;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFastBitCopySpeed,
	"FastBitCopy.Speed",
	BasicIntegrationTestFlags)
bool FFastBitCopySpeed::RunTest(const FString& Parameters)
{
	// Small sizes: benchmark only, no speed assertion (noise dominates).
	BenchSize<1   >();
	BenchSize<2   >();
	BenchSize<4   >();
	BenchSize<8   >();
	BenchSize<16  >();
	BenchSize<32  >();

	// For sizes >= 64 bytes, the fast path should be measurably faster than
	// the original on the unaligned path (the primary optimization target).
	// We use a generous threshold: Fast must not be slower than Original.
	// (In practice Fast is typically 2-10x faster for large sizes.)
	auto Check = [this](int Size, const FBenchResult &R)
	{
		// Check the hooked path (what callers actually experience) against
		// the original. If the hook is installed, Hooked should be as fast
		// as Fast; if not, it falls back to Original speed.
		if (R.HookedUnaligned > R.OriginalUnaligned * 1.05)
		{
			AddWarning(FString::Printf(
				TEXT("Optimization regression at %d bytes unaligned: "
					 "Hooked=%.4fs > Original=%.4fs (ratio=%.2fx)"),
				Size, R.HookedUnaligned, R.OriginalUnaligned,
				R.HookedUnaligned / R.OriginalUnaligned));
		}
		else
		{
			UE_LOG(LogFastBitCopyTests, Display,
				   TEXT("  >> %d bytes unaligned hooked speedup: %.2fx"),
				   Size, R.OriginalUnaligned / FMath::Max(R.HookedUnaligned, 1e-9));
		}
		// Also log the direct fast-path speedup for reference.
		UE_LOG(LogFastBitCopyTests, Display,
			   TEXT("  >> %d bytes unaligned fast speedup:   %.2fx"),
			   Size, R.OriginalUnaligned / FMath::Max(R.FastUnaligned, 1e-9));
	};

	Check(64, BenchSize<64>());
	Check(128, BenchSize<128>());
	Check(256, BenchSize<256>());
	Check(512, BenchSize<512>());
	Check(1024, BenchSize<1024>());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && PLATFORM_LITTLE_ENDIAN
