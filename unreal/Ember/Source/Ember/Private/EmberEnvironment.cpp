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

void AEmberEnvironment::SetExposure(float Ev100Bias)
{
	ExposureBias = Ev100Bias;
	if (Post)
	{
		Post->Settings.bOverride_AutoExposureBias = true;
		Post->Settings.AutoExposureBias = Ev100Bias;
	}
}
