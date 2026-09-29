// Ground cover near the camera (ground plane v1, EPIC_5_PLAN 8f GP4): grass tufts, ferns,
// shrubs, rocks, logs, stumps placed by worldcore/cover.cpp from the look's ground mix, streamed
// in square cells within RadiusM of the camera.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "emberworld/cover.h"

#include "EmberGroundCoverActor.generated.h"

class AEmberTerrainActor;
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
	int64 GetInstanceCount() const { return Instances; }
	int32 GetCellCount() const { return Cells.Num(); }

private:
	struct FCoverCell
	{
		TArray<TObjectPtr<UInstancedStaticMeshComponent>> Components;
		bool bPending = false;  // some ground was not loaded yet: rebuild on a later update
		int32 Attempts = 0;     // pending rebuilds tried (capped: a cell may never get the finest LOD)
		int64 Count = 0;
	};

	void UpdateCells(const FVector& CamUE);
	void BuildCell(const FIntPoint& Key, FCoverCell& Cell);
	void ClearCell(FCoverCell& Cell);

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
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
