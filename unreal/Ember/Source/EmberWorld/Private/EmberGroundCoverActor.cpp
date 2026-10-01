#include "EmberGroundCoverActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "EmberTerrainActor.h"
#include "EmberVegetationActor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/scatter.h"
THIRD_PARTY_INCLUDES_END

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

bool AEmberGroundCoverActor::PlaceLying(const emberworld::CoverInstance& In, const FBoxSphereBounds& B, double S,
	emberworld::CoverPose Pose, FTransform& Out, bool& bOutPending) const
{
	// Lying meshes (logs, poles) run along +X from their foot at x = 0 and sit on z = 0.
	const double LenM = FMath::Max(0.1, (B.Origin.X + B.BoxExtent.X) * S / 100.0);
	const double DiaM = FMath::Max(0.05, 2.0 * B.BoxExtent.Z * S / 100.0);
	// UE yaw (X east, Y south) -> world direction (x east, y north)
	const double Dx = FMath::Cos(In.yaw_rad), Dy = -FMath::Sin(In.yaw_rad);
	double Z0 = 0.0, Z1 = 0.0, Zm = 0.0;
	if (!Terrain->SurfaceAt(In.x, In.y, Z0))
	{
		return false;
	}
	const uint64 H = emberworld::scatter::hash64({static_cast<uint64>(FMath::RoundToInt64(In.x * 100.0)),
		static_cast<uint64>(FMath::RoundToInt64(In.y * 100.0)), 0x1065ull});
	const double U1 = emberworld::scatter::u01(H);
	const double U2 = emberworld::scatter::u01(H * 0x9E3779B97F4A7C15ull + 1);
	FVector Fwd, Up;
	double Zfoot;
	if (Pose == emberworld::CoverPose::Leaner)
	{
		// Hung-up tree (8g, Brad: they floated - mid-air or on a crown's outermost twigs). Anchor on
		// a REAL tree first: a host >= 8 m within 12 m; contact on its trunk at 30-70 % of its
		// height, pushed a little into the crown toward the foot side; the foot on the rendered
		// ground for a 20-45 deg lean; the stem scaled so the contact sits ~70 % up it (the top
		// pokes past the host into its crown). No sane host / foot -> no hung-up tree here.
		if (!Vegetation)
		{
			return false;
		}
		// Ladder fuel (Brad): the look's density is the maximum, reached where crowns start low
		// (LANDFIRE CBH); where they start high only 15 % of it remains.
		float Ladder = 0.f;
		if (!Terrain->LadderAt(In.x, In.y, Ladder))
		{
			Ladder = 0.5f;               // no CBH layer: half density
		}
		if (emberworld::scatter::u01(H ^ 0x1ADDE5ull) > 0.15 + 0.85 * Ladder)
		{
			++LeanersThinned;
			return false;
		}
		TArray<AEmberVegetationActor::FTreeInfo> Near;
		if (!Vegetation->TreesNear(In.x, In.y, 12.0, Near))
		{
			bOutPending = true;          // vegetation not streamed in here yet: retry the cell
			return false;
		}
		Near.RemoveAll([](const AEmberVegetationActor::FTreeInfo& T) { return T.HeightM < 8.f; });
		if (Near.Num() == 0)
		{
			++LeanersNoHost;
			return false;
		}
		Near.Sort([](const AEmberVegetationActor::FTreeInfo& A, const AEmberVegetationActor::FTreeInfo& B)
			{ return A.X != B.X ? A.X < B.X : A.Y < B.Y; });   // order-independent pick
		const AEmberVegetationActor::FTreeInfo& Host = Near[static_cast<int32>(U2 * Near.Num()) % Near.Num()];
		// foot side: from the host toward the candidate point (the scatter's random position)
		FVector2D Dir(In.x - Host.X, In.y - Host.Y);
		if (!Dir.Normalize())
		{
			Dir = FVector2D(Dx, Dy);
		}
		const double Frac = 0.3 + 0.4 * U1;
		const double Lean = FMath::DegreesToRadians(20.0 + 25.0 * emberworld::scatter::u01(H ^ 0x5151ull));
		const FVector Contact = Host.BaseUE + Host.UpUE * (Host.HeightM * Frac * 100.0)
			+ FVector(Dir.X, -Dir.Y, 0.0) * (Host.CrownRadiusM * 0.25 * (1.0 - Frac) * 100.0);
		double Fx = 0, Fy = 0, Fz = 0;
		const emberworld::Frame& F = Terrain->GetFrame();
		const double Cx = F.anchor_x + Contact.X / 100.0, Cy = F.anchor_y - Contact.Y / 100.0;
		double Cz = Contact.Z / 100.0 + F.anchor_z;
		double Dist = 3.0;
		bool bFoot = false;
		for (int32 It = 0; It < 3; ++It)   // foot height depends on where the foot lands: iterate
		{
			Fx = Cx + Dir.X * Dist;
			Fy = Cy + Dir.Y * Dist;
			if (!Terrain->SurfaceAt(Fx, Fy, Fz))
			{
				break;
			}
			bFoot = true;
			Dist = FMath::Max(1.0, (Cz - Fz) / FMath::Tan(Lean));
		}
		if (!bFoot)
		{
			return false;
		}
		Fx = Cx + Dir.X * Dist;
		Fy = Cy + Dir.Y * Dist;
		if (!Terrain->SurfaceAt(Fx, Fy, Fz))
		{
			return false;
		}
		const FVector Foot = Terrain->WorldToUE(Fx, Fy, Fz - 0.15);   // butt dug in 15 cm
		const FVector Span = Contact - Foot;
		const double SpanM = Span.Size() / 100.0;
		const double Pitch = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Span.Z / FMath::Max(Span.Size(), 1.0), -1.0, 1.0)));
		if (SpanM < 4.0 || SpanM > 22.0 || Pitch < 12.0 || Pitch > 55.0)
		{
			++LeanersRejected;
			return false;                // too short / long / flat / steep to read as hung up
		}
		for (const AEmberVegetationActor::FTreeInfo& T : Near)   // the foot must not stand in a trunk
		{
			if (&T != &Host && FMath::Square(T.X - Fx) + FMath::Square(T.Y - Fy) < 1.0)
			{
				return false;
			}
		}
		const double MeshLenCm = FMath::Max(100.0, B.Origin.X + B.BoxExtent.X);
		const double Scale = Span.Size() / (0.7 * MeshLenCm);
		Out = FTransform(FRotationMatrix::MakeFromXZ(Span.GetSafeNormal(), FVector::UpVector).ToQuat(), Foot, FVector(Scale));
		++LeanersPlaced;
		// facts: the hung-up tree nearest the camera (deterministic for a given camera)
		const FVector Mid = 0.5 * (Foot + Contact);
		if (LastLeanerFootUE.IsZero() || FVector::DistSquared2D(Mid, LastCamera) < FVector::DistSquared2D(0.5 * (LastLeanerFootUE + LastLeanerContactUE), LastCamera))
		{
			LastLeanerFootUE = Foot;
			LastLeanerContactUE = Contact;
		}
		return true;
	}
	else
	{
		const double Mx = In.x + Dx * LenM * 0.5, My = In.y + Dy * LenM * 0.5;
		if (!Terrain->SurfaceAt(In.x + Dx * LenM, In.y + Dy * LenM, Z1) || !Terrain->SurfaceAt(Mx, My, Zm))
		{
			return false;
		}
		// Rest on a hump under the middle rather than cut through it; bury 20-35 % of the
		// diameter (20 %: ~half buried); ~15 % propped, the far end on something 0.3-0.9 m up.
		const double Lift = FMath::Max(0.0, Zm - 0.5 * (Z0 + Z1));
		const double Bury = (U1 < 0.2 ? 0.45 + 0.1 * U2 : 0.2 + 0.15 * U2) * DiaM;
		const double Prop = U1 > 0.85 ? 0.3 + 0.6 * U2 : 0.0;
		Zfoot = Z0 + Lift - Bury;
		const double Zend = Z1 + Lift - Bury + Prop;
		Fwd = FVector(Dx * LenM, -Dy * LenM, Zend - Zfoot).GetSafeNormal();
		// Up follows the ground across the log (its normal at the middle), so a log on a side
		// slope rolls with it instead of hanging one flank in the air.
		double Ze = Zm, Zw = Zm, Zn = Zm, Zs = Zm;
		Terrain->SurfaceAt(Mx + 1.0, My, Ze);
		Terrain->SurfaceAt(Mx - 1.0, My, Zw);
		Terrain->SurfaceAt(Mx, My + 1.0, Zn);
		Terrain->SurfaceAt(Mx, My - 1.0, Zs);
		Up = FVector(-0.5 * (Ze - Zw), 0.5 * (Zn - Zs), 1.0).GetSafeNormal();   // world (-gx, -gy, 1) in UE axes
	}
	const FQuat Q = FRotationMatrix::MakeFromXZ(Fwd, Up).ToQuat();
	Out = FTransform(Q, Terrain->WorldToUE(In.x, In.y, Zfoot), FVector(S));
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
	Cell.BuiltGeneration = Terrain->GetTileGeneration();
	Cell.BuiltVegGeneration = VegGeneration();
	TArray<TArray<FTransform>> PerMesh;
	TArray<TArray<float>> PerMeshCanopy;   // M_Veg custom data 0: canopy over the instance (sky occlusion)
	PerMesh.SetNum(Meshes.Num());
	PerMeshCanopy.SetNum(Meshes.Num());
	for (const emberworld::CoverInstance& In : Out)
	{
		const int32 Mi = FirstMesh[In.item] + In.variant;
		const FBoxSphereBounds B = Meshes[Mi]->GetBounds();
		const double MeshH = FMath::Max(1.0, 2.0 * B.BoxExtent.Z);
		const double S = In.height_m * 100.0 / MeshH;
		// Canopy over the instance = the litter weight of the ground mix (canopy_to_litter), as
		// M_Terrain uses it: the trees are not in the distance-field scene, so Lumen barely shades
		// what stands under them from the sky - ferns and rocks glowed cyan under the canopy.
		float Mx[4] = {0, 0, 0, 0};
		const float Canopy = Terrain->GroundMixAt(In.x, In.y, Mx, nullptr) ? FMath::Clamp(Mx[0], 0.f, 1.f) : 0.f;
		const emberworld::CoverPose Pose = Rules.items[In.item].pose;
		if (Pose != emberworld::CoverPose::Upright)
		{
			FTransform Xf;
			bool bPend = false;
			const bool bPlaced = PlaceLying(In, B, S, Pose, Xf, bPend);
			bMissing |= bPend;
			if (bPlaced)
			{
				PerMesh[Mi].Add(Xf);
				PerMeshCanopy[Mi].Add(Canopy);
			}
			continue;
		}
		// Sit on the RENDERED ground at the lowest point under the footprint: on a slope the
		// downhill edge touches and the uphill side is buried (a rock was seen floating when z
		// came from the nearest DEM corner).
		const double Rm = FMath::Max(B.BoxExtent.X, B.BoxExtent.Y) * S / 100.0 * 0.7;
		double Z = 0.0;
		if (!Terrain->SurfaceAt(In.x, In.y, Z))
		{
			continue;
		}
		for (const FVector2D& O : {FVector2D(Rm, 0), FVector2D(-Rm, 0), FVector2D(0, Rm), FVector2D(0, -Rm)})
		{
			double Zo = 0.0;
			if (Terrain->SurfaceAt(In.x + O.X, In.y + O.Y, Zo))
			{
				Z = FMath::Min(Z, Zo);
			}
		}
		FVector Loc = Terrain->WorldToUE(In.x, In.y, Z);
		Loc.Z -= (B.Origin.Z - B.BoxExtent.Z) * S + 0.06 * B.BoxExtent.Z * 2.0 * S;  // ~6 % sunk
		PerMesh[Mi].Add(FTransform(FRotator(0.0, FMath::RadiansToDegrees(In.yaw_rad), 0.0), Loc, FVector(S)));
		PerMeshCanopy[Mi].Add(Canopy);
	}
	// pending: a coarse-LOD mix, or trees not streamed in for a hung-up tree's host
	Cell.bPending = bMissing && ++Cell.Attempts < 120;
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
		// WPO (wind + the burned-plant collapse, M_Veg Consume) must reach as far as cover is
		// drawn: with WPO cut at RadiusM but cells drawn to RadiusM + CellM, burned cover stood
		// full and green in a ring around the camera (Brad, S_jolly_play).
		C->SetWorldPositionOffsetDisableDistance(static_cast<int32>((RadiusM + 2.0 * CellM) * 100.0));
		C->SetCullDistances(0, static_cast<int32>((RadiusM + CellM) * 100.0));
		C->bAffectDistanceFieldLighting = false;
		C->NumCustomDataFloats = 1;
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(PerMesh[Mi], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		for (int32 I = 0; I < PerMeshCanopy[Mi].Num(); ++I)
		{
			C->SetCustomDataValue(I, 0, PerMeshCanopy[Mi][I], false);
		}
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
		Queue.Remove(K);
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
				Cells.Add(K);
				Queue.AddUnique(K);
			}
			else if (C->bPending && (C->BuiltGeneration != Terrain->GetTileGeneration()
				|| C->BuiltVegGeneration != VegGeneration()))
			{
				// New terrain or vegetation tiles since it was built: try again. Vegetation counts
				// too - a hung-up tree waiting for its host only resolves when the trees arrive, and
				// retrying on terrain alone left it to streaming order (a trunk came and went between
				// runs of S_ground_tq/trunk_slope).
				Queue.AddUnique(K);
			}
		}
	}
}

