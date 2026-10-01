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
 * the 13 m terrain mesh cannot. It is drawn over the terrain with each tile's own material and UVs
 * (one mesh section per tile under it), so it shades exactly like the ground it covers, but has
 * real silhouettes and casts / receives real shadows. Relief fades to zero toward the patch edge,
 * where the patch dips a few cm under the terrain (no seam, no z-fight). Rebuilt when the camera
 * has moved RecentreM from the patch centre or terrain tiles changed.
 */
UCLASS()
class EMBERWORLD_API AEmberGroundRelief : public AActor
{
	GENERATED_BODY()

public:
	AEmberGroundRelief();

	void Init(AEmberTerrainActor* InTerrain, double InRadiusM, double InSpacingM);
	/** CameraUE: the camera location (UE cm). Rebuilds when needed; bForce always. */
	void Update(const FVector& CameraUE, bool bForce = false);
	/** Follows the player camera (as AEmberGroundCoverActor does). */
	virtual void Tick(float DeltaSeconds) override;

	emberworld::ReliefParams Params;
	double LiftM = 0.03;          // the whole patch sits this far above the terrain (no z-fight)
	double RecentreM = 8.0;

	// Facts
	int32 NumVerts = 0;
	int32 NumSections = 0;
	int32 Builds = 0;
	double LastBuildMs = 0.0;

private:
	void Build(double CenterX, double CenterY);

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;
	double RadiusM = 40.0;
	double SpacingM = 0.25;
	double Cx = 0.0, Cy = 0.0;
	bool bBuilt = false;
	int32 BuiltGeneration = -1;
};
