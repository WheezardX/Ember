#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberFireActor.h"

#include "EmberFirebrandActor.generated.h"

class AEmberTerrainActor;
class UInstancedStaticMeshComponent;

/**
 * Firebrands and spot fires (EPIC_5_PLAN HCP4 H4-4: spot fires light up where the stream says;
 * ember streaks from the head toward them). Legibility first, no particle sim.
 *
 * Every firebrand the stream launched in the last TrailS of sim time draws as a lofted arc of
 * glowing sparks from its launch cell to its landing cell, fading with age; one that ignited a
 * spot fire marks its landing cell with a bright glow for SpotGlowS after it lands (the CA already
 * burns the cell; the marker says "this one jumped"). Sparks keep a minimum on-screen size, so a
 * 150 m shower still reads from 10 km up. Every sprite is a pure function of (brand, sim time,
 * camera): stills are deterministic and scrubbing is free. Cards face the camera; M_Firebrand is
 * additive, so no sorting.
 */
UCLASS()
class EMBERWORLD_API AEmberFirebrandActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberFirebrandActor();

	bool Init(AEmberTerrainActor* InTerrain, FString& OutError);
	void SetBrands(const TArray<FEmberFirebrand>& InBrands) { Brands = InBrands; }
	/** Lay out the sprites for this camera (after the camera moves / the state changes). */
	void Rebuild(const FVector& CameraLoc, const FRotator& CameraRot);

	static constexpr double TrailS = 1200.0;       // a shower stays visible 20 sim minutes
	static constexpr double SpotGlowS = 1800.0;    // a new spot fire is marked for 30 sim minutes
	static constexpr int32 SparksPerBrand = 14;
	static constexpr double MinPixelAngle = 0.0025; // min sprite size as a fraction of distance

	// Facts
	int32 NumShowers = 0;     // firebrands drawn as a spark arc
	int32 NumSpotGlows = 0;   // spot fires marked
	int32 NumSprites = 0;

private:
	UPROPERTY(Transient)
	TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Sprites;

	TArray<FEmberFirebrand> Brands;
};
