// Copyright (c) chen3feng. All Rights Reserved.
//
// Optimized bit-copy routine used as a drop-in replacement for UE's appBitsCpy.
// On aligned copies this is ~30x faster than the stock implementation,
// on unaligned copies ~5x faster. See README for benchmarks.

#include "BitCopyFast.h"
#include "CoreMinimal.h"
#include "Math/UnrealMathUtility.h"
#include <atomic>
#include <cstring>

// A *real* compiler barrier. std::atomic_signal_fence turned out to be
// insufficient on Clang/GCC -O2 (it disciplines reorderings with respect to
// signal handlers, but doesn't stop the optimizer from concluding that a
// uint8* store cannot affect a subsequent uint64* read). An inline-asm
// memory clobber, on the other hand, forces the compiler to treat every
// reachable memory location as potentially read/written.
#if defined(__GNUC__) || defined(__clang__)
#define FASTBITCOPY_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#elif defined(_MSC_VER)
#include <intrin.h>
#define FASTBITCOPY_COMPILER_BARRIER() _ReadWriteBarrier()
#else
#define FASTBITCOPY_COMPILER_BARRIER() std::atomic_thread_fence(std::memory_order_seq_cst)
#endif

// FASTBITCOPY_NOINLINE was a defensive noinline attribute added to prevent
// GCC/Clang from inlining the hot functions into callers and then applying
// aggressive interprocedural dead-store elimination. The root causes of
// the observed -O2 divergence turned out to be a dangling-reference bug
// in FMath::Min/Max (PR #6) and alignment UB (PR #7), not an inlining
// issue. Removed: the compiler is now free to inline as it sees fit.

// FASTBITCOPY_SAFE_OPT was a per-function optimization override that
// compiled the hot helpers at -O0 on GCC/Clang to work around a
// miscompilation observed in issue #2. The root cause turned out to be
// a dangling-reference bug in the CI shim's FMath::Min/Max (fixed in
// PR #6) and an alignment-UB in CopyBitsSrcAligned (fixed in PR #7),
// not a compiler bug. Removed: all functions now compile at the
// project-level optimization (-O2 in Release).

#if PLATFORM_LITTLE_ENDIAN && PLATFORM_SUPPORTS_UNALIGNED_LOADS

// Copy bits when BitOffset of Src and Dest are same.
static void BitsCopyFastAligned(uint8 *Dest, uint8 *Src, int BitOffset, int BitCount)
{
	// Copy leading bits: Align to byte boundary
	if (BitOffset != 0)
	{
		int SrcCopyBits = 8 - BitOffset;
		int CopyBits = FMath::Min(SrcCopyBits, BitCount);
		BitCount -= CopyBits;
		// NOTE: SrcCopyBits - CopyBits is in [0, 7]. Cast both to int to avoid
		// accidental unsigned promotion producing a huge shift count on
		// GCC/Clang.
		const int TailShift = SrcCopyBits - CopyBits;
		uint8 Mask = uint8((uint32(0xFF) << BitOffset) & (uint32(0xFF) >> uint32(TailShift)));
		uint8 Word = uint8(*Src & Mask);
		*Dest = uint8((*Dest & ~Mask) | Word);
		Dest += (CopyBits + BitOffset) / 8;
		++Src;
	}

	// Copy middle bytes
	int NumBytes = BitCount / 8;
	FMemory::Memcpy(Dest, Src, NumBytes);
	BitCount -= NumBytes * 8;

	// Copy tail bits
	if (BitCount > 0)
	{
		uint8* SrcB = (uint8*)Src + NumBytes;
		uint8* DestB = (uint8*)Dest + NumBytes;
		// BitCount is in [1, 7] here, so 8 - BitCount is in [1, 7]. Safe.
		uint8 Mask = uint8(uint32(0xFF) >> uint32(8 - BitCount));
		uint8 SrcBits = uint8(*SrcB & Mask);
		*DestB = uint8((*DestB & ~Mask) | SrcBits);
	}
}

