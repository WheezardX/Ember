#include "EmberGroundCoverActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "EmberTerrainActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEmberCover, Log, All);

AEmberGroundCoverActor::AEmberGroundCoverActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberGroundCoverActor::Init(AEmberTerrainActor* InTerrain, const FString& LookPath, FString& OutError)
{
	Terrain = InTerrain;
	const emberworld::CoverRulesResult R = emberworld::load_cover_rules(TCHAR_TO_UTF8(*LookPath));
	if (!R.ok())
	{
		OutError = UTF8_TO_TCHAR(R.error.c_str());
		return false;
	}
	if (!R.present)
	{
		OutError = TEXT("the look has no [cover]");
		return false;
	}
	Rules = R.rules;
	for (const emberworld::CoverItem& It : Rules.items)
	{
		const FString Key = UTF8_TO_TCHAR(It.mesh.c_str());
		const bool bGrass = Key == TEXT("grass");  // the scatter's bunchgrass meshes, as tufts
		const FString MiPath = bGrass ? TEXT("/Game/Ember/Generated/Veg/MI_Veg_bunchgrass.MI_Veg_bunchgrass")
		                              : FString::Printf(TEXT("/Game/Ember/Generated/Cover/MI_Cover_%s.MI_Cover_%s"), *Key, *Key);
		UMaterialInterface* Mi = LoadObject<UMaterialInterface>(nullptr, *MiPath);
		if (!Mi)
		{
			OutError = TEXT("missing ") + MiPath + TEXT(" (ember-dev regen-assets)");
			return false;
		}
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Mi, this);
		Mid->SetScalarParameterValue(TEXT("Consume"), It.consume);
		Mid->SetScalarParameterValue(TEXT("Smoulder"), It.smoulder);
		Materials.Add(Mid);
		FirstMesh.Add(Meshes.Num());
		for (int32 V = 0; V < It.variants; ++V)
		{
			const FString N = bGrass ? FString::Printf(TEXT("SM_bunchgrass_v%d"), V) : FString::Printf(TEXT("SM_Cover_%s_v%d"), *Key, V);
			const FString P = (bGrass ? TEXT("/Game/Ember/Generated/Veg/") : TEXT("/Game/Ember/Generated/Cover/")) + N + TEXT(".") + N;
			UStaticMesh* SM = LoadObject<UStaticMesh>(nullptr, *P);
			if (!SM)
			{
				OutError = TEXT("missing ") + P + TEXT(" (ember-dev regen-assets)");
				return false;
			}
			Meshes.Add(SM);
		}
	}
	bInitialised = true;
	UE_LOG(LogEmberCover, Display, TEXT("ground cover: %d item(s), %d mesh(es), radius %.0f m"), Rules.items.size(), Meshes.Num(), RadiusM);
	return true;
}

void AEmberGroundCoverActor::ClearCell(FCoverCell& Cell)
{
	for (UInstancedStaticMeshComponent* C : Cell.Components)
	{
		if (C)
		{
			C->DestroyComponent();
		}
	}
	Cell.Components.Reset();
	Instances -= Cell.Count;
	Cell.Count = 0;
}

