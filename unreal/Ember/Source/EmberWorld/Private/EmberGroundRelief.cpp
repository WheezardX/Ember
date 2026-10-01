#include "EmberGroundRelief.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"

#include "EmberTerrainActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEmberRelief, Log, All);

AEmberGroundRelief::AEmberGroundRelief()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AEmberGroundRelief::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		Update(PC->PlayerCameraManager->GetCameraLocation());
	}
}

void AEmberGroundRelief::Init(AEmberTerrainActor* InTerrain, double InRadiusM, double InSpacingM)
{
	Terrain = InTerrain;
	RadiusM = FMath::Max(8.0, InRadiusM);
	SpacingM = FMath::Clamp(InSpacingM, 0.1, 2.0);
	Mesh = NewObject<UProceduralMeshComponent>(this);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	Mesh->bCastFarShadow = false;
	Mesh->SetupAttachment(RootComponent);
	Mesh->RegisterComponent();
}

void AEmberGroundRelief::Update(const FVector& CameraUE, bool bForce)
{
	if (!Terrain || !Mesh)
	{
		return;
	}
	const emberworld::Frame& F = Terrain->GetFrame();
	const double X = F.anchor_x + CameraUE.X / 100.0;
	const double Y = F.anchor_y - CameraUE.Y / 100.0;
	const bool bMoved = !bBuilt || FMath::Max(FMath::Abs(X - Cx), FMath::Abs(Y - Cy)) > RecentreM;
	if (bForce || bMoved || Terrain->GetTileGeneration() != BuiltGeneration)
	{
		Build(X, Y);
	}
}

void AEmberGroundRelief::Build(double CenterX, double CenterY)
{
	const double T0 = FPlatformTime::Seconds();
	// Snap the patch to its own lattice so the same world point always gets the same vertex.
	Cx = FMath::RoundToDouble(CenterX / SpacingM) * SpacingM;
	Cy = FMath::RoundToDouble(CenterY / SpacingM) * SpacingM;
	BuiltGeneration = Terrain->GetTileGeneration();
	bBuilt = true;
	const int32 Half = FMath::CeilToInt32(RadiusM / SpacingM);
	const int32 N = 2 * Half + 1;
	const double X0 = Cx - Half * SpacingM, Y0 = Cy + Half * SpacingM;   // NW corner, rows run south
	TArray<double> Z;
	TArray<uint8> Valid;
	Z.SetNumUninitialized(N * N);
	Valid.SetNumZeroed(N * N);
	const double FadeIn = RadiusM - 8.0, FadeOut = RadiusM - SpacingM;
	for (int32 J = 0; J < N; ++J)
	{
		for (int32 I = 0; I < N; ++I)
		{
			const double X = X0 + I * SpacingM, Y = Y0 - J * SpacingM;
			double Zs = 0.0;
			if (!Terrain->SurfaceAt(X, Y, Zs))
			{
				continue;
			}
			float W4[4] = {0, 0, 0, 0};
			Terrain->GroundMixAt(X, Y, W4);
			const double R = emberworld::micro_relief(X, Y, {W4[0], W4[1], W4[2], W4[3]}, Params);
			// Full relief in the middle; at the rim it sinks 6 cm under the terrain (no seam).
			const double D = FMath::Max(FMath::Abs(X - Cx), FMath::Abs(Y - Cy));
			const double T = FMath::Clamp((D - FadeIn) / (FadeOut - FadeIn), 0.0, 1.0);
			const double Fd = 1.0 - T * T * (3.0 - 2.0 * T);
			Z[J * N + I] = Zs + Fd * (LiftM + R) - (1.0 - Fd) * 0.06;
			Valid[J * N + I] = 1;
		}
	}
	struct FSection
	{
		AEmberTerrainActor::FTileShading S;
		TArray<FVector> V;
		TArray<FVector> Nrm;
		TArray<FVector2D> UV0, UV1;
		TArray<int32> Tris;
		TMap<int32, int32> Remap;
	};
	TArray<FSection> Sections;
	TMap<uint64, int32> SectionOf;
	auto Vert = [&](FSection& S, int32 I, int32 J) -> int32
	{
		const int32 Node = J * N + I;
		if (const int32* Found = S.Remap.Find(Node))
		{
			return *Found;
		}
		const double X = X0 + I * SpacingM, Y = Y0 - J * SpacingM;
		auto Zc = [&](int32 Ii, int32 Jj)
		{
			Ii = FMath::Clamp(Ii, 0, N - 1);
			Jj = FMath::Clamp(Jj, 0, N - 1);
			return Valid[Jj * N + Ii] ? Z[Jj * N + Ii] : Z[Node];
		};
		// UE normal: X east, Y south
		const double Dzdx = (Zc(I + 1, J) - Zc(I - 1, J)) / (2.0 * SpacingM);
		const double DzdySouth = (Zc(I, J + 1) - Zc(I, J - 1)) / (2.0 * SpacingM);
		const int32 Idx = S.V.Num();
		S.V.Add(Terrain->WorldToUE(X, Y, Z[Node]));
		S.Nrm.Add(FVector(-Dzdx, -DzdySouth, 1.0).GetSafeNormal());
		S.UV0.Emplace((X - S.S.MinX) / S.S.Width, (S.S.MaxY - Y) / S.S.Height);
		S.UV1.Emplace((X - S.S.RMinX) / S.S.RWidth, (S.S.RMaxY - Y) / S.S.RHeight);
		S.Remap.Add(Node, Idx);
		return Idx;
	};
	for (int32 J = 0; J + 1 < N; ++J)
	{
		for (int32 I = 0; I + 1 < N; ++I)
		{
			const int32 A = J * N + I;
			if (!Valid[A] || !Valid[A + 1] || !Valid[A + N] || !Valid[A + N + 1])
			{
				continue;
			}
			AEmberTerrainActor::FTileShading Sh;
			if (!Terrain->TileShadingAt(X0 + (I + 0.5) * SpacingM, Y0 - (J + 0.5) * SpacingM, Sh))
			{
				continue;
			}
			int32* Si = SectionOf.Find(Sh.Key);
			if (!Si)
			{
				FSection& New = Sections.AddDefaulted_GetRef();
				New.S = Sh;
				Si = &SectionOf.Add(Sh.Key, Sections.Num() - 1);
			}
			FSection& S = Sections[*Si];
			const int32 V00 = Vert(S, I, J), V10 = Vert(S, I + 1, J);
			const int32 V01 = Vert(S, I, J + 1), V11 = Vert(S, I + 1, J + 1);
			// the terrain's winding and diagonal (heightfield.cpp: a c b, b c d)
			S.Tris.Append({V00, V01, V10, V10, V01, V11});
		}
	}
	Mesh->ClearAllMeshSections();
	NumVerts = 0;
	for (int32 K = 0; K < Sections.Num(); ++K)
	{
		FSection& S = Sections[K];
		Mesh->CreateMeshSection(K, S.V, S.Tris, S.Nrm, S.UV0, S.UV1, TArray<FVector2D>(), TArray<FVector2D>(),
			TArray<FColor>(), TArray<FProcMeshTangent>(), false);
		Mesh->SetMaterial(K, S.S.Material);
		NumVerts += S.V.Num();
	}
	NumSections = Sections.Num();
	++Builds;
	LastBuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	UE_LOG(LogEmberRelief, Verbose, TEXT("ground relief: %d verts, %d sections, %.1f ms"), NumVerts, NumSections, LastBuildMs);
}