// Unaligned word load/store. Clang/GCC/MSVC all fold sizeof-bounded
// memcpy of a trivially-copyable scalar to a single machine load/store
// at -O1 and above, so this is both language-legal (no strict-aliasing
// or alignment UB) *and* generates the same single `mov` we want on
// x86/x64 and arm64. On platforms with hardware-misaligned-load support
// (PLATFORM_SUPPORTS_UNALIGNED_LOADS) the runtime cost is identical to
// a raw `*p` dereference; the difference is strictly in what the C++
// standard and UBSan consider well-defined.
//
// Rationale for not just reverting to `Dest[i] = Src[i]`: `CopyBitsSrcAligned`
// is called from `BitsCopyFastUnaligned` after a leading-byte alignment pass
// that only guarantees *byte* alignment of the remaining pointer, then casts
// it to `uint64*`. UBSan with `-fsanitize=alignment` correctly flags that
// cast-then-dereference sequence as UB; wrapping the access in memcpy is the
// standard, zero-cost resolution.
template <typename WordType>
static FORCEINLINE WordType LoadWordUnaligned(const WordType *P)
{
	WordType W;
	std::memcpy(&W, P, sizeof(W));
	return W;
}

template <typename WordType>
static FORCEINLINE void StoreWordUnaligned(WordType *P, WordType W)
{
	std::memcpy(P, &W, sizeof(W));
}

// Copy bits with source bit offset aligned.
template <typename WordType>
static void CopyBitsSrcAligned(WordType *Dest, int DestBit, WordType *Src, int BitCount)
{
	// Handle middle words
	const int BitsPerWord = sizeof(WordType) * 8;
	const WordType AllOnes = ~WordType(0);
	const int DestCopyBits = BitsPerWord - DestBit;
	const WordType Mask = WordType(AllOnes << uint32(DestBit));
	int LoopCount = BitCount / BitsPerWord;
	// For any type larger than 1 byte, the last word is not guaranteed to be accessible
	if (sizeof(WordType) > 1)
	{
		--LoopCount;
	}
	if (LoopCount > 0)
	{
		// Fast path: DestBit == 0 means both Src and Dest are word-aligned at
		// this point, so we can just memcpy the middle words. This also
		// dodges the shift-by-BitsPerWord that the generic loop below would
		// otherwise perform (`Word >> DestCopyBits` with DestCopyBits ==
		// BitsPerWord is UB in C/C++, and Clang/GCC do not silently fold it
		// to zero the way MSVC does).
		if (DestBit == 0)
		{
			for (int i = 0; i < LoopCount; ++i)
			{
				StoreWordUnaligned(&Dest[i], LoadWordUnaligned(&Src[i]));
			}
		}
		else
		{
			const uint32 UDestBit = uint32(DestBit);
			const uint32 UDestCopyBits = uint32(DestCopyBits);
			for (int i = 0; i < LoopCount; ++i)
			{
				const WordType Word = LoadWordUnaligned(&Src[i]);
				const WordType D0 = LoadWordUnaligned(&Dest[i]);
				const WordType D1 = LoadWordUnaligned(&Dest[i + 1]);
				StoreWordUnaligned(&Dest[i], WordType((D0 & ~Mask) | WordType(Word << UDestBit)));
				StoreWordUnaligned(&Dest[i + 1], WordType((D1 & Mask) | WordType(Word >> UDestCopyBits)));
			}
		}
		Src += LoopCount;
		Dest += LoopCount;
		BitCount -= LoopCount * BitsPerWord;
	}

	// Copy tail bits
	if (BitCount > 0)
	{
		if (sizeof(WordType) == 1)
		{
			// BitCount is in [1, BitsPerWord] = [1, 8] here.
			const int HeadTailShift = BitsPerWord - BitCount; // [0, 7]
			WordType TailMask = WordType(AllOnes >> uint32(HeadTailShift));
			const WordType Word = WordType(*Src & TailMask);
			const int CopyBits = FMath::Min(BitCount, DestCopyBits); // [1, 8]
			const int TailShift2 = DestCopyBits - CopyBits;			 // [0, 7]
			TailMask = WordType((AllOnes << uint32(DestBit)) & (AllOnes >> uint32(TailShift2)));
			*Dest = WordType((*Dest & ~TailMask) | WordType(Word << uint32(DestBit)));
			Dest += (CopyBits + DestBit) / BitsPerWord;
			DestBit = (CopyBits + DestBit) % BitsPerWord;
			BitCount -= CopyBits;
			if (BitCount > 0)
			{
				// BitCount is in [1, 7] here.
				TailMask = WordType(AllOnes >> uint32(BitsPerWord - BitCount));
				*Dest = WordType((*Dest & ~TailMask) | WordType(Word >> uint32(CopyBits)));
				DestBit = BitCount;
			}
		}
		else
		{
			// Handle the tail bits byte by byte.
			uint8* DestB = (uint8*)Dest + DestBit / 8;
			DestBit %= 8;
			CopyBitsSrcAligned(DestB, DestBit, (uint8*)Src, BitCount);
		}
	}
}

