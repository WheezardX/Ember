#include "EmberSmokeActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "EmberTerrainActor.h"

namespace
{
	/** Integer hash -> [0, 1): per-puff seeds that do not depend on evaluation order. */
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

	double SmoothStep(double A, double B, double X)
	{
		const double T = FMath::Clamp((X - A) / (B - A), 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T);
	}

	struct FPuff
	{
		FVector Loc;
		double RadiusM;
		double Stretch;
		double Roll;
		float Data[4];
		double Dist2;
	};
}

AEmberSmokeActor::AEmberSmokeActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberSmokeActor::Init(AEmberTerrainActor* InTerrain, FString& OutError)
{
	Terrain = InTerrain;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Smoke.M_Smoke"));
	if (!Terrain || !Plane || !Mat)
	{
		OutError = TEXT("smoke needs the terrain, /Engine/BasicShapes/Plane and M_Smoke (ember-dev regen-assets)");
		return false;
	}
	Puffs = NewObject<UInstancedStaticMeshComponent>(this);
	Puffs->SetStaticMesh(Plane);
	Puffs->SetMaterial(0, Mat);
	Puffs->NumCustomDataFloats = 4;
	Puffs->SetCastShadow(false);
	Puffs->bAffectDistanceFieldLighting = false;
	Puffs->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Puffs->SetupAttachment(RootComponent);
	Puffs->RegisterComponent();
	return true;
}

void AEmberSmokeActor::SetWind(double FromDeg, double SpeedMs)
{
	WindFromDeg = FromDeg;
	WindMs = FMath::Max(1.0, SpeedMs);
}

void AEmberSmokeActor::SetSources(const TArray<FEmberSmokeSource>& InSources)
{
	Sources = InSources;
	// Strongest first: the puff budget goes to the plumes that matter.
	Sources.Sort([](const FEmberSmokeSource& A, const FEmberSmokeSource& B)
	{
		return A.Strength != B.Strength ? A.Strength > B.Strength : A.Key < B.Key;
	});
	NumSources = Sources.Num();
}

