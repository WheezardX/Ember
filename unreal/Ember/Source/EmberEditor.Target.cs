using UnrealBuildTool;

// The inner loop builds this target: uncooked `-game` runs and headless commandlets both use
// the editor binary with these modules (EPIC_5_PLAN §2). The editor UI is never opened.
public class EmberEditorTarget : TargetRules
{
	public EmberEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "Ember", "EmberWorld" });
	}
}
