#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/relief.h"
THIRD_PARTY_INCLUDES_END

#include "EmberGroundRelief.generated.h"

class AEmberTerrainActor;
class UProceduralMeshComponent;

/**
 * Near-ground relief (ground feel; EPIC_5_PLAN "ground still flat - no volume", option 3): a dense
 * patch around the camera carrying worldcore's micro_relief - hummocks, duff mounds, lumps - that
 * the 13 m terrain mesh cannot. Drawn over the terrain with each tile's own material and UVs (a
 * mesh section per tile under a block), so it shades like the ground it covers but has real
 * silhouettes and shadows. Relief fades to zero toward the patch rim, where the patch dips a few
 * cm under the terrain (no seam, no z-fight).
 *
 * Built in square blocks (BlockCells x BlockCells quads, one mesh component each; Brad: the v1
 * patch rebuilt whole, ~160 ms every 8 m of walking). The patch centre steps a block at a time;
 * then only blocks that are new or touch the faded rim are rebuilt - interior blocks are
 * unchanged - from a queue, nearest first, within BuildBudgetMs per frame. Captures (bSync)
 * build everything at once. A terrain tile change re-queues every block (the old mesh stays up
 * until its replacement is built).
 */
UCLASS()
class EMBERWORLD_API AEmberGroundRelief : public AActor
{
	GENERATED_BODY()

public:
	AEmberGroundRelief();

	void Init(AEmberTerrainActor* InTerrain, double InRadiusM, double InSpacingM);
	/** Follows the player camera (as AEmberGroundCoverActor does). */
	virtual void Tick(float DeltaSeconds) override;
	void Update(const FVector& CameraUE);
	bool IsBusy() const { return Queue.Num() > 0; }

	emberworld::ReliefParams Params;
	double LiftM = 0.03;          // the whole patch sits this far above the terrain (no z-fight)
	static constexpr int32 BlockCells = 32;
	double BuildBudgetMs = 3.0;
	bool bSync = false;           // captures: build every queued block in the same frame

	// Facts
	int32 NumVerts = 0;
	int32 NumSections = 0;        // blocks drawn
	int32 Builds = 0;             // block builds so far
	double LastBuildMs = 0.0;     // last frame's build time

private:
	struct FBlock
	{
		UProceduralMeshComponent* Comp = nullptr;
		int32 Verts = 0;
		int32 Generation = -1;      // terrain tile generation it was built against
		FIntPoint Centre = FIntPoint(INT32_MIN, INT32_MIN);   // patch centre block it was built for
		bool bFull = false;         // no vertex in the fade zone: independent of the centre
	};
	void BuildBlock(const FIntPoint& B, FBlock& Out);
	void Requeue(const FVector2D& CameraXY);
	double FadeAt(double X, double Y) const;

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
	TMap<FIntPoint, FBlock> Blocks;
	TArray<UProceduralMeshComponent*> Pool;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> AllComps;   // keeps pooled components alive
	TArray<FIntPoint> Queue;
	double RadiusM = 56.0;
	double SpacingM = 0.3;
	double BlockM = 9.6;
	int32 Rb = 6;                 // blocks from the centre block to the rim
	FIntPoint Centre = FIntPoint(INT32_MIN, INT32_MIN);
	int32 Generation = -1;
	double FadeInM = 0.0, FadeOutM = 0.0;
};
