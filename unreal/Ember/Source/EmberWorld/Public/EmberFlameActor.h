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
 * flame cards (M_Flame, additive) on every burning cell within Radius: a creeping surface fire
 * is knee-high, class 2 a few metres, crown fire tall. Freshness (time since arrival) and the
 * head (rate of spread) scale them like the terrain glow does, and they fade out with distance
 * where the glow takes over. Cards are vertical and turn to the camera; every card is a pure
 * function of (cell, card index, sim time, camera) and the flame animation runs on the fire
 * clock, so stills stay deterministic.
 */
UCLASS()
class EMBERWORLD_API AEmberFlameActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberFlameActor();

	bool Init(AEmberTerrainActor* InTerrain, AEmberFireActor* InFire, FString& OutError);
	/** The fire clock (s) the flames animate on (frozen in stills). */
	void SetFireTime(double Seconds);
	/** Lay out the cards for this camera (after the camera moves / the fire time changes). */
	void Rebuild(const FVector& CameraLoc);

	double RadiusM = 400.0;      // cards out to here
	double FadeStartM = 220.0;   // ... fading from here (the terrain glow reads beyond)

	// Facts
	int32 NumCells = 0;
	int32 NumCards = 0;

private:
	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient)
	TObjectPtr<AEmberFireActor> Fire;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Cards;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;
};
