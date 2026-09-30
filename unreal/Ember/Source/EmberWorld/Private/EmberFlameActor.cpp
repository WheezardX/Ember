#include "EmberFlameActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "EmberFireActor.h"
#include "EmberTerrainActor.h"

namespace
{
	/** Integer hash -> [0, 1): per-card seeds that do not depend on evaluation order. */
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

	// Per intensity class (index 1..3): cards per 30 m cell and flame height range (m).
	constexpr int32 CardsPerCell[4] = {0, 14, 10, 6};
	constexpr double MinH[4] = {0.0, 0.4, 1.2, 6.0};
	constexpr double MaxH[4] = {0.0, 1.1, 3.5, 22.0};   // crown fire: over the trees
	constexpr double Persist[4] = {0.0, 0.25, 0.45, 0.6};  // as M_Terrain's glow
	// M_Flame instance data: intensity, seed, half height, half width, card centre (UE cm)
	constexpr int32 DataFloats = 7;
}

AEmberFlameActor::AEmberFlameActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberFlameActor::Init(AEmberTerrainActor* InTerrain, AEmberFireActor* InFire, FString& OutError)
{
	Terrain = InTerrain;
	Fire = InFire;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Flame.M_Flame"));
	if (!Terrain || !Fire || !Plane || !Mat)
	{
		OutError = TEXT("flames need the terrain, the fire, /Engine/BasicShapes/Plane and M_Flame (ember-dev regen-assets)");
		return false;
	}
	Mid = UMaterialInstanceDynamic::Create(Mat, this);
	Cards = NewObject<UInstancedStaticMeshComponent>(this);
	Cards->SetStaticMesh(Plane);
	Cards->SetMaterial(0, Mid);
	Cards->NumCustomDataFloats = DataFloats;
	Cards->SetCastShadow(false);
	Cards->bAffectDistanceFieldLighting = false;
	Cards->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Cards->SetupAttachment(RootComponent);
	Cards->RegisterComponent();
	return true;
}

void AEmberFlameActor::SetFireTime(double Seconds)
{
	if (Mid)
	{
		Mid->SetScalarParameterValue(TEXT("FireTime"), static_cast<float>(Seconds));
	}
}

void AEmberFlameActor::Rebuild(const FVector& CameraLoc)
{
	if (!Cards || !Terrain || !Fire)
	{
		return;
	}
	const emberworld::Frame& Fr = Terrain->GetFrame();
	const double CamX = Fr.anchor_x + CameraLoc.X / 100.0;
	const double CamY = Fr.anchor_y - CameraLoc.Y / 100.0;
	TArray<FEmberBurningCell> Cells;
	Fire->BurningNear(CamX, CamY, RadiusM, Cells);
	const bool bClasses = Fire->bIntensityReported;
	const double CellM = Fire->CellM;

	TArray<FTransform> Xf;
	TArray<float> Data;
	for (const FEmberBurningCell& C : Cells)
	{
		// Freshness as the terrain glow: the front is tallest; with classes a cell keeps burning
		// (lower) for its whole residence, without them only the front window shows.
		const double AgeH = C.AgeS / 3600.0;
		const double Front = FMath::Exp(-AgeH / 0.7);
		const double Alive = bClasses ? FMath::Lerp(Persist[C.Cls], 1.0, Front) : Front;
		if (Alive < 0.08)
		{
			continue;
		}
		const double Head = bClasses ? FMath::Lerp(0.8, 1.3, static_cast<double>(C.Spread)) : 1.0;
		const int32 N = CardsPerCell[C.Cls];
		for (int32 K = 0; K < N; ++K)
		{
			const double H1 = Hash01(C.Index, K * 5u + 1u);
			const double H2 = Hash01(C.Index, K * 5u + 2u);
			const double H3 = Hash01(C.Index, K * 5u + 3u);
			const double H4 = Hash01(C.Index, K * 5u + 4u);
			const double X = C.X + (H1 - 0.5) * CellM;
			const double Y = C.Y + (H2 - 0.5) * CellM;
			const double DistM = FMath::Sqrt(FMath::Square(X - CamX) + FMath::Square(Y - CamY));
			const double Far = 1.0 - FMath::Clamp((DistM - FadeStartM) / (RadiusM - FadeStartM), 0.0, 1.0);
			if (Far <= 0.0)
			{
				continue;
			}
			double Gz = 0.0;
			if (!Terrain->SurfaceAt(X, Y, Gz) && !Terrain->GroundHeightAt(X, Y, Gz))
			{
				continue;
			}
			const double Hm = FMath::Lerp(MinH[C.Cls], MaxH[C.Cls], H3 * H3) * Head * FMath::Lerp(0.45, 1.0, Alive);
			if (DistM < FMath::Max(6.0, 1.2 * Hm))
			{
				continue;   // a card at the lens fills the frame with a glow column
			}
			const double Wm = Hm * (0.18 + 0.12 * H4);   // tongues, not walls
			// Base a little below the surface (the card's depth fade melts it into the ground).
			const FVector Base = Terrain->WorldToUE(X, Y, Gz - 0.1 * Hm);
			const FVector Centre = Base + FVector(0.0, 0.0, Hm * 50.0);
			FVector ToCam = CameraLoc - Centre;
			ToCam.Z = 0.0;
			if (!ToCam.Normalize())
			{
				ToCam = FVector::ForwardVector;
			}
			// Plane normal (local Z) to the camera, local X up: a vertical card.
			const FQuat Q = FRotationMatrix::MakeFromZX(ToCam, FVector::UpVector).ToQuat();
			// The flame's shape is defined in world units from the centre (M_Flame); the card is
			// oversized so the tongue never meets its edge (a clipped card reads as a rectangle).
			Xf.Add(FTransform(Q, Centre, FVector(Hm * 1.3, Wm * 2.0, 1.0)));   // the engine plane is 1 m
			Data.Add(static_cast<float>(Alive * Far * (0.6 + 0.4 * H4)));
			Data.Add(static_cast<float>(H1 * 0.7 + H2 * 0.3));
			Data.Add(static_cast<float>(Hm * 50.0));
			Data.Add(static_cast<float>(Wm * 50.0));
			Data.Add(static_cast<float>(Centre.X));
			Data.Add(static_cast<float>(Centre.Y));
			Data.Add(static_cast<float>(Centre.Z));
		}
	}
	Cards->ClearInstances();
	Cards->AddInstances(Xf, false, true);
	for (int32 I = 0; I < Xf.Num(); ++I)
	{
		Cards->SetCustomData(I, TArrayView<const float>(&Data[I * DataFloats], DataFloats), false);
	}
	Cards->MarkRenderStateDirty();
	NumCells = Cells.Num();
	NumCards = Xf.Num();
}
