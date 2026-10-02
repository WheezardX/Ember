#include "EmberStarsActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** B-V colour index -> a star's tint (blue-white O/B .. red M), half desaturated: to the eye
	 *  most stars read near white with a hint of colour. */
	FLinearColor StarTint(double Bv)
	{
		static const double Keys[] = {-0.3, 0.0, 0.3, 0.6, 1.0, 1.5, 2.0};
		static const FLinearColor Cols[] = {
			{0.62f, 0.72f, 1.0f}, {0.8f, 0.86f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.95f, 0.85f},
			{1.0f, 0.85f, 0.65f}, {1.0f, 0.72f, 0.48f}, {1.0f, 0.62f, 0.4f}};
		const double B = FMath::Clamp(Bv, -0.3, 2.0);
		int32 I = 0;
		while (I < 5 && B > Keys[I + 1]) ++I;
		const float T = static_cast<float>((B - Keys[I]) / (Keys[I + 1] - Keys[I]));
		return FMath::Lerp(FLinearColor::White, FMath::Lerp(Cols[I], Cols[I + 1], T), 0.6f);
	}
}

AEmberStarsActor::AEmberStarsActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberStarsActor::Init(FString& OutError)
{
	const FString Path = FPaths::ProjectContentDir() / TEXT("Ember/Data/stars.csv");
	TArray<FString> Lines;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Star.M_Star"));
	if (!FFileHelper::LoadFileToStringArray(Lines, *Path) || !Plane || !Mat)
	{
		OutError = TEXT("stars need Content/Ember/Data/stars.csv (ember-dev fetch-stars), /Engine/BasicShapes/Plane and M_Star (ember-dev regen-assets)");
		return false;
	}
	for (const FString& L : Lines)
	{
		TArray<FString> F;
		if (L.StartsWith(TEXT("#")) || L.ParseIntoArray(F, TEXT(","), true) < 4)
		{
			continue;
		}
		Stars.Add({FCString::Atod(*F[0]), FCString::Atod(*F[1]), FCString::Atod(*F[2]), FCString::Atod(*F[3])});
	}
	NumStars = Stars.Num();
	Mid = UMaterialInstanceDynamic::Create(Mat, this);
	Mid->SetScalarParameterValue(TEXT("Gain"), Gain);
	Sprites = NewObject<UInstancedStaticMeshComponent>(this);
	Sprites->SetStaticMesh(Plane);
	Sprites->SetMaterial(0, Mid);
	Sprites->NumCustomDataFloats = 3;
	Sprites->SetCastShadow(false);
	Sprites->bAffectDistanceFieldLighting = false;
	Sprites->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sprites->SetupAttachment(RootComponent);
	Sprites->RegisterComponent();
	// the sphere is far beyond the terrain: never cull it by distance
	Sprites->SetCullDistances(0, 0);
	Sprites->bNeverDistanceCull = true;
	return NumStars > 0;
}

void AEmberStarsActor::Follow(const FVector& CameraLoc)
{
	SetActorLocation(CameraLoc);
}

