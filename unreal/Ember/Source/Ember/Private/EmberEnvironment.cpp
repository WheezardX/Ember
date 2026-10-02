#include "EmberEnvironment.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "Curves/CurveFloat.h"

AEmberEnvironment::AEmberEnvironment()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AEmberEnvironment::Build()
{
	UWorld* W = GetWorld();
	if (!W || Sun)
	{
		return;
	}
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	Sun = W->SpawnActor<ADirectionalLight>(P);
	if (UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
	{
		L->SetMobility(EComponentMobility::Movable);
		L->SetAtmosphereSunLight(true);
		L->SetIntensity(10.f);  // lux; UE default sun, exposure below is tuned to it
		L->SetDynamicShadowDistanceMovableLight(200000.f);
		L->SetCastShadows(true);
	}

	Atmosphere = W->SpawnActor<ASkyAtmosphere>(P);

	Sky = W->SpawnActor<ASkyLight>(P);
	if (USkyLightComponent* S = Sky->GetLightComponent())
	{
		S->SetMobility(EComponentMobility::Movable);
		S->bRealTimeCapture = true;
		S->SourceType = ESkyLightSourceType::SLS_CapturedScene;
		S->MarkRenderStateDirty();
	}

	Fog = W->SpawnActor<AExponentialHeightFog>(P);
	if (UExponentialHeightFogComponent* F = Fog->GetComponent())
	{
		F->SetFogDensity(0.002f);
		F->SetFogHeightFalloff(0.05f);
	}

	Post = W->SpawnActor<APostProcessVolume>(P);
	Post->bUnbound = true;
	FPostProcessSettings& S = Post->Settings;
	S.bOverride_AutoExposureMethod = true;
	S.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	S.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	S.AutoExposureApplyPhysicalCameraExposure = false;
	S.bOverride_MotionBlurAmount = true;
	S.MotionBlurAmount = 0.f;
	S.bOverride_VignetteIntensity = true;
	S.VignetteIntensity = 0.f;
	// Shade white balance (ground v2): under canopy the frame is lit almost only by blue sky,
	// and auto exposure lifts that into a cyan forest floor. A mild warm white balance in the
	// shadows (and a touch in the midtones) keeps shade neutral-green / brown as the eye adapts
	// it; sunlit tones (highlights) are untouched.
	S.bOverride_ColorGainShadows = true;
	S.ColorGainShadows = FVector4(1.10, 1.0, 0.86, 1.0);
	S.bOverride_ColorGainMidtones = true;
	S.ColorGainMidtones = FVector4(1.04, 1.0, 0.95, 1.0);
	SetExposure(ExposureBias);
	SetSun(TEXT("noon"));
}

bool AEmberEnvironment::SetSun(const FString& Preset)
{
	// Compass azimuth the sun is IN (0 = north, 90 = east) + elevation. Teanaway latitude
	// summer-ish values; exact solar geometry arrives with the timeline in Phase 2.
	float Az = 0.f, El = 0.f;
	if (Preset == TEXT("dawn")) { Az = 75.f; El = 6.f; }
	else if (Preset == TEXT("morning")) { Az = 110.f; El = 30.f; }
	else if (Preset == TEXT("noon")) { Az = 180.f; El = 58.f; }
	else if (Preset == TEXT("afternoon")) { Az = 245.f; El = 35.f; }
	else if (Preset == TEXT("dusk")) { Az = 285.f; El = 6.f; }
	else if (Preset == TEXT("night")) { Az = 345.f; El = -28.f; }   // ~1 am, full dark (the star sky)
	else
	{
		FString A, E;
		if (!Preset.Split(TEXT(","), &A, &E))
		{
			return false;
		}
		Az = FCString::Atof(*A);
		El = FCString::Atof(*E);
	}
	SunPreset = Preset;
	SunAzimuth = Az;
	SunElevation = El;
	if (Sun)
	{
		// Light travels AWAY from the sun: direction azimuth = Az + 180. UE yaw = compass - 90
		// (X = east, Y = south). Pitch negative = pointing down.
		const float Yaw = (Az + 180.f) - 90.f;
		Sun->SetActorRotation(FRotator(-El, Yaw, 0.f));
	}
	return true;
}

void AEmberEnvironment::SetHaze(float Density, float Falloff, float StartM, double BaseUEZ)
{
	if (!Fog)
	{
		return;
	}
	Fog->SetActorLocation(FVector(0.0, 0.0, BaseUEZ));
	if (UExponentialHeightFogComponent* F = Fog->GetComponent())
	{
		F->SetVisibility(Density > 0.f);
		F->SetFogDensity(Density);
		F->SetFogHeightFalloff(Falloff);
		F->SetStartDistance(StartM * 100.f);
		// Haze colour follows the time of day: the sky atmosphere's own ambient light, not a fixed
		// colour (a fixed pale blue turned the warm dusk sky grey), plus a glow toward the sun
		// (directional inscattering, lit by the atmosphere-transmitted sun, so orange at dusk).
		F->SetFogInscatteringColor(FLinearColor(0.01f, 0.01f, 0.012f));
		// x1.8: the horizon sky is brighter than the sky's mean ambient, and the fog covers
		// the far horizon (it also hides the dark planet ground past the region's edge)
		F->SkyAtmosphereAmbientContributionColorScale = FLinearColor(1.8f, 1.8f, 1.8f);
		F->SetDirectionalInscatteringExponent(6.f);
		F->SetDirectionalInscatteringStartDistance(2000.f * 100.f);
		F->SetDirectionalInscatteringColor(FLinearColor(0.3f, 0.28f, 0.25f));
		F->MarkRenderStateDirty();
	}
}

void AEmberEnvironment::SetSmokePall(float Pall01)
{
	// Reference (Three Queens 2026): a smoke layer filling the valleys with the peaks standing
	// above it (high aerial); on the heaviest days a brown-orange sky, terrain gone within a few
	// km, a dim red sun (the pall over Kachess, the orange-sky road shots).
	const float P = FMath::Clamp(Pall01, 0.f, 1.f);
	SmokePall = P;
	if (Fog)
	{
		if (UExponentialHeightFogComponent* F = Fog->GetComponent())
		{
			// Second layer: hugs the valleys (~400 m scale height above the fog base). Thin: the
			// reference keeps the near ground crisp and puts the smoke in the distance / the valleys
			// (v1 first pass at 0.03 turned whole frames murky).
			F->SecondFogData.FogDensity = 0.004f * P;
			F->SecondFogData.FogHeightFalloff = 0.025f;
			F->SecondFogData.FogHeightOffset = 0.f;
			// Smoke scatters a lot of light: the reference pall is a PALE tan sky, not dark brown.
			F->SetDirectionalInscatteringColor(FMath::Lerp(FLinearColor(0.3f, 0.28f, 0.25f), FLinearColor(0.9f, 0.62f, 0.34f), P));
			F->SkyAtmosphereAmbientContributionColorScale = FMath::Lerp(FLinearColor(1.8f, 1.8f, 1.8f), FLinearColor(2.6f, 2.25f, 1.75f), P);
			F->MarkRenderStateDirty();
		}
	}
	if (Sun)
	{
		if (UDirectionalLightComponent* L = Cast<UDirectionalLightComponent>(Sun->GetLightComponent()))
		{
			L->SetIntensity(10.f * (1.f - 0.15f * P));  // 0.3 read as a dark brown frame; the reference pall is bright
			// mildly warm: (1, 0.8, 0.58) turned every sunlit column brown; the reference columns stay
			// white in the sun under a moderate pall
			L->SetLightColor(FMath::Lerp(FLinearColor::White, FLinearColor(1.f, 0.88f, 0.74f), P));
		}
	}
}

void AEmberEnvironment::SetSkylightLeaking(float Leak)
{
	if (Post && Leak > 0.f)
	{
		Post->Settings.bOverride_LumenSkylightLeaking = true;
		Post->Settings.LumenSkylightLeaking = Leak;
		// The leaked ambient stands in for light bounced off the forest itself: warm and dim,
		// not the open sky's blue (a forest floor lit by leaked sky read cyan - ground v2).
		Post->Settings.bOverride_LumenSkylightLeakingTint = true;
		Post->Settings.LumenSkylightLeakingTint = FLinearColor(0.75f, 0.62f, 0.45f);
	}
}

void AEmberEnvironment::SetAutoExposure(float MinEv, float MaxEv, float BiasEv, float SpeedEv, float DarkAdapt, float DayEv)
{
	if (!Post)
	{
		return;
	}
	FPostProcessSettings& S = Post->Settings;
	S.bOverride_AutoExposureMethod = true;
	S.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
	S.bOverride_AutoExposureMinBrightness = true;
	S.AutoExposureMinBrightness = MinEv;
	S.bOverride_AutoExposureMaxBrightness = true;
	S.AutoExposureMaxBrightness = MaxEv;
	S.bOverride_AutoExposureLowPercent = true;
	S.AutoExposureLowPercent = 10.f;
	S.bOverride_AutoExposureHighPercent = true;
	S.AutoExposureHighPercent = 90.f;
	S.bOverride_AutoExposureSpeedUp = true;
	S.AutoExposureSpeedUp = SpeedEv;
	S.bOverride_AutoExposureSpeedDown = true;
	S.AutoExposureSpeedDown = SpeedEv;
	S.bOverride_AutoExposureBias = true;
	S.AutoExposureBias = BiasEv;
	// Partial adaptation below daylight: a scene DayEv - x metres darker is lifted by only
	// DarkAdapt * x (compensation curve over the metered EV100), so a forest floor or a dusk
	// view becomes readable without turning into noon.
	if (!ExposureCurve)
	{
		ExposureCurve = NewObject<UCurveFloat>(this);
	}
	FRichCurve& C = ExposureCurve->FloatCurve;
	C.Reset();
	C.AddKey(DayEv - 10.f, -(1.f - DarkAdapt) * 10.f);
	C.AddKey(DayEv, 0.f);
	C.AddKey(DayEv + 10.f, 0.f);
	for (auto It = C.GetKeyHandleIterator(); It; ++It)
	{
		C.SetKeyInterpMode(*It, RCIM_Linear);
	}
	S.bOverride_AutoExposureBiasCurve = true;
	S.AutoExposureBiasCurve = ExposureCurve;
	ExposureBias = BiasEv;
	bAutoExposure = true;
}

void AEmberEnvironment::SetExposure(float Ev100Bias)
{
	ExposureBias = Ev100Bias;
	if (Post)
	{
		Post->Settings.bOverride_AutoExposureBias = true;
		Post->Settings.AutoExposureBias = Ev100Bias;
	}
}
