// Copyright (c) chen3feng. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Same signature as UE's CORE_API `appBitsCpy`.
 *
 * Copies `BitCount` bits from `Src` (starting at bit `SrcBit`) to `Dest`
 * (starting at bit `DestBit`). This is a drop-in, much faster replacement
 * for UE's implementation.
 *
 * Only supports little-endian platforms (x64, arm64, etc.).
 */
FASTBITCOPY_API void appBitsCpyFastImpl(uint8 *Dest, int32 DestBit, uint8 *Src, int32 SrcBit, int32 BitCount);

/** Copy of the original appBitsCpy, kept for benchmarking. */
FASTBITCOPY_API void OriginalAppBitsCpyForTest(uint8 *Dest, int32 DestBit, uint8 *Src, int32 SrcBit, int32 BitCount);
