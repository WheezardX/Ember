#include "EmberFirebrandActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "EmberTerrainActor.h"

namespace
{
	/** Integer hash -> [0, 1): per-spark seeds that do not depend on evaluation order. */
	double Hash01(uint32 A, uint32 B)
	{
		uint32 H = A * 0x9E3779B1u ^ (B + 0x7F4A7C15u) * 0x85EBCA77u;
		H ^= H >> 15;
		H *= 0x2C1B3C6Du;
		H ^= H >> 12;
		H *= 0x297A2D39u;
		H ^= H >> 15;
		return (H & 0xFFFFFF) / 16777216.0;
	}

	struct FSprite
	{
		FVector Loc;
		FVector Dir = FVector::ZeroVector;  // flight direction (UE): the card stretches along it
		double SizeM;
		double Stretch = 1.0;
		float Data[2];
	};
}

AEmberFirebrandActor::AEmberFirebrandActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberFirebrandActor::Init(AEmberTerrainActor* InTerrain, FString& OutError)
{
	Terrain = InTerrain;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Firebrand.M_Firebrand"));
	if (!Terrain || !Plane || !Mat)
	{
		OutError = TEXT("firebrands need the terrain, /Engine/BasicShapes/Plane and M_Firebrand (ember-dev regen-assets)");
		return false;
	}
	Sprites = NewObject<UInstancedStaticMeshComponent>(this);
	Sprites->SetStaticMesh(Plane);
	Sprites->SetMaterial(0, Mat);
	Sprites->NumCustomDataFloats = 2;
	Sprites->SetCastShadow(false);
	Sprites->bAffectDistanceFieldLighting = false;
	Sprites->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sprites->SetupAttachment(RootComponent);
	Sprites->RegisterComponent();
	return true;
}

void AEmberFirebrandActor::Rebuild(const FVector& CameraLoc, const FRotator& CameraRot)
{
	if (!Sprites || !Terrain)
	{
		return;
	}
	TArray<FSprite> All;
	NumShowers = 0;
	NumSpotGlows = 0;
	for (const FEmberFirebrand& B : Brands)
	{
		double Z0 = 0.0, Z1 = 0.0;
		if (!Terrain->GroundHeightAt(B.X0, B.Y0, Z0) || !Terrain->GroundHeightAt(B.X1, B.Y1, Z1))
		{
			continue;
		}
		if (B.AgeS <= TrailS)
		{
			// A shower from the launch cell to the landing cell: brands ride the column up and fall
			// out downwind. Sparks scatter around the flight line and cool (yellow -> red) as they fly.
			++NumShowers;
			const double D = FMath::Sqrt(FMath::Square(B.X1 - B.X0) + FMath::Square(B.Y1 - B.Y0));
			const double Loft = 15.0 + 0.3 * D;
			const double Fade = FMath::Pow(1.0 - B.AgeS / TrailS, 1.5);
			const double Nx = D > 0.0 ? -(B.Y1 - B.Y0) / D : 0.0;   // across the flight line
			const double Ny = D > 0.0 ? (B.X1 - B.X0) / D : 0.0;
			// Brands shoot up out of the column and glide down downwind: the peak sits early.
			auto Path = [&](double F) -> FVector
			{
				const double G = FMath::Pow(FMath::Clamp(F, 0.0, 1.0), 0.6);
				return FVector(FMath::Lerp(B.X0, B.X1, F), FMath::Lerp(B.Y0, B.Y1, F),
					FMath::Lerp(Z0, Z1, F) + 4.0 * Loft * G * (1.0 - G) + 3.0);
			};
			for (int32 I = 0; I < SparksPerBrand; ++I)
			{
				const double H1 = Hash01(B.Key, I * 4u + 1u);
				const double H2 = Hash01(B.Key, I * 4u + 2u);
				const double H3 = Hash01(B.Key, I * 4u + 3u);
				const double H4 = Hash01(B.Key, I * 4u + 4u);
				const double F = FMath::Clamp((I + H1) / SparksPerBrand + (H4 - 0.5) * 0.1, 0.0, 1.0);
				// A loose shower around the flight line, not a neat arch.
				const double Env = FMath::Sin(PI * F);
				const double Side = (H2 - 0.5) * 0.6 * D * Env;
				const double Up = (H4 - 0.5) * 0.6 * Loft * Env;
				const FVector P = Path(F);
				const FVector Tan = Path(F + 0.02) - P;
				FSprite S;
				S.Loc = Terrain->WorldToUE(P.X + Nx * Side, P.Y + Ny * Side, P.Z + Up);
				// A short streak along the flight direction (dots read as a string of pearls).
				S.Dir = (Terrain->WorldToUE(P.X + Tan.X, P.Y + Tan.Y, P.Z + Tan.Z) - Terrain->WorldToUE(P.X, P.Y, P.Z)).GetSafeNormal();
				S.SizeM = 2.0 + 3.5 * H3 * H3;
				S.Stretch = 1.4 + 1.2 * H2;
				S.Data[0] = static_cast<float>(Fade * (0.25 + 0.75 * H3 * H3 * H3));
				S.Data[1] = static_cast<float>(0.75 - 0.6 * F);         // cooling in flight
				All.Add(S);
			}
		}
		if (B.bIgnited && B.LandAgeS >= 0.0 && B.LandAgeS <= SpotGlowS)
		{
			// The new spot fire: a flare-up on its landing cell (M_Firebrand fades it into the
			// ground, so it sits on the slope instead of floating), fading over SpotGlowS.
			++NumSpotGlows;
			FSprite S;
			S.Loc = Terrain->WorldToUE(B.X1, B.Y1, Z1 + 4.0);
			S.SizeM = 60.0;
			S.Data[0] = static_cast<float>(1.3 * (1.0 - B.LandAgeS / SpotGlowS));
			S.Data[1] = 0.45f;
			All.Add(S);
		}
	}

	const FVector CamRight = FRotationMatrix(CameraRot).GetUnitAxis(EAxis::Y);
	TArray<FTransform> Xf;
	Xf.Reserve(All.Num());
	for (const FSprite& S : All)
	{
		const FVector ToCam = (CameraLoc - S.Loc).GetSafeNormal();
		// Card X along the flight direction as seen from the camera (streaks), else camera right.
		FVector Along = S.Dir - FVector::DotProduct(S.Dir, ToCam) * ToCam;
		const bool bStreak = Along.SizeSquared() > 1e-4;
		const FQuat Q = FRotationMatrix::MakeFromZX(ToCam, bStreak ? Along.GetSafeNormal() : CamRight).ToQuat();
		// Keep a minimum on-screen size: a spark 4 m across vanishes from altitude.
		const double DistM = FVector::Dist(CameraLoc, S.Loc) / 100.0;
		const double Scale = FMath::Max(S.SizeM, MinPixelAngle * DistM);  // the engine plane is 1 m
		Xf.Add(FTransform(Q, S.Loc, FVector(Scale * (bStreak ? S.Stretch : 1.0), Scale, 1.0)));
	}
	Sprites->ClearInstances();
	Sprites->AddInstances(Xf, false, true);
	for (int32 I = 0; I < All.Num(); ++I)
	{
		Sprites->SetCustomData(I, TArrayView<const float>(All[I].Data, 2), false);
	}
	Sprites->MarkRenderStateDirty();
	NumSprites = All.Num();
}
