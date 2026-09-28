using UnrealBuildTool;

public class EmberTarget : TargetRules
{
	public EmberTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Ember", "EmberWorld" });
	}
}
