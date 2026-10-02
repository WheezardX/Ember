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
 * the source's strength, drift downwind, spread and fade. v3: the puffs are a persistent card
 * pool - born at the base, advanced by their own age, recycled when they expire - so they only
 * ever rise and grow (v2 re-derived every puff from the current fire each frame, and a fire that
 * changed at playback speed pulled them back down). Stills / scrubs re-seed the pool with full,
 * deterministic plumes. Cards face the camera and are sorted back to front (M_Smoke, translucent,
 * lit per pixel).
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
	/** Lay out the puffs for this camera (after the camera moves / the state changes). */
	void Rebuild(const FVector& CameraLoc, const FRotator& CameraRot);

	static constexpr int32 MaxPuffs = 32000;   // the card pool

	// Facts
	int32 NumSources = 0;
	int32 NumPlumes = 0;      // emitters drawn this frame (strongest first, while the pool has room)
	int32 NumMerged = 0;      // of those, far-field plumes pooled from several sources (facts)
	int32 NumResets = 0;      // pool rebuilt from scratch (stills, scrubs, play start)
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

	/** One card of the pool (Brad 2026-10-01: "cards should only ever grow, never shrink"). Born at
	 *  its plume's base with the plume frozen into it (height, rise, wind, width, density); from then
	 *  on its place and size are a function of its own age only - it rises, drifts, grows, fades out
	 *  and expires. A changing fire only shapes the cards born after the change. */
	struct FPuffCard
	{
		int32 Emitter = 0;
		uint8 Kind = 0;          // 0 column (rises at constant speed), 1 drift (leaves the top), 2 smoulder wisp
		bool bCell = false, bInterior = false;
		float Age = 0.f, LifeS = 1.f;
		double Ox = 0.0, Oy = 0.0, Gz = 0.0;   // base (region CRS, m)
		float Ux = 0.f, Uy = 1.f, Wind = 5.f;  // downwind unit vector and speed at birth
		float H = 0.f, Tau = 1.f, L = 0.f, R0 = 0.f, Cell = 0.f, Tcol = 0.f, Ldrift = 0.f;
		float Base = 0.f, Hot = 0.f, HotZ = 1.f;
		float S1 = 0.f, S2 = 0.f, S3 = 0.f;    // per-card seeds
		float Vis = 0.f;          // follows its emitter's merge cross-fade / flaming fade; fades if it goes
	};
	struct FEmitterState { double AccCol = 0.0, AccDrift = 0.0; uint32 Born = 0; double LastSeen = 0.0; double Vis = 0.0; };
	TArray<FPuffCard> Pool;
	TMap<int32, FEmitterState> Emitters;
	double PoolClock = 0.0;       // the clock the pool has been advanced to
	bool bResetPool = true;       // next Rebuild re-seeds the pool (a still / a jump in the clock)

public:
	/** Render clock (s). A small step forward advances the pool; anything else (a still, a scrub, the
	 *  start of play) re-seeds it: full plumes, deterministic for a given clock, sources and camera. */
	void SetClock(double Seconds)
	{
		if (!(Seconds > PoolClock && Seconds <= PoolClock + 2.0)) bResetPool = true;
		ClockS = Seconds;
	}
};
