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

void AEmberGroundRelief::Init(AEmberTerrainActor* InTerrain, double InRadiusM, double InSpacingM)
{
	Terrain = InTerrain;
	SpacingM = FMath::Clamp(InSpacingM, 0.1, 2.0);
	BlockM = BlockCells * SpacingM;
	Rb = FMath::Max(1, FMath::CeilToInt32(FMath::Max(8.0, InRadiusM) / BlockM - 0.5));
	RadiusM = (Rb + 0.5) * BlockM;
	FadeOutM = RadiusM;
	FadeInM = FMath::Max(BlockM, RadiusM - 8.0);
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

double AEmberGroundRelief::FadeAt(double X, double Y) const
{
	const double Px = (Centre.X + 0.5) * BlockM, Py = (Centre.Y + 0.5) * BlockM;
	const double D = FMath::Max(FMath::Abs(X - Px), FMath::Abs(Y - Py));
	const double T = FMath::Clamp((D - FadeInM) / FMath::Max(1e-3, FadeOutM - FadeInM), 0.0, 1.0);
	return 1.0 - T * T * (3.0 - 2.0 * T);
}

void AEmberGroundRelief::Requeue(const FVector2D& CameraXY)
{
	// Drop blocks outside the patch (components back to the pool).
	for (auto It = Blocks.CreateIterator(); It; ++It)
	{
		if (FMath::Abs(It.Key().X - Centre.X) > Rb || FMath::Abs(It.Key().Y - Centre.Y) > Rb)
		{
			if (It.Value().Comp)
			{
				It.Value().Comp->ClearAllMeshSections();
				It.Value().Comp->SetVisibility(false);
				Pool.Add(It.Value().Comp);
			}
			It.RemoveCurrent();
		}
	}
	Queue.Reset();
	for (int32 By = Centre.Y - Rb; By <= Centre.Y + Rb; ++By)
	{
		for (int32 Bx = Centre.X - Rb; Bx <= Centre.X + Rb; ++Bx)
		{
			const FIntPoint K(Bx, By);
			const FBlock* B = Blocks.Find(K);
			// The block's farthest corner from the patch centre (Chebyshev) - inside FadeIn, the
			// relief there does not depend on where the centre is.
			const double Px = (Centre.X + 0.5) * BlockM, Py = (Centre.Y + 0.5) * BlockM;
			const double Far = FMath::Max(FMath::Max(FMath::Abs(Bx * BlockM - Px), FMath::Abs((Bx + 1) * BlockM - Px)),
				FMath::Max(FMath::Abs(By * BlockM - Py), FMath::Abs((By + 1) * BlockM - Py)));
			const bool bFullNow = Far <= FadeInM;
			const bool bStale = !B || B->Generation != Generation || !(B->bFull && bFullNow) && B->Centre != Centre;
			if (bStale)
			{
				Queue.Add(K);
			}
		}
	}
	// Nearest first (the far rim can wait a few frames)
	Queue.Sort([&](const FIntPoint& A, const FIntPoint& B)
	{
		const double Da = FVector2D::DistSquared(FVector2D((A.X + 0.5) * BlockM, (A.Y + 0.5) * BlockM), CameraXY);
		const double Db = FVector2D::DistSquared(FVector2D((B.X + 0.5) * BlockM, (B.Y + 0.5) * BlockM), CameraXY);
		return Da > Db;   // popped from the end
	});
}

void AEmberGroundRelief::Update(const FVector& CameraUE)
{
	if (!Terrain)
	{
		return;
	}
	const emberworld::Frame& F = Terrain->GetFrame();
	const FVector2D Cam(F.anchor_x + CameraUE.X / 100.0, F.anchor_y - CameraUE.Y / 100.0);
	const FIntPoint C(FMath::FloorToInt32(Cam.X / BlockM), FMath::FloorToInt32(Cam.Y / BlockM));
	const int32 Gen = Terrain->GetTileGeneration();
	if (C != Centre || Gen != Generation)
	{
		Centre = C;
		Generation = Gen;
		Requeue(Cam);
	}
	const double T0 = FPlatformTime::Seconds();
	while (Queue.Num() && (bSync || (FPlatformTime::Seconds() - T0) * 1000.0 < BuildBudgetMs))
	{
		const FIntPoint K = Queue.Pop(EAllowShrinking::No);
		BuildBlock(K, Blocks.FindOrAdd(K));
	}
	LastBuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	NumSections = Blocks.Num();
	NumVerts = 0;
	for (const auto& Kv : Blocks)
	{
		NumVerts += Kv.Value.Verts;
	}
}

void AEmberGroundRelief::BuildBlock(const FIntPoint& Bk, FBlock& Out)
{
	if (!Out.Comp)
	{
		if (Pool.Num())
		{
			Out.Comp = Pool.Pop();
			Out.Comp->SetVisibility(true);
		}
		else
		{
			Out.Comp = NewObject<UProceduralMeshComponent>(this);
			Out.Comp->SetMobility(EComponentMobility::Movable);
			Out.Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Out.Comp->SetCastShadow(true);
			Out.Comp->bCastFarShadow = false;
			Out.Comp->SetupAttachment(RootComponent);
			Out.Comp->RegisterComponent();
			AllComps.Add(Out.Comp);
		}
	}
	// Vertex lattice of the block plus a one-cell apron (normals across block edges). Rows run
	// south from the block's north edge; shared edges get the same world points -> no cracks.
	const int32 C = BlockCells;
	const int32 N = C + 3;                         // -1 .. C+1
	const double X0 = Bk.X * BlockM, Y0 = (Bk.Y + 1) * BlockM;
	TArray<double> Z;
	TArray<uint8> Valid;
	Z.SetNumUninitialized(N * N);
	Valid.SetNumZeroed(N * N);
	bool bFull = true;
	for (int32 J = 0; J < N; ++J)
	{
		for (int32 I = 0; I < N; ++I)
		{
			const double X = X0 + (I - 1) * SpacingM, Y = Y0 - (J - 1) * SpacingM;
			double Zs = 0.0;
			if (!Terrain->SurfaceAt(X, Y, Zs))
			{
				continue;
			}
			float W4[4] = {0, 0, 0, 0};
			Terrain->GroundMixAt(X, Y, W4);
			const double R = emberworld::micro_relief(X, Y, {W4[0], W4[1], W4[2], W4[3]}, Params);
			const double Fd = FadeAt(X, Y);
			bFull &= Fd >= 1.0;
			Z[J * N + I] = Zs + Fd * (LiftM + R) - (1.0 - Fd) * 0.06;   // rim: 6 cm under the terrain
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
	TArray<FSection, TInlineAllocator<2>> Sections;
	TMap<uint64, int32> SectionOf;
	auto Vert = [&](FSection& S, int32 I, int32 J) -> int32   // I, J in 0..C (lattice, no apron)
	{
		const int32 Node = (J + 1) * N + (I + 1);
		if (const int32* Found = S.Remap.Find(Node))
		{
			return *Found;
		}
		const double X = X0 + I * SpacingM, Y = Y0 - J * SpacingM;
		auto Zc = [&](int32 Di, int32 Dj)
		{
			const int32 M = Node + Dj * N + Di;
			return Valid[M] ? Z[M] : Z[Node];
		};
		const double Dzdx = (Zc(1, 0) - Zc(-1, 0)) / (2.0 * SpacingM);
		const double DzdySouth = (Zc(0, 1) - Zc(0, -1)) / (2.0 * SpacingM);
		const int32 Idx = S.V.Num();
		S.V.Add(Terrain->WorldToUE(X, Y, Z[Node]));
		S.Nrm.Add(FVector(-Dzdx, -DzdySouth, 1.0).GetSafeNormal());   // UE: X east, Y south
		S.UV0.Emplace((X - S.S.MinX) / S.S.Width, (S.S.MaxY - Y) / S.S.Height);
		S.UV1.Emplace((X - S.S.RMinX) / S.S.RWidth, (S.S.RMaxY - Y) / S.S.RHeight);
		S.Remap.Add(Node, Idx);
		return Idx;
	};
	for (int32 J = 0; J < C; ++J)
	{
		for (int32 I = 0; I < C; ++I)
		{
			const int32 A = (J + 1) * N + (I + 1);
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
			S.Tris.Append({V00, V01, V10, V10, V01, V11});   // the terrain's winding (heightfield.cpp)
		}
	}
	Out.Comp->ClearAllMeshSections();
	Out.Verts = 0;
	for (int32 K = 0; K < Sections.Num(); ++K)
	{
		FSection& S = Sections[K];
		Out.Comp->CreateMeshSection(K, S.V, S.Tris, S.Nrm, S.UV0, S.UV1, TArray<FVector2D>(), TArray<FVector2D>(),
			TArray<FColor>(), TArray<FProcMeshTangent>(), false);
		Out.Comp->SetMaterial(K, S.S.Material);
		Out.Verts += S.V.Num();
	}
	Out.Generation = Generation;
	Out.Centre = Centre;
	Out.bFull = bFull;
	++Builds;
}
