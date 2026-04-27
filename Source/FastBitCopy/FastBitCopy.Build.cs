// Copyright (c) chen3feng. All Rights Reserved.

using UnrealBuildTool;

public class FastBitCopy : ModuleRules
{
	public FastBitCopy(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Projects",
		});

		// We need low-level OS APIs for the runtime function hook.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("psapi.lib");
		}
	}
}
