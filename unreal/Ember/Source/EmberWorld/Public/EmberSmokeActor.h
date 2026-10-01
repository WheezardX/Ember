#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberFireActor.h"

#include "EmberSmokeActor.generated.h"

class AEmberTerrainActor;
class UInstancedStaticMeshComponent;

/**
 * Smoke v0 (EPIC_5_PLAN HCP3; D6 legibility-first, wind-coherent plume, no fluid sim).
 *
 * Each smoke source (a 300 m bin of recently burning cells, from the fire player) emits a chain
 * of puff cards along a bent-over plume: they rise toward an injection height that grows with
 * the source's strength, drift downwind, spread and fade. Every puff is a pure function of
 * (source bin, puff index, render clock), so stills are pixel-deterministic, scrubbing is free,
 * and a timelapse re-derives the plumes from the fire state each frame. Cards face the camera
 * and are sorted back to front (M_Smoke, translucent, lit per pixel).
 */
UCLASS()
class EMBERWORLD_API AEmberSmokeActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberSmokeActor();

	bool Init(AEmberTerrainActor* InTerrain, FString& OutError);
	/** Wind the plumes bend with: compass direction it blows FROM, and speed. */
	void SetWind(double FromDeg, double SpeedMs);
	void SetSources(const TArray<FEmberSmokeSource>& InSources);
	/** Render clock (s): puffs move along their plumes at real speed. */
	void SetClock(double Seconds) { ClockS = Seconds; }
	/** Lay out the puffs for this camera (after the camera moves / the state changes). */
	void Rebuild(const FVector& CameraLoc, const FRotator& CameraRot);

	static constexpr int32 MaxPuffs = 20000;

	// Facts
	int32 NumSources = 0;
	int32 NumPlumes = 0;      // sources that got puffs (strongest first, under MaxPuffs)
	int32 NumMerged = 0;      // of those, far-field plumes pooled from several sources (facts)
	double MergeNearM = 3000.0;   // sources closer than this draw as their own columns
	int32 NumPuffs = 0;
	double MaxTopM = 0.0;     // highest plume injection height

	struct FGroundHit { double X = 0.0, Y = 0.0, Z = 0.0; };
	TMap<int32, FGroundHit> GroundCache;   // per source bin: ground height under its centroid

private:
	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Puffs;

	TArray<FEmberSmokeSource> Sources;
	double WindFromDeg = 270.0;
	double WindMs = 8.0;
	double ClockS = 0.0;
};