void AEmberStarsActor::SetSky(double LatDeg, int64 Unix, double SunAzDeg, double SunElDeg, double Pall01)
{
	if (!Sprites)
	{
		return;
	}
	const int64 Day = Unix / 86400;
	const FString Key = FString::Printf(TEXT("%.2f|%lld|%.1f|%.1f|%.2f"), LatDeg, Day, SunAzDeg, SunElDeg, Pall01);
	if (Key == LastKey)
	{
		return;
	}
	LastKey = Key;
	const double D2R = PI / 180.0;
	// The sun's right ascension on that date (low-precision solar position, ~0.01 deg).
	const double N = static_cast<double>(Unix) / 86400.0 + 2440587.5 - 2451545.0;   // days from J2000
	const double Lm = FMath::Fmod(280.46 + 0.9856474 * N, 360.0);
	const double G = FMath::Fmod(357.528 + 0.9856003 * N, 360.0) * D2R;
	const double Lam = (Lm + 1.915 * FMath::Sin(G) + 0.020 * FMath::Sin(2.0 * G)) * D2R;
	const double Eps = 23.439 * D2R;
	const double SunRa = FMath::Atan2(FMath::Cos(Eps) * FMath::Sin(Lam), FMath::Cos(Lam));
	// The hour angle the shown sun implies (azimuth from north, clockwise; west of south = positive).
	const double Phi = LatDeg * D2R;
	const double A = SunAzDeg * D2R, H = SunElDeg * D2R;
	const double Ha = FMath::Atan2(-FMath::Sin(A) * FMath::Cos(H), FMath::Cos(Phi) * FMath::Sin(H) - FMath::Sin(Phi) * FMath::Cos(H) * FMath::Cos(A));
	const double Lst = SunRa + Ha;
	LstDeg = FMath::Fmod(Lst / D2R + 720.0, 360.0);
	// Limiting magnitude: nothing in daylight, the brightest from civil twilight, the full sky
	// below astronomical twilight (-18); the smoke pall takes several magnitudes off.
	const double Dark = FMath::Clamp((-SunElDeg - 3.0) / 15.0, 0.0, 1.0);
	LimitingMag = FMath::Lerp(-2.5, 6.0, FMath::Pow(Dark, 0.7))   // (-2.5: nothing at all by day - Sirius is -1.5)
		 - 4.5 * FMath::Clamp(Pall01, 0.0, 1.0);
	const double SinPhi = FMath::Sin(Phi), CosPhi = FMath::Cos(Phi);
	const double R = RadiusM * 100.0;

	TArray<FTransform> Xf;
	TArray<float> Data;
	for (const FStar& S : Stars)
	{
		if (S.Mag > LimitingMag + 0.5)
		{
			break;   // sorted brightest first
		}
		const double Dec = S.Dec * D2R;
		const double Hs = Lst - S.Ra * D2R;
		const double SinAlt = SinPhi * FMath::Sin(Dec) + CosPhi * FMath::Cos(Dec) * FMath::Cos(Hs);
		const double Alt = FMath::Asin(FMath::Clamp(SinAlt, -1.0, 1.0));
		if (Alt < -0.5 * D2R)
		{
			continue;
		}
		const double Az = FMath::Atan2(-FMath::Cos(Dec) * FMath::Sin(Hs), FMath::Sin(Dec) * CosPhi - FMath::Cos(Dec) * SinPhi * FMath::Cos(Hs));
		// Airmass extinction (~0.25 mag per airmass, Kasten-Young), and the haze at the horizon.
		const double AltDeg = Alt / D2R;
		const double X = 1.0 / (FMath::Sin(Alt) + 0.50572 * FMath::Pow(FMath::Max(AltDeg, 0.0) + 6.07995, -1.6364));
		const double M = S.Mag + 0.25 * (X - 1.0);
		const double Vis = FMath::Clamp(LimitingMag + 0.5 - M, 0.0, 1.0) * FMath::Clamp(AltDeg / 2.0, 0.0, 1.0);
		if (Vis <= 0.0)
		{
			continue;
		}
		// Displayed brightness: flux^0.35 - the eye compresses the 1000:1 range hard; bright stars read
		// by their size and glare as much as by brightness.
		const double Flux = FMath::Pow(10.0, -0.14 * (M - 1.0)) * Vis;   // (flux^0.6 clipped the bright half and lost the faint: the faint dust of a dark sky matters more)
		const FLinearColor C = StarTint(S.Bv) * static_cast<float>(Flux);
		// direction: X east, Y south, Z up (UE)
		const FVector Dir(FMath::Sin(Az) * FMath::Cos(Alt), -FMath::Cos(Az) * FMath::Cos(Alt), FMath::Sin(Alt));
		const FQuat Q = FRotationMatrix::MakeFromZ(-Dir).ToQuat();   // the plane faces the camera
		const double Size = 2.0 * SpriteRad * RadiusM * (1.0 + 0.3 * FMath::Max(0.0, 2.0 - M));   // m; the plane is 1 m
		Xf.Add(FTransform(Q, Dir * R, FVector(Size, Size, 1.0)));
		Data.Add(C.R);
		Data.Add(C.G);
		Data.Add(C.B);
	}
	Sprites->ClearInstances();
	Sprites->AddInstances(Xf, false, false);
	for (int32 I = 0; I < Xf.Num(); ++I)
	{
		Sprites->SetCustomData(I, TArrayView<const float>(&Data[I * 3], 3), false);
	}
	Sprites->MarkRenderStateDirty();
	NumVisible = Xf.Num();
}