void AEmberGroundCoverActor::BuildCell(const FIntPoint& Key, FCoverCell& Cell)
{
	ClearCell(Cell);
	const double X0 = Key.X * CellM, Y0 = Key.Y * CellM;
	// A cell is final only when every sample came from the finest LOD: a coarse tile's mix (all
	// that may be loaded when the camera arrives) differs, and placement must not depend on
	// streaming timing (the goldens caught a shrub that moved between runs).
	bool bMissing = false;
	const int32 Finest = Terrain->GetRegion() ? Terrain->GetRegion()->finest_lod() : 0;
	std::vector<emberworld::CoverInstance> Out;
	emberworld::scatter_cover(Rules, X0, Y0, X0 + CellM, Y0 + CellM,
		[&](double X, double Y, std::array<float, 4>& W) {
			float M[4];
			int32 Lod = -1;
			if (!Terrain->GroundMixAt(X, Y, M, &Lod))
			{
				bMissing = true;
				return false;
			}
			if (Lod < Finest)
			{
				bMissing = true;
			}
			W = {M[0], M[1], M[2], M[3]};
			return true;
		}, Out);
	Cell.bPending = bMissing && ++Cell.Attempts < 120;
	TArray<TArray<FTransform>> PerMesh;
	PerMesh.SetNum(Meshes.Num());
	for (const emberworld::CoverInstance& In : Out)
	{
		double Z = 0.0;
		if (!Terrain->GroundHeightAt(In.x, In.y, Z))
		{
			continue;
		}
		const int32 Mi = FirstMesh[In.item] + In.variant;
		const FBoxSphereBounds B = Meshes[Mi]->GetBounds();
		const double MeshH = FMath::Max(1.0, 2.0 * B.BoxExtent.Z);
		const double S = In.height_m * 100.0 / MeshH;
		FVector Loc = Terrain->WorldToUE(In.x, In.y, Z);
		Loc.Z -= (B.Origin.Z - B.BoxExtent.Z) * S + 2.0;  // bottom a touch into the ground
		PerMesh[Mi].Add(FTransform(FRotator(0.0, FMath::RadiansToDegrees(In.yaw_rad), 0.0), Loc, FVector(S)));
	}
	for (int32 Mi = 0; Mi < PerMesh.Num(); ++Mi)
	{
		if (PerMesh[Mi].Num() == 0)
		{
			continue;
		}
		int32 Item = 0;
		while (Item + 1 < FirstMesh.Num() && FirstMesh[Item + 1] <= Mi)
		{
			++Item;
		}
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
		C->SetStaticMesh(Meshes[Mi]);
		C->SetMaterial(0, Materials[Item]);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(true);
		C->SetMobility(EComponentMobility::Movable);
		C->InstancingRandomSeed = static_cast<int32>((uint32(Key.X) * 73856093u) ^ (uint32(Key.Y) * 19349663u) ^ uint32(Mi)) | 1;
		C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;  // wind WPO
		C->SetWorldPositionOffsetDisableDistance(static_cast<int32>(RadiusM * 100.0));
		C->SetCullDistances(0, static_cast<int32>((RadiusM + CellM) * 100.0));
		C->bAffectDistanceFieldLighting = false;
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(PerMesh[Mi], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		Cell.Components.Add(C);
		Cell.Count += PerMesh[Mi].Num();
	}
	Instances += Cell.Count;
}

void AEmberGroundCoverActor::UpdateCells(const FVector& CamUE)
{
	const emberworld::Frame& F = Terrain->GetFrame();
	const double Cx = F.anchor_x + CamUE.X / 100.0, Cy = F.anchor_y - CamUE.Y / 100.0;
	const int32 R = FMath::CeilToInt(RadiusM / CellM);
	const int32 Kx = FMath::FloorToInt(Cx / CellM), Ky = FMath::FloorToInt(Cy / CellM);
	auto CellDist = [&](const FIntPoint& K) {  // camera to the nearest point of the cell
		const double Dx = FMath::Max3(K.X * CellM - Cx, 0.0, Cx - (K.X + 1) * CellM);
		const double Dy = FMath::Max3(K.Y * CellM - Cy, 0.0, Cy - (K.Y + 1) * CellM);
		return FMath::Sqrt(Dx * Dx + Dy * Dy);
	};
	TArray<FIntPoint> Drop;
	for (const TPair<FIntPoint, FCoverCell>& P : Cells)
	{
		if (CellDist(P.Key) > RadiusM + CellM)
		{
			Drop.Add(P.Key);
		}
	}
	for (const FIntPoint& K : Drop)
	{
		ClearCell(Cells[K]);
		Cells.Remove(K);
	}
	for (int32 Dy = -R; Dy <= R; ++Dy)
	{
		for (int32 Dx = -R; Dx <= R; ++Dx)
		{
			const FIntPoint K(Kx + Dx, Ky + Dy);
			if (CellDist(K) > RadiusM)
			{
				continue;
			}
			FCoverCell* C = Cells.Find(K);
			if (!C)
			{
				C = &Cells.Add(K);
				BuildCell(K, *C);
			}
			else if (C->bPending)
			{
				BuildCell(K, *C);
			}
		}
	}
}

void AEmberGroundCoverActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!bInitialised || !Terrain || !PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	bool bPending = false;
	for (const TPair<FIntPoint, FCoverCell>& P : Cells)
	{
		bPending |= P.Value.bPending;
	}
	if (bPending || FVector::Dist(Cam, LastCamera) > 400.0)  // re-select every 4 m moved
	{
		LastCamera = Cam;
		UpdateCells(Cam);
	}
}

void AEmberGroundCoverActor::SetWind(double StrengthCm, double FromDeg)
{
	const double To = FMath::DegreesToRadians(FromDeg + 180.0);
	const FLinearColor Dir(FMath::Sin(To), -FMath::Cos(To), 0.0, 0.0);
	for (UMaterialInstanceDynamic* M : Materials)
	{
		M->SetScalarParameterValue(TEXT("WindStrength"), static_cast<float>(StrengthCm));
		M->SetVectorParameterValue(TEXT("WindDir"), Dir);
	}
}

void AEmberGroundCoverActor::SetWindTime(double Seconds)
{
	for (UMaterialInstanceDynamic* M : Materials)
	{
		M->SetScalarParameterValue(TEXT("WindTime"), static_cast<float>(Seconds));
	}
}

void AEmberGroundCoverActor::SetFire(UTexture* FireTex, const FLinearColor& FireRect)
{
	for (UMaterialInstanceDynamic* M : Materials)
	{
		M->SetTextureParameterValue(TEXT("FireTex"), FireTex);
		M->SetVectorParameterValue(TEXT("FireRect"), FireRect);
		M->SetScalarParameterValue(TEXT("FireOn"), FireTex ? 1.f : 0.f);
	}
}

void AEmberGroundCoverActor::SetFireTime(double Seconds)
{
	for (UMaterialInstanceDynamic* M : Materials)
	{
		M->SetScalarParameterValue(TEXT("FireTime"), static_cast<float>(Seconds));
	}
}
