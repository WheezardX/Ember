#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberEnvironment.generated.h"

class ADirectionalLight;
class ASkyLight;
class ASkyAtmosphere;
class AExponentialHeightFog;
class APostProcessVolume;

/**
 * Runtime-spawned lighting rig: sun + sky atmosphere + real-time sky light + height fog +
 * an unbound post-process volume. Exposure is manual (SetExposure) or clamped histogram auto
 * (SetAutoExposure); captures adapt instantly so goldens do not drift with warmup.
 * Everything movable; nothing baked; no .umap content needed.
 */
UCLASS()
class EMBER_API AEmberEnvironment : public AActor
{
	GENERATED_BODY()

public:
	AEmberEnvironment();

	/** Spawn the rig into the world (idempotent per actor). */
	void Build();

	/**
	 * Named sun presets (dawn | morning | noon | afternoon | dusk) or "az,el" in degrees
	 * (compass azimuth the sun is *in*, elevation above horizon). Returns false if unknown.
	 */
	bool SetSun(const FString& Preset);

	/** Manual exposure, EV100 bias. */
	void SetExposure(float Ev100Bias);

	/**
	 * Histogram auto exposure clamped to [MinEv, MaxEv] (EV100), brightest 10 % of the frame ignored
	 * (flames, glints), compensation BiasEv. SpeedEv = f-stops per second (captures: very fast, so
	 * a still settles on the same value every run).
	 */
	void SetAutoExposure(float MinEv, float MaxEv, float BiasEv, float SpeedEv, float DarkAdapt = 1.0f, float DayEv = 2.f);

	/** Lumen skylight leaking (0 = engine default): ambient floor under closed canopy. */
	void SetSkylightLeaking(float Leak);

	UPROPERTY(Transient) TObjectPtr<ADirectionalLight> Sun;
	UPROPERTY(Transient) TObjectPtr<ASkyLight> Sky;
	UPROPERTY(Transient) TObjectPtr<ASkyAtmosphere> Atmosphere;
	UPROPERTY(Transient) TObjectPtr<AExponentialHeightFog> Fog;
	UPROPERTY(Transient) TObjectPtr<APostProcessVolume> Post;

	FString SunPreset;
	float SunAzimuth = 180.f;
	float SunElevation = 45.f;
	float ExposureBias = -2.f;  // EV100 bias; HCP0 sweep picked -2 for the 10 lux sun rig
	bool bAutoExposure = false;
	UPROPERTY(Transient) TObjectPtr<class UCurveFloat> ExposureCurve;
};
