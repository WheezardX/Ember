// Interactive free-fly camera (EPIC_5_PLAN 8f "then F1-lite", pulled forward from HCP6):
// `ember-dev play <scenario>` loads the scenario like a capture run, then hands the view to this
// pawn. Speed follows the height above ground, so the same keys cross a 20 km region from the air
// and stroll through a stand at eye level; the camera never goes below the ground.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "EmberFlyPawn.generated.h"

class AEmberTerrainActor;
class UCameraComponent;

UCLASS()
class EMBER_API AEmberFlyPawn : public APawn
{
	GENERATED_BODY()

public:
	AEmberFlyPawn();
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;

	/** Height of the camera above the ground (m); negative when no ground is loaded below. */
	double GetAglM() const { return AglM; }
	double GetSpeedMs() const { return SpeedMs; }
	bool IsWalking() const { return bWalk; }

	double SpeedScale = 1.0;       // mouse wheel
	double EyeM = 1.7;             // walk mode eye height
	double MinAglM = 1.0;          // never closer to the ground than this
	float LookSensitivity = 0.12f; // degrees per mouse count

private:
	bool GroundZ(const FVector& UE, double& OutUEZ) const;
	bool bWalk = false;
	double AglM = -1.0, SpeedMs = 0.0;
};
