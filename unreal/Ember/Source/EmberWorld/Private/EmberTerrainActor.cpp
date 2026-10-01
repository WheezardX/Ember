#include "EmberTerrainActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/lod.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogEmberWorld, Log, All);

namespace
{
uint64 TileKeyOf(int32 Lod, int32 X, int32 Y)
{
	return (uint64(uint32(Lod)) << 48) | (uint64(uint32(X) & 0xFFFFFF) << 24) | uint64(uint32(Y) & 0xFFFFFF);
}
uint64 TileKey(const emberworld::TileEntry& T)
{
	return TileKeyOf(T.lod, T.x, T.y);
}
bool Overlaps(const emberworld::Bounds& A, const emberworld::Bounds& B)
{
	return A.min_x < B.max_x && B.min_x < A.max_x && A.min_y < B.max_y && B.min_y < A.max_y;
}
}  // namespace

AEmberTerrainActor::AEmberTerrainActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Terrain"));
	Mesh->SetMobility(EComponentMobility::Static);  // tile components attach here (static too)
	Mesh->bUseAsyncCooking = true;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(true);
	RootComponent = Mesh;
	WaterMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Water"));
	WaterMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WaterMesh->SetCastShadow(false);
	WaterMesh->SetupAttachment(Mesh);
}

void AEmberTerrainActor::SetBaseColor(const FLinearColor& Color)
{
	if (Look.IsValid())
	{
		return;  // look mode owns every section's material; clay would overwrite it
	}
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
		for (const auto& KV : Loaded)
		{
			if (KV.Value.Comp)
			{
				KV.Value.Comp->SetMaterial(0, Material);
			}
		}
	}
	Material->SetVectorParameterValue(TEXT("Color"), Color);
}

bool AEmberTerrainActor::SetWaterDir(const FString& Dir, FString& OutError)
{
	WaterMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Water.M_Water"));
	if (!WaterMaterial)
	{
		OutError = TEXT("M_Water missing: run ember-dev regen-assets");
		return false;
	}
	WaterDir = Dir;
	return true;
}

bool AEmberTerrainActor::SetLook(const FString& LookPath, FString& OutError)
{
	emberworld::LookResult L = emberworld::load_look(TCHAR_TO_UTF8(*LookPath));
	if (!L.ok())
	{
		OutError = UTF8_TO_TCHAR(L.error.c_str());
		return false;
	}
	TerrainMaster = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Terrain.M_Terrain"));
	if (!TerrainMaster)
	{
		OutError = TEXT("M_Terrain missing: run ember-dev regen-assets");
		return false;
	}
	Look = MakeUnique<emberworld::TerrainLook>(std::move(L.look));
	LookName = UTF8_TO_TCHAR(Look->name.c_str());
	return true;
}

// A transient texture with a full CPU mip chain (NeverStream: a single-mip transient texture
// sampled at distance returned black - found building HCP1).
static UTexture2D* MakeMippedTexture(const std::vector<emberworld::Albedo>& Mips, EPixelFormat Format, bool bSRGB)
{
	UTexture2D* Tex = UTexture2D::CreateTransient(Mips[0].width, Mips[0].height, Format);
	Tex->SRGB = bSRGB;
	Tex->Filter = TF_Trilinear;
	Tex->AddressX = TA_Clamp;
	Tex->AddressY = TA_Clamp;
	Tex->NeverStream = true;
	FTexturePlatformData* PD = Tex->GetPlatformData();
	for (size_t Level = 0; Level < Mips.size(); ++Level)
	{
		const emberworld::Albedo& M = Mips[Level];
		if (Level > 0)
		{
			PD->Mips.Add(new FTexture2DMipMap(M.width, M.height));
		}
		FTexture2DMipMap& Mip = PD->Mips[Level];
		void* Dst = Mip.BulkData.Lock(LOCK_READ_WRITE);
		if (Level > 0)
		{
			Dst = Mip.BulkData.Realloc(M.bgra.size());
		}
		FMemory::Memcpy(Dst, M.bgra.data(), M.bgra.size());
		Mip.BulkData.Unlock();
	}
	Tex->UpdateResource();
	return Tex;
}

