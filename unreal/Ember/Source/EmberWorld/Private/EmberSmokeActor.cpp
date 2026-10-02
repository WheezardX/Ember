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
	// Cross-faded, not switched (Brad: columns "pop" - a hard 3 km line and hard cell-size steps
	// swapped a plume between its own column and a merged one in one frame): a source fades out of
	// its own column over 0.8-1.2 x MergeNearM while fading into its cell, and near the top of each
	// distance octave it shares itself between this cell size and the next. Its smoke counts into
	// each cell by the weight it gives it, so merged plumes thicken / thin smoothly.
	struct FDraw { FEmberSmokeSource S; double CellM = 0.0; double W = 0.0; float Alpha = 1.f; };
	TArray<FDraw> Draw;
	TMap<uint64, int32> CellIndex;
	auto AddToCell = [&](const FEmberSmokeSource& S, int32 Level, double Wt)
	{
		const double Cell = 600.0 * (1 << Level);
		const int64 Cx = FMath::FloorToInt64(S.X / Cell), Cy = FMath::FloorToInt64(S.Y / Cell);
		const uint64 K = (static_cast<uint64>(Level) << 60) ^ (static_cast<uint64>(Cx & 0x3FFFFFFF) << 30) ^ static_cast<uint64>(Cy & 0x3FFFFFFF);
		const double Q = FMath::Max(0.01, static_cast<double>(S.Strength + S.Heat + S.Smoulder)) * Wt;
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
		D.S.Strength += S.Strength * Wt;
		D.S.Heat += S.Heat * Wt;
		D.S.Smoulder += S.Smoulder * Wt;
		D.S.Burning += S.Burning * Wt;
		D.S.Cluster = FMath::Max(D.S.Cluster, S.Cluster);
		D.S.HeatCluster = FMath::Max(D.S.HeatCluster, S.HeatCluster);
	};
	for (const FEmberSmokeSource& S : Sources)
	{
		const double Dist = FVector2D::Distance(Cam, FVector2D(S.X, S.Y));
		const double Own = 1.0 - SmoothStep(0.8 * MergeNearM, 1.2 * MergeNearM, Dist);
		if (Own > 0.02)
		{
			Draw.Add({S, 0.0, 0.0, static_cast<float>(Own)});
		}
		if (Own > 0.98)
		{
			continue;
		}
		const double Lf = FMath::Max(0.0, FMath::Log2(FMath::Max(Dist, 1.0) / MergeNearM));
		const int32 Level = FMath::Clamp(FMath::FloorToInt32(Lf), 0, 3);
		const bool bWeak = S.Strength < 0.3 && S.Heat <= 0.f;
		if (bWeak && Level >= 1)
		{
			continue;
		}
		const double Up = Level < 3 ? SmoothStep(0.75, 1.0, Lf - Level) : 0.0;   // share with the next size
		AddToCell(S, Level, (1.0 - Own) * (1.0 - Up));
		if (Up > 0.02)
		{
			AddToCell(S, Level + 1, (1.0 - Own) * Up);
		}
	}
	for (FDraw& D : Draw)
	{
		if (D.CellM > 0.0)
		{
			D.S.X /= FMath::Max(1e-6, D.W);
			D.S.Y /= FMath::Max(1e-6, D.W);
			++NumMerged;
		}
	}
	// strongest first (the puff budget goes to the plumes that matter)
	Draw.Sort([](const FDraw& A, const FDraw& B)
	{
		const double Qa = A.S.Strength + A.S.Heat, Qb = B.S.Strength + B.S.Heat;
		return Qa != Qb ? Qa > Qb : A.S.Key < B.S.Key;
	});

	// v3 card pool (Brad 2026-10-01, after v2: "it looks like the smoke is animating back into the
	// ground at least 50% of the time ... mid and far distances are very bouncy"; "create a card
	// pool and let cards rise up, fade out and expire and then recycle cards ... cards should only
	// ever grow, never shrink"). v2 re-derived every puff from the CURRENT plume each frame: with the
	// fire playing at hours per second the plumes' heights, rise times and (far off) merged-cell
	// centroids changed every frame, and every puff moved with them - half the time downward.
	// Now each emitter (a source's own column, or a far-field cell) spawns cards at its base at a
	// steady rate; a card freezes its plume at birth and from then on moves by its own age only.
	struct FEmit
	{
		int32 Key = 0;
		double X = 0.0, Y = 0.0, Gz = 0.0;
		bool bCell = false, bInterior = false, bSmoulder = false;
		double H = 0.0, Tau = 1.0, L = 0.0, R0 = 0.0, Cell = 0.0, Base = 0.0, Hot = 0.0, HotZ = 1.0;
		double Life = 1.0, Tcol = 1.0, Ldrift = 1.0;
		int32 Kc = 0, Kd = 0;
		double Target = 0.0;     // merge cross-fade x flaming fade
	};
	TArray<FEmit> Emit;
	Emit.Reserve(Draw.Num());
	for (const FDraw& Dw : Draw)
	{
		const FEmberSmokeSource& S = Dw.S;
		const double Cell = Dw.CellM;           // > 0: a merged far-field plume
		// A bin with no flaming front left only smoulders: a low, lazy wisp (v1, reference:
		// IR scattered heat over most of the scar for weeks).
		// Interior (observed heat, no front here): pockets burning inside the scar stand up as their
		// own columns, lower than a running front's (reference: the Kachess aerials, several columns
		// of a few hundred m to ~1.5 km off both shores days after the front passed).
		// (the flaming column fades out over strength 0.8 -> 0.3 instead of vanishing at 0.5 - that
		// switch popped a tall column into a wisp in one frame)
		const bool bInterior = S.Strength < 0.3 && S.Heat > 0.f;
		const bool bSmoulder = S.Strength < 0.3 && !bInterior;
		const double Flaming = (bInterior || bSmoulder) ? 1.0 : SmoothStep(0.3, 0.8, S.Strength);
		const double Q = bInterior ? S.Heat : bSmoulder ? S.Smoulder : S.Strength;
		const double SqQ = FMath::Sqrt(Q);
		// Ground under the source: the in-memory tile surface every frame (cheap); the per-bin cache
		// only backs the slow region-DEM fallback (40-70 ms a frame with a few hundred sources).
		double Gz = 0.0;
		if (!Terrain->SurfaceAt(S.X, S.Y, Gz))
		{
			FGroundHit* Hit = GroundCache.Find(S.Key);
			if (Hit && FMath::Abs(Hit->X - S.X) < 20.0 && FMath::Abs(Hit->Y - S.Y) < 20.0)
			{
				Gz = Hit->Z;
			}
			else
			{
				if (!Terrain->GroundHeightAt(S.X, S.Y, Gz))
				{
					continue;
				}
				GroundCache.Add(S.Key, FGroundHit{S.X, S.Y, Gz});
			}
		}
		FEmit E;
		E.Key = S.Key;
		E.X = S.X;
		E.Y = S.Y;
		E.Gz = Gz;
		E.bCell = Cell > 0.0;
		E.bInterior = bInterior;
		E.bSmoulder = bSmoulder;
		E.Cell = Cell;
		// Neighbouring bins burn as one convective column: height from the 1.5 km cluster.
		// Columns (v1): the reference columns stand 1-3 km+ over every active area; 90 x sqrt
		// (cluster) gave ~500 m tops on the real Three Queens record.
		E.H = bInterior ? FMath::Clamp(90.0 * FMath::Sqrt(static_cast<double>(S.HeatCluster)), 200.0, 1200.0)
			: bSmoulder ? FMath::Clamp(60.0 + 40.0 * SqQ, 60.0, 300.0)
			: FMath::Clamp(250.0 * FMath::Sqrt(static_cast<double>(S.Cluster)), 300.0, 5000.0);
		MaxTopM = FMath::Max(MaxTopM, E.H);
		E.L = bSmoulder ? FMath::Clamp(10.0 * E.H, 600.0, 3000.0)
			: FMath::Clamp(6.0 * E.H, bInterior ? 1200.0 : 1500.0, bInterior ? 10000.0 : 18000.0);  // plume length before it thins out
		// Buoyant rise (v1, reference: the columns over Kachess stand near-vertical for a km or
		// more before they lean): rise speed W0 against the wind, e-folding time Tau to the top.
		const double W0 = bSmoulder ? 1.5 : (bInterior ? 4.0 : 6.0) + 3.0 * FMath::Sqrt(E.H / 100.0);
		E.Tau = E.H / W0;
		E.Life = FMath::Max(E.L / WindMs, 3.0 * E.Tau);        // a wisp card's lifetime (s)
		E.R0 = bSmoulder ? 40.0 : bInterior ? 45.0 + 6.0 * SqQ : 60.0 + 7.0 * SqQ;  // ~a bin across at full strength
		E.Base = bSmoulder ? 0.22 * (1.0 - FMath::Exp(-Q / 3.0))
			: (bInterior ? 0.75 : 0.9) * (1.0 - FMath::Exp(-Q / 4.0));  // dense columns, faint wisps
		if (E.bCell)
		{
			// merged: as wide as a good part of its cell, and denser (several columns' worth of smoke)
			E.R0 = FMath::Max(E.R0, 0.3 * Cell);
			E.Base = FMath::Min(0.97, E.Base * 1.15 + 0.05);
		}
		// Two kinds of card per column (v2, the lab: one stream spaced evenly in time left the fast-
		// rising lower column empty and the plumes floated): COLUMN cards climb the column at a
		// constant speed (so they are evenly spaced in height) to 0.95 H in Tcol, DRIFT cards leave the
		// top and travel downwind for the rest of the plume. Fixed counts per plume type.
		E.Kc = bSmoulder ? 0 : E.bCell ? 24 : bInterior ? 16 : 28;
		E.Kd = bSmoulder ? 8 : E.bCell ? 40 : bInterior ? 28 : 64;
		if (E.Kc > 0)
		{
			E.R0 = FMath::Max(E.R0, 0.6 * E.H / E.Kc);   // column cards at least overlap their spacing
		}
		E.Tcol = 1.5 * E.Tau;
		E.Ldrift = bSmoulder ? E.Life : FMath::Max(E.Life - E.Tcol, E.Tau);
		// Interior pockets light their own smoke from below too (the night reference: the smoke over
		// the burning slope glows orange)
		E.Hot = bInterior ? 0.6 * FMath::Min(1.0, Q / 8.0)
			: FMath::Min(1.0, S.Burning / FMath::Max(1.0, Q)) * FMath::Min(1.0, S.Burning / 5.0);
		E.HotZ = bInterior ? 0.4 * E.H : E.R0 + 80.0;   // how far up the glow reaches
		E.Target = Dw.Alpha * Flaming;
		Emit.Add(E);
	}

	// Advance the pool. A small clock step ages it; anything else (SetClock) re-seeds it below.
	const bool bReset = bResetPool;
	const double Dt = bReset ? 0.0 : FMath::Max(0.0, ClockS - PoolClock);
	if (bReset)
	{
		Pool.Reset();
		Emitters.Reset();
		++NumResets;
	}
	bResetPool = false;
	PoolClock = ClockS;
	const double VisK = 1.0 - FMath::Exp(-Dt / 1.0);    // emitter fades: ~1 s
	for (const FEmit& E : Emit)
	{
		if (FEmitterState* St = Emitters.Find(E.Key))
		{
			St->LastSeen = ClockS;
			St->Vis += (E.Target - St->Vis) * VisK;
		}
	}
	for (auto It = Emitters.CreateIterator(); It; ++It)
	{
		if (It.Value().LastSeen < ClockS)
		{
			It.Value().Vis *= 1.0 - VisK;                   // gone this frame: its cards fade
			if (ClockS - It.Value().LastSeen > 5.0)
			{
				It.RemoveCurrent();                         // back after this: a fresh emitter
			}
		}
	}
	{
		int32 W = 0;
		for (int32 I = 0; I < Pool.Num(); ++I)
		{
			FPuffCard C = Pool[I];
			C.Age += static_cast<float>(Dt);
			const FEmitterState* St = Emitters.Find(C.Emitter);
			C.Vis = St ? static_cast<float>(St->Vis) : C.Vis * static_cast<float>(1.0 - VisK);
			if (C.Age >= C.LifeS || (!St && C.Vis < 0.003f))
			{
				continue;                                   // expired: the slot is recycled
			}
			Pool[W++] = C;
		}
		Pool.SetNum(W, EAllowShrinking::No);
	}

	// Spawn. A new emitter (or every emitter on a re-seed) starts with a full plume: cards at
	// evenly spread ages (fading in over ~1 s unless this is a still); after that it adds a card
	// each time its accumulator passes one, aged by the fraction of the step since then.
	const double WindNow = WindMs;
	auto Spawn = [&](const FEmit& E, FEmitterState& St, uint8 Kind, double Age, double Vis)
	{
		if (Pool.Num() >= MaxPuffs)
		{
			return;
		}
		FPuffCard C;
		C.Emitter = E.Key;
		C.Kind = Kind;
		C.bCell = E.bCell;
		C.bInterior = E.bInterior;
		C.Age = static_cast<float>(Age);
		C.LifeS = static_cast<float>(Kind == 0 ? E.Tcol : E.Ldrift);
		C.Ox = E.X;
		C.Oy = E.Y;
		C.Gz = E.Gz;
		C.Ux = static_cast<float>(Ux);
		C.Uy = static_cast<float>(Uy);
		C.Wind = static_cast<float>(WindNow);
		C.H = static_cast<float>(E.H);
		C.Tau = static_cast<float>(E.Tau);
		C.L = static_cast<float>(E.L);
		C.R0 = static_cast<float>(E.R0);
		C.Cell = static_cast<float>(E.Cell);
		C.Tcol = static_cast<float>(E.Tcol);
		C.Ldrift = static_cast<float>(E.Ldrift);
		C.Base = static_cast<float>(E.Base);
		C.Hot = static_cast<float>(E.Hot);
		C.HotZ = static_cast<float>(E.HotZ);
		const uint32 Seed = St.Born++;
		C.S1 = static_cast<float>(Hash01(E.Key, Seed * 4u + 1u));
		C.S2 = static_cast<float>(Hash01(E.Key, Seed * 4u + 2u));
		C.S3 = static_cast<float>(Hash01(E.Key, Seed * 4u + 3u));
		C.Vis = static_cast<float>(Vis);
		Pool.Add(C);
	};
	const uint8 DriftKind = 1, WispKind = 2;
	for (const FEmit& E : Emit)
	{
		const uint8 Dk = E.bSmoulder ? WispKind : DriftKind;
		FEmitterState* St = Emitters.Find(E.Key);
		if (!St)
		{
			if (Pool.Num() + E.Kc + E.Kd > MaxPuffs)
			{
				continue;                                   // no room: try again next frame
			}
			FEmitterState& N = Emitters.Add(E.Key);
			N.LastSeen = ClockS;
			N.Vis = bReset ? E.Target : 0.0;
			const double Ph = Hash01(E.Key, 0x51u);
			for (int32 I = 0; I < E.Kc; ++I)
			{
				Spawn(E, N, 0, (I + Ph) / E.Kc * E.Tcol, N.Vis);
			}
			for (int32 I = 0; I < E.Kd; ++I)
			{
				Spawn(E, N, Dk, (I + Ph) / E.Kd * E.Ldrift, N.Vis);
			}
			N.AccCol = N.AccDrift = Ph;
			++NumPlumes;
			continue;
		}
		++NumPlumes;
		if (E.Kc > 0)
		{
			const double Rate = E.Kc / E.Tcol;
			St->AccCol += Dt * Rate;
			while (St->AccCol >= 1.0)
			{
				St->AccCol -= 1.0;
				Spawn(E, *St, 0, St->AccCol / Rate, St->Vis);
			}
		}
		const double Rate = E.Kd / E.Ldrift;
		St->AccDrift += Dt * Rate;
		while (St->AccDrift >= 1.0)
		{
			St->AccDrift -= 1.0;
			Spawn(E, *St, Dk, St->AccDrift / Rate, St->Vis);
		}
	}

	// Lay the cards out: each one's place, size and density from its own age and frozen plume.
	// Monotonic in age: it only rises, drifts on and grows.
	TArray<FPuff> All;
	All.Reserve(Pool.Num());
	for (const FPuffCard& C : Pool)
	{
		const double A = C.Age, H = C.H, Tau = C.Tau, R0 = C.R0;
		double Z, D, Fade;
		if (C.Kind == 0)
		{
			// column: constant climb to 0.95 H, handing over to the drift cards at the top
			const double Sf = A / C.Tcol;
			Z = 0.95 * H * Sf;
			D = FMath::Min(C.Wind * A, static_cast<double>(C.L));
			Fade = 1.0 - SmoothStep(0.9, 1.0, Sf);
		}
		else if (C.Kind == 1)
		{
			// drift: from the column top, the last 5 % of the rise and on downwind
			Z = H * (0.95 + 0.05 * (1.0 - FMath::Exp(-A / Tau)));
			D = FMath::Min(C.Wind * (C.Tcol + A), static_cast<double>(C.L));
			const double F = (C.Tcol + A) / (C.Tcol + C.Ldrift);
			Fade = SmoothStep(0.0, 0.15 * C.Tcol, A) * (1.0 - SmoothStep(0.5, 1.0, F));
		}
		else
		{
			// smoulder wisp: a lazy rise from the ground
			Z = H * (1.0 - FMath::Exp(-A / Tau));
			D = FMath::Min(C.Wind * A, static_cast<double>(C.L));
			Fade = 1.0 - SmoothStep(0.5, 1.0, A / C.LifeS);
		}
		if (C.Kind != 1)
		{
			// Fade in as the column forms (no discs on the ground); the reference smoke is thick right
			// off the flames, so only over a couple of radii.
			Fade *= SmoothStep(0.0, 2.5 * R0, FMath::Sqrt(D * D + Z * Z)) * SmoothStep(-0.5 * R0, 1.2 * R0, Z);
		}
		// Entrainment widens the column as it climbs; the top spreads (cauliflower / anvil).
		// (interior columns stay narrow and spread downwind; merged far-field plumes spread into a
		// sheet that joins their neighbours' - the "thick cloud dominating the sky")
		const double R = C.bInterior ? R0 + 0.12 * D + 0.06 * Z + 0.05 * H * SmoothStep(0.7, 1.0, Z / H)
			: C.bCell ? R0 + 0.1 * D + 0.15 * Z + 0.6 * H * SmoothStep(0.45, 1.0, Z / H)
			: R0 + 0.1 * D + 0.12 * Z + 0.15 * H * SmoothStep(0.7, 1.0, Z / H);
		// merged: cards leave from anywhere along the cell's front (a wall of smoke, not one column)
		const double Lat = (C.S1 - 0.5) * (R * 0.9 + (C.bCell ? 0.9 * C.Cell : 0.0));
		const double Wx = C.Ox + C.Ux * D - C.Uy * Lat;
		const double Wy = C.Oy + C.Uy * D + C.Ux * Lat;
		const double Wz = C.Gz + Z + R * 0.5 + (C.S2 - 0.5) * R * 0.5;
		const double Thin = FMath::Max(0.25, FMath::Sqrt(R0 / R));
		FPuff P;
		P.Loc = Terrain->WorldToUE(Wx, Wy, Wz);
		P.RadiusM = R;
		P.Stretch = 1.0 + 0.6 * C.S1;  // elongated, so no card reads as a disc
		P.Roll = C.S3 * 2.0 * PI;
		P.Data[0] = static_cast<float>(C.Base * Fade * Thin * C.Vis);
		P.Data[1] = static_cast<float>(0.5 * C.Hot * FMath::Exp(-Z / C.HotZ));
		P.Data[2] = static_cast<float>(FMath::FloorToInt32(C.S2 * 4.0) & 3);
		P.Data[3] = static_cast<float>(R * 40.0);  // depth fade (cm): 0.4 x radius
		P.Dist2 = FVector::DistSquared(P.Loc, CameraLoc);
		// A card around the camera would fill the screen: fade cards we are inside, and thin big
		// ones as the camera nears them (inside a heavy plume the stack made a solid wall).
		const double DistM = FMath::Sqrt(P.Dist2) / 100.0;
		P.Data[0] *= static_cast<float>(SmoothStep(0.6 * R, 1.6 * R, DistM) * (0.35 + 0.65 * SmoothStep(0.0, 4.0 * R, DistM)));
		if (P.Data[0] > 0.002f)
		{
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
