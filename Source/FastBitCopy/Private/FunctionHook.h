// Copyright (c) chen3feng. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Minimal cross-platform function hooking (inline / trampoline hook).
 *
 * Supports:
 *   - x86_64 (Windows / Linux / macOS)
 *   - arm64  (Windows / Linux / macOS)
 *
 * The hook overwrites the first N bytes of the target function with a jump
 * to `Detour`. The bytes replaced are relocated into a newly-allocated
 * executable trampoline, followed by a jump back to the rest of the
 * original function, so the original can still be invoked through the
 * returned trampoline pointer.
 */
class FFunctionHook
{
public:
	FFunctionHook();
	~FFunctionHook();

	FFunctionHook(const FFunctionHook&) = delete;
	FFunctionHook& operator=(const FFunctionHook&) = delete;

	/**
	 * Install the hook.
	 * @param Target   Address of the function to hook.
	 * @param Detour   Address of the replacement function.
	 * @param OutTrampoline [optional] Receives a callable pointer that
	 *                 behaves like the original, unhooked `Target`.
	 * @return true on success.
	 */
	bool Install(void* Target, void* Detour, void** OutTrampoline = nullptr);

	/** Revert the hook, restoring the original first instructions. */
	bool Uninstall();

	bool IsInstalled() const { return bInstalled; }

private:
	void*   TargetAddr    = nullptr;
	void*   TrampolineMem = nullptr;
	uint8   OriginalBytes[32] = {};
	int32   PatchSize     = 0;
	bool    bInstalled    = false;
};