struct AEmberTerrainActor::FPreparedTile
{
	const emberworld::TileEntry* Tile = nullptr;
	uint64 Key = 0;
	emberworld::MeshResult Mesh;
	bool bWater = false;
	emberworld::MeshResult WaterMesh;
	bool bSurface = false;
	emberworld::SurfaceSampler Surface;
	bool bLook = false;
	std::string LookError;
	emberworld::Albedo Albedo;                  // level 0 (+ mix); mips below
	std::vector<emberworld::Albedo> Mips, MixMips;
	int32 LW = 0, LH = 0;
	TArray<uint8> Ladder;                       // ladder-fuel weights, native res
	double ComposeMs = 0.0;
};

AEmberTerrainActor::FPreparedPtr AEmberTerrainActor::PrepareTile(const emberworld::TileEntry& Tile) const
{
	// Worker thread: only worldcore (pure) and file reads. Region, Look, Frame and WaterDir are
	// fixed once the region is loaded.
	FPreparedPtr P = MakeShared<FPreparedTile, ESPMode::ThreadSafe>();
	P->Tile = &Tile;
	P->Key = TileKey(Tile);
	emberworld::MeshOptions Opt;
	TOptional<emberworld::Raster> Water;
	if (!WaterDir.IsEmpty())
	{
		const FString WPath = WaterDir / FString::Printf(TEXT("z%d/x%d/y%d/water_level.tif"), Tile.lod, Tile.x, Tile.y);
		if (FPaths::FileExists(WPath))
		{
			emberworld::TiffResult WR = emberworld::read_tiff(TCHAR_TO_UTF8(*WPath));
			if (WR)
			{
				Water = MoveTemp(*WR.raster);
				Opt.water = &Water.GetValue();
			}
		}
	}
	P->Mesh = emberworld::load_tile_mesh(*Region, Tile, Frame, Opt);
	{
		// CPU copy of the rendered surface (ground cover placement; same corners, same water rule)
		emberworld::TiffResult HR = emberworld::read_tiff(Region->path(Tile.height_tif));
		P->bSurface = HR && P->Surface.build(*Region, Tile, *HR.raster, Opt.water, Opt.bed_depth_m);
	}
	if (Water.IsSet())
	{
		P->WaterMesh = emberworld::build_water_mesh(*Region, Tile, Water.GetValue(), Frame);
		P->bWater = P->WaterMesh.ok() && P->WaterMesh.mesh.surface_triangles > 0;
	}
	if (Look.IsValid())
	{
		const double T0 = FPlatformTime::Seconds();
		emberworld::LookInputs In;
		if (emberworld::load_look_inputs(*Region, Tile, In, P->LookError))
		{
			P->Albedo = emberworld::compose_albedo(*Look, In);
			if (!In.cbh.empty())
			{
				P->LW = In.width;
				P->LH = In.height;
				P->Ladder.SetNumUninitialized(In.width * In.height);
				for (int32 I = 0; I < In.width * In.height; ++I)
				{
					P->Ladder[I] = static_cast<uint8>(255.f * emberworld::ladder_weight(In.cbh[I], In.cc[I]) + 0.5f);
				}
			}
			P->Mips = emberworld::build_mips(P->Albedo);
			if (!P->Albedo.mix.empty())
			{
				P->MixMips = emberworld::build_mix_mips(P->Albedo);
			}
			P->bLook = true;
		}
		P->ComposeMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	}
	return P;
}

UProceduralMeshComponent* AEmberTerrainActor::NewTileComponent(bool bWater)
{
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(this);
	// Terrain never moves. Far shadows: VSM's far-shadow culling (r.Shadow.Virtual.
	// UseFarShadowCulling) drops casters without bCastFarShadow from the distant clipmap levels.
	// The single all-tiles component always touched the near levels; per-tile components did not,
	// and mountains stopped shadowing the valleys (S_veg_lineup lineup_backlit 0.82, alpine_nw).
	C->SetMobility(EComponentMobility::Static);
	C->bCastFarShadow = !bWater;
	// M_Terrain has a geomorph WPO that is non-zero only for ~0.6 s after a tile swap; without
	// this every tile's virtual shadow pages would be invalidated every frame for having WPO.
	C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
	C->bUseAsyncCooking = true;
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(!bWater);
	C->SetupAttachment(Mesh);
	C->RegisterComponent();
	return C;
}

