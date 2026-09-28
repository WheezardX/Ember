#include "EmberTerrainActor.h"

#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogEmberWorld, Log, All);

AEmberTerrainActor::AEmberTerrainActor()
{
	PrimaryActorTick.bCanEverTick = false;
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

bool AEmberTerrainActor::LoadRegion(const FString& RegionDir, int32 Lod, FString& OutError)
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
	if (Lod < 0)
	{
		Lod = Region->finest_lod();
	}

	Mesh->ClearAllMeshSections();
	TilesTotal = Region->tiles.size();
	TilesLoaded = 0;
	Triangles = SkirtTriangles = NodataCorners = 0;
	LodHistogram.Reset();

	int32 Section = 0;
	bool bAnyValid = false;
	for (const emberworld::TileEntry* Tile : Region->tiles_at(Lod))
	{
		emberworld::MeshResult M = emberworld::load_tile_mesh(*Region, *Tile, Frame);
		if (!M.ok())
		{
			OutError = FString::Printf(TEXT("tile z%d/x%d/y%d: %s"), Tile->lod, Tile->x, Tile->y, UTF8_TO_TCHAR(M.error.c_str()));
			return false;
		}
		const emberworld::TileMesh& TM = M.mesh;
		if (TM.has_valid)
		{
			const emberworld::Bounds& V = TM.valid_bounds;
			if (!bAnyValid)
			{
				DataExtent = V;
				bAnyValid = true;
			}
			DataExtent.min_x = FMath::Min(DataExtent.min_x, V.min_x);
			DataExtent.min_y = FMath::Min(DataExtent.min_y, V.min_y);
			DataExtent.max_x = FMath::Max(DataExtent.max_x, V.max_x);
			DataExtent.max_y = FMath::Max(DataExtent.max_y, V.max_y);
		}
		TArray<FVector> Verts;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0, UV1, Empty;
		TArray<int32> Tris;
		Verts.Reserve(TM.positions.size());
		Normals.Reserve(TM.normals.size());
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
		Mesh->CreateMeshSection(Section, Verts, Tris, Normals, UV0, UV1, Empty, Empty,
			TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
		if (Material)
		{
			Mesh->SetMaterial(Section, Material);
		}
		++Section;
		++TilesLoaded;
		Triangles += TM.surface_triangles;
		SkirtTriangles += TM.skirt_triangles;
		NodataCorners += TM.nodata_corners;
		LodHistogram.FindOrAdd(Tile->lod)++;
	}
	if (!bAnyValid)
	{
		DataExtent = Region->extent();
	}
	LoadMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	UE_LOG(LogEmberWorld, Log, TEXT("Loaded region %s lod %d: %d tiles, %lld triangles (+%lld skirt), %.1f ms"),
		*RegionName, Lod, TilesLoaded, Triangles, SkirtTriangles, LoadMs);
	return true;
}

bool AEmberTerrainActor::GroundHeightAt(double WorldX, double WorldY, double& OutZ) const
{
	return Region.IsValid() && emberworld::sample_height(*Region, WorldX, WorldY, OutZ);
}

FVector AEmberTerrainActor::WorldToUE(double X, double Y, double Z) const
{
	return FVector((X - Frame.anchor_x) * 100.0, (Frame.anchor_y - Y) * 100.0, (Z - Frame.anchor_z) * 100.0);
}
