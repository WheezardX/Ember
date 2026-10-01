#include "EmberVegetationActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Async/Async.h"
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
	SlotFirstMesh.Reset();
	SlotNumMeshes.Reset();
	SlotFirstLite.Reset();
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
				// Generated variants SM_<key>_v0..N (assets/generators/veg_species.py) with the
				// species' own material instance; engine cone/sphere + clay as the fallback.
				TArray<UStaticMesh*> Variants;
				for (int32 V = 0; V < 16; ++V)
				{
					UStaticMesh* Gen = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Ember/Generated/Veg/SM_%s_v%d.SM_%s_v%d"), *Key, V, *Key, V));
					if (!Gen) break;
					Variants.Add(Gen);
				}
				UMaterialInterface* SpeciesMI = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Ember/Generated/Veg/MI_Veg_%s.MI_Veg_%s"), *Key, *Key));
				const bool bGen = Variants.Num() > 0 && (SpeciesMI || VegMaster);
				UMaterialInterface* Base = bGen ? (SpeciesMI ? SpeciesMI : VegMaster) : Clay;
				if (!bGen)
				{
					Variants = {IsBroadleaf(Key) ? Sphere : Cone};
				}
				if (!Variants[0] || !Base)
				{
					OutError = TEXT("no mesh/material for species ") + Key;
					return false;
				}
				GeneratedSpecies += bGen ? 1 : 0;
				SlotFirstMesh.Add(SpeciesMesh.Num());
				SlotNumMeshes.Add(Variants.Num());
				for (UStaticMesh* M : Variants) SpeciesMesh.Add(M);
				// Mid-tier lite meshes SM_<key>_v<N>_lite: the same trees with less foliage.
				TArray<UStaticMesh*> Lite;
				for (int32 V = 0; bGen && V < Variants.Num(); ++V)
				{
					UStaticMesh* L = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/Ember/Generated/Veg/SM_%s_v%d_lite.SM_%s_v%d_lite"), *Key, V, *Key, V));
					if (!L) break;
					Lite.Add(L);
				}
				SlotFirstLite.Add(Lite.Num() == Variants.Num() ? SpeciesMesh.Num() : INDEX_NONE);
				if (Lite.Num() == Variants.Num())
				{
					for (UStaticMesh* M : Lite) SpeciesMesh.Add(M);
				}
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, this);
				if (!bGen)
				{
					MID->SetVectorParameterValue(TEXT("Color"), SpeciesColor(Key));
				}
				SpeciesMaterial.Add(MID);
			}
			IndexToSlot.Add(Slot);
		}
	}
	MeshBounds.Reset();
	for (UStaticMesh* M : SpeciesMesh)
	{
		MeshBounds.Add(M ? M->GetBounds() : FBoxSphereBounds(FVector::ZeroVector, FVector(100.0), 100.0));
	}
	bInitialised = true;
	return true;
}

struct AEmberVegetationActor::FPreparedVeg
{
	uint64 Key = 0;
	FVegTile Tile;
	int64 Ungrounded = 0, NoSurface = 0;
	double Ms = 0.0;
	FString Error;
};