void AEmberTerrainActor::BindLook(int32 Section, UProceduralMeshComponent* Comp, const emberworld::TileEntry& Tile, FPreparedTile& P)
{
	if (!P.bLook)
	{
		UE_LOG(LogEmberWorld, Warning, TEXT("look inputs z%d/x%d/y%d: %s"), Tile.lod, Tile.x, Tile.y, UTF8_TO_TCHAR(P.LookError.c_str()));
		return;
	}
	const emberworld::Albedo& A = P.Albedo;
	UTexture2D* Tex = MakeMippedTexture(P.Mips, PF_B8G8R8A8, true);
	UTexture2D* MixTex = P.MixMips.empty() ? nullptr : MakeMippedTexture(P.MixMips, PF_R8G8B8A8, false);

	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(TerrainMaster, this);
	const double Full = Region->tile_px + 2.0 * Region->overlap_px;
	MID->SetTextureParameterValue(TEXT("Albedo"), Tex);
	if (MixTex)
	{
		MID->SetTextureParameterValue(TEXT("GroundMix"), MixTex);
		MID->SetScalarParameterValue(TEXT("GroundOn"), 1.0f);
		FTileMix& TM = TileMixes.Add(TileKey(Tile));
		TM.Lod = Tile.lod;
		TM.MinX = Tile.apron.min_x;
		TM.MaxY = Tile.apron.max_y;
		TM.Width = Tile.apron.width();
		TM.Height = Tile.apron.height();
		TM.W = A.width;
		TM.H = A.height;
		TM.Rgba = TArray<uint8>(A.mix.data(), static_cast<int32>(A.mix.size()));
		TM.LW = P.LW;
		TM.LH = P.LH;
		TM.Ladder = MoveTemp(P.Ladder);
	}
	MID->SetScalarParameterValue(TEXT("CanopyFarStart"), static_cast<float>(CanopyFarStartM * 100.0));
	MID->SetScalarParameterValue(TEXT("AlbedoScale"), Region->tile_px / Full);
	MID->SetScalarParameterValue(TEXT("AlbedoOffset"), Region->overlap_px / Full);
	ApplyFire(MID);
	Comp->SetMaterial(0, MID);
	{
		double Sum[3] = {0, 0, 0};
		size_t N = 0;
		for (size_t i = 0; i + 3 < A.bgra.size(); i += 4)
		{
			if (A.bgra[i + 3] == 0) continue;
			Sum[0] += A.bgra[i + 2]; Sum[1] += A.bgra[i + 1]; Sum[2] += A.bgra[i];
			++N;
		}
		UTexture* Bound = nullptr;
		MID->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Albedo")), Bound);
		double MixSum[4] = {0, 0, 0, 0};
		for (size_t i = 0; i + 3 < A.mix.size(); i += 4)
		{
			for (int c = 0; c < 4; ++c) MixSum[c] += A.mix[i + c];
		}
		const double MixN = A.mix.empty() ? 1.0 : A.mix.size() / 4.0 * 255.0;
		float GroundOn = -1.0f;
		MID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("GroundOn")), GroundOn);
		UE_LOG(LogEmberWorld, Log, TEXT("look z%d/x%d/y%d: mean sRGB (%.0f, %.0f, %.0f) over %llu px; bound=%s; ground mix (%.2f, %.2f, %.2f, %.2f) on=%.0f"),
			Tile.lod, Tile.x, Tile.y, N ? Sum[0] / N : 0.0, N ? Sum[1] / N : 0.0, N ? Sum[2] / N : 0.0,
			(unsigned long long)N, Bound == Tex ? TEXT("yes") : TEXT("NO"),
			MixSum[0] / MixN, MixSum[1] / MixN, MixSum[2] / MixN, MixSum[3] / MixN, GroundOn);
	}
	if (SectionTextures.Num() <= Section)
	{
		SectionTextures.SetNum(Section + 1);
		SectionMaterials.SetNum(Section + 1);
		SectionMixTextures.SetNum(Section + 1);
	}
	SectionTextures[Section] = Tex;
	SectionMixTextures[Section] = MixTex;
	SectionMaterials[Section] = MID;
	ComposeMs += P.ComposeMs;
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

	LodGrids.Reset();
	FinestLod = Region->finest_lod();
	CoarsestLod = FinestLod;
	for (const emberworld::TileEntry& T : Region->tiles)
	{
		CoarsestLod = FMath::Min(CoarsestLod, T.lod);
		FLodGrid& G = LodGrids.FindOrAdd(T.lod);
		if (!G.bValid)
		{
			G.Span = T.content.width();
			G.MinX = T.content.min_x - T.x * G.Span;
			G.MinY = T.content.min_y - T.y * G.Span;
			G.bValid = true;
		}
	}
	for (const emberworld::TileEntry& T : Region->tiles)  // the key grid must reproduce every tile
	{
		uint64 K = 0;
		const double Cx = 0.5 * (T.content.min_x + T.content.max_x), Cy = 0.5 * (T.content.min_y + T.content.max_y);
		if (!KeyAt(T.lod, Cx, Cy, K) || K != TileKey(T))
		{
			UE_LOG(LogEmberWorld, Error, TEXT("tile key grid does not match tile z%d/x%d/y%d"), T.lod, T.x, T.y);
			LodGrids.FindOrAdd(T.lod).bValid = false;
		}
	}

	for (const auto& KV : Loaded)
	{
		if (KV.Value.Comp) KV.Value.Comp->DestroyComponent();
		if (KV.Value.WaterComp) KV.Value.WaterComp->DestroyComponent();
	}
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

