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
	/**
	 * Vegetation tiers (docs/viz/D11-lod-world-context.md section 2). Each streamed tile is split
	 * into CellsPerSide^2 cells (320 m at 10 m / 128 px tiles); every cell picks its tier from its
	 * distance to the camera (horizontal + height above ground): NEAR cells draw the full meshes
	 * with wind (per instance within WindRadiusM) and live shadows; MID cells (to RadiusM) draw the
	 * lite meshes (same trees, less foliage) with WPO off and cached shadows, and skip trees shorter
	 * than MidMinHeightM (understory hidden under the canopy from afar). Past RadiusM the terrain's
	 * canopy colouring carries the forest (far tier). A cell whose tier changes is rebuilt from the
	 * tile's cached trees - no re-scatter.
	 */
	double NearRadiusM = 500.0;
	bool bFire = false;            // a fire texture is bound (SetFire): near-tier WPO covers the whole tier
	double MidMinHeightM = 12.0;
	double WindRadiusM = 250.0;    // near-tier trees sway only within this distance (per instance)

	/** Wind sway (M_Veg WPO): crown-top sway in cm for a 10 m tree; compass dir wind comes FROM. */
	void SetWind(double StrengthCm, double FromDeg);
	/** The wind clock (s). The harness owns it: frozen for stills, frame/fps for orbits (D7). */
	void SetWindTime(double Seconds);
	/** Fire state (EmberFireActor): each tree samples it at its pivot (char, crown flames). */
	void SetFire(class UTexture* FireTex, const FLinearColor& FireRect);
	void SetFireTime(double Seconds);

	/**
	 * B3 silhouette sheet: one tree per species at its palette mid height, in a row centred on
	 * (WorldX, WorldY) along compass heading RowDeg, grounded on the surface, plus a 1.8 m post
	 * for scale. Disables tile streaming.
	 */
	void SpawnLineup(double WorldX, double WorldY, double RowDeg);

	/**
	 * Async after init (8g hitching): tile scatter + grounding run on worker threads; cells are
	 * (re)built from a queue within BuildBudgetMs per frame. bSyncStreaming keeps the old
	 * everything-now behaviour for capture runs (reproducible goldens).
	 */
	bool IsStreamingBusy() const;

	/** A tree as placed (8g hung-up trees anchor on real trees). */
	struct FTreeInfo
	{
		double X = 0, Y = 0;     // trunk base, world metres
		FVector BaseUE;          // trunk base, UE cm (sunk / leaned as rendered)
		FVector UpUE;            // trunk axis (unit, UE)
		float HeightM = 0.f;
		float CrownRadiusM = 0.f;
	};
	/** Trees whose base lies within QueryM of (X, Y) (world m), from loaded tiles. Returns false
	 *  if the area is not fully covered by loaded tiles (caller retries later). */
	bool TreesNear(double X, double Y, double QueryM, TArray<FTreeInfo>& Out) const;
	/** Bumped whenever the set of loaded tiles changes (TreesNear answers may change). */
	int32 GetTileGeneration() const { return TileGeneration; }
	int32 TileGeneration = 0;
	bool bSyncStreaming = false;
	double BuildBudgetMs = 3.0;
	int32 MaxInFlight = 4;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	// Facts
	int64 InstancesTotal = 0;
	int32 TilesLoaded = 0;
	double ScatterMs = 0.0;       // cumulative CPU time in scatter + grounding
	int64 UngroundedInstances = 0; // drawn at Terrain's z because the tile's surface failed (error)
	int64 NoSurfaceInstances = 0;  // dropped: no rendered surface under them (outside the DEM mask)
	int32 TilesNear = 0;           // tiles with at least one near cell
	int32 CellsNear = 0;
	int64 InstancesNear = 0;       // drawn with full meshes, wind and live shadows
	int64 MidCulled = 0;           // understory skipped in mid-tier cells (currently loaded)
	int32 GeneratedSpecies = 0;    // species drawn with a generated mesh (vs placeholder)
	TMap<FString, int64> BySpecies;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> SpeciesMesh;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> SpeciesMaterial;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> LineupParts;

private:
	static constexpr int32 CellsPerSide = 4;
	enum class ETier : uint8 { None, Near, Mid };
	struct FVegTree
	{
		FTransform Xf;
		int32 Slot = 0;
		int32 Variant = 0;
		float HeightM = 0.f;
		float CrownRadiusM = 0.f;
	};
	struct FVegCell
	{
		TArray<int32> Trees;                                // indices into FVegTile::Trees
		TArray<UInstancedStaticMeshComponent*> Components;  // owned by this actor
		TArray<int64> PerSpecies;
		ETier Tier = ETier::None;
		int64 Culled = 0;
		double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;      // world metres
	};
	struct FVegTile
	{
		TArray<FVegTree> Trees;
		FVegCell Cells[CellsPerSide * CellsPerSide];
		double MinX = 0, MaxY = 0, CW = 1, CH = 1;   // content bounds (world m) and cell size
	};

	bool LoadTile(const emberworld::TileEntry& Tile, FString& OutError);
	struct FPreparedVeg;  // a tile's trees, scattered and grounded off the game thread (cpp)
	using FPreparedVegPtr = TSharedPtr<FPreparedVeg, ESPMode::ThreadSafe>;
	FPreparedVegPtr PrepareTile(const emberworld::TileEntry& Tile) const;  // thread-safe
	bool InstallTile(FPreparedVeg& P, FString& OutError);                  // game thread
	void SelectTiles(const FVector& CameraUE);
	void QueueTiers();
	void PumpStreaming();
	TArray<const emberworld::TileEntry*> WantTiles;
	TSet<uint64> WantKeys;
	TMap<uint64, TFuture<FPreparedVegPtr>> InFlight;
	TArray<TPair<uint64, int32>> BuildQueue;   // (tile, cell) whose tier changed
	TArray<FBoxSphereBounds> MeshBounds;       // SpeciesMesh bounds, cached for worker threads
	double CamWx = 0, CamWy = 0, CamHag = 0;   // last selection's camera (world metres)
	void BuildCell(uint64 TileKey, int32 CellIndex, ETier Tier);
	void ClearCell(FVegCell& Cell);
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
	TArray<int32> SlotFirstLite;       // slot -> first mid-tier lite mesh, INDEX_NONE if none
	TArray<double> SlotHeightLineupM;  // lineup: a mature tree of the species
	TArray<double> SlotCrownRatio;
	TMap<uint64, FVegTile> Tiles;
	// burn-mosaic survival field (worldcore survival_field), cached per SurvivalCellM lattice node
	TMap<FIntPoint, float> SurvivalCache;
	static constexpr double SurvivalCellM = 25.0;
	FVector LastCamera = FVector::ZeroVector;
	bool bInitialised = false;
	bool bFirstUpdate = true;
	bool bLineup = false;
};
