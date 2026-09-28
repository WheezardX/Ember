#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/scatter.h"
#include "emberworld/veg.h"
THIRD_PARTY_INCLUDES_END

#include "EmberVegetationActor.generated.h"

class AEmberTerrainActor;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;

/**
 * Vegetation instancing (EPIC_5_PLAN C4, D5). Streams finest-LOD tiles within RadiusM of the
 * camera; each tile is scattered with the Terrain-conformant C++ port (emberworld::scatter_tile)
 * and instanced as one HISM per species. Instances are grounded on the RENDERED surface
 * (SurfaceSampler) rather than Terrain's raw DEM-cell z — positions/species/height/yaw/scale
 * are exactly Terrain's.
 *
 * Meshes: generated per palette key (B3, /Game/Ember/Generated/Veg/SM_<key>, M_Veg), fitted to each
 * instance's height and crown radius; engine Cone/Sphere only as a fallback for an ungenerated key.
 */
UCLASS()
class EMBERWORLD_API AEmberVegetationActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberVegetationActor();
	virtual void Tick(float DeltaSeconds) override;

	bool Init(AEmberTerrainActor* InTerrain, FString& OutError);

	/** Re-select tiles for a camera (UE cm). Returns tiles added + removed. */
	int32 UpdateStreaming(const FVector& CameraUE);

	double RadiusM = 1500.0;

	// Facts
	int64 InstancesTotal = 0;
	int32 TilesLoaded = 0;
	double ScatterMs = 0.0;       // cumulative CPU time in scatter + grounding
	int64 UngroundedInstances = 0; // fell back to Terrain's z (no valid surface under them)
	int32 GeneratedSpecies = 0;    // species drawn with a generated mesh (vs placeholder)
	TMap<FString, int64> BySpecies;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> SpeciesMesh;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SpeciesMaterial;

private:
	struct FVegTile
	{
		TArray<UHierarchicalInstancedStaticMeshComponent*> Components;  // owned by this actor
		TArray<int64> PerSpecies;
	};

	bool LoadTile(const emberworld::TileEntry& Tile, FString& OutError);
	void UnloadTile(uint64 Key);
	void RecomputeStats();

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;

	emberworld::scatter::Palette Palette;
	emberworld::ScatterInput Input;
	TArray<FString> SpeciesKeys;
	TArray<double> SpeciesMinCrownRatio;
	TArray<double> SpeciesHalfHeightFrac;  // mesh bounds (placeholder fit)
	TMap<uint64, FVegTile> Tiles;
	FVector LastCamera = FVector::ZeroVector;
	bool bInitialised = false;
	bool bFirstUpdate = true;
};