bool AEmberTerrainActor::ApplyTile(FPreparedTile& P, FString& OutError)
{
	const emberworld::TileEntry& Tile = *P.Tile;
	emberworld::MeshResult& M = P.Mesh;
	// Geomorph (8g item 4: "chunks come in solid"): each vertex's height offset from what the
	// screen shows there NOW (the coarser / finer tile it replaces) - read before this tile's own
	// surface joins TileSurfaces. M_Terrain eases the offset to zero over MorphSeconds.
	TArray<FVector2D> Morph;
	bool bMorph = false;
	if (MorphSeconds > 0.0 && M.ok() && TileSurfaces.Num() > 0)
	{
		const emberworld::TileMesh& TM0 = M.mesh;
		Morph.SetNumZeroed(static_cast<int32>(TM0.positions.size()));
		for (size_t i = 0; i < TM0.positions.size(); ++i)
		{
			const auto& V = TM0.positions[i];
			double Z = 0.0;
			if (SurfaceAt(Frame.anchor_x + V.x / 100.0, Frame.anchor_y - V.y / 100.0, Z))
			{
				const double Dz = WorldToUE(0.0, 0.0, Z).Z - V.z;
				if (FMath::Abs(Dz) > 1.0)
				{
					Morph[i].X = static_cast<float>(Dz);
					bMorph = true;
				}
			}
		}
	}
	if (P.bSurface)
	{
		FTileSurface S;
		S.Lod = Tile.lod;
		S.Sampler = MoveTemp(P.Surface);
		TileSurfaces.Add(P.Key, MoveTemp(S));
	}
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
	UProceduralMeshComponent* Comp = NewTileComponent(false);
	Comp->CreateMeshSection(0, Verts, Tris, Normals, UV0, UV1, bMorph ? Morph : Empty, Empty,
		TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (Look.IsValid())
	{
		BindLook(Section, Comp, Tile, P);
	}
	else if (Material)
	{
		Comp->SetMaterial(0, Material);
	}
	if (bMorph)
	{
		if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Comp->GetMaterial(0)))
		{
			MID->SetScalarParameterValue(TEXT("MorphStart"), GetWorld()->GetTimeSeconds());
			MID->SetScalarParameterValue(TEXT("MorphSeconds"), static_cast<float>(MorphSeconds));
		}
		++TilesMorphed;
	}
	UProceduralMeshComponent* WaterComp = nullptr;
	int64 WaterTris = 0;
	if (P.bWater)
	{
		const emberworld::TileMesh& W = P.WaterMesh.mesh;
		TArray<FVector> WV, WN;
		TArray<FVector2D> WUV, WEmpty;
		TArray<int32> WT;
		for (size_t i = 0; i < W.positions.size(); ++i)
		{
			WV.Emplace(W.positions[i].x, W.positions[i].y, W.positions[i].z);
			WN.Emplace(0.0, 0.0, 1.0);
			WUV.Emplace(W.uv0[i].u, W.uv0[i].v);
		}
		for (uint32 Idx : W.indices)
		{
			WT.Add(static_cast<int32>(Idx));
		}
		WaterComp = NewTileComponent(true);
		WaterComp->CreateMeshSection(0, WV, WT, WN, WUV, WEmpty, WEmpty, WEmpty,
			TArray<FColor>(), TArray<FProcMeshTangent>(), false);
		WaterComp->SetMaterial(0, WaterMaterial);
		WaterTris = W.surface_triangles;
	}
	FLoadedTile& L = Loaded.Add(P.Key);
	L.WaterTriangles = WaterTris;
	L.Section = Section;
	L.Lod = Tile.lod;
	L.Triangles = TM.surface_triangles;
	L.SkirtTriangles = TM.skirt_triangles;
	L.NodataCorners = TM.nodata_corners;
	L.Content = Tile.content;
	L.Comp = Comp;
	L.WaterComp = WaterComp;
	++TileGeneration;
	return true;
}