AEmberVegetationActor::FPreparedVegPtr AEmberVegetationActor::PrepareTile(const emberworld::TileEntry& Tile) const
{
	// Worker thread: worldcore scatter + surface, per-tree transforms from cached mesh bounds.
	const double T0 = FPlatformTime::Seconds();
	FPreparedVegPtr P = MakeShared<FPreparedVeg, ESPMode::ThreadSafe>();
	P->Key = VegKey(Tile);
	const emberworld::Region& R = *Terrain->GetRegion();
	emberworld::TileScatterResult TS = emberworld::scatter_tile(R, Tile, Palette, Input);
	if (!TS.ok())
	{
		P->Error = UTF8_TO_TCHAR(TS.error.c_str());
		return P;
	}
	emberworld::SurfaceSampler Surface;
	emberworld::TiffResult H = emberworld::read_tiff(R.path(Tile.height_tif));
	const bool bSurface = H && Surface.build(R, Tile, *H.raster);

	FVegTile& VT = P->Tile;
	const double CW = Tile.content.width() / CellsPerSide;
	const double CH = Tile.content.height() / CellsPerSide;
	VT.MinX = Tile.content.min_x;
	VT.MaxY = Tile.content.max_y;
	VT.CW = CW;
	VT.CH = CH;
	for (int32 Cy = 0; Cy < CellsPerSide; ++Cy)
	{
		for (int32 Cx = 0; Cx < CellsPerSide; ++Cx)
		{
			FVegCell& C = VT.Cells[Cy * CellsPerSide + Cx];
			C.MinX = Tile.content.min_x + Cx * CW;
			C.MaxX = C.MinX + CW;
			C.MaxY = Tile.content.max_y - Cy * CH;
			C.MinY = C.MaxY - CH;
		}
	}
	VT.Trees.Reserve(TS.instances.size());
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
			++P->Ungrounded;  // the tile's surface failed to build: Terrain's z (an error)
		}
		else if (!Surface.height_at(In.x, In.y, Z))
		{
			// Render policy: nothing to stand on (Terrain scatters over fuels outside the DEM's AOI
			// mask with z = 0 - upstream U8). Dropped, counted, never drawn underground.
			++P->NoSurface;
			continue;
		}
		// Variant: a hash of the instance's position (render policy; Terrain's instance is untouched).
		const uint64 VariantHash = emberworld::scatter::hash64({static_cast<uint64>(FMath::RoundToInt64(In.x * 100.0)),
			static_cast<uint64>(FMath::RoundToInt64(In.y * 100.0)), 0x5EEDull});
		FVegTree T;
		T.Slot = Slot;
		T.Variant = static_cast<int32>(VariantHash % static_cast<uint64>(SlotNumMeshes[Slot]));
		T.HeightM = static_cast<float>(In.height_m);
		T.CrownRadiusM = static_cast<float>(In.radius_m);
		// Grounding (Brad, 8g): on a slope the trunk's downhill side floated over the ground and
		// every tree stood perfectly plumb. The rendered surface's slope at the trunk sinks the base
		// by slope x trunk radius (+10 cm), and the tree leans a little downhill (15 % of the slope
		// angle, <= 4 deg) plus up to 1.5 deg of per-tree tilt (a position hash, render policy).
		double Gx = 0.0, Gy = 0.0;
		if (bSurface)
		{
			double Ze = Z, Zw = Z, Zn = Z, Zs = Z;
			if (Surface.height_at(In.x + 1.0, In.y, Ze) && Surface.height_at(In.x - 1.0, In.y, Zw)
				&& Surface.height_at(In.x, In.y + 1.0, Zn) && Surface.height_at(In.x, In.y - 1.0, Zs))
			{
				Gx = 0.5 * (Ze - Zw);   // dz/dx, east
				Gy = 0.5 * (Zn - Zs);   // dz/dy, north
			}
		}
		const double Slope = FMath::Sqrt(Gx * Gx + Gy * Gy);
		const double TrunkR = FMath::Clamp(0.015 * In.height_m, 0.1, 0.6);
		Z -= Slope * TrunkR + 0.1;
		const uint64 TiltHash = emberworld::scatter::hash64({static_cast<uint64>(FMath::RoundToInt64(In.x * 100.0)),
			static_cast<uint64>(FMath::RoundToInt64(In.y * 100.0)), 0x7117ull});
		const double TiltAz = (TiltHash & 0xFFFF) / 65536.0 * 2.0 * PI;
		const double TiltDeg = ((TiltHash >> 16) & 0xFFFF) / 65536.0 * 1.5;
		const double LeanDeg = FMath::Min(4.0, 0.15 * FMath::RadiansToDegrees(FMath::Atan(Slope)));
		// Lean vector in the world plane (x east, y north): downhill + tilt, as angle x direction.
		FVector2D Lean = FVector2D::ZeroVector;
		if (Slope > 1e-6)
		{
			Lean += FVector2D(-Gx, -Gy) / Slope * LeanDeg;
		}
		Lean += FVector2D(FMath::Cos(TiltAz), FMath::Sin(TiltAz)) * TiltDeg;
		T.Xf = FitInstance(SlotFirstMesh[Slot] + T.Variant, In.height_m, In.radius_m, In.yaw_rad, Terrain->WorldToUE(In.x, In.y, Z));
		const double LeanTotal = Lean.Size();
		if (LeanTotal > 1e-6)
		{
			// Tip the tree about its base: UE axes are x east, y south, so the world lean (e, n)
			// is the UE direction (e, -n); rotating +Z toward it turns about Z x dir.
			const FVector Dir(Lean.X / LeanTotal, -Lean.Y / LeanTotal, 0.0);
			const FQuat Tip(FVector::CrossProduct(FVector::UpVector, Dir).GetSafeNormal(), FMath::DegreesToRadians(LeanTotal));
			T.Xf.SetRotation(Tip * T.Xf.GetRotation());
		}
		const int32 Cx = FMath::Clamp(static_cast<int32>((In.x - Tile.content.min_x) / CW), 0, CellsPerSide - 1);
		const int32 Cy = FMath::Clamp(static_cast<int32>((Tile.content.max_y - In.y) / CH), 0, CellsPerSide - 1);
		VT.Cells[Cy * CellsPerSide + Cx].Trees.Add(VT.Trees.Add(MoveTemp(T)));
	}
	P->Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	return P;
}

