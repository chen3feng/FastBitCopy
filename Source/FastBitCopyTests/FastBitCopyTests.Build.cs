// Copyright (c) chen3feng. All Rights Reserved.

using UnrealBuildTool;

public class FastBitCopyTests : ModuleRules
{
	public FastBitCopyTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"FastBitCopy",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
		});
	}
}
