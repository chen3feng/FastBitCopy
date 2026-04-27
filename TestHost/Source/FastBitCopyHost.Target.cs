// Copyright (c) chen3feng. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class FastBitCopyHostTarget : TargetRules
{
	public FastBitCopyHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("FastBitCopyHost");
	}
}
