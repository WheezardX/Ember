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
		for (int32 i = 0; i < Mesh->GetNumSections(); ++i)
		{
			Mesh->SetMaterial(i, Material);
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

void AEmberTerrainActor::BindLook(int32 Section, const emberworld::TileEntry& Tile)
{
	const double T0 = FPlatformTime::Seconds();
	emberworld::LookInputs In;
	std::string Err;
	if (!emberworld::load_look_inputs(*Region, Tile, In, Err))
	{
		UE_LOG(LogEmberWorld, Warning, TEXT("look inputs z%d/x%d/y%d: %s"), Tile.lod, Tile.x, Tile.y, UTF8_TO_TCHAR(Err.c_str()));
		return;
	}
	const emberworld::Albedo A = emberworld::compose_albedo(*Look, In);
	UTexture2D* Tex = MakeMippedTexture(emberworld::build_mips(A), PF_B8G8R8A8, true);
	UTexture2D* MixTex = A.mix.empty() ? nullptr : MakeMippedTexture(emberworld::build_mix_mips(A), PF_R8G8B8A8, false);

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
	}
	MID->SetScalarParameterValue(TEXT("AlbedoScale"), Region->tile_px / Full);
	MID->SetScalarParameterValue(TEXT("AlbedoOffset"), Region->overlap_px / Full);
	ApplyFire(MID);
	Mesh->SetMaterial(Section, MID);
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
	ComposeMs += (FPlatformTime::Seconds() - T0) * 1000.0;
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
	// Water layer for this tile (optional): lakebed rule for the terrain + a flat surface.
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
			else
			{
				UE_LOG(LogEmberWorld, Warning, TEXT("water %s: %s"), *WPath, UTF8_TO_TCHAR(WR.error.message.c_str()));
			}
		}
	}
	emberworld::MeshResult M = emberworld::load_tile_mesh(*Region, Tile, Frame, Opt);
	{
		// CPU copy of the rendered surface (ground cover placement; same corners, same water rule)
		emberworld::TiffResult HR = emberworld::read_tiff(Region->path(Tile.height_tif));
		if (HR)
		{
			FTileSurface S;
			S.Lod = Tile.lod;
			if (S.Sampler.build(*Region, Tile, *HR.raster, Opt.water, Opt.bed_depth_m))
			{
				TileSurfaces.Add(TileKey(Tile), MoveTemp(S));
			}
		}
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
	Mesh->CreateMeshSection(Section, Verts, Tris, Normals, UV0, UV1, Empty, Empty,
		TArray<FColor>(), TArray<FProcMeshTangent>(), /*bCreateCollision=*/false);
	if (Look.IsValid())
	{
		BindLook(Section, Tile);
	}
	else if (Material)
	{
		Mesh->SetMaterial(Section, Material);
	}
	int64 WaterTris = 0;
	if (Water.IsSet())
	{
		emberworld::MeshResult WM = emberworld::build_water_mesh(*Region, Tile, Water.GetValue(), Frame);
		if (WM.ok() && WM.mesh.surface_triangles > 0)
		{
			TArray<FVector> WV, WN;
			TArray<FVector2D> WUV, WEmpty;
			TArray<int32> WT;
			for (size_t i = 0; i < WM.mesh.positions.size(); ++i)
			{
				WV.Emplace(WM.mesh.positions[i].x, WM.mesh.positions[i].y, WM.mesh.positions[i].z);
				WN.Emplace(0.0, 0.0, 1.0);
				WUV.Emplace(WM.mesh.uv0[i].u, WM.mesh.uv0[i].v);
			}
			for (uint32 Idx : WM.mesh.indices)
			{
				WT.Add(static_cast<int32>(Idx));
			}
			WaterMesh->CreateMeshSection(Section, WV, WT, WN, WUV, WEmpty, WEmpty, WEmpty,
				TArray<FColor>(), TArray<FProcMeshTangent>(), false);
			WaterMesh->SetMaterial(Section, WaterMaterial);
			WaterTris = WM.mesh.surface_triangles;
		}
	}
	FLoadedTile& L = Loaded.Add(TileKey(Tile));
	L.WaterTriangles = WaterTris;
	L.Section = Section;
	L.Lod = Tile.lod;
	L.Triangles = TM.surface_triangles;
	L.SkirtTriangles = TM.skirt_triangles;
	L.NodataCorners = TM.nodata_corners;
	return true;
}

void AEmberTerrainActor::UnloadTile(uint64 Key)
{
	TileMixes.Remove(Key);
	TileSurfaces.Remove(Key);
	if (const FLoadedTile* L = Loaded.Find(Key))
	{
		Mesh->ClearMeshSection(L->Section);
		if (L->Section < WaterMesh->GetNumSections())
		{
			WaterMesh->ClearMeshSection(L->Section);
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

bool AEmberTerrainActor::GroundMixAt(double WorldX, double WorldY, float OutW[4], int32* OutLod) const
{
	const FTileMix* Best = nullptr;
	for (const TPair<uint64, FTileMix>& P : TileMixes)
	{
		const FTileMix& M = P.Value;
		if (WorldX < M.MinX || WorldX >= M.MinX + M.Width || WorldY > M.MaxY || WorldY <= M.MaxY - M.Height)
		{
			continue;
		}
		if (!Best || M.Lod > Best->Lod)
		{
			Best = &M;
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

bool AEmberTerrainActor::SurfaceAt(double WorldX, double WorldY, double& OutZ) const
{
	int32 BestLod = -1;
	bool bHit = false;
	for (const TPair<uint64, FTileSurface>& P : TileSurfaces)
	{
		double Z = 0.0;
		if (P.Value.Lod > BestLod && P.Value.Sampler.surface_at(WorldX, WorldY, Z))
		{
			BestLod = P.Value.Lod;
			OutZ = Z;
			bHit = true;
		}
	}
	return bHit;
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
