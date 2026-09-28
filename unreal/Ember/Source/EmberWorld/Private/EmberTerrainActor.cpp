#include "EmberTerrainActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/lod.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogEmberWorld, Log, All);

namespace
{
uint64 TileKey(const emberworld::TileEntry& T)
{
	return (uint64(uint32(T.lod)) << 48) | (uint64(uint32(T.x) & 0xFFFFFF) << 24) | uint64(uint32(T.y) & 0xFFFFFF);
}
}  // namespace

AEmberTerrainActor::AEmberTerrainActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Terrain"));
	Mesh->bUseAsyncCooking = true;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	RootComponent = Mesh;
}

void AEmberTerrainActor::SetBaseColor(const FLinearColor& Color)
{
	if (!Material)
	{
		// Generated clay master (assets/generators/m_ember_gray.py); the engine's
		// BasicShapeMaterial is only a fallback for a checkout that has not run regen-assets.
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(
			nullptr, TEXT("/Game/Ember/Generated/M_EmberGray.M_EmberGray"));
		if (!Base)
		{
			UE_LOG(LogEmberWorld, Warning, TEXT("M_EmberGray missing (run ember-dev regen-assets); using BasicShapeMaterial"));
			Base = LoadObject<UMaterialInterface>(
				nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
		}
		if (!Base)
		{
			UE_LOG(LogEmberWorld, Warning, TEXT("no terrain material found; using the default material"));
			return;
		}
		Material = UMaterialInstanceDynamic::Create(Base, this);
		for (int32 i = 0; i < Mesh->GetNumSections(); ++i)
		{
			Mesh->SetMaterial(i, Material);
		}
	}
	Material->SetVectorParameterValue(TEXT("Color"), Color);
}

bool AEmberTerrainActor::LoadRegion(const FString& RegionDir, int32 FixedLod, FString& OutError)
{
	const double T0 = FPlatformTime::Seconds();
	emberworld::RegionResult R = emberworld::load_region(TCHAR_TO_UTF8(*RegionDir));
	if (!R)
	{
		OutError = UTF8_TO_TCHAR(R.error.c_str());
		return false;
	}
	Region = MakeUnique<emberworld::Region>(std::move(*R.region));
	Frame = emberworld::region_frame(*Region);
	RegionName = UTF8_TO_TCHAR(Region->name.c_str());
	TilesTotal = Region->tiles.size();
	std::string Err;
	if (!emberworld::data_extent(*Region, DataExtent, Err))
	{
		UE_LOG(LogEmberWorld, Warning, TEXT("data extent: %s"), UTF8_TO_TCHAR(Err.c_str()));
	}

	Mesh->ClearAllMeshSections();
	Loaded.Reset();
	FreeSections.Reset();
	NextSection = 0;
	LoadMs = 0.0;

	bStreaming = FixedLod < 0;
	SetActorTickEnabled(bStreaming);
	if (!bStreaming)
	{
		for (const emberworld::TileEntry* Tile : Region->tiles_at(FixedLod))
		{
			if (!LoadTile(*Tile, OutError))
			{
				return false;
			}
		}
	}
	RecomputeStats();
	LoadMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	UE_LOG(LogEmberWorld, Log, TEXT("Loaded region %s (%s): %d tiles, %lld triangles (+%lld skirt), %.1f ms"),
		*RegionName, bStreaming ? TEXT("streaming") : *FString::Printf(TEXT("fixed lod %d"), FixedLod),
		TilesLoaded, Triangles, SkirtTriangles, LoadMs);
	return true;
}

bool AEmberTerrainActor::LoadTile(const emberworld::TileEntry& Tile, FString& OutError)
{
	emberworld::MeshResult M = emberworld::load_tile_mesh(*Region, Tile, Frame);
	if (!M.ok())
	{
		OutError = FString::Printf(TEXT("tile z%d/x%d/y%d: %s"), Tile.lod, Tile.x, Tile.y, UTF8_TO_TCHAR(M.error.c_str()));
		return false;
	}
	const emberworld::TileMesh& TM = M.mesh;
	TArray<FVector> Verts;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0, UV1, Empty;
	TArray<int32> Tris;
	Verts.Reserve(TM.positions.size());
	Normals.Reserve(TM.normals.size());
	UV0.Reserve(TM.uv0.size());
	UV1.Reserve(TM.uv1.size());
	for (size_t i = 0; i < TM.positions.size(); ++i)
	{
		Verts.Emplace(TM.positions[i].x, TM.positions[i].y, TM.positions[i].z);
		Normals.Emplace(TM.normals[i].x, TM.normals[i].y, TM.normals[i].z);
		UV0.Emplace(TM.uv0[i].u, TM.uv0[i].v);
		UV1.Emplace(TM.uv1[i].u, TM.uv1[i].v);
	}
	Tris.Reserve(TM.indices.size());
	for (uint32 Idx : TM.indices)
	{
		Tris.Add(static_cast<int32>(Idx));
	}
	const int32 Section = FreeSections.Num() ? FreeSections.Pop() : NextSection++;
	Mesh->CreateMeshSection(Section, Verts, Tris, Normals, UV0, UV1, Empty, Empty,
		TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (Material)
	{
		Mesh->SetMaterial(Section, Material);
	}
	FLoadedTile& L = Loaded.Add(TileKey(Tile));
	L.Section = Section;
	L.Lod = Tile.lod;
	L.Triangles = TM.surface_triangles;
	L.SkirtTriangles = TM.skirt_triangles;
	L.NodataCorners = TM.nodata_corners;
	return true;
}

void AEmberTerrainActor::UnloadTile(uint64 Key)
{
	if (const FLoadedTile* L = Loaded.Find(Key))
	{
		Mesh->ClearMeshSection(L->Section);
		FreeSections.Add(L->Section);
		Loaded.Remove(Key);
	}
}

void AEmberTerrainActor::RecomputeStats()
{
	TilesLoaded = Loaded.Num();
	Triangles = SkirtTriangles = NodataCorners = 0;
	LodHistogram.Reset();
	for (const auto& KV : Loaded)
	{
		Triangles += KV.Value.Triangles;
		SkirtTriangles += KV.Value.SkirtTriangles;
		NodataCorners += KV.Value.NodataCorners;
		LodHistogram.FindOrAdd(KV.Value.Lod)++;
	}
}

int32 AEmberTerrainActor::UpdateStreaming(const FVector& CameraUE)
{
	if (!Region.IsValid())
	{
		return 0;
	}
	const double T0 = FPlatformTime::Seconds();
	const double Wx = Frame.anchor_x + CameraUE.X / 100.0;
	const double Wy = Frame.anchor_y - CameraUE.Y / 100.0;
	const double Wz = Frame.anchor_z + CameraUE.Z / 100.0;
	emberworld::LodOptions Opt;
	Opt.refine_factor = RefineFactor;
	const std::vector<const emberworld::TileEntry*> Want = emberworld::select_tiles(*Region, Wx, Wy, Wz, Opt);

	TSet<uint64> WantKeys;
	for (const emberworld::TileEntry* T : Want)
	{
		WantKeys.Add(TileKey(*T));
	}
	TArray<uint64> Drop;
	for (const auto& KV : Loaded)
	{
		if (!WantKeys.Contains(KV.Key))
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
	for (const emberworld::TileEntry* T : Want)
	{
		if (!Loaded.Contains(TileKey(*T)))
		{
			if (!LoadTile(*T, Err))
			{
				UE_LOG(LogEmberWorld, Error, TEXT("%s"), *Err);
				continue;
			}
			++Changes;
		}
	}
	if (Changes)
	{
		RecomputeStats();
		LastStreamMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		++StreamUpdates;
	}
	return Changes;
}

void AEmberTerrainActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bStreaming)
	{
		return;
	}
	const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}
	const FVector Cam = PC->PlayerCameraManager->GetCameraLocation();
	if (StreamUpdates == 0 || FVector::Dist(Cam, LastStreamCamera) > 100.0)  // re-select every metre moved
	{
		LastStreamCamera = Cam;
		UpdateStreaming(Cam);
		if (StreamUpdates == 0)
		{
			++StreamUpdates;  // count the first selection even if it changed nothing
		}
	}
}

bool AEmberTerrainActor::GroundHeightAt(double WorldX, double WorldY, double& OutZ) const
{
	return Region.IsValid() && emberworld::sample_height(*Region, WorldX, WorldY, OutZ);
}

FVector AEmberTerrainActor::WorldToUE(double X, double Y, double Z) const
{
	return FVector((X - Frame.anchor_x) * 100.0, (Frame.anchor_y - Y) * 100.0, (Z - Frame.anchor_z) * 100.0);
}