static void BitsCopyFastUnaligned(uint8 *Dest, int DestBit, uint8 *Src, int SrcBit, int BitCount)
{
	// Align SrcBit to 0
	if (SrcBit != 0)
	{
		const int CopySrcBits = 8 - SrcBit;						// [1, 7]
		const int CopyBits = FMath::Min(CopySrcBits, BitCount); // [1, 7]
		BitCount -= CopyBits;
		// Both shift amounts are in [0, 7] here.
		const int PreMaskTailShift = CopySrcBits - CopyBits; // [0, 6]
		uint8 Mask = uint8((uint32(0xFF) << SrcBit) & (uint32(0xFF) >> uint32(PreMaskTailShift)));
		const uint8 Word = uint8(*Src & Mask);

		const int DestCopyBits = 8 - DestBit;						   // [1, 8]
		const int OverlappedBits = FMath::Min(CopyBits, DestCopyBits); // [0, 7]
		const int DestTailShift = DestCopyBits - OverlappedBits;	   // [0, 7]
		Mask = uint8((uint32(0xFF) << DestBit) & (uint32(0xFF) >> uint32(DestTailShift)));

		// Shift Word from its SrcBit position to the DestBit position. The
		// shift count is in [-7, 7] but we always reduce it to a non-negative
		// left OR right shift. Using uint32 intermediates keeps the shift
		// well-defined on all platforms (no implicit int promotion weirdness).
		uint32 Shifted;
		if (DestBit >= SrcBit)
		{
			Shifted = uint32(Word) << uint32(DestBit - SrcBit);
		}
		else
		{
			Shifted = uint32(Word) >> uint32(SrcBit - DestBit);
		}
		*Dest = uint8((*Dest & ~Mask) | (uint8(Shifted) & Mask));

		Dest += (OverlappedBits + DestBit) / 8;
		DestBit = (OverlappedBits + DestBit) % 8;

		int RemainingBits = CopyBits - OverlappedBits; // [0, 7]
		if (RemainingBits > 0)
		{
			// RemainingBits is in [1, 7], so (8 - RemainingBits) is in [1, 7].
			Mask = uint8(uint32(0xFF) >> uint32(8 - RemainingBits));
			const uint32 TailShift = uint32(SrcBit) + uint32(OverlappedBits); // [1, 14], but Word's top bit is at position 7
			const uint8 TailBits = uint8(uint32(Word) >> TailShift);
			*Dest = uint8((*Dest & ~Mask) | (TailBits & Mask));
			DestBit = RemainingBits;
		}
		++Src;
		SrcBit = 0;
	}
	// Compiler barrier: GCC/Clang otherwise aggressively elide our uint8*
	// stores above on the assumption they don't alias the uint64* reads that
	// follow. This is a pure compile-time fence (no runtime cost) that
	// forces the compiler to keep them.
	FASTBITCOPY_COMPILER_BARRIER();
	CopyBitsSrcAligned((uint64*)Dest, DestBit, (uint64*)Src, BitCount);
}

