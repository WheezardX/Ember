#include "EmberSmokeActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

#include "EmberTerrainActor.h"

namespace
{
	/** Integer hash -> [0, 1): per-puff seeds that do not depend on evaluation order. */
	double Hash01(uint32 A, uint32 B)
	{
		uint32 H = A * 0x9E3779B1u ^ (B + 0x7F4A7C15u) * 0x85EBCA77u;
		H ^= H >> 15;
		H *= 0x2C1B3C6Du;
		H ^= H >> 12;
		H *= 0x297A2D39u;
		H ^= H >> 15;
		return (H & 0xFFFFFF) / 16777216.0;
	}

	double SmoothStep(double A, double B, double X)
	{
		const double T = FMath::Clamp((X - A) / (B - A), 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T);
	}

	struct FPuff
	{
		FVector Loc;
		double RadiusM;
		double Stretch;
		double Roll;
		float Data[4];
		double Dist2;
	};
}

AEmberSmokeActor::AEmberSmokeActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberSmokeActor::Init(AEmberTerrainActor* InTerrain, FString& OutError)
{
	Terrain = InTerrain;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Smoke.M_Smoke"));
	if (!Terrain || !Plane || !Mat)
	{
		OutError = TEXT("smoke needs the terrain, /Engine/BasicShapes/Plane and M_Smoke (ember-dev regen-assets)");
		return false;
	}
	Puffs = NewObject<UInstancedStaticMeshComponent>(this);
	Puffs->SetStaticMesh(Plane);
	Puffs->SetMaterial(0, Mat);
	Puffs->NumCustomDataFloats = 4;
	Puffs->SetCastShadow(false);
	Puffs->bAffectDistanceFieldLighting = false;
	Puffs->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Puffs->SetupAttachment(RootComponent);
	Puffs->RegisterComponent();
	return true;
}

void AEmberSmokeActor::SetWind(double FromDeg, double SpeedMs)
{
	WindFromDeg = FromDeg;
	WindMs = FMath::Max(1.0, SpeedMs);
}

void AEmberSmokeActor::SetSources(const TArray<FEmberSmokeSource>& InSources)
{
	Sources = InSources;
	// Strongest first: the puff budget goes to the plumes that matter.
	Sources.Sort([](const FEmberSmokeSource& A, const FEmberSmokeSource& B)
	{
		if (A.Strength != B.Strength) return A.Strength > B.Strength;
		if (A.Heat != B.Heat) return A.Heat > B.Heat;
		if (A.Smoulder != B.Smoulder) return A.Smoulder > B.Smoulder;
		return A.Key < B.Key;
	});
	NumSources = Sources.Num();
}

