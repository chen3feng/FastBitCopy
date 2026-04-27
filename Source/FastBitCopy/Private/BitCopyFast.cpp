// Copyright (c) chen3feng. All Rights Reserved.
//
// Optimized bit-copy routine used as a drop-in replacement for UE's appBitsCpy.
// On aligned copies this is ~30x faster than the stock implementation,
// on unaligned copies ~5x faster. See README for benchmarks.

#include "BitCopyFast.h"
#include "CoreMinimal.h"
#include "Math/UnrealMathUtility.h"

#if PLATFORM_LITTLE_ENDIAN && PLATFORM_SUPPORTS_UNALIGNED_LOADS

// Copy bits when BitOffset of Src and Dest are same.
static void BitsCopyFastAligned(uint8* Dest, uint8* Src, int BitOffset, int BitCount)
{
	// Copy leading bits: Align to byte boundary
	if (BitOffset != 0)
	{
		int SrcCopyBits = 8 - BitOffset;
		int CopyBits = FMath::Min(SrcCopyBits, BitCount);
		BitCount -= CopyBits;
		uint8 Mask = (0xFF << BitOffset) & (0xFF >> uint32(SrcCopyBits - CopyBits));
		uint8 Word = *Src & Mask;
		*Dest &= ~Mask; // Clear dest
		*Dest |= Word;  // Copy bits
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
		uint8 Mask = 0xFF >> uint32(8 - BitCount);
		uint8 SrcBits = *SrcB & Mask;
		*DestB &= ~Mask;
		*DestB |= SrcBits;
	}
}

// Copy bits with source bit offset aligned.
template <typename WordType>
static void CopyBitsSrcAligned(WordType* Dest, int DestBit, WordType* Src, int BitCount)
{
	// Handle middle words
	const int BitsPerWord = sizeof(WordType) * 8;
	const WordType AllOnes = ~WordType(0);
	int DestCopyBits = BitsPerWord - DestBit;
	WordType Mask = AllOnes << DestBit;
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
				Dest[i] = Src[i];
			}
		}
		else
		{
			for (int i = 0; i < LoopCount; ++i)
			{
				WordType Word = Src[i];
				Dest[i] &= ~Mask;							 // Clear high bits
				Dest[i] |= Word << DestBit;					 // Set high bits
				Dest[i + 1] &= Mask;						 // Clear low bits
				Dest[i + 1] |= Word >> uint32(DestCopyBits); // Set low bits
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
			Mask = AllOnes >> (BitsPerWord - BitCount);
			WordType Word = *Src & Mask;
			WordType CopyBits = FMath::Min(BitCount, DestCopyBits);
			Mask = (AllOnes << DestBit) & (AllOnes >> uint32(DestCopyBits - CopyBits));
			*Dest &= ~Mask;
			*Dest |= Word << DestBit;
			Dest += (CopyBits + DestBit) / BitsPerWord;
			DestBit = (CopyBits + DestBit) % BitsPerWord;
			BitCount -= CopyBits;
			if (BitCount > 0)
			{
				Mask = AllOnes >> uint32(BitsPerWord - BitCount);
				*Dest &= ~Mask;
				*Dest |= Word >> CopyBits;
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

static void BitsCopyFastUnaligned(uint8* Dest, int DestBit, uint8* Src, int SrcBit, int BitCount)
{
	// Align SrcBit to 0
	if (SrcBit != 0)
	{
		int CopySrcBits = 8 - SrcBit;
		int CopyBits = FMath::Min(CopySrcBits, BitCount);
		BitCount -= CopyBits;
		uint8 Mask = (0xFF << SrcBit) & (0xFF >> uint32(CopySrcBits - CopyBits));
		uint8 Word = *Src & Mask;
		int DestCopyBits = 8 - DestBit;
		uint32 OverlappedBits = FMath::Min(CopyBits, DestCopyBits);
		Mask = (0xFF << DestBit) & (0xFF >> uint32(DestCopyBits - OverlappedBits));
		*Dest &= ~Mask;
		if (DestBit > SrcBit)
			*Dest |= Word << (DestBit - SrcBit);
		else
			*Dest |= Word >> (SrcBit - DestBit);
		Dest += (OverlappedBits + DestBit) / 8;
		DestBit = (OverlappedBits + DestBit) % 8;
		CopyBits -= OverlappedBits;
		if (CopyBits > 0)
		{
			Mask = 0xFF >> (8 - CopyBits);
			*Dest &= ~Mask;
			*Dest |= Word >> uint32(SrcBit + OverlappedBits);
			DestBit = CopyBits;
		}
		++Src;
		SrcBit = 0;
	}
	CopyBitsSrcAligned((uint64*)Dest, DestBit, (uint64*)Src, BitCount);
}

// ----------------------------------------------------------------------------
// Original appBitsCpy (kept verbatim for benchmarking & as a fallback reference)
// ----------------------------------------------------------------------------
static FORCEINLINE void OriginalAppBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
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
void appBitsCpyFastImpl(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
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
