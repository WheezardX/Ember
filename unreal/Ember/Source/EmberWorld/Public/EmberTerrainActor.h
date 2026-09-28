#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/heightfield.h"
#include "emberworld/region.h"
THIRD_PARTY_INCLUDES_END

#include "EmberTerrainActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * A Terrain tile-store region rendered as runtime meshes (EPIC_5_PLAN C2, D4): one
 * procedural-mesh section per tile. Phase 0 loads a single LOD for the whole region; the
 * camera-driven streaming manager (C3) replaces LoadRegion's "all tiles" policy.
 *
 * Frame: UE X = east, Y = south, Z = up, centimetres, anchored at the region's extent centre
 * and z_min (emberworld::region_frame).
 */
UCLASS()
class EMBERWORLD_API AEmberTerrainActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberTerrainActor();

	/** Load `RegionDir` (contains manifest.json) at `Lod` (-1 = finest). */
	bool LoadRegion(const FString& RegionDir, int32 Lod, FString& OutError);

	/** Ground height (metres, region CRS) at a world point, from the finest LOD. */
	bool GroundHeightAt(double WorldX, double WorldY, double& OutZ) const;

	/** Region-CRS metres -> UE world location. */
	FVector WorldToUE(double X, double Y, double Z) const;

	/** Base colour for the Phase 0 gray-shaded material. */
	void SetBaseColor(const FLinearColor& Color);

	const emberworld::Region* GetRegion() const { return Region.IsValid() ? Region.Get() : nullptr; }

	/** World-metre bounds of the loaded valid data (tiles can extend past the AOI). */
	const emberworld::Bounds& GetDataExtent() const { return DataExtent; }
	const emberworld::Frame& GetFrame() const { return Frame; }

	// Facts (A2) — read by the scene-facts subsystem.
	int32 TilesTotal = 0;
	int32 TilesLoaded = 0;
	int64 Triangles = 0;
	int64 SkirtTriangles = 0;
	int64 NodataCorners = 0;
	double LoadMs = 0.0;
	TMap<int32, int32> LodHistogram;
	FString RegionName;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

private:
	TUniquePtr<emberworld::Region> Region;
	emberworld::Frame Frame;
	emberworld::Bounds DataExtent;
};
