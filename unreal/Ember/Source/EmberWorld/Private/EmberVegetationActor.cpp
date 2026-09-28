#include "EmberVegetationActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
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

// Placeholder colours per species key (sRGB); B3 replaces meshes and materials.
FLinearColor SpeciesColor(const FString& Key, bool bConifer)
{
	if (Key.Contains(TEXT("pseudotsuga"))) return FLinearColor::FromSRGBColor(FColor(0x2E, 0x3B, 0x26));
	if (Key.Contains(TEXT("pinus"))) return FLinearColor::FromSRGBColor(FColor(0x3A, 0x44, 0x28));
	if (Key.Contains(TEXT("abies"))) return FLinearColor::FromSRGBColor(FColor(0x26, 0x33, 0x1F));
	if (Key.Contains(TEXT("grass"))) return FLinearColor::FromSRGBColor(FColor(0x9A, 0x8F, 0x5E));
	return bConifer ? FLinearColor::FromSRGBColor(FColor(0x2E, 0x3B, 0x26)) : FLinearColor::FromSRGBColor(FColor(0x6B, 0x6A, 0x45));
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

	UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_EmberGray.M_EmberGray"));
	if (!Cone || !Sphere || !Base)
	{
		OutError = TEXT("placeholder vegetation assets missing (engine Cone/Sphere or M_EmberGray)");
		return false;
	}
	SpeciesKeys.Reset();
	SpeciesMesh.Reset();
	SpeciesMaterial.Reset();
	for (const emberworld::scatter::Group& G : Palette.groups)
	{
		const bool bConifer = G.name == "conifer_forest";
		for (const emberworld::scatter::Species& S : G.species)
		{
			const FString Key = UTF8_TO_TCHAR(S.key.c_str());
			SpeciesKeys.Add(Key);
			SpeciesMesh.Add(bConifer ? Cone : Sphere);
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
			MID->SetVectorParameterValue(TEXT("Color"), SpeciesColor(Key, bConifer));
			SpeciesMaterial.Add(MID);
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
		if (In.species < 0 || In.species >= NS)
		{
			continue;
		}
		double Z = In.z;
		if (!bSurface || !Surface.height_at(In.x, In.y, Z))
		{
			Z = In.z;
			++UngroundedInstances;
		}
		const UStaticMesh* M = SpeciesMesh[In.species];
		const FBoxSphereBounds B = M->GetBounds();
		const double MeshH = FMath::Max(1.0, 2.0 * B.BoxExtent.Z);
		const double MeshW = FMath::Max(1.0, 2.0 * B.BoxExtent.X);
		const double HeightCm = In.height_m * 100.0;
		const double CrownCm = 2.0 * In.radius_m * 100.0 * FMath::Clamp(In.scale, 0.5, 1.5);
		const FVector Scale(CrownCm / MeshW, CrownCm / MeshW, HeightCm / MeshH);
		FVector Loc = Terrain->WorldToUE(In.x, In.y, Z);
		Loc.Z -= (B.Origin.Z - B.BoxExtent.Z) * Scale.Z;  // mesh bottom on the ground
		PerSpecies[In.species].Emplace(FRotator(0.0, FMath::RadiansToDegrees(In.yaw_rad), 0.0), Loc, Scale);
	}

	FVegTile& VT = Tiles.Add(VegKey(Tile));
	VT.PerSpecies.Init(0, NS);
	for (int32 S = 0; S < NS; ++S)
	{
		if (PerSpecies[S].Num() == 0)
		{
			continue;
		}
		UHierarchicalInstancedStaticMeshComponent* C = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
		C->SetStaticMesh(SpeciesMesh[S]);
		C->SetMaterial(0, SpeciesMaterial[S]);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(true);
		C->SetMobility(EComponentMobility::Movable);
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(PerSpecies[S], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		VT.Components.Add(C);
		VT.PerSpecies[S] = PerSpecies[S].Num();
	}
	ScatterMs += (FPlatformTime::Seconds() - T0) * 1000.0;
	return true;
}

void AEmberVegetationActor::UnloadTile(uint64 Key)
{
	if (FVegTile* VT = Tiles.Find(Key))
	{
		for (UHierarchicalInstancedStaticMeshComponent* C : VT->Components)
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
	if (!bInitialised || !PC || !PC->PlayerCameraManager)
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
