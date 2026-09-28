#include "EmberVegetationActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "EmberTerrainActor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/heightfield.h"
#include "emberworld/tiff.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogEmberVeg, Log, All);

namespace
{
uint64 VegKey(const emberworld::TileEntry& T)
{
	return (uint64(uint32(T.x) & 0xFFFFFF) << 24) | uint64(uint32(T.y) & 0xFFFFFF);
}

// Broadleaf / shrub / grass keys (fallback mesh: sphere; everything else is a conifer: cone).
bool IsBroadleaf(const FString& Key)
{
	for (const TCHAR* B : {TEXT("alnus"), TEXT("populus"), TEXT("acer"), TEXT("artemisia"), TEXT("grass")})
	{
		if (Key.Contains(B)) return true;
	}
	return false;
}

// Placeholder colours per species key (sRGB); B3 replaces meshes and materials.
FLinearColor SpeciesColor(const FString& Key)
{
	if (Key.Contains(TEXT("pseudotsuga"))) return FLinearColor::FromSRGBColor(FColor(0x3F, 0x52, 0x34));
	if (Key.Contains(TEXT("pinus"))) return FLinearColor::FromSRGBColor(FColor(0x56, 0x64, 0x3A));
	if (Key.Contains(TEXT("abies"))) return FLinearColor::FromSRGBColor(FColor(0x34, 0x48, 0x2D));
	if (Key.Contains(TEXT("tsuga"))) return FLinearColor::FromSRGBColor(FColor(0x38, 0x4E, 0x33));
	if (Key.Contains(TEXT("thuja"))) return FLinearColor::FromSRGBColor(FColor(0x46, 0x57, 0x2E));
	if (Key.Contains(TEXT("grass"))) return FLinearColor::FromSRGBColor(FColor(0xB3, 0xA5, 0x71));
	return IsBroadleaf(Key) ? FLinearColor::FromSRGBColor(FColor(0x5C, 0x6E, 0x34)) : FLinearColor::FromSRGBColor(FColor(0x3F, 0x52, 0x34));
}
}  // namespace

AEmberVegetationActor::AEmberVegetationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberVegetationActor::Init(AEmberTerrainActor* InTerrain, FString& OutError)
{
	Terrain = InTerrain;
	const emberworld::Region* R = Terrain ? Terrain->GetRegion() : nullptr;
	if (!R)
	{
		OutError = TEXT("vegetation needs a loaded terrain region");
		return false;
	}
	emberworld::ScatterInputResult SI = emberworld::load_scatter_input(*R);
	if (!SI.ok())
	{
		OutError = UTF8_TO_TCHAR(SI.error.c_str());
		return false;
	}
	Input = SI.input;
	emberworld::scatter::PaletteResult PR = emberworld::scatter::load_palette(emberworld::resolve_palette(*R, Input.palette_ref));
	if (!PR.ok())
	{
		OutError = UTF8_TO_TCHAR(PR.error.c_str());
		return false;
	}
	Palette = std::move(PR.palette);

	// Generated species meshes (assets/generators/veg_species.py, keyed by palette key) with
	// M_Veg; engine Cone/Sphere + clay are the fallback for a key with no generated mesh.
	UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UMaterialInterface* Clay = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_EmberGray.M_EmberGray"));
	UMaterialInterface* VegMaster = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Veg.M_Veg"));
	SpeciesKeys.Reset();
	SpeciesMesh.Reset();
	SpeciesMaterial.Reset();
	IndexToSlot.Reset();
	SlotHeightLineupM.Reset();
	SlotCrownRatio.Reset();
	GeneratedSpecies = 0;
	// A key can appear in several palette groups (one mix per forest type): one slot per key.
	for (const emberworld::scatter::Group& G : Palette.groups)
	{
		for (const emberworld::scatter::Species& S : G.species)
		{
			const FString Key = UTF8_TO_TCHAR(S.key.c_str());
			int32 Slot = SpeciesKeys.IndexOfByKey(Key);
			if (Slot == INDEX_NONE)
			{
				Slot = SpeciesKeys.Add(Key);
				SlotHeightLineupM.Add(FMath::Max(S.height_min_m, 0.55 * S.height_max_m));  // a mature tree
				SlotCrownRatio.Add(S.crown_ratio);
				UStaticMesh* Gen = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Ember/Generated/Veg/SM_%s.SM_%s"), *Key, *Key));
				UMaterialInterface* Base = (Gen && VegMaster) ? VegMaster : Clay;
				UStaticMesh* Mesh = Gen ? Gen : (IsBroadleaf(Key) ? Sphere : Cone);
				if (!Mesh || !Base)
				{
					OutError = TEXT("no mesh/material for species ") + Key;
					return false;
				}
				GeneratedSpecies += Gen ? 1 : 0;
				SpeciesMesh.Add(Mesh);
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
				MID->SetVectorParameterValue(TEXT("Color"), SpeciesColor(Key));
				SpeciesMaterial.Add(MID);
			}
			IndexToSlot.Add(Slot);
		}
	}
	bInitialised = true;
	return true;
}

