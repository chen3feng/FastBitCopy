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

		// iOS/iPadOS/tvOS enforce W^X and code-signing on all executable
		// pages, making runtime inline-hook patching impossible. The module
		// still compiles (graceful no-op) but we mark it unsupported so
		// packaging pipelines can strip it entirely if desired.
		if (Target.Platform == UnrealTargetPlatform.IOS ||
			Target.Platform == UnrealTargetPlatform.TVOS)
		{
			// Define a macro so C++ code can skip hook logic at compile time.
			PublicDefinitions.Add("FASTBITCOPY_PLATFORM_SUPPORTS_HOOK=0");
		}
		else
		{
			PublicDefinitions.Add("FASTBITCOPY_PLATFORM_SUPPORTS_HOOK=1");
		}
	}
}
