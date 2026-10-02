#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberStarsActor.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;

/**
 * The night sky (EPIC_5_PLAN: "Night sky: stars, the real sky for place and date"; Brad
 * 2026-10-01: "we can probably even work out what the night sky should be for that location and
 * time of year").
 *
 * The ~5,000 naked-eye stars of the Yale Bright Star Catalogue (ember-dev fetch-stars ->
 * Content/Ember/Data/stars.csv) placed for the scene's latitude and date. The time of night comes
 * from the SUN the scene shows (a preset or "az,el"), not a clock: the sun's right ascension on
 * that date plus the hour angle its azimuth / elevation imply give the local sidereal time, so the
 * stars always agree with the lighting. Each star is a tiny additive sprite on a camera-centred
 * sphere (beyond the terrain, unfogged), coloured by its B-V index, dimmed by airmass near the
 * horizon, by twilight (limiting magnitude from the sun's depth) and by the smoke pall.
 */
UCLASS()
class EMBERWORLD_API AEmberStarsActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberStarsActor();

	bool Init(FString& OutError);
	/** Latitude (deg), a unix time on the date shown (only the date matters), the sun shown
	 *  (compass azimuth it is in, elevation, deg) and the smoke pall 0..1. Cheap when unchanged. */
	void SetSky(double LatDeg, int64 Unix, double SunAzDeg, double SunElDeg, double Pall01);
	/** Keep the sphere on the camera (stars are at infinity). */
	void Follow(const FVector& CameraLoc);

	double RadiusM = 200000.0;   // beyond any world we load: terrain always occludes stars
	double SpriteRad = 0.0022;   // sprite size (rad): ~3 px at 1600 px / 60 deg
	float Gain = 80.f;           // M_Star brightness (L_stars at the default night exposure)

	// Facts
	int32 NumStars = 0;          // in the catalogue
	int32 NumVisible = 0;        // drawn now
	double LimitingMag = 0.0;    // faintest drawn (twilight / pall)
	double LstDeg = 0.0;         // local sidereal time the sky is drawn for

private:
	struct FStar { double Ra, Dec, Mag, Bv; };
	TArray<FStar> Stars;
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> Sprites;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;
	FString LastKey;
};
