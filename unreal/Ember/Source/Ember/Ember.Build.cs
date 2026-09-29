using UnrealBuildTool;

// Game/viewer shell + agent iteration harness (EPIC_5_PLAN A2, F3 skeleton).
public class Ember : ModuleRules
{
	public Ember(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "EmberWorld" });
		PrivateDependencyModuleNames.AddRange(new string[] {
			"Json", "RenderCore", "RHI", "ImageCore", "ProceduralMeshComponent", "InputCore" });

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("dxgi.lib");  // per-process VRAM usage (scene facts)
		}
	}
}