int32 AEmberGroundCoverActor::VegGeneration() const
{
	return Vegetation ? Vegetation->GetTileGeneration() : 0;
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
	const bool bNewTiles = Terrain->GetTileGeneration() != LastGeneration || VegGeneration() != LastVegGeneration;
	const bool bPending = bNewTiles;
	if (bNewTiles || FVector::Dist(Cam, LastCamera) > 400.0)  // re-select every 4 m moved
	{
		LastGeneration = Terrain->GetTileGeneration();
		LastVegGeneration = VegGeneration();
		LastCamera = Cam;
		const double T0 = FPlatformTime::Seconds();
		UpdateCells(Cam);
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (Ms > 5.0)
		{
			UE_LOG(LogEmberCover, Display, TEXT("stream-cost cover %.1f ms (%d cell(s), pending %d)"), Ms, Cells.Num(), bPending ? 1 : 0);
		}
	}
	BuildQueued(Cam);
}

void AEmberGroundCoverActor::BuildQueued(const FVector& CamUE)
{
	const double T0 = FPlatformTime::Seconds();
	const emberworld::Frame& F = Terrain->GetFrame();
	const double Cx = F.anchor_x + CamUE.X / 100.0, Cy = F.anchor_y - CamUE.Y / 100.0;
	int32 Built = 0;
	while (Queue.Num() && (bSyncStreaming || Built == 0 || (FPlatformTime::Seconds() - T0) * 1000.0 < BuildBudgetMs))
	{
		int32 Best = 0;
		double BestD = TNumericLimits<double>::Max();
		for (int32 i = 0; i < Queue.Num(); ++i)
		{
			const double D = FMath::Square((Queue[i].X + 0.5) * CellM - Cx) + FMath::Square((Queue[i].Y + 0.5) * CellM - Cy);
			if (D < BestD)
			{
				BestD = D;
				Best = i;
			}
		}
		const FIntPoint K = Queue[Best];
		Queue.RemoveAtSwap(Best);
		if (FCoverCell* C = Cells.Find(K))
		{
			BuildCell(K, *C);
			++Built;
		}
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
