#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/firestate.h"
THIRD_PARTY_INCLUDES_END

#include "EmberFireActor.generated.h"

class AEmberTerrainActor;
class UTexture2D;

/**
 * The fire state player (EPIC_5_PLAN D1, D7). Loads an Epic 4 replay (+ its state stream) and,
 * for any sim time, writes the fire state into one texture over the replay's world grid:
 *   R  burning (flame strength)      G  burned (1 for burning and burned cells)
 *   B  sqrt(hours since arrival / 200): minute-scale near the front, ~8 days at 1
 *   A  1 inside the grid
 * Terrain (M_Terrain) and trees (M_Veg) sample it by world position through FireTex / FireRect /
 * FireOn / FireTime; this actor owns the clock (scrub = SetTime).
 */
UCLASS()
class EMBERWORLD_API AEmberFireActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberFireActor();

	/** Load `<name>.replay.json` and its stream; the terrain gives the world -> UE frame. */
	bool Load(const FString& ReplayPath, AEmberTerrainActor* InTerrain, FString& OutError);

	/** Show the state as of sim time T (seconds since the replay's t0). */
	void SetTime(double TSeconds);

	UTexture2D* GetTexture() const { return Texture; }
	/** The grid in UE cm: (x0, y0, width, height) - M_Terrain / M_Veg `FireRect`. */
	FLinearColor GetRect() const { return Rect; }

	// Facts
	double TimeS = 0.0;
	uint32 Tick = 0;
	int64 CellsBurning = 0;
	int64 CellsBurned = 0;       // burned or burning (the scar so far)
	int64 StreamBurned = 0;      // the stream's own metric at this tick
	int64 T0Unix = 0;
	int32 StartS = 0;
	int32 EndS = 0;
	FString ModelId;
	double CellM = 30.0;
	int32 Nx = 0;
	int32 Ny = 0;

	/** The state's phase at a world point (metres, region CRS); -1 outside the grid. Probes. */
	int32 PhaseAt(double WorldX, double WorldY) const;

private:
	emberworld::fire::Stream Stream;
	emberworld::fire::GridGeo Grid;
	emberworld::fire::State State;
	TArray<uint8> Pixels;
	FLinearColor Rect = FLinearColor(0, 0, 1, 1);

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Texture;
};
