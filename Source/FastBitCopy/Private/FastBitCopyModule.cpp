// Copyright (c) chen3feng. All Rights Reserved.

#include "FastBitCopyModule.h"
#include "FastBitCopy.h"
#include "FunctionHook.h"

#include "Modules/ModuleManager.h"
#include "Misc/CoreMiscDefines.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogFastBitCopy, Log, All);

// Forward declaration of UE's exported bit copy function.
// CORE_API is already set on the real declaration in BitReader.h; re-declaring
// with CORE_API here guarantees we link to the same exported symbol.
CORE_API void appBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount);

// Build-path self-identification probe, defined in FastBitCopy.cpp. Returns 1
// iff that translation unit compiled the real optimized path (the
// PLATFORM_LITTLE_ENDIAN branch), 0 if it compiled the fallback-to-stock
// path. Installing the hook in the fallback case would create an infinite
// recursion (HookedAppBitsCpy -> FastBitCopy -> appBitsCpy ->
// HookedAppBitsCpy -> ...), so we gate on this probe.
int FastBitCopy_IsOptimizedBuild();

namespace
{
	static FFunctionHook GBitsCpyHook;
	static bool          GBitsCpyHookInstalled = false;

	// Hook detour: forward to our fast implementation.
	static void HookedAppBitsCpy(uint8* Dest, int32 DestBit, uint8* Src, int32 SrcBit, int32 BitCount)
	{
		FastBitCopy(Dest, DestBit, Src, SrcBit, BitCount);
	}
}

void FFastBitCopyModule::StartupModule()
{
	UE_LOG(LogFastBitCopy, Log, TEXT("FastBitCopy: StartupModule called."));

#if !PLATFORM_LITTLE_ENDIAN
	UE_LOG(LogFastBitCopy, Log, TEXT("FastBitCopy: big-endian platform, skipping hook."));
#elif !FASTBITCOPY_PLATFORM_SUPPORTS_HOOK
	// iOS/tvOS: kernel enforces W^X + code-signing on all executable pages.
	// Runtime inline-hook patching is impossible; skip without attempting.
	UE_LOG(LogFastBitCopy, Log, TEXT("FastBitCopy: platform does not support runtime code patching (iOS/tvOS), skipping hook."));
#else
	if (FastBitCopy_IsOptimizedBuild() == 0)
	{
		// FastBitCopy.cpp compiled the fallback path (FastBitCopy
		// is just a thunk to appBitsCpy). Installing the hook here would
		// turn that thunk into an infinite self-call. Skip.
		UE_LOG(LogFastBitCopy, Log,
			   TEXT("FastBitCopy: compiled in fallback mode (platform does not meet requirements); skipping hook."));
		return;
	}

	void* Target = (void*)&appBitsCpy;
	void* Detour = (void*)&HookedAppBitsCpy;

	UE_LOG(LogFastBitCopy, Log, TEXT("FastBitCopy: Target=%p, Detour=%p"), Target, Detour);

	if (Target == Detour)
	{
		UE_LOG(LogFastBitCopy, Warning, TEXT("appBitsCpy and the detour resolve to the same address — skipping hook."));
		return;
	}

	// Dump the first 32 bytes of the target function for diagnostics.
	{
		const uint8 *P = (const uint8 *)Target;
		UE_LOG(LogFastBitCopy, Log,
			   TEXT("FastBitCopy: Target prologue: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X"),
			   P[0], P[1], P[2], P[3], P[4], P[5], P[6], P[7],
			   P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15],
			   P[16], P[17], P[18], P[19], P[20], P[21], P[22], P[23],
			   P[24], P[25], P[26], P[27], P[28], P[29], P[30], P[31]);
	}

	const bool bOK = GBitsCpyHook.Install(Target, Detour, nullptr);
	GBitsCpyHookInstalled = bOK;
	if (bOK)
	{
		UE_LOG(LogFastBitCopy, Log, TEXT("Installed runtime hook for appBitsCpy -> FastBitCopy."));
	}
	else
	{
		UE_LOG(LogFastBitCopy, Warning, TEXT("Failed to install runtime hook for appBitsCpy. Falling back to UE's implementation."));
	}
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
