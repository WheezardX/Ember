#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberFlameActor.generated.h"

class AEmberFireActor;
class AEmberTerrainActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;

/**
 * Eye-level flames (EPIC_5_PLAN HCP4 H4-5: flames with shape instead of paint).
 *
 * Near the camera the terrain's flame glow is a flat orange wash; this actor stands procedural
 * flame cards (M_Flame, additive) where the fire burns within Radius.
 * v2 (Brad 2026-10-01: surface flames "too tall and separate", "our weakest link"; build it like
 * the smoke card pool): the FRONT LINE is drawn through each 30 m cell - arrival time is
 * interpolated between cell centres, so every point knows when the front reaches it and how fast
 * it moves - and fuel points flame in a band a few metres deep behind that line, the way a real
 * surface front is a ragged line with black behind it and fuel ahead; a few keep smaller flames
 * (logs, stumps) for a while after. Each flaming point emits short-lived LICKS (a card pool, as
 * game fire is built: many small short-lived flames spawned along the burning edge, rising,
 * growing, fading out): a lick freezes its size at birth, rises, grows and fades - it never
 * shrinks or moves back - and leans downwind. Stills / scrubs re-seed the pool (deterministic).
 */
UCLASS()
class EMBERWORLD_API AEmberFlameActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberFlameActor();

	bool Init(AEmberTerrainActor* InTerrain, AEmberFireActor* InFire, FString& OutError);
	/** The fire clock (s) the flames animate on (frozen in stills). A small step forward advances
	 *  the lick pool; anything else (a still, a scrub, play start) re-seeds it. */
	void SetFireTime(double Seconds);
	/** Lay out the cards for this camera (after the camera moves / the fire time changes). */
	void Rebuild(const FVector& CameraLoc);

	double RadiusM = 400.0;      // cards out to here
	double FadeStartM = 220.0;   // ... fading from here (the terrain glow reads beyond)
	static constexpr int32 MaxLicks = 40000;

	// Facts
	int32 NumCells = 0;
	int32 NumCards = 0;
	int32 NumSites = 0;          // fuel points flaming now
	double NearestX = 0.0, NearestY = 0.0;   // the flaming point nearest the camera (bookmark authoring)

private:
	struct FLick
	{
		uint64 Site = 0;
		double Ox = 0.0, Oy = 0.0, Gz = 0.0;   // base (region CRS, m)
		float Age = 0.f, Life = 1.f;
		float H = 1.f, W = 0.3f, I = 1.f;      // frozen at birth: full height, half width, intensity
		float Lean = 0.f, Lx = 0.f, Ly = 0.f;  // lean (tan) and its downwind direction
		float Seed = 0.f;
		float BaseUp = 0.f;     // flames v3: height of the lick's base above the ground (crown flames sit in the canopy)
	};
	struct FSite { double Acc = 0.0; uint32 Born = 0; double LastSeen = 0.0; };
	TArray<FLick> Pool;
	TMap<uint64, FSite> Sites;
	double ClockS = 0.0;
	double PoolClock = 0.0;
	bool bResetPool = true;

	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient)
	TObjectPtr<AEmberFireActor> Fire;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Cards;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;
};
