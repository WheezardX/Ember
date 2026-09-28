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
 * A Terrain tile-store region rendered as runtime meshes (EPIC_5_PLAN C2/C3, D4): one
 * procedural-mesh section per tile.
 *   fixed LOD  (LoadRegion(dir, lod >= 0)): every tile of that level, once — test fixtures.
 *   streaming  (LoadRegion(dir, -1)):       each tick, emberworld::select_tiles picks a
 *               crack-free-with-skirts LOD cut around the camera; sections are diffed in/out.
 *
 * Frame: UE X = east, Y = south, Z = up, centimetres, anchored at the region's tile-grid centre
 * and z_min (emberworld::region_frame).
 */
UCLASS()
class EMBERWORLD_API AEmberTerrainActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberTerrainActor();
	virtual void Tick(float DeltaSeconds) override;

	/** Load `RegionDir` (contains manifest.json). FixedLod >= 0: that level only; -1: stream. */
	bool LoadRegion(const FString& RegionDir, int32 FixedLod, FString& OutError);

	/** Re-select tiles for a camera (UE cm). Returns the number of tiles added + removed. */
	int32 UpdateStreaming(const FVector& CameraUE);

	/** Ground height (metres, region CRS) at a world point, from the finest LOD. */
	bool GroundHeightAt(double WorldX, double WorldY, double& OutZ) const;

	/** Region-CRS metres -> UE world location. */
	FVector WorldToUE(double X, double Y, double Z) const;

	/** Base colour for the clay material. */
	void SetBaseColor(const FLinearColor& Color);

	const emberworld::Region* GetRegion() const { return Region.IsValid() ? Region.Get() : nullptr; }

	/** World-metre bounds of the valid data (tiles can extend past the AOI). */
	const emberworld::Bounds& GetDataExtent() const { return DataExtent; }
	const emberworld::Frame& GetFrame() const { return Frame; }
	bool IsStreaming() const { return bStreaming; }

	/** Streaming: refine a tile while camera distance < RefineFactor * tile span. */
	double RefineFactor = 1.5;

	// Facts (A2) — read by the scene-facts subsystem; describe what is loaded right now.
	int32 TilesTotal = 0;
	int32 TilesLoaded = 0;
	int64 Triangles = 0;
	int64 SkirtTriangles = 0;
	int64 NodataCorners = 0;
	double LoadMs = 0.0;
	double LastStreamMs = 0.0;
	int32 StreamUpdates = 0;
	TMap<int32, int32> LodHistogram;
	FString RegionName;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

private:
	struct FLoadedTile
	{
		int32 Section = 0;
		int32 Lod = 0;
		int64 Triangles = 0, SkirtTriangles = 0, NodataCorners = 0;
	};

	bool LoadTile(const emberworld::TileEntry& Tile, FString& OutError);
	void UnloadTile(uint64 Key);
	void RecomputeStats();

	TUniquePtr<emberworld::Region> Region;
	emberworld::Frame Frame;
	emberworld::Bounds DataExtent;
	TMap<uint64, FLoadedTile> Loaded;
	TArray<int32> FreeSections;
	int32 NextSection = 0;
	bool bStreaming = false;
	FVector LastStreamCamera = FVector::ZeroVector;
};