bool AEmberVegetationActor::InstallTile(FPreparedVeg& P, FString& OutError)
{
	if (!P.Error.IsEmpty())
	{
		OutError = P.Error;
		return false;
	}
	Tiles.Add(P.Key, MoveTemp(P.Tile));
	++TileGeneration;
	UngroundedInstances += P.Ungrounded;
	NoSurfaceInstances += P.NoSurface;
	ScatterMs += P.Ms;
	return true;
}

bool AEmberVegetationActor::TreesNear(double X, double Y, double QueryM, TArray<FTreeInfo>& Out) const
{
	Out.Reset();
	const emberworld::Frame& F = Terrain->GetFrame();
	const double R2 = QueryM * QueryM;
	// Coverage: the query square's corners must each fall in a loaded tile.
	int32 Covered = 0;
	const FVector2D Corners[4] = {{X - QueryM, Y - QueryM}, {X + QueryM, Y - QueryM}, {X - QueryM, Y + QueryM}, {X + QueryM, Y + QueryM}};
	for (const auto& KV : Tiles)
	{
		const FVegTile& VT = KV.Value;
		const double MaxX = VT.MinX + VT.CW * CellsPerSide, MinY = VT.MaxY - VT.CH * CellsPerSide;
		for (const FVector2D& C : Corners)
		{
			Covered += (C.X >= VT.MinX && C.X < MaxX && C.Y > MinY && C.Y <= VT.MaxY) ? 1 : 0;
		}
		if (X + QueryM < VT.MinX || X - QueryM >= MaxX || Y + QueryM <= MinY || Y - QueryM > VT.MaxY)
		{
			continue;
		}
		const int32 Cx0 = FMath::Clamp(static_cast<int32>((X - QueryM - VT.MinX) / VT.CW), 0, CellsPerSide - 1);
		const int32 Cx1 = FMath::Clamp(static_cast<int32>((X + QueryM - VT.MinX) / VT.CW), 0, CellsPerSide - 1);
		const int32 Cy0 = FMath::Clamp(static_cast<int32>((VT.MaxY - (Y + QueryM)) / VT.CH), 0, CellsPerSide - 1);
		const int32 Cy1 = FMath::Clamp(static_cast<int32>((VT.MaxY - (Y - QueryM)) / VT.CH), 0, CellsPerSide - 1);
		for (int32 Cy = Cy0; Cy <= Cy1; ++Cy)
		{
			for (int32 Cx = Cx0; Cx <= Cx1; ++Cx)
			{
				for (int32 Ti : VT.Cells[Cy * CellsPerSide + Cx].Trees)
				{
					const FVegTree& T = VT.Trees[Ti];
					const FVector B = T.Xf.GetLocation();
					const double Tx = F.anchor_x + B.X / 100.0, Ty = F.anchor_y - B.Y / 100.0;
					if (FMath::Square(Tx - X) + FMath::Square(Ty - Y) > R2)
					{
						continue;
					}
					FTreeInfo I;
					I.X = Tx;
					I.Y = Ty;
					I.BaseUE = B;
					I.UpUE = T.Xf.GetRotation().GetUpVector();
					I.HeightM = T.HeightM;
					I.CrownRadiusM = T.CrownRadiusM;
					Out.Add(I);
				}
			}
		}
	}
	return Covered >= 4;
}

