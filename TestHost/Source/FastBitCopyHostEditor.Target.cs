// Copyright (c) chen3feng. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class FastBitCopyHostEditorTarget : TargetRules
{
	public FastBitCopyHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("FastBitCopyHost");
	}
}
