// Copyright (c) chen3feng. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * FastBitCopy module.
 *
 * On startup, this module hooks UE's CORE_API `appBitsCpy` with our optimized
 * implementation so that all existing callers (FBitReader / FBitWriter, network
 * serialization, replication, etc.) transparently get the speed-up.
 *
 * On shutdown, the hook is reverted so Core is left in a clean state.
 */
class FFastBitCopyModule : public IModuleInterface
{
public:
	// IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Returns true if the appBitsCpy hook is currently installed. */
	static bool IsHookInstalled();
};
