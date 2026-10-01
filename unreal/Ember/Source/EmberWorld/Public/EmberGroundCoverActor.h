// Ground cover near the camera (ground plane v1, EPIC_5_PLAN 8f GP4): grass tufts, ferns,
// shrubs, rocks, logs, stumps placed by worldcore/cover.cpp from the look's ground mix, streamed
// in square cells within RadiusM of the camera.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "emberworld/cover.h"

#include "EmberGroundCoverActor.generated.h"

class AEmberTerrainActor;
class AEmberVegetationActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;

UCLASS()
class EMBERWORLD_API AEmberGroundCoverActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberGroundCoverActor();
	virtual void Tick(float DeltaSeconds) override;

	/** Rules from the look file's [cover]; false (with a reason) if it has none or assets are missing. */
	bool Init(AEmberTerrainActor* InTerrain, const FString& LookPath, FString& OutError);

	void SetWind(double StrengthCm, double FromDeg);
	void SetWindTime(double Seconds);
	void SetFire(class UTexture* FireTex, const FLinearColor& FireRect);
	void SetFireTime(double Seconds);

	double RadiusM = 60.0;
	/** Cells are built from a queue, nearest first, within BuildBudgetMs per frame (8g hitching);
	 *  bSyncStreaming (captures) builds everything at once. Pending cells (their finest tile not
	 *  in yet) rebuild only when the terrain's tiles change. */
	double BuildBudgetMs = 2.0;
	bool bSyncStreaming = false;
	bool IsStreamingBusy() const { return Queue.Num() > 0; }
	int64 GetInstanceCount() const { return Instances; }
	int32 GetCellCount() const { return Cells.Num(); }
	// Hung-up trees (8g): placed, and rejected for no host / no sane foot (facts, tuning).
	mutable int64 LeanersPlaced = 0, LeanersNoHost = 0, LeanersRejected = 0, LeanersThinned = 0;
	mutable FVector LastLeanerFootUE = FVector::ZeroVector, LastLeanerContactUE = FVector::ZeroVector;

private:
	struct FCoverCell
	{
		TArray<TObjectPtr<UInstancedStaticMeshComponent>> Components;
		bool bPending = false;  // some ground was not loaded yet: rebuild on a later update
		int32 Attempts = 0;     // pending rebuilds tried (capped: a cell may never get the finest LOD)
		int32 BuiltGeneration = -1;  // terrain tile generation this cell was built against
		int32 BuiltVegGeneration = -1;  // vegetation tile generation (hung-up tree hosts)
		int64 Count = 0;
	};

	void UpdateCells(const FVector& CamUE);
	void BuildQueued(const FVector& CamUE);
	TArray<FIntPoint> Queue;
	int32 LastGeneration = -1;
	int32 LastVegGeneration = -1;
	int32 VegGeneration() const;
	void BuildCell(const FIntPoint& Key, FCoverCell& Cell);
	void ClearCell(FCoverCell& Cell);
	/** Conform (logs) / leaner (hung-up stems) placement on the rendered surface; false = skip. */
	bool PlaceLying(const emberworld::CoverInstance& In, const FBoxSphereBounds& B, double S,
		emberworld::CoverPose Pose, FTransform& Out, bool& bOutPending) const;

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
public:
	/** Hung-up trees anchor on real trees (8g): set by the harness when trees are drawn. */
	UPROPERTY(Transient)
	TObjectPtr<AEmberVegetationActor> Vegetation;
private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> Meshes;          // per item, per variant (flattened)
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;  // per item
	TArray<int32> FirstMesh;                          // per item: index of its variant 0 in Meshes

	emberworld::CoverRules Rules;
	TMap<FIntPoint, FCoverCell> Cells;
	FVector LastCamera = FVector(1e30);
	bool bInitialised = false;
	int64 Instances = 0;
	static constexpr double CellM = 32.0;
};
