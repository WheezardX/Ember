#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/scatter.h"
#include "emberworld/veg.h"
THIRD_PARTY_INCLUDES_END

#include "EmberVegetationActor.generated.h"

class AEmberTerrainActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Vegetation instancing (EPIC_5_PLAN C4, D5). Streams finest-LOD tiles within RadiusM of the
 * camera; each tile is scattered with the Terrain-conformant C++ port (emberworld::scatter_tile)
 * and instanced as one ISM per species. Instances are grounded on the RENDERED surface
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

	/** Wind sway (M_Veg WPO): crown-top sway in cm for a 10 m tree; compass dir wind comes FROM. */
	void SetWind(double StrengthCm, double FromDeg);
	/** The wind clock (s). The harness owns it: frozen for stills, frame/fps for orbits (D7). */
	void SetWindTime(double Seconds);

	/**
	 * B3 silhouette sheet: one tree per species at its palette mid height, in a row centred on
	 * (WorldX, WorldY) along compass heading RowDeg, grounded on the surface, plus a 1.8 m post
	 * for scale. Disables tile streaming.
	 */
	void SpawnLineup(double WorldX, double WorldY, double RowDeg);

	// Facts
	int64 InstancesTotal = 0;
	int32 TilesLoaded = 0;
	double ScatterMs = 0.0;       // cumulative CPU time in scatter + grounding
	int64 UngroundedInstances = 0; // drawn at Terrain's z because the tile's surface failed (error)
	int64 NoSurfaceInstances = 0;  // dropped: no rendered surface under them (outside the DEM mask)
	int32 GeneratedSpecies = 0;    // species drawn with a generated mesh (vs placeholder)
	TMap<FString, int64> BySpecies;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> SpeciesMesh;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SpeciesMaterial;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> LineupParts;

private:
	struct FVegTile
	{
		TArray<UInstancedStaticMeshComponent*> Components;  // owned by this actor
		TArray<int64> PerSpecies;
	};

	bool LoadTile(const emberworld::TileEntry& Tile, FString& OutError);
	/** Instance transform for a tree drawn with SpeciesMesh[Mesh] at this height / crown radius / yaw. */
	FTransform FitInstance(int32 Mesh, double HeightM, double CrownRadiusM, double YawRad, FVector Loc) const;
	void UnloadTile(uint64 Key);
	void RecomputeStats();

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;

	emberworld::scatter::Palette Palette;
	emberworld::ScatterInput Input;
	TArray<FString> SpeciesKeys;
	TArray<int32> IndexToSlot;         // palette species index -> unique-key slot
	TArray<int32> SlotFirstMesh;       // slot -> first of its variant meshes in SpeciesMesh
	TArray<int32> SlotNumMeshes;       // slot -> number of variants
	TArray<double> SlotHeightLineupM;  // lineup: a mature tree of the species
	TArray<double> SlotCrownRatio;
	TMap<uint64, FVegTile> Tiles;
	FVector LastCamera = FVector::ZeroVector;
	bool bInitialised = false;
	bool bFirstUpdate = true;
	bool bLineup = false;
};