void AEmberSmokeActor::Rebuild(const FVector& CameraLoc, const FRotator& CameraRot)
{
	if (!Puffs || !Terrain)
	{
		return;
	}
	// Downwind unit vector in the region CRS (x east, y north).
	const double To = FMath::DegreesToRadians(WindFromDeg + 180.0);
	const double Ux = FMath::Sin(To);
	const double Uy = FMath::Cos(To);
	const FVector CamRight = FRotationMatrix(CameraRot).GetUnitAxis(EAxis::Y);

	TArray<FPuff> All;
	All.Reserve(MaxPuffs);
	NumPlumes = 0;
	MaxTopM = 0.0;
	for (const FEmberSmokeSource& S : Sources)
	{
		const double Q = S.Strength;
		const double SqQ = FMath::Sqrt(Q);
		double Gz = 0.0;
		if (!Terrain->GroundHeightAt(S.X, S.Y, Gz))
		{
			continue;
		}
		// Neighbouring bins burn as one convective column: height from the 1.5 km cluster.
		const double H = FMath::Clamp(90.0 * FMath::Sqrt(static_cast<double>(S.Cluster)), 150.0, 4000.0);
		MaxTopM = FMath::Max(MaxTopM, H);
		const double L = FMath::Clamp(6.0 * H, 1500.0, 18000.0);     // plume length before it thins out
		const double Life = L / WindMs;                              // puff lifetime (s)
		const double R0 = 60.0 + 7.0 * SqQ;                        // ~a bin across at full strength
		const double Base = 0.5 * (1.0 - FMath::Exp(-Q / 25.0));    // weak sources: faint wisps
		// Enough puffs that neighbours always overlap (spacing ~0.4 x the mid-plume radius).
		const double RMid = R0 + 0.06 * L + 0.125 * H;
		const int32 K = FMath::Clamp(FMath::CeilToInt32(L / (0.25 * RMid)), 8, 96);
		if (All.Num() + K > MaxPuffs)
		{
			break;
		}
		++NumPlumes;
		const double Hot = FMath::Min(1.0, S.Burning / FMath::Max(1.0, Q)) * FMath::Min(1.0, S.Burning / 5.0);
		const double Phase = Hash01(S.Key, 0x51u);
		// A puff advances one slot (L / K) every Life / K seconds, so it crosses the plume in Life.
		// Index I after W wraps is the same physical puff as index I + 1 after W + 1: seed on W - I.
		const double Slots = ClockS * K / Life + Phase * K;
		const double Cycle = FMath::Frac(Slots);
		const int64 Wraps = FMath::FloorToInt64(Slots);
		for (int32 I = 0; I < K; ++I)
		{
			const double F = (I + Cycle) / K;                          // life fraction 0..1
			const double D = F * L;                                    // downwind distance (m)
			const double Z = H * (1.0 - FMath::Exp(-D / (0.8 * H)));   // bent-over rise
			const double R = R0 + 0.12 * D + 0.25 * Z;
			const uint32 Seed = static_cast<uint32>(Wraps - I);
			const double H1 = Hash01(S.Key, Seed * 4u + 1u);
			const double H2 = Hash01(S.Key, Seed * 4u + 2u);
			const double H3 = Hash01(S.Key, Seed * 4u + 3u);
			const double Lat = (H1 - 0.5) * R * 0.9;
			const double Wx = S.X + Ux * D - Uy * Lat;
			const double Wy = S.Y + Uy * D + Ux * Lat;
			const double Wz = Gz + Z + R * 0.5 + (H2 - 0.5) * R * 0.5;

			// Fade in as the column forms (no discs on the ground), thin out downwind.
			const double Life01 = SmoothStep(0.0, 3.0 * R0, D) * (1.0 - SmoothStep(0.5, 1.0, F));
			const double Thin = FMath::Max(0.25, FMath::Sqrt(R0 / R));
			FPuff P;
			P.Loc = Terrain->WorldToUE(Wx, Wy, Wz);
			P.RadiusM = R;
			P.Stretch = 1.0 + 0.6 * H1;  // elongated, so no card reads as a disc
			P.Roll = H3 * 2.0 * PI;
			P.Data[0] = static_cast<float>(Base * Life01 * Thin);
			P.Data[1] = static_cast<float>(0.5 * Hot * FMath::Exp(-Z / (R0 + 80.0)));
			P.Data[2] = static_cast<float>(FMath::FloorToInt32(H2 * 4.0) & 3);
			P.Data[3] = static_cast<float>(R * 40.0);  // depth fade (cm): 0.4 x radius
			P.Dist2 = FVector::DistSquared(P.Loc, CameraLoc);
			// A card around the camera would fill the screen: fade puffs we are inside.
			const double DistM = FMath::Sqrt(P.Dist2) / 100.0;
			P.Data[0] *= static_cast<float>(SmoothStep(0.6 * R, 1.6 * R, DistM));
			All.Add(P);
		}
	}
	// Back to front: translucent instances blend in buffer order.
	All.Sort([](const FPuff& A, const FPuff& B) { return A.Dist2 > B.Dist2; });

	TArray<FTransform> Xf;
	Xf.Reserve(All.Num());
	for (const FPuff& P : All)
	{
		const FVector ToCam = (CameraLoc - P.Loc).GetSafeNormal();
		const FVector Right = CamRight.RotateAngleAxisRad(P.Roll, ToCam);
		const FQuat Q = FRotationMatrix::MakeFromZX(ToCam, Right).ToQuat();
		const double Scale = 2.0 * P.RadiusM;  // the engine plane is 1 m square
		Xf.Add(FTransform(Q, P.Loc, FVector(Scale * P.Stretch, Scale, 1.0)));
	}
	Puffs->ClearInstances();
	Puffs->AddInstances(Xf, false, true);
	for (int32 I = 0; I < All.Num(); ++I)
	{
		Puffs->SetCustomData(I, TArrayView<const float>(All[I].Data, 4), false);
	}
	Puffs->MarkRenderStateDirty();
	NumPuffs = All.Num();
}