bool AEmberVegetationActor::LoadTile(const emberworld::TileEntry& Tile, FString& OutError)
{
	const double T0 = FPlatformTime::Seconds();
	const emberworld::Region& R = *Terrain->GetRegion();
	emberworld::TileScatterResult TS = emberworld::scatter_tile(R, Tile, Palette, Input);
	if (!TS.ok())
	{
		OutError = UTF8_TO_TCHAR(TS.error.c_str());
		return false;
	}
	emberworld::SurfaceSampler Surface;
	emberworld::TiffResult H = emberworld::read_tiff(R.path(Tile.height_tif));
	const bool bSurface = H && Surface.build(R, Tile, *H.raster);

	const int32 NS = SpeciesKeys.Num();
	TArray<TArray<FTransform>> PerSpecies;
	PerSpecies.SetNum(NS);
	for (const emberworld::scatter::Instance& In : TS.instances)
	{
		if (In.species < 0 || In.species >= IndexToSlot.Num())
		{
			continue;
		}
		const int32 Slot = IndexToSlot[In.species];
		double Z = In.z;
		if (!bSurface)
		{
			++UngroundedInstances;  // the tile's surface failed to build: Terrain's z (an error)
		}
		else if (!Surface.height_at(In.x, In.y, Z))
		{
			// Render policy: nothing to stand on (Terrain scatters over fuels outside the DEM's AOI
			// mask with z = 0 - upstream U8). Dropped, counted, never drawn underground.
			++NoSurfaceInstances;
			continue;
		}
		PerSpecies[Slot].Add(FitInstance(Slot, In.height_m, In.radius_m, In.yaw_rad, Terrain->WorldToUE(In.x, In.y, Z)));
	}

	FVegTile& VT = Tiles.Add(VegKey(Tile));
	VT.PerSpecies.Init(0, NS);
	for (int32 S = 0; S < NS; ++S)
	{
		if (PerSpecies[S].Num() == 0)
		{
			continue;
		}
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
		C->SetStaticMesh(SpeciesMesh[S]);
		C->SetMaterial(0, SpeciesMaterial[S]);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(true);
		C->SetMobility(EComponentMobility::Movable);
		C->InstancingRandomSeed = static_cast<int32>((VegKey(Tile) * 31 + S) & 0x7FFFFFFF) | 1;  // wind phase: deterministic
		// Wind WPO moves the trees: without this the virtual shadow map cache keeps stale pages
		// and crowns get saw-toothed self-shadows (seen in S_forest_teanaway ground_ridge).
		C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
		// Trees stay out of the distance-field scene: ~1.4 M DF objects cost ~0.9 GB VRAM (4.23 -> 3.32 GB
		// at the S_tq_forest perf pose, D8 budget 4 GB). Lumen still sees them via screen traces.
		C->bAffectDistanceFieldLighting = false;
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(PerSpecies[S], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		VT.Components.Add(C);
		VT.PerSpecies[S] = PerSpecies[S].Num();
	}
	ScatterMs += (FPlatformTime::Seconds() - T0) * 1000.0;
	return true;
}

FTransform AEmberVegetationActor::FitInstance(int32 Slot, double HeightM, double CrownRadiusM, double YawRad, FVector Loc) const
{
	// Terrain's scatter v2 sizes every crown (0.5 x crown_ratio x height x 0.85..1.15): the mesh's
	// bounds are fitted to that height and crown diameter, no render-side crown policy.
	const FBoxSphereBounds B = SpeciesMesh[Slot]->GetBounds();
	const double MeshH = FMath::Max(1.0, 2.0 * B.BoxExtent.Z);
	const double MeshW = FMath::Max(1.0, 2.0 * B.BoxExtent.X);
	const double HeightCm = HeightM * 100.0;
	const double CrownCm = FMath::Max(1.0, 2.0 * CrownRadiusM * 100.0);
	const FVector S(CrownCm / MeshW, CrownCm / MeshW, HeightCm / MeshH);
	Loc.Z -= (B.Origin.Z - B.BoxExtent.Z) * S.Z;  // mesh bottom on the ground
	return FTransform(FRotator(0.0, FMath::RadiansToDegrees(YawRad), 0.0), Loc, S);
}

void AEmberVegetationActor::SetWind(double StrengthCm, double FromDeg)
{
	// Trees lean downwind. Compass heading h -> UE (X east, Y south): (sin h, -cos h).
	const double To = FMath::DegreesToRadians(FromDeg + 180.0);
	const FLinearColor Dir(FMath::Sin(To), -FMath::Cos(To), 0.0, 0.0);
	for (UMaterialInstanceDynamic* M : SpeciesMaterial)
	{
		M->SetScalarParameterValue(TEXT("WindStrength"), static_cast<float>(StrengthCm));
		M->SetVectorParameterValue(TEXT("WindDir"), Dir);
	}
}

void AEmberVegetationActor::SetWindTime(double Seconds)
{
	for (UMaterialInstanceDynamic* M : SpeciesMaterial)
	{
		M->SetScalarParameterValue(TEXT("WindTime"), static_cast<float>(Seconds));
	}
}

void AEmberVegetationActor::SpawnLineup(double WorldX, double WorldY, double RowDeg)
{
	bLineup = true;
	for (const auto& KV : Tiles)
	{
		for (UInstancedStaticMeshComponent* C : KV.Value.Components)
		{
			if (C) C->DestroyComponent();
		}
	}
	Tiles.Reset();
	const double Rh = FMath::DegreesToRadians(RowDeg);
	const FVector2D Dir(FMath::Sin(Rh), FMath::Cos(Rh));  // world metres, x east / y north
	struct FItem { int32 Species; double HeightM; double WidthM; };
	TArray<FItem> Items;
	for (int32 Slot = 0; Slot < SpeciesKeys.Num(); ++Slot)
	{
		const double H = SlotHeightLineupM[Slot];
		Items.Add({Slot, H, H * SlotCrownRatio[Slot]});
	}
	Items.Add({-1, 1.8, 0.4});  // the post
	const double Gap = 4.0;
	double Total = -Gap;
	for (const FItem& I : Items) Total += I.WidthM + Gap;
	double At = -0.5 * Total;
	UMaterialInterface* Clay = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_EmberGray.M_EmberGray"));
	for (const FItem& I : Items)
	{
		const double Off = At + 0.5 * I.WidthM;
		At += I.WidthM + Gap;
		const double Wx = WorldX + Dir.X * Off, Wy = WorldY + Dir.Y * Off;
		double Wz = 0.0;
		Terrain->GroundHeightAt(Wx, Wy, Wz);
		const FVector Loc = Terrain->WorldToUE(Wx, Wy, Wz);
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		if (I.Species >= 0)
		{
			C->SetStaticMesh(SpeciesMesh[I.Species]);
			C->SetMaterial(0, SpeciesMaterial[I.Species]);
			C->SetWorldTransform(FitInstance(I.Species, I.HeightM, 0.5 * I.WidthM, 0.0, Loc));
		}
		else
		{
			C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
			C->SetMaterial(0, Clay);
			C->SetWorldTransform(FTransform(FRotator::ZeroRotator, Loc + FVector(0, 0, 90.0), FVector(0.4, 0.4, 1.8)));
		}
		C->SetMobility(EComponentMobility::Movable);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		LineupParts.Add(C);
	}
	InstancesTotal = Items.Num() - 1;
	UE_LOG(LogEmberVeg, Log, TEXT("vegetation lineup: %d species + post"), Items.Num() - 1);
}

void AEmberVegetationActor::UnloadTile(uint64 Key)
{
	if (FVegTile* VT = Tiles.Find(Key))
	{
		for (UInstancedStaticMeshComponent* C : VT->Components)
		{
			if (C)
			{
				C->DestroyComponent();
			}
		}
		Tiles.Remove(Key);
	}
}

void AEmberVegetationActor::RecomputeStats()
{
	InstancesTotal = 0;
	TilesLoaded = Tiles.Num();
	BySpecies.Reset();
	for (const auto& KV : Tiles)
	{
		for (int32 S = 0; S < KV.Value.PerSpecies.Num(); ++S)
		{
			InstancesTotal += KV.Value.PerSpecies[S];
			BySpecies.FindOrAdd(SpeciesKeys[S]) += KV.Value.PerSpecies[S];
		}
	}
}

int32 AEmberVegetationActor::UpdateStreaming(const FVector& CameraUE)
{
	if (!bInitialised)
	{
		return 0;
	}
	const emberworld::Region& R = *Terrain->GetRegion();
	const emberworld::Frame& F = Terrain->GetFrame();
	const double Wx = F.anchor_x + CameraUE.X / 100.0;
	const double Wy = F.anchor_y - CameraUE.Y / 100.0;
	TSet<uint64> Want;
	TArray<const emberworld::TileEntry*> WantTiles;
	for (const emberworld::TileEntry* T : R.tiles_at(R.finest_lod()))
	{
		const double Dx = FMath::Max3(T->content.min_x - Wx, 0.0, Wx - T->content.max_x);
		const double Dy = FMath::Max3(T->content.min_y - Wy, 0.0, Wy - T->content.max_y);
		if (FMath::Sqrt(Dx * Dx + Dy * Dy) <= RadiusM)
		{
			Want.Add(VegKey(*T));
			WantTiles.Add(T);
		}
	}
	TArray<uint64> Drop;
	for (const auto& KV : Tiles)
	{
		if (!Want.Contains(KV.Key))
		{
			Drop.Add(KV.Key);
		}
	}
	for (uint64 K : Drop)
	{
		UnloadTile(K);
	}
	int32 Changes = Drop.Num();
	FString Err;
	for (const emberworld::TileEntry* T : WantTiles)
	{
		if (!Tiles.Contains(VegKey(*T)))
		{
			if (!LoadTile(*T, Err))
			{
				UE_LOG(LogEmberVeg, Error, TEXT("veg tile z%d/x%d/y%d: %s"), T->lod, T->x, T->y, *Err);
				continue;
			}
			++Changes;
		}
	}
	if (Changes)
	{
		RecomputeStats();
		UE_LOG(LogEmberVeg, Log, TEXT("vegetation: %d tiles, %lld instances (%.0f ms scatter total)"), TilesLoaded, InstancesTotal, ScatterMs);
	}
	return Changes;
}

void AEmberVegetationActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!bInitialised || bLineup || !PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	if (bFirstUpdate || FVector::Dist(Cam, LastCamera) > 5000.0)  // re-select every 50 m moved
	{
		bFirstUpdate = false;
		LastCamera = Cam;
		UpdateStreaming(Cam);
	}
}
