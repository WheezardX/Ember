using System.IO;
using UnrealBuildTool;

// World streaming (EPIC_5_PLAN workstream C). The engine-free core lives in <repo>/worldcore
// (tested by CMake in CI) and is compiled into this module by WorldCoreUnity.cpp — one source
// of truth, no copies.
public class EmberWorld : ModuleRules
{
	public EmberWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		string Repo = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "..", ".."));
		PublicIncludePaths.Add(Path.Combine(Repo, "worldcore", "include"));
		PrivateIncludePaths.Add(Path.Combine(Repo, "worldcore"));
		PrivateIncludePaths.Add(Path.Combine(Repo, "sim", "third_party"));
		bEnableExceptions = true;  // nlohmann/json + toml++ inside the core
		FPSemantics = FPSemanticsMode.Precise;  // scatter port must match CPython doubles exactly (C4)
		PublicDefinitions.Add("EMBERWORLD_CORE_API=EMBERWORLD_API");  // export core symbols from this DLL

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "ProceduralMeshComponent" });
	}
}
