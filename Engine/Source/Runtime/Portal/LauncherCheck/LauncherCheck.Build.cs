// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class LauncherCheck : ModuleRules
{
	public LauncherCheck(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
                 "HTTP",
			}
		);

        // Need to make this an option as it pulls in a developer module
        if (UEBuildConfiguration.bUseLauncherChecks)
        {
            Definitions.Add("WITH_LAUNCHERCHECK=1");
            PublicDependencyModuleNames.Add("DesktopPlatform");
        }
    }
}
