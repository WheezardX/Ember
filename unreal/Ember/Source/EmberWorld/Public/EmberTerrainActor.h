#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/heightfield.h"
#include "emberworld/look.h"
#include "emberworld/region.h"
THIRD_PARTY_INCLUDES_END

#include "EmberTerrainActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;

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
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/**
	 * Streaming is asynchronous after the first selection (8g: flying hitched on synchronous tile
	 * builds): tiles are prepared on worker threads (mesh, surface, look textures - all worldcore)
	 * and applied on the game thread within ApplyBudgetMs per frame; a tile that is no longer
	 * wanted stays until every wanted tile over its footprint is in (no holes).
	 */
	bool IsStreamingBusy() const;
	/** Bumps whenever a tile is applied or unloaded (ground cover rebuilds pending cells on it). */
	int32 GetTileGeneration() const { return TileGeneration; }
	double ApplyBudgetMs = 4.0;
	int32 MaxInFlight = 6;
	/**
	 * Load every selected tile in the frame it is selected (the pre-async behaviour). Capture runs
	 * use it: tiles arriving over many frames leave Lumen's surface cache in a different state
	 * (S_tq_terrain alpine_nw 0.965 SSIM, darker indirect), so goldens stay reproducible. Play
	 * mode and perf windows stream asynchronously.
	 */
	bool bSyncStreaming = false;

	/** Load `RegionDir` (contains manifest.json). FixedLod >= 0: that level only; -1: stream. */
	bool LoadRegion(const FString& RegionDir, int32 FixedLod, FString& OutError);

	/** Re-select tiles for a camera (UE cm). Returns the number of tiles added + removed. */
	int32 UpdateStreaming(const FVector& CameraUE);

	/** Ground height (metres, region CRS) at a world point, from the finest LOD. */
	bool GroundHeightAt(double WorldX, double WorldY, double& OutZ) const;
	/**
	 * Ground-plane weights {litter, grass, rock, shrub} at a world point (region CRS metres), from
	 * the finest loaded tile's composed ground mix (look [ground]). False where no tile covers it.
	 */
	bool GroundMixAt(double WorldX, double WorldY, float OutW[4], int32* OutLod = nullptr) const;
	/**
	 * Height of the RENDERED surface (metres): the finest loaded tile's triangles, exactly as
	 * meshed (lakebeds included). GroundHeightAt is the nearest DEM corner read from disk - on a
	 * slope that floats or sinks objects by metres. False where no loaded tile covers the point.
	 */
	bool SurfaceAt(double WorldX, double WorldY, double& OutZ) const;

	/** Region-CRS metres -> UE world location. */
	FVector WorldToUE(double X, double Y, double Z) const;
	/** Inverse of WorldToUE for heights: UE Z (cm) -> region-CRS metres. */
	double UEToWorldZ(double UEZ) const { return Frame.anchor_z + UEZ / 100.0; }

	/** Base colour for the clay material. */
	void SetBaseColor(const FLinearColor& Color);
	/** Bind the fire state (EmberFireActor) to every tile material, now and as tiles stream in. */
	void SetFire(class UTexture* FireTex, const FLinearColor& FireRect);
	/** The fire player's clock for flame flicker (s). */
	void SetFireTime(double Seconds);

	/** Water layer dir (ember-dev water: <dir>/z{lod}/x{x}/y{y}/water_level.tif). Before LoadRegion. */
	bool SetWaterDir(const FString& Dir, FString& OutError);

	/** Use a terrain look (viz/looks/*.toml) instead of clay. Call before LoadRegion. */
	bool SetLook(const FString& LookPath, FString& OutError);
	FString LookName = TEXT("clay");

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

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> WaterMesh;  // section index == terrain section index

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> WaterMaterial;

	// Water facts
	int32 WaterTilesLoaded = 0;
	int64 WaterTriangles = 0;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Material;

	// Look mode: one albedo texture + material instance per section (index = section).
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> SectionTextures;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SectionMaterials;
	// Ground mix per section (look [ground]): linear RGBA weights litter / grass / rock / shrub.
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> SectionMixTextures;
	// CPU copy of each loaded tile's ground mix (ground cover placement reads it).
	struct FTileMix
	{
		int32 Lod = 0;
		double MinX = 0, MaxY = 0, Width = 1, Height = 1;  // apron bounds, metres
		int32 W = 0, H = 0;
		TArray<uint8> Rgba;
	};
	TMap<uint64, FTileMix> TileMixes;
	struct FTileSurface
	{
		int32 Lod = 0;
		emberworld::SurfaceSampler Sampler;
	};
	TMap<uint64, FTileSurface> TileSurfaces;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture> FireTexture;
	FLinearColor FireRect = FLinearColor(0, 0, 1, 1);
	double FireTimeS = 0.0;
	bool bFireClasses = false;  // the replay reports intensity classes (M_Terrain FireClasses)
	void SetFireClasses(bool bOn);
	void ApplyFire(UMaterialInstanceDynamic* MID) const;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> TerrainMaster;
	double ComposeMs = 0.0;  // total CPU time spent composing albedo (facts)

private:
	struct FLoadedTile
	{
		int32 Section = 0;
		int32 Lod = 0;
		int64 Triangles = 0, SkirtTriangles = 0, NodataCorners = 0;
		int64 WaterTriangles = 0;
		emberworld::Bounds Content;
		// One component per tile: a ProceduralMeshComponent rebuilds its WHOLE scene proxy when
		// any section changes (81 ms for ~90 tile sections in S_stream_tq), so tiles no longer
		// share one. Owned by this actor (attached to the root).
		UProceduralMeshComponent* Comp = nullptr;
		UProceduralMeshComponent* WaterComp = nullptr;
	};
	struct FPreparedTile;  // everything a tile needs, built off the game thread (cpp)
	using FPreparedPtr = TSharedPtr<FPreparedTile, ESPMode::ThreadSafe>;

	FPreparedPtr PrepareTile(const emberworld::TileEntry& Tile) const;  // thread-safe
	bool ApplyTile(FPreparedTile& P, FString& OutError);                 // game thread
	bool LoadTile(const emberworld::TileEntry& Tile, FString& OutError); // both, synchronously
	void UnloadTile(uint64 Key);
	void RecomputeStats();
	void PumpStreaming();

	void BindLook(int32 Section, UProceduralMeshComponent* Comp, const emberworld::TileEntry& Tile, FPreparedTile& P);
	UProceduralMeshComponent* NewTileComponent(bool bWater);

	TMap<uint64, TFuture<FPreparedPtr>> InFlight;
	TArray<FPreparedPtr> Ready;
	TSet<uint64> WantKeys;
	TArray<const emberworld::TileEntry*> WantTiles;
	int32 TileGeneration = 0;

	// Tile key straight from a world point, per LOD (surface / mix lookups without a scan).
	struct FLodGrid
	{
		double MinX = 0, MinY = 0, Span = 1;   // tile (0, 0)'s SW corner; y index grows north
		bool bValid = false;
	};
	TMap<int32, FLodGrid> LodGrids;
	int32 FinestLod = 0, CoarsestLod = 0;
	bool KeyAt(int32 Lod, double X, double Y, uint64& OutKey) const;

	TUniquePtr<emberworld::Region> Region;
	TUniquePtr<emberworld::TerrainLook> Look;
	FString WaterDir;
	emberworld::Frame Frame;
	emberworld::Bounds DataExtent;
	TMap<uint64, FLoadedTile> Loaded;
	TArray<int32> FreeSections;
	int32 NextSection = 0;
	bool bStreaming = false;
	FVector LastStreamCamera = FVector::ZeroVector;
};