bool AEmberTerrainActor::LoadTile(const emberworld::TileEntry& Tile, FString& OutError)
{
	FPreparedPtr P = PrepareTile(Tile);
	return ApplyTile(*P, OutError);
}

void AEmberTerrainActor::UnloadTile(uint64 Key)
{
	TileMixes.Remove(Key);
	TileSurfaces.Remove(Key);
	++TileGeneration;
	if (const FLoadedTile* L = Loaded.Find(Key))
	{
		if (L->Comp)
		{
			L->Comp->DestroyComponent();
		}
		if (L->WaterComp)
		{
			L->WaterComp->DestroyComponent();
		}
		if (SectionTextures.IsValidIndex(L->Section))
		{
			SectionTextures[L->Section] = nullptr;
			SectionMaterials[L->Section] = nullptr;
			SectionMixTextures[L->Section] = nullptr;
		}
		FreeSections.Add(L->Section);
		Loaded.Remove(Key);
	}
}

void AEmberTerrainActor::RecomputeStats()
{
	TilesLoaded = Loaded.Num();
	Triangles = SkirtTriangles = NodataCorners = 0;
	WaterTilesLoaded = 0;
	WaterTriangles = 0;
	LodHistogram.Reset();
	for (const auto& KV : Loaded)
	{
		WaterTilesLoaded += KV.Value.WaterTriangles > 0 ? 1 : 0;
		WaterTriangles += KV.Value.WaterTriangles;
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

	WantKeys.Reset();
	WantTiles.Reset();
	for (const emberworld::TileEntry* T : Want)
	{
		WantKeys.Add(TileKey(*T));
		WantTiles.Add(T);
	}
	int32 Changes = 0;
	if (StreamUpdates == 0 || bSyncStreaming)
	{
		// First selection (the first frame has ground) and capture runs: synchronous.
		FString Err;
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
		Changes = Drop.Num();
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
		RecomputeStats();
		LastStreamMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		++StreamUpdates;
	}
	return Changes;
}

void AEmberTerrainActor::PumpStreaming()
{
	const double T0 = FPlatformTime::Seconds();
	bool bChanged = false;
	// 1. collect finished preparations (results no longer wanted are dropped)
	TArray<uint64> Done;
	for (auto& KV : InFlight)
	{
		if (KV.Value.IsReady())
		{
			Done.Add(KV.Key);
		}
	}
	for (uint64 K : Done)
	{
		FPreparedPtr P = InFlight[K].Get();
		InFlight.Remove(K);
		if (P.IsValid() && WantKeys.Contains(K) && !Loaded.Contains(K))
		{
			Ready.Add(P);
		}
	}
	// 2. start preparing wanted tiles (selection order: coarse-to-fine as select_tiles returns)
	for (const emberworld::TileEntry* T : WantTiles)
	{
		if (InFlight.Num() >= MaxInFlight)
		{
			break;
		}
		const uint64 K = TileKey(*T);
		if (Loaded.Contains(K) || InFlight.Contains(K)
			|| Ready.ContainsByPredicate([&](const FPreparedPtr& P) { return P->Key == K; }))
		{
			continue;
		}
		InFlight.Add(K, Async(EAsyncExecution::ThreadPool, [this, T]() { return PrepareTile(*T); }));
	}
	// 3. apply within the frame budget (at least one per frame so it always progresses)
	FString Err;
	int32 Applied = 0;
	while (Ready.Num() && (Applied == 0 || (FPlatformTime::Seconds() - T0) * 1000.0 < ApplyBudgetMs))
	{
		FPreparedPtr P = Ready[0];
		Ready.RemoveAt(0);
		if (!WantKeys.Contains(P->Key) || Loaded.Contains(P->Key))
		{
			continue;
		}
		if (!ApplyTile(*P, Err))
		{
			UE_LOG(LogEmberWorld, Error, TEXT("%s"), *Err);
		}
		++Applied;
		bChanged = true;
	}
	// 4. retire tiles no longer wanted once every wanted tile over their footprint is in
	TArray<uint64> Drop;
	for (const auto& KV : Loaded)
	{
		if (WantKeys.Contains(KV.Key))
		{
			continue;
		}
		bool bCovered = true;
		for (const emberworld::TileEntry* T : WantTiles)
		{
			if (Overlaps(KV.Value.Content, T->content) && !Loaded.Contains(TileKey(*T)))
			{
				bCovered = false;
				break;
			}
		}
		if (bCovered)
		{
			Drop.Add(KV.Key);
		}
	}
	for (uint64 K : Drop)
	{
		UnloadTile(K);
		bChanged = true;
	}
	if (bChanged)
	{
		RecomputeStats();
		LastStreamMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (LastStreamMs > 8.0)
		{
			UE_LOG(LogEmberWorld, Display, TEXT("stream-cost terrain-pump %.1f ms (applied %d, dropped %d, in flight %d, ready %d)"),
				LastStreamMs, Applied, Drop.Num(), InFlight.Num(), Ready.Num());
		}
	}
}

bool AEmberTerrainActor::IsStreamingBusy() const
{
	if (!bStreaming)
	{
		return false;
	}
	if (InFlight.Num() || Ready.Num())
	{
		return true;
	}
	for (uint64 K : WantKeys)
	{
		if (!Loaded.Contains(K))
		{
			return true;
		}
	}
	for (const auto& KV : Loaded)
	{
		if (!WantKeys.Contains(KV.Key))
		{
			return true;
		}
	}
	return false;
}

void AEmberTerrainActor::EndPlay(const EEndPlayReason::Type Reason)
{
	for (auto& KV : InFlight)
	{
		KV.Value.Wait();  // workers read this actor's region / look
	}
	InFlight.Reset();
	Ready.Reset();
	Super::EndPlay(Reason);
}

bool AEmberTerrainActor::KeyAt(int32 Lod, double X, double Y, uint64& OutKey) const
{
	const FLodGrid* G = LodGrids.Find(Lod);
	if (!G || !G->bValid)
	{
		return false;
	}
	const int32 Ix = FMath::FloorToInt((X - G->MinX) / G->Span);
	const int32 Iy = FMath::FloorToInt((Y - G->MinY) / G->Span);  // Terrain's tile y grows north
	OutKey = TileKeyOf(Lod, Ix, Iy);
	return true;
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
	PumpStreaming();
	if (StreamUpdates == 0 || FVector::Dist(Cam, LastStreamCamera) > 100.0)  // re-select every metre moved
	{
		LastStreamCamera = Cam;
		const double T0 = FPlatformTime::Seconds();
		const int32 N = UpdateStreaming(Cam);
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (Ms > 5.0)
		{
			UE_LOG(LogEmberWorld, Display, TEXT("stream-cost terrain %.1f ms (%d change(s))"), Ms, N);
		}
		if (StreamUpdates == 0)
		{
			++StreamUpdates;  // count the first selection even if it changed nothing
		}
	}
}

bool AEmberTerrainActor::GroundMixAt(double WorldX, double WorldY, float OutW[4], int32* OutLod) const
{
	const FTileMix* Best = nullptr;
	for (int32 Lod = FinestLod; Lod >= CoarsestLod && !Best; --Lod)
	{
		uint64 K = 0;
		if (KeyAt(Lod, WorldX, WorldY, K))
		{
			Best = TileMixes.Find(K);
		}
	}
	if (!Best)
	{
		return false;
	}
	const int32 X = FMath::Clamp(static_cast<int32>((WorldX - Best->MinX) / Best->Width * Best->W), 0, Best->W - 1);
	const int32 Y = FMath::Clamp(static_cast<int32>((Best->MaxY - WorldY) / Best->Height * Best->H), 0, Best->H - 1);
	if (OutLod)
	{
		*OutLod = Best->Lod;
	}
	const uint8* Px = &Best->Rgba[(static_cast<int64>(Y) * Best->W + X) * 4];
	for (int32 C = 0; C < 4; ++C)
	{
		OutW[C] = Px[C] / 255.0f;
	}
	return true;
}

bool AEmberTerrainActor::LadderAt(double WorldX, double WorldY, float& OutW) const
{
	for (int32 Lod = FinestLod; Lod >= CoarsestLod; --Lod)
	{
		uint64 K = 0;
		const FTileMix* M = KeyAt(Lod, WorldX, WorldY, K) ? TileMixes.Find(K) : nullptr;
		if (!M || M->Ladder.Num() == 0)
		{
			continue;
		}
		const int32 X = FMath::Clamp(static_cast<int32>((WorldX - M->MinX) / M->Width * M->LW), 0, M->LW - 1);
		const int32 Y = FMath::Clamp(static_cast<int32>((M->MaxY - WorldY) / M->Height * M->LH), 0, M->LH - 1);
		OutW = M->Ladder[Y * M->LW + X] / 255.f;
		return true;
	}
	return false;
}

bool AEmberTerrainActor::SurfaceAt(double WorldX, double WorldY, double& OutZ) const
{
	for (int32 Lod = FinestLod; Lod >= CoarsestLod; --Lod)
	{
		uint64 K = 0;
		if (KeyAt(Lod, WorldX, WorldY, K))
		{
			if (const FTileSurface* S = TileSurfaces.Find(K))
			{
				if (S->Sampler.surface_at(WorldX, WorldY, OutZ))
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool AEmberTerrainActor::GroundHeightAt(double WorldX, double WorldY, double& OutZ) const
{
	return Region.IsValid() && emberworld::sample_height(*Region, WorldX, WorldY, OutZ);
}

void AEmberTerrainActor::ApplyFire(UMaterialInstanceDynamic* MID) const
{
	if (!MID || !FireTexture)
	{
		return;
	}
	MID->SetTextureParameterValue(TEXT("FireTex"), FireTexture);
	MID->SetVectorParameterValue(TEXT("FireRect"), FireRect);
	MID->SetScalarParameterValue(TEXT("FireOn"), 1.f);
	MID->SetScalarParameterValue(TEXT("FireTime"), static_cast<float>(FireTimeS));
	MID->SetScalarParameterValue(TEXT("FireClasses"), bFireClasses ? 1.f : 0.f);
}

void AEmberTerrainActor::SetFireClasses(bool bOn)
{
	bFireClasses = bOn;
	for (UMaterialInstanceDynamic* MID : SectionMaterials)
	{
		ApplyFire(MID);
	}
}

void AEmberTerrainActor::SetFire(UTexture* FireTex, const FLinearColor& InRect)
{
	FireTexture = FireTex;
	FireRect = InRect;
	for (UMaterialInstanceDynamic* MID : SectionMaterials)
	{
		ApplyFire(MID);
	}
}

void AEmberTerrainActor::SetFireTime(double Seconds)
{
	FireTimeS = Seconds;
	for (UMaterialInstanceDynamic* MID : SectionMaterials)
	{
		if (MID && FireTexture)
		{
			MID->SetScalarParameterValue(TEXT("FireTime"), static_cast<float>(Seconds));
		}
	}
}

FVector AEmberTerrainActor::WorldToUE(double X, double Y, double Z) const
{
	return FVector((X - Frame.anchor_x) * 100.0, (Frame.anchor_y - Y) * 100.0, (Z - Frame.anchor_z) * 100.0);
}
