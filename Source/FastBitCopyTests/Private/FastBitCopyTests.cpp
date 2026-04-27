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
#include "BitCopyFast.h"
#include "FastBitCopy.h"

#include "Misc/AutomationTest.h"
#include "Misc/MemStack.h"
#include "HAL/PlatformTime.h"

#include <algorithm>
#include <bitset>
#include <string>

#if WITH_DEV_AUTOMATION_TESTS

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

		appBitsCpyFastImpl      (DstFast, DstBit, Src, SrcBit, BitCount);
		OriginalAppBitsCpyForTest(DstRef , DstBit, Src, SrcBit, BitCount);

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
	appBitsCpyFastImpl (B, DstBit, Src, SrcBit, BitCount); // direct

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

	appBitsCpyFastImpl(Dst, 9,     Src, 10,  PageSize * 8 - 10);
	appBitsCpyFastImpl(Dst, 23890, Src, 464, 8839);

	const int TestBits = PageSize * 8;
	for (int i = 0; i < 10000; ++i)
	{
		int DstBit   = rand() % TestBits;
		int SrcBit   = rand() % (TestBits - DstBit);
		int BitCount = rand() % (TestBits - FMath::Max(DstBit, SrcBit));
		appBitsCpyFastImpl(Dst, DstBit, Src, SrcBit, BitCount);
	}

	FPageAllocator::Get().Free(Src);
	FPageAllocator::Get().Free(Dst);
	return true;
}

// ----------------------------------------------------------------------------
// Speed benchmark.
// ----------------------------------------------------------------------------
enum EBitsCopyType { Original, Hooked, Fast };
enum EAlignment    { Aligned, Unaligned };

template <int BytesSize, EBitsCopyType Type, EAlignment Alignment>
static void BenchOne(int LoopCount)
{
	const bool IsAligned = (Alignment == Aligned);
	const TCHAR* AlignName = IsAligned ? TEXT("Aligned  ") : TEXT("Unaligned");
	const TCHAR* FuncName =
		Type == Original ? TEXT("Original") :
		Type == Hooked   ? TEXT("Hooked  ") :
		                   TEXT("Fast    ");

	uint8 Dest[BytesSize]; FMemory::Memset(Dest, 0, sizeof(Dest));
	uint8 Src[1] = { 0xFF };
	constexpr int Bits = BytesSize * 8;

	const double StartTime = FPlatformTime::Seconds();
	for (int i = 0; i < LoopCount; ++i)
	{
		if constexpr (Type == Original)
		{
			if (IsAligned) OriginalAppBitsCpyForTest(Dest, 0, Src, 0, Bits);
			else           OriginalAppBitsCpyForTest(Dest, 0, Src, 1, Bits - 1);
		}
		else if constexpr (Type == Hooked)
		{
			if (IsAligned) appBitsCpy(Dest, 0, Src, 0, Bits);
			else           appBitsCpy(Dest, 0, Src, 1, Bits - 1);
		}
		else
		{
			if (IsAligned) appBitsCpyFastImpl(Dest, 0, Src, 0, Bits);
			else           appBitsCpyFastImpl(Dest, 0, Src, 1, Bits - 1);
		}
	}
	const double EndTime = FPlatformTime::Seconds();
	UE_LOG(LogFastBitCopyTests, Display,
		TEXT("  %s %s %d bytes : %.4f s"), FuncName, AlignName, BytesSize, EndTime - StartTime);
}

template <int BytesSize>
static void BenchSize()
{
	constexpr int LoopCount = 1000000;
	UE_LOG(LogFastBitCopyTests, Display, TEXT("--- %d bytes ---"), BytesSize);
	BenchOne<BytesSize, Original, Aligned  >(LoopCount);
	BenchOne<BytesSize, Original, Unaligned>(LoopCount);
	BenchOne<BytesSize, Fast    , Aligned  >(LoopCount);
	BenchOne<BytesSize, Fast    , Unaligned>(LoopCount);
	BenchOne<BytesSize, Hooked  , Aligned  >(LoopCount);
	BenchOne<BytesSize, Hooked  , Unaligned>(LoopCount);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFastBitCopySpeed,
	"FastBitCopy.Speed",
	BasicIntegrationTestFlags)
bool FFastBitCopySpeed::RunTest(const FString& Parameters)
{
	BenchSize<1   >();
	BenchSize<2   >();
	BenchSize<4   >();
	BenchSize<8   >();
	BenchSize<16  >();
	BenchSize<32  >();
	BenchSize<64  >();
	BenchSize<128 >();
	BenchSize<256 >();
	BenchSize<512 >();
	BenchSize<1024>();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
