// Copyright (c) chen3feng. All Rights Reserved.

#include "FastBitCopy.h"
#include "BitCopyFast.h"
#include "FunctionHook.h"

#include "Modules/ModuleManager.h"
#include "Misc/CoreMiscDefines.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogFastBitCopy, Log, All);

// Forward declaration of UE's exported bit copy function.
// CORE_API is already set on the real declaration in BitReader.h; re-declaring
// with CORE_API here guarantees we link to the same exported symbol.
CORE_API void appBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount);

namespace
{
	static FFunctionHook GBitsCpyHook;
	static bool          GBitsCpyHookInstalled = false;

	// Hook detour: forward to our fast implementation.
	static void HookedAppBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
	{
		appBitsCpyFastImpl(Dest, DestBit, Src, SrcBit, BitCount);
	}
}

void FFastBitCopyModule::StartupModule()
{
#if PLATFORM_LITTLE_ENDIAN && PLATFORM_SUPPORTS_UNALIGNED_LOADS
	void* Target = (void*)&appBitsCpy;
	void* Detour = (void*)&HookedAppBitsCpy;

	if (Target == Detour)
	{
		UE_LOG(LogFastBitCopy, Warning, TEXT("appBitsCpy and the detour resolve to the same address — skipping hook."));
		return;
	}

	const bool bOK = GBitsCpyHook.Install(Target, Detour, nullptr);
	GBitsCpyHookInstalled = bOK;
	if (bOK)
	{
		UE_LOG(LogFastBitCopy, Log, TEXT("Installed runtime hook for appBitsCpy -> appBitsCpyFastImpl."));
	}
	else
	{
		UE_LOG(LogFastBitCopy, Warning, TEXT("Failed to install runtime hook for appBitsCpy. Falling back to UE's implementation."));
	}
#else
	UE_LOG(LogFastBitCopy, Log, TEXT("FastBitCopy: unsupported platform, skipping hook."));
#endif
}

void FFastBitCopyModule::ShutdownModule()
{
	if (GBitsCpyHookInstalled)
	{
		GBitsCpyHook.Uninstall();
		GBitsCpyHookInstalled = false;
		UE_LOG(LogFastBitCopy, Log, TEXT("Uninstalled runtime hook for appBitsCpy."));
	}
}

bool FFastBitCopyModule::IsHookInstalled()
{
	return GBitsCpyHookInstalled;
}

IMPLEMENT_MODULE(FFastBitCopyModule, FastBitCopy);