void AEmberSmokeActor::Rebuild(const FVector& CameraLoc, const FRotator& CameraRot)
{
	if (!Puffs || !Terrain)
	{
		return;
	}
	// Downwind unit vector in the region CRS (x east, y north).
	const double To = FMath::DegreesToRadians(WindFromDeg + 180.0);
	const double Ux = FMath::Sin(To);
	const double Uy = FMath::Cos(To);
	const FVector CamRight = FRotationMatrix(CameraRot).GetUnitAxis(EAxis::Y);

	TArray<FPuff> All;
	All.Reserve(MaxPuffs);
	NumPlumes = 0;
	NumMerged = 0;
	MaxTopM = 0.0;

	// Distance merging (Brad 2026-10-01: "when we zoom out the individual columns should combine
	// into a denser, thicker cloud ... the smoke is thick and dominates the sky"). Sources within
	// MergeNearM of the camera draw as their own columns; farther out they are pooled into square
	// cells that double in size with distance (600 m from 3 km, 1.2 km from 6 km, 2.4 km from
	// 12 km, 4.8 km from 24 km): one wide, dense plume per cell whose base spans the burning area
	// and whose top spreads into a sheet - so a front's columns read as one mass from afar, and the
	// far field costs fewer puffs. Weak smoulder wisps are dropped past 6 km (invisible there).
	const emberworld::Frame& Fr = Terrain->GetFrame();
	const FVector2D Cam(Fr.anchor_x + CameraLoc.X / 100.0, Fr.anchor_y - CameraLoc.Y / 100.0);
	struct FDraw { FEmberSmokeSource S; double CellM = 0.0; double W = 0.0; };
	TArray<FDraw> Draw;
	TMap<uint64, int32> CellIndex;
	for (const FEmberSmokeSource& S : Sources)
	{
		const double Dist = FVector2D::Distance(Cam, FVector2D(S.X, S.Y));
		if (Dist < MergeNearM)
		{
			Draw.Add({S, 0.0, 0.0});
			continue;
		}
		const int32 Level = FMath::Clamp(FMath::FloorToInt32(FMath::Log2(Dist / MergeNearM)), 0, 3);
		const bool bWeak = S.Strength < 0.5 && S.Heat <= 0.f;
		if (bWeak && Level >= 1)
		{
			continue;
		}
		const double Cell = 600.0 * (1 << Level);
		const int64 Cx = FMath::FloorToInt64(S.X / Cell), Cy = FMath::FloorToInt64(S.Y / Cell);
		const uint64 K = (static_cast<uint64>(Level) << 60) ^ (static_cast<uint64>(Cx & 0x3FFFFFFF) << 30) ^ static_cast<uint64>(Cy & 0x3FFFFFFF);
		const double Q = FMath::Max(0.01, static_cast<double>(S.Strength + S.Heat + S.Smoulder));
		int32* Found = CellIndex.Find(K);
		if (!Found)
		{
			FDraw D;
			D.S = S;
			D.S.X = D.S.Y = 0.0;
			D.S.Strength = D.S.Heat = D.S.Smoulder = D.S.Burning = 0.f;
			D.S.Cluster = D.S.HeatCluster = 0.f;
			// stable per cell (seeds the puffs); high bit keeps it clear of the 300 m bin keys
			D.S.Key = static_cast<int32>(0x40000000u | static_cast<uint32>(HashCombine(GetTypeHash(K), 0x5EED)) & 0x3FFFFFFFu);
			D.CellM = Cell;
			Found = &CellIndex.Add(K, Draw.Add(D));
		}
		FDraw& D = Draw[*Found];
		D.S.X += S.X * Q;
		D.S.Y += S.Y * Q;
		D.W += Q;
		D.S.Strength += S.Strength;
		D.S.Heat += S.Heat;
		D.S.Smoulder += S.Smoulder;
		D.S.Burning += S.Burning;
		D.S.Cluster = FMath::Max(D.S.Cluster, S.Cluster);
		D.S.HeatCluster = FMath::Max(D.S.HeatCluster, S.HeatCluster);
	}
	for (FDraw& D : Draw)
	{
		if (D.CellM > 0.0)
		{
			D.S.X /= D.W;
			D.S.Y /= D.W;
			++NumMerged;
		}
	}
	// strongest first (the puff budget goes to the plumes that matter)
	Draw.Sort([](const FDraw& A, const FDraw& B)
	{
		const double Qa = A.S.Strength + A.S.Heat, Qb = B.S.Strength + B.S.Heat;
		return Qa != Qb ? Qa > Qb : A.S.Key < B.S.Key;
	});

	for (const FDraw& Dw : Draw)
	{
		const FEmberSmokeSource& S = Dw.S;
		const double Cell = Dw.CellM;           // > 0: a merged far-field plume
		// A bin with no flaming front left only smoulders: a low, lazy wisp (v1, reference:
		// IR scattered heat over most of the scar for weeks).
		// Interior (observed heat, no front here): pockets burning inside the scar stand up as their
		// own columns, lower than a running front's (reference: the Kachess aerials, several columns
		// of a few hundred m to ~1.5 km off both shores days after the front passed).
		const bool bInterior = S.Strength < 0.5 && S.Heat > 0.f;
		const bool bSmoulder = S.Strength < 0.5 && !bInterior;
		const double Q = bInterior ? S.Heat : bSmoulder ? S.Smoulder : S.Strength;
		const double SqQ = FMath::Sqrt(Q);
		// Ground under the source, cached per bin (the region DEM read was 40-70 ms per frame with a
		// few hundred interior-heat sources - most of the play-mode frame). Re-read if the bin's
		// centroid moved more than ~20 m.
		double Gz = 0.0;
		FGroundHit* Hit = GroundCache.Find(S.Key);
		if (Hit && FMath::Abs(Hit->X - S.X) < 20.0 && FMath::Abs(Hit->Y - S.Y) < 20.0)
		{
			Gz = Hit->Z;
		}
		else
		{
			if (!Terrain->SurfaceAt(S.X, S.Y, Gz) && !Terrain->GroundHeightAt(S.X, S.Y, Gz))
			{
				continue;
			}
			GroundCache.Add(S.Key, FGroundHit{S.X, S.Y, Gz});
		}
		// Neighbouring bins burn as one convective column: height from the 1.5 km cluster.
		// Columns (v1): the reference columns stand 1-3 km+ over every active area; 90 x sqrt
		// (cluster) gave ~500 m tops on the real Three Queens record.
		const double H = bInterior ? FMath::Clamp(90.0 * FMath::Sqrt(static_cast<double>(S.HeatCluster)), 200.0, 1200.0)
			: bSmoulder ? FMath::Clamp(60.0 + 40.0 * SqQ, 60.0, 300.0)
			: FMath::Clamp(250.0 * FMath::Sqrt(static_cast<double>(S.Cluster)), 300.0, 5000.0);
		MaxTopM = FMath::Max(MaxTopM, H);
		const double L = bSmoulder ? FMath::Clamp(10.0 * H, 600.0, 3000.0)
			: FMath::Clamp(6.0 * H, bInterior ? 1200.0 : 1500.0, bInterior ? 10000.0 : 18000.0);  // plume length before it thins out
		// Buoyant rise (v1, reference: the columns over Kachess stand near-vertical for a km or
		// more before they lean): rise speed W0 against the wind, e-folding time Tau to the top.
		const double W0 = bSmoulder ? 1.5 : (bInterior ? 4.0 : 6.0) + 3.0 * FMath::Sqrt(H / 100.0);
		const double Tau = H / W0;
		const double Life = FMath::Max(L / WindMs, 3.0 * Tau);        // puff lifetime (s)
		double R0 = bSmoulder ? 40.0 : bInterior ? 45.0 + 6.0 * SqQ : 60.0 + 7.0 * SqQ;  // ~a bin across at full strength
		double Base = bSmoulder ? 0.22 * (1.0 - FMath::Exp(-Q / 3.0))
			: (bInterior ? 0.75 : 0.9) * (1.0 - FMath::Exp(-Q / 4.0));  // dense columns, faint wisps
		if (Cell > 0.0)
		{
			// merged: as wide as a good part of its cell, and denser (several columns' worth of smoke)
			R0 = FMath::Max(R0, 0.3 * Cell);
			Base = FMath::Min(0.97, Base * 1.15 + 0.05);
		}
		// Enough puffs that neighbours always overlap (spacing ~0.4 x the mid-plume radius).
		const double RMid = R0 + 0.05 * L + 0.06 * H;   // matches the v1 (smaller) puff radius
		// The rising column gets its own puffs, evenly spaced in HEIGHT (v2, the lab: spaced evenly in
		// time over the whole drift, consecutive puffs were ~800 m apart in the fast-rising column, so
		// the lower column was empty and plumes floated; merged plumes broke into blobs).
		const double RCol = R0 + 0.06 * H;
		const int32 Kc = bSmoulder ? 0 : FMath::Clamp(FMath::CeilToInt32(H / (0.45 * RCol)), 6, 64);
		const int32 K = bSmoulder ? 8 : Kc + FMath::Clamp(FMath::CeilToInt32(L / (0.25 * RMid)), 6, 160);
		const double Uc = static_cast<double>(Kc) / K;     // life fraction where the column ends
		const double Tc = 3.0 * Tau;                        // ~95 % of the rise
		if (All.Num() + K > MaxPuffs)
		{
			break;
		}
		++NumPlumes;
		// Interior pockets light their own smoke from below too (the night reference: the smoke over
		// the burning slope glows orange)
		const double Hot = bInterior ? 0.6 * FMath::Min(1.0, Q / 8.0)
			: FMath::Min(1.0, S.Burning / FMath::Max(1.0, Q)) * FMath::Min(1.0, S.Burning / 5.0);
		const double HotZ = bInterior ? 0.4 * H : R0 + 80.0;   // how far up the glow reaches
		const double Phase = Hash01(S.Key, 0x51u);
		// A puff advances one slot (L / K) every Life / K seconds, so it crosses the plume in Life.
		// Index I after W wraps is the same physical puff as index I + 1 after W + 1: seed on W - I.
		const double Slots = ClockS * K / Life + Phase * K;
		const double Cycle = FMath::Frac(Slots);
		const int64 Wraps = FMath::FloorToInt64(Slots);
		for (int32 I = 0; I < K; ++I)
		{
			// Life fraction 0..1, packed toward the base (u^1.5): low puffs are small and rise fast, so
			// even spacing in life left them apart as a row of discs; still a monotonic, continuous
			// map, so puffs keep moving smoothly and keep their seeds. Opacity follows the spacing
			// (u^2 without it doubled the base density into a wall).
			// Life fraction U -> puff age T, monotonic and continuous (puffs keep moving smoothly and
			// keep their seeds): U < Uc climbs the column with height uniform in U (z = 0.95 H U / Uc,
			// T = -Tau ln(1 - z / H)), U >= Uc drifts linearly in time from Tc to Life.
			const double U = (I + Cycle) / K;
			double T;
			if (bSmoulder || Kc == 0)
			{
				T = U * Life;
			}
			else if (U < Uc)
			{
				T = -Tau * FMath::Loge(1.0 - 0.95 * U / Uc);
			}
			else
			{
				T = Tc + (U - Uc) / FMath::Max(1e-6, 1.0 - Uc) * FMath::Max(0.0, Life - Tc);
			}
			const double F = T / Life;                                 // life fraction (fade-out)
			const double Pack = 1.0;
			const double D = FMath::Min(WindMs * T, L);                // downwind distance (m)
			const double Z = H * (1.0 - FMath::Exp(-T / Tau));         // buoyant rise to the top
			// Entrainment widens the column as it climbs; the top spreads (cauliflower / anvil).
			// (v1: smaller puffs than the column is wide, so billows read instead of one blur)
			// (interior columns stay narrow - the reference ones are a few hundred m wide - and
			// spread downwind rather than into a cap)
			// (merged far-field plumes: the top spreads much wider into a sheet that joins its
			// neighbours' - the "thick cloud dominating the sky" of the reference pall shots)
			const double R = bInterior ? R0 + 0.12 * D + 0.06 * Z + 0.05 * H * SmoothStep(0.7, 1.0, Z / H)
				: Cell > 0.0 ? R0 + 0.1 * D + 0.15 * Z + 0.6 * H * SmoothStep(0.45, 1.0, Z / H)
				: R0 + 0.1 * D + 0.12 * Z + 0.15 * H * SmoothStep(0.7, 1.0, Z / H);
			const uint32 Seed = static_cast<uint32>(Wraps - I);
			const double H1 = Hash01(S.Key, Seed * 4u + 1u);
			const double H2 = Hash01(S.Key, Seed * 4u + 2u);
			const double H3 = Hash01(S.Key, Seed * 4u + 3u);
			// merged: puffs leave from anywhere along the cell's front (a wall of smoke, not one column)
			const double Lat = (H1 - 0.5) * (R * 0.9 + (Cell > 0.0 ? 0.9 * Cell : 0.0));
			const double Wx = S.X + Ux * D - Uy * Lat;
			const double Wy = S.Y + Uy * D + Ux * Lat;
			const double Wz = Gz + Z + R * 0.5 + (H2 - 0.5) * R * 0.5;

			// Fade in as the column forms (no discs on the ground), thin out downwind.
			// (and by height: low puffs in a shaded valley read as a row of grey discs at the column base)
			// (v2, the lab: fading over 7 R0 / 3 R0 left the lower column empty - the plumes floated
			// as clouds with nothing under them; the reference smoke is thick right off the flames)
			const double Life01 = SmoothStep(0.0, 2.5 * R0, FMath::Sqrt(D * D + Z * Z)) * SmoothStep(-0.5 * R0, 1.2 * R0, Z)
				* (1.0 - SmoothStep(0.5, 1.0, F));
			const double Thin = FMath::Max(0.25, FMath::Sqrt(R0 / R));
			FPuff P;
			P.Loc = Terrain->WorldToUE(Wx, Wy, Wz);
			P.RadiusM = R;
			P.Stretch = 1.0 + 0.6 * H1;  // elongated, so no card reads as a disc
			P.Roll = H3 * 2.0 * PI;
			P.Data[0] = static_cast<float>(Base * Life01 * Thin * Pack);
			P.Data[1] = static_cast<float>(0.5 * Hot * FMath::Exp(-Z / HotZ));
			P.Data[2] = static_cast<float>(FMath::FloorToInt32(H2 * 4.0) & 3);
			P.Data[3] = static_cast<float>(R * 40.0);  // depth fade (cm): 0.4 x radius
			P.Dist2 = FVector::DistSquared(P.Loc, CameraLoc);
			// A card around the camera would fill the screen: fade puffs we are inside.
			const double DistM = FMath::Sqrt(P.Dist2) / 100.0;
			// v1: and thin big puffs as the camera nears them - inside a heavy plume the stacked cards
			// made a solid wall; the reference pall still shows the terrain through it.
			P.Data[0] *= static_cast<float>(SmoothStep(0.6 * R, 1.6 * R, DistM) * (0.35 + 0.65 * SmoothStep(0.0, 4.0 * R, DistM)));
			All.Add(P);
		}
	}
	// Back to front: translucent instances blend in buffer order.
	All.Sort([](const FPuff& A, const FPuff& B) { return A.Dist2 > B.Dist2; });

	TArray<FTransform> Xf;
	Xf.Reserve(All.Num());
	for (const FPuff& P : All)
	{
		const FVector ToCam = (CameraLoc - P.Loc).GetSafeNormal();
		const FVector Right = CamRight.RotateAngleAxisRad(P.Roll, ToCam);
		const FQuat Q = FRotationMatrix::MakeFromZX(ToCam, Right).ToQuat();
		const double Scale = 2.0 * P.RadiusM;  // the engine plane is 1 m square
		Xf.Add(FTransform(Q, P.Loc, FVector(Scale * P.Stretch, Scale, 1.0)));
	}
	Puffs->ClearInstances();
	Puffs->AddInstances(Xf, false, true);
	for (int32 I = 0; I < All.Num(); ++I)
	{
		Puffs->SetCustomData(I, TArrayView<const float>(All[I].Data, 4), false);
	}
	Puffs->MarkRenderStateDirty();
	NumPuffs = All.Num();
}
