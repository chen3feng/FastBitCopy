// Copyright (c) chen3feng. All Rights Reserved.

using UnrealBuildTool;

public class FastBitCopyHost : ModuleRules
{
	public FastBitCopyHost(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine", "FastBitCopy"
		});
	}
}