// ----------------------------------------------------------------------------
// Original appBitsCpy (kept verbatim for benchmarking & as a fallback reference)
// ----------------------------------------------------------------------------
// The original appBitsCpy reference implementation. FORCEINLINE on all
// platforms: it is only called from OriginalAppBitsCpyForTest (benchmark
// harness) and the compiler can decide whether to actually inline it.
static FORCEINLINE void OriginalAppBitsCpy(uint8 *Dest, int32 DestBit, uint8 *Src, int32 SrcBit, int32 BitCount)
{
	if (BitCount <= 8)
	{
		uint32 DestIndex = DestBit / 8;
		uint32 SrcIndex = SrcBit / 8;
		uint32 LastDest = (DestBit + BitCount - 1) / 8;
		uint32 LastSrc = (SrcBit + BitCount - 1) / 8;
		uint32 ShiftSrc = SrcBit & 7;
		uint32 ShiftDest = DestBit & 7;
		uint32 FirstMask = 0xFF << ShiftDest;
		uint32 LastMask = 0xFE << ((DestBit + BitCount - 1) & 7);
		uint32 Accu;

		if (SrcIndex == LastSrc)
			Accu = (Src[SrcIndex] >> ShiftSrc);
		else
			Accu = ((Src[SrcIndex] >> ShiftSrc) | (Src[LastSrc] << (8 - ShiftSrc)));

		if (DestIndex == LastDest)
		{
			uint32 MultiMask = FirstMask & ~LastMask;
			Dest[DestIndex] = ((Dest[DestIndex] & ~MultiMask) | ((Accu << ShiftDest) & MultiMask));
		}
		else
		{
			Dest[DestIndex] = (uint8)((Dest[DestIndex] & ~FirstMask) | ((Accu << ShiftDest) & FirstMask));
			Dest[LastDest] = (uint8)((Dest[LastDest] & LastMask) | ((Accu >> (8 - ShiftDest)) & ~LastMask));
		}
		return;
	}

	uint32 DestIndex = DestBit / 8;
	uint32 FirstSrcMask = 0xFF << (DestBit & 7);
	uint32 LastDest = (DestBit + BitCount) / 8;
	uint32 LastSrcMask = 0xFF << ((DestBit + BitCount) & 7);
	uint32 SrcIndex = SrcBit / 8;
	uint32 LastSrc = (SrcBit + BitCount) / 8;
	int32  ShiftCount = (DestBit & 7) - (SrcBit & 7);
	int32  DestLoop = LastDest - DestIndex;
	int32  SrcLoop = LastSrc - SrcIndex;
	uint32 FullLoop;
	uint32 BitAccu;

	if (ShiftCount >= 0)
	{
		FullLoop = FMath::Max(DestLoop, SrcLoop);
		BitAccu = Src[SrcIndex] << ShiftCount;
		ShiftCount += 8;
	}
	else
	{
		ShiftCount += 8;
		FullLoop = FMath::Max(DestLoop, SrcLoop - 1);
		BitAccu = Src[SrcIndex] << ShiftCount;
		SrcIndex++;
		ShiftCount += 8;
		BitAccu = (((uint32)Src[SrcIndex] << ShiftCount) + (BitAccu)) >> 8;
	}

	Dest[DestIndex] = (uint8)((BitAccu & FirstSrcMask) | (Dest[DestIndex] & ~FirstSrcMask));
	SrcIndex++;
	DestIndex++;

	for (; FullLoop > 1; FullLoop--)
	{
		BitAccu = (((uint32)Src[SrcIndex] << ShiftCount) + (BitAccu)) >> 8;
		SrcIndex++;
		Dest[DestIndex] = (uint8)BitAccu;
		DestIndex++;
	}

	if (LastSrcMask != 0xFF)
	{
		if ((uint32)(SrcBit + BitCount - 1) / 8 == SrcIndex)
		{
			BitAccu = (((uint32)Src[SrcIndex] << ShiftCount) + (BitAccu)) >> 8;
		}
		else
		{
			BitAccu = BitAccu >> 8;
		}
		Dest[DestIndex] = (uint8)((Dest[DestIndex] & LastSrcMask) | (BitAccu & ~LastSrcMask));
	}
}

void OriginalAppBitsCpyForTest(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
{
	OriginalAppBitsCpy(Dest, DestBit, Src, SrcBit, BitCount);
}

// Our optimized bit copy entry point.
void appBitsCpyFastImpl(uint8 *Dest, int32 DestBit, uint8 *Src, int32 SrcBit, int32 BitCount)
{
	// Align to byte bound.
	Dest += DestBit / 8;
	DestBit %= 8;
	Src += SrcBit / 8;
	SrcBit %= 8;

	if (SrcBit == DestBit)
	{
		BitsCopyFastAligned(Dest, Src, SrcBit, BitCount);
		return;
	}
	BitsCopyFastUnaligned(Dest, DestBit, Src, SrcBit, BitCount);
}

// Build-path self-identification probe (used by the CI harness to confirm
// we're on the optimized path, not the fallback).
CORE_API int FastBitCopy_IsOptimizedBuild() { return 1; }

#else // !PLATFORM_LITTLE_ENDIAN || !PLATFORM_SUPPORTS_UNALIGNED_LOADS

// Fallback: just forward to UE's implementation by declaring it and calling through.
CORE_API void appBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount);

void appBitsCpyFastImpl(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
{
	appBitsCpy(Dest, DestBit, Src, SrcBit, BitCount);
}

void OriginalAppBitsCpyForTest(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
{
	appBitsCpy(Dest, DestBit, Src, SrcBit, BitCount);
}

CORE_API int FastBitCopy_IsOptimizedBuild() { return 0; }

#endif