bool AEmberVegetationActor::LoadTile(const emberworld::TileEntry& Tile, FString& OutError)
{
	FPreparedVegPtr P = PrepareTile(Tile);
	return InstallTile(*P, OutError);
}

void AEmberVegetationActor::ClearCell(FVegCell& Cell)
{
	for (UInstancedStaticMeshComponent* C : Cell.Components)
	{
		if (C)
		{
			C->DestroyComponent();
		}
	}
	Cell.Components.Reset();
	Cell.PerSpecies.Reset();
	Cell.Culled = 0;
	Cell.Tier = ETier::None;
}

void AEmberVegetationActor::BuildCell(uint64 TileKey, int32 CellIndex, ETier Tier)
{
	FVegTile& VT = Tiles[TileKey];
	FVegCell& Cell = VT.Cells[CellIndex];
	ClearCell(Cell);
	Cell.Tier = Tier;
	const int32 NS = SpeciesKeys.Num();
	Cell.PerSpecies.Init(0, NS);
	if (Tier == ETier::None)
	{
		return;
	}
	const bool bNear = Tier == ETier::Near;
	TArray<TArray<FTransform>> PerMesh;
	TArray<TArray<float>> PerMeshData;   // M_Veg custom data: [canopy over the tree, tree height cm]
	PerMesh.SetNum(SpeciesMesh.Num());
	PerMeshData.SetNum(SpeciesMesh.Num());
	TArray<int32> MeshSlot;
	MeshSlot.Init(0, SpeciesMesh.Num());
	const emberworld::Frame& Fr = Terrain->GetFrame();
	for (int32 Ti : Cell.Trees)
	{
		const FVegTree& T = VT.Trees[Ti];
		if (!bNear && T.HeightM < MidMinHeightM)
		{
			++Cell.Culled;  // mid tier: understory under the canopy is invisible from this far
			continue;
		}
		const int32 First = (!bNear && SlotFirstLite[T.Slot] != INDEX_NONE) ? SlotFirstLite[T.Slot] : SlotFirstMesh[T.Slot];
		PerMesh[First + T.Variant].Add(T.Xf);
		MeshSlot[First + T.Variant] = T.Slot;
		// Sky occlusion under the canopy (ground v2, as M_Terrain / ground cover): the trees are not
		// in the distance-field scene, so an understory tree's needles got the whole blue sky.
		float Mx[4] = {0, 0, 0, 0};
		const FVector L = T.Xf.GetLocation();
		const float Canopy = Terrain->GroundMixAt(Fr.anchor_x + L.X / 100.0, Fr.anchor_y - L.Y / 100.0, Mx, nullptr)
			? FMath::Clamp(Mx[0], 0.f, 1.f) : 0.f;
		PerMeshData[First + T.Variant].Add(Canopy);
		PerMeshData[First + T.Variant].Add(static_cast<float>(T.HeightM * 100.0));
	}
	for (int32 Mi = 0; Mi < PerMesh.Num(); ++Mi)
	{
		if (PerMesh[Mi].Num() == 0)
		{
			continue;
		}
		const int32 S = MeshSlot[Mi];
		UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(this);
		C->SetStaticMesh(SpeciesMesh[Mi]);
		C->SetMaterial(0, SpeciesMaterial[S]);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(true);
		C->SetMobility(EComponentMobility::Movable);
		C->InstancingRandomSeed = static_cast<int32>((TileKey * 31 + CellIndex * 977 + Mi) & 0x7FFFFFFF) | 1;  // deterministic
		if (bNear)
		{
			// Wind WPO moves the trees: without this the virtual shadow map cache keeps stale pages
			// and crowns get saw-toothed self-shadows (seen in S_forest_teanaway ground_ridge).
			C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
			// Nanite evaluates WPO per instance only within this distance: swaying trees are the
			// programmable (slow) raster path, and sway is invisible past a few hundred metres.
			// With a fire bound, WPO also strips burned trees (bare / consumed outcomes, M_Veg): it
			// must cover the whole near tier, or burned trees past WindRadiusM keep their needles
			// and lose them as the camera approaches (the ground-cover "ring" bug, for trees).
			C->SetWorldPositionOffsetDisableDistance(static_cast<int32>((bFire ? FMath::Max(WindRadiusM, NearRadiusM + 50.0) : WindRadiusM) * 100.0));
		}
		else
		{
			// Mid tier: no wind (sway is sub-pixel this far out), so shadows can stay cached - the
			// per-frame VSM redraw of every tree was half the forest's GPU cost.
			C->SetEvaluateWorldPositionOffset(false);
			C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Static;
		}
		// Trees stay out of the distance-field scene: ~1.4 M DF objects cost ~0.9 GB VRAM (4.23 -> 3.32 GB
		// at the S_tq_forest perf pose, D8 budget 4 GB). Lumen still sees them via screen traces.
		C->bAffectDistanceFieldLighting = false;
		C->NumCustomDataFloats = 2;
		C->SetupAttachment(RootComponent);
		C->RegisterComponent();
		C->AddInstances(PerMesh[Mi], /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
		for (int32 I = 0; I < PerMesh[Mi].Num(); ++I)
		{
			C->SetCustomData(I, TArrayView<const float>(&PerMeshData[Mi][I * 2], 2), false);
		}
		Cell.Components.Add(C);
		Cell.PerSpecies[S] += PerMesh[Mi].Num();
	}
}

FTransform AEmberVegetationActor::FitInstance(int32 Mesh, double HeightM, double CrownRadiusM, double YawRad, FVector Loc) const
{
	// Terrain's scatter v2 sizes every crown (0.5 x crown_ratio x height x 0.85..1.15): the mesh's
	// bounds are fitted to that height and crown diameter, no render-side crown policy.
	const FBoxSphereBounds B = MeshBounds.IsValidIndex(Mesh) ? MeshBounds[Mesh] : SpeciesMesh[Mesh]->GetBounds();
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

void AEmberVegetationActor::SetFire(UTexture* FireTex, const FLinearColor& FireRect)
{
	for (UMaterialInstanceDynamic* M : SpeciesMaterial)
	{
		M->SetTextureParameterValue(TEXT("FireTex"), FireTex);
		M->SetVectorParameterValue(TEXT("FireRect"), FireRect);
		M->SetScalarParameterValue(TEXT("FireOn"), FireTex ? 1.f : 0.f);
	}
	bFire = FireTex != nullptr;
}

void AEmberVegetationActor::SetFireTime(double Seconds)
{
	for (UMaterialInstanceDynamic* M : SpeciesMaterial)
	{
		M->SetScalarParameterValue(TEXT("FireTime"), static_cast<float>(Seconds));
	}
}

void AEmberVegetationActor::SpawnLineup(double WorldX, double WorldY, double RowDeg)
{
	bLineup = true;
	for (auto& KV : Tiles)
	{
		for (FVegCell& C : KV.Value.Cells)
		{
			ClearCell(C);
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
			C->SetStaticMesh(SpeciesMesh[SlotFirstMesh[I.Species]]);
			C->SetMaterial(0, SpeciesMaterial[I.Species]);
			C->SetWorldTransform(FitInstance(SlotFirstMesh[I.Species], I.HeightM, 0.5 * I.WidthM, 0.0, Loc));
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
		for (FVegCell& C : VT->Cells)
		{
			ClearCell(C);
		}
		Tiles.Remove(Key);
		++TileGeneration;
	}
}

void AEmberVegetationActor::RecomputeStats()
{
	InstancesTotal = 0;
	TilesLoaded = Tiles.Num();
	TilesNear = 0;
	CellsNear = 0;
	InstancesNear = 0;
	MidCulled = 0;
	BySpecies.Reset();
	for (const auto& KV : Tiles)
	{
		bool bAnyNear = false;
		for (const FVegCell& C : KV.Value.Cells)
		{
			const bool bNear = C.Tier == ETier::Near;
			bAnyNear |= bNear;
			CellsNear += bNear ? 1 : 0;
			MidCulled += C.Culled;
			for (int32 S = 0; S < C.PerSpecies.Num(); ++S)
			{
				InstancesTotal += C.PerSpecies[S];
				InstancesNear += bNear ? C.PerSpecies[S] : 0;
				BySpecies.FindOrAdd(SpeciesKeys[S]) += C.PerSpecies[S];
			}
		}
		TilesNear += bAnyNear ? 1 : 0;
	}
}

void AEmberVegetationActor::SelectTiles(const FVector& CameraUE)
{
	const emberworld::Region& R = *Terrain->GetRegion();
	const emberworld::Frame& F = Terrain->GetFrame();
	CamWx = F.anchor_x + CameraUE.X / 100.0;
	CamWy = F.anchor_y - CameraUE.Y / 100.0;
	// Height above the ground under the camera: trees right below an aerial camera are not near.
	double Gz = 0.0;
	const double CamZ = Terrain->UEToWorldZ(CameraUE.Z);
	CamHag = (Terrain->SurfaceAt(CamWx, CamWy, Gz) || Terrain->GroundHeightAt(CamWx, CamWy, Gz)) ? FMath::Max(0.0, CamZ - Gz) : 0.0;
	WantTiles.Reset();
	WantKeys.Reset();
	for (const emberworld::TileEntry* T : R.tiles_at(R.finest_lod()))
	{
		const double Dx = FMath::Max3(T->content.min_x - CamWx, 0.0, CamWx - T->content.max_x);
		const double Dy = FMath::Max3(T->content.min_y - CamWy, 0.0, CamWy - T->content.max_y);
		if (FMath::Sqrt(Dx * Dx + Dy * Dy + CamHag * CamHag) <= RadiusM)
		{
			WantKeys.Add(VegKey(*T));
			WantTiles.Add(T);
		}
	}
}

void AEmberVegetationActor::QueueTiers()
{
	for (auto& KV : Tiles)
	{
		for (int32 Ci = 0; Ci < CellsPerSide * CellsPerSide; ++Ci)
		{
			const FVegCell& C = KV.Value.Cells[Ci];
			const double Dx = FMath::Max3(C.MinX - CamWx, 0.0, CamWx - C.MaxX);
			const double Dy = FMath::Max3(C.MinY - CamWy, 0.0, CamWy - C.MaxY);
			const double D = FMath::Sqrt(Dx * Dx + Dy * Dy + CamHag * CamHag);
			const ETier Tier = D <= NearRadiusM ? ETier::Near : (D <= RadiusM ? ETier::Mid : ETier::None);
			if (Tier != C.Tier || (Tier != ETier::None && C.PerSpecies.Num() == 0))
			{
				BuildQueue.AddUnique(TPair<uint64, int32>(KV.Key, Ci));
			}
		}
	}
}

void AEmberVegetationActor::PumpStreaming()
{
	const double T0 = FPlatformTime::Seconds();
	bool bChanged = false;
	FString Err;
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
		FPreparedVegPtr P = InFlight[K].Get();
		InFlight.Remove(K);
		if (P.IsValid() && WantKeys.Contains(K) && !Tiles.Contains(K))
		{
			if (!InstallTile(*P, Err))
			{
				UE_LOG(LogEmberVeg, Error, TEXT("veg tile: %s"), *Err);
				continue;
			}
			bChanged = true;
		}
	}
	for (const emberworld::TileEntry* T : WantTiles)
	{
		if (InFlight.Num() >= MaxInFlight)
		{
			break;
		}
		const uint64 K = VegKey(*T);
		if (!Tiles.Contains(K) && !InFlight.Contains(K))
		{
			InFlight.Add(K, Async(EAsyncExecution::ThreadPool, [this, T]() { return PrepareTile(*T); }));
		}
	}
	TArray<uint64> Drop;
	for (const auto& KV : Tiles)
	{
		if (!WantKeys.Contains(KV.Key))
		{
			Drop.Add(KV.Key);
		}
	}
	for (uint64 K : Drop)
	{
		BuildQueue.RemoveAll([K](const TPair<uint64, int32>& Q) { return Q.Key == K; });
		UnloadTile(K);
		bChanged = true;
	}
	if (bChanged)
	{
		QueueTiers();
	}
	// build cells within the budget, nearest first (at least one per frame)
	int32 Built = 0;
	while (BuildQueue.Num() && (Built == 0 || (FPlatformTime::Seconds() - T0) * 1000.0 < BuildBudgetMs))
	{
		int32 Best = 0;
		double BestD = TNumericLimits<double>::Max();
		for (int32 i = 0; i < BuildQueue.Num(); ++i)
		{
			const FVegTile* VT = Tiles.Find(BuildQueue[i].Key);
			if (!VT)
			{
				continue;
			}
			const FVegCell& C = VT->Cells[BuildQueue[i].Value];
			const double D = FMath::Square(0.5 * (C.MinX + C.MaxX) - CamWx) + FMath::Square(0.5 * (C.MinY + C.MaxY) - CamWy);
			if (D < BestD)
			{
				BestD = D;
				Best = i;
			}
		}
		const TPair<uint64, int32> Q = BuildQueue[Best];
		BuildQueue.RemoveAtSwap(Best);
		FVegTile* VT = Tiles.Find(Q.Key);
		if (!VT)
		{
			continue;
		}
		const FVegCell& C = VT->Cells[Q.Value];
		const double Dx = FMath::Max3(C.MinX - CamWx, 0.0, CamWx - C.MaxX);
		const double Dy = FMath::Max3(C.MinY - CamWy, 0.0, CamWy - C.MaxY);
		const double D = FMath::Sqrt(Dx * Dx + Dy * Dy + CamHag * CamHag);
		const ETier Tier = D <= NearRadiusM ? ETier::Near : (D <= RadiusM ? ETier::Mid : ETier::None);
		BuildCell(Q.Key, Q.Value, Tier);
		++Built;
		bChanged = true;
	}
	if (bChanged)
	{
		RecomputeStats();
	}
	const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	if (Ms > 8.0)
	{
		UE_LOG(LogEmberVeg, Display, TEXT("stream-cost vegetation-pump %.1f ms (built %d, queue %d, dropped %d)"), Ms, Built, BuildQueue.Num(), Drop.Num());
	}
}

bool AEmberVegetationActor::IsStreamingBusy() const
{
	if (InFlight.Num() || BuildQueue.Num())
	{
		return true;
	}
	for (uint64 K : WantKeys)
	{
		if (!Tiles.Contains(K))
		{
			return true;
		}
	}
	return false;
}

void AEmberVegetationActor::EndPlay(const EEndPlayReason::Type Reason)
{
	for (auto& KV : InFlight)
	{
		KV.Value.Wait();
	}
	InFlight.Reset();
	Super::EndPlay(Reason);
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
	// Height above the ground under the camera: trees right below an aerial camera are not near.
	double Gz = 0.0;
	const double CamZ = Terrain->UEToWorldZ(CameraUE.Z);
	const double Hag = Terrain->GroundHeightAt(Wx, Wy, Gz) ? FMath::Max(0.0, CamZ - Gz) : 0.0;
	auto Dist = [&](double MinX, double MinY, double MaxX, double MaxY)
	{
		const double Dx = FMath::Max3(MinX - Wx, 0.0, Wx - MaxX);
		const double Dy = FMath::Max3(MinY - Wy, 0.0, Wy - MaxY);
		return FMath::Sqrt(Dx * Dx + Dy * Dy + Hag * Hag);
	};

	TSet<uint64> Want;
	TArray<const emberworld::TileEntry*> SyncWantTiles;
	for (const emberworld::TileEntry* T : R.tiles_at(R.finest_lod()))
	{
		if (Dist(T->content.min_x, T->content.min_y, T->content.max_x, T->content.max_y) <= RadiusM)
		{
			Want.Add(VegKey(*T));
			SyncWantTiles.Add(T);
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
	for (const emberworld::TileEntry* T : SyncWantTiles)
	{
		const uint64 Key = VegKey(*T);
		if (!Tiles.Contains(Key))
		{
			if (!LoadTile(*T, Err))
			{
				UE_LOG(LogEmberVeg, Error, TEXT("veg tile z%d/x%d/y%d: %s"), T->lod, T->x, T->y, *Err);
				continue;
			}
			++Changes;
		}
		FVegTile& VT = Tiles[Key];
		for (int32 Ci = 0; Ci < CellsPerSide * CellsPerSide; ++Ci)
		{
			const FVegCell& C = VT.Cells[Ci];
			const double D = Dist(C.MinX, C.MinY, C.MaxX, C.MaxY);
			const ETier Tier = D <= NearRadiusM ? ETier::Near : (D <= RadiusM ? ETier::Mid : ETier::None);
			if (Tier != C.Tier || (Tier != ETier::None && C.PerSpecies.Num() == 0))
			{
				BuildCell(Key, Ci, Tier);
				++Changes;
			}
		}
	}
	if (Changes)
	{
		RecomputeStats();
		UE_LOG(LogEmberVeg, Log, TEXT("vegetation: %d tiles, %d near cells, %lld instances (%lld near, %lld understory skipped in mid) (%.0f ms scatter total)"),
			TilesLoaded, CellsNear, InstancesTotal, InstancesNear, MidCulled, ScatterMs);
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
	if (!bSyncStreaming && !bFirstUpdate)
	{
		if (FVector::Dist(Cam, LastCamera) > 5000.0)  // re-select every 50 m moved
		{
			LastCamera = Cam;
			SelectTiles(Cam);
			QueueTiers();
		}
		PumpStreaming();
		return;
	}
	if (bFirstUpdate || FVector::Dist(Cam, LastCamera) > 5000.0)  // re-select every 50 m moved
	{
		bFirstUpdate = false;
		LastCamera = Cam;
		const double T0 = FPlatformTime::Seconds();
		const int32 N = UpdateStreaming(Cam);
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		if (Ms > 5.0)
		{
			UE_LOG(LogEmberVeg, Display, TEXT("stream-cost vegetation %.1f ms (%d change(s))"), Ms, N);
		}
	}
}
