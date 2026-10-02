#include "EmberFlameActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "EmberFireActor.h"
#include "EmberTerrainActor.h"

namespace
{
	/** Integer hash -> [0, 1): per-site / per-lick seeds that do not depend on evaluation order. */
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

	// Per intensity class (index 1..3). v2 (Brad 2026-10-01: "too tall and separate"; the reference
	// surface front: a knee-to-head-high ragged line of flame a few metres deep).
	constexpr double FlameL[4] = {0.0, 0.6, 1.6, 2.8};    // surface flame length (m)
	constexpr double BandM[4] = {0.0, 2.0, 4.0, 8.0};     // flaming depth behind the front line (m)
	constexpr double SiteM[4] = {0.0, 0.9, 1.4, 2.0};     // fuel-point spacing near the camera (m)
	constexpr double Persist[4] = {0.0, 0.06, 0.12, 0.2}; // share of points that keep burning (logs, stumps)
	// Crown fire (class 3): a share of the points torch - tall licks over the trees, a deeper band.
	constexpr double CrownShare = 0.07;   // (0.18: tall pale licks filled the gaps between trees)
	constexpr double CrownL = 12.0;
	constexpr double CrownBandM = 20.0;
	// Source-driven (ADR 0010): share of points torching per source crown class, and the cap on
	// ground flames under a crowning canopy.
	constexpr double SourcePassiveShare = 0.25;
	constexpr double SourceActiveShare = 0.6;
	constexpr double GroundUnderCrownM = 2.5;
	constexpr int32 MaxSitesPerCell = 500;
	// M_Flame instance data: intensity, seed, half height, half width, card centre (UE cm), lean x / y
	constexpr int32 DataFloats = 9;   // + the lean (tan, along UE X / Y) - flames v3
}

AEmberFlameActor::AEmberFlameActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberFlameActor::Init(AEmberTerrainActor* InTerrain, AEmberFireActor* InFire, FString& OutError)
{
	Terrain = InTerrain;
	Fire = InFire;
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ember/Generated/M_Flame.M_Flame"));
	if (!Terrain || !Fire || !Plane || !Mat)
	{
		OutError = TEXT("flames need the terrain, the fire, /Engine/BasicShapes/Plane and M_Flame (ember-dev regen-assets)");
		return false;
	}
	Mid = UMaterialInstanceDynamic::Create(Mat, this);
	Cards = NewObject<UInstancedStaticMeshComponent>(this);
	Cards->SetStaticMesh(Plane);
	Cards->SetMaterial(0, Mid);
	Cards->NumCustomDataFloats = DataFloats;
	Cards->SetCastShadow(false);
	Cards->bAffectDistanceFieldLighting = false;
	Cards->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Cards->SetupAttachment(RootComponent);
	Cards->RegisterComponent();
	return true;
}

void AEmberFlameActor::SetFireTime(double Seconds)
{
	if (Mid)
	{
		Mid->SetScalarParameterValue(TEXT("FireTime"), static_cast<float>(Seconds));
	}
	if (!(Seconds > PoolClock && Seconds <= PoolClock + 2.0))
	{
		bResetPool = true;
	}
	ClockS = Seconds;
}

void AEmberFlameActor::Rebuild(const FVector& CameraLoc)
{
	if (!Cards || !Terrain || !Fire)
	{
		return;
	}
	const emberworld::Frame& Fr = Terrain->GetFrame();
	const double CamX = Fr.anchor_x + CameraLoc.X / 100.0;
	const double CamY = Fr.anchor_y - CameraLoc.Y / 100.0;
	const double CellM = Fire->CellM;
	const double SimS = Fire->TimeS;
	TArray<FEmberFlameCell> Cells;
	Fire->FlameCellsNear(CamX, CamY, RadiusM, 3.0 * 3600.0, Cells);

	const bool bReset = bResetPool;
	const double Dt = bReset ? 0.0 : FMath::Max(0.0, ClockS - PoolClock);
	if (bReset)
	{
		Pool.Reset();
		Sites.Reset();
	}
	bResetPool = false;
	PoolClock = ClockS;

	// Age the pool; expired licks free their slots.
	{
		int32 W = 0;
		for (int32 I = 0; I < Pool.Num(); ++I)
		{
			FLick L = Pool[I];
			L.Age += static_cast<float>(Dt);
			if (L.Age < L.Life)
			{
				Pool[W++] = L;
			}
		}
		Pool.SetNum(W, EAllowShrinking::No);
	}

	// Wind (the fire's own, 10 m): flames lean downwind with mid-flame wind ~half of it.
	const double Wu = Fire->WindU, Wv = Fire->WindV;
	const double Ws = FMath::Sqrt(Wu * Wu + Wv * Wv);
	const double Dx = Ws > 0.1 ? Wu / Ws : 0.0, Dy = Ws > 0.1 ? Wv / Ws : 0.0;

	auto Spawn = [&](uint64 Key, FSite& St, double X, double Y, double L, double Wf, double I, double BaseUp,
		double DirE, double DirN, double LeanTan, double Age, bool bFrac = false)
	{
		if (Pool.Num() >= MaxLicks)
		{
			return;
		}
		double Gz = 0.0;
		if (!Terrain->SurfaceAt(X, Y, Gz) && !Terrain->GroundHeightAt(X, Y, Gz))
		{
			return;
		}
		const uint32 S = St.Born++;
		const uint32 K32 = static_cast<uint32>(Key ^ (Key >> 32));
		const double H1 = Hash01(K32, S * 4u + 1u), H2 = Hash01(K32, S * 4u + 2u), H3 = Hash01(K32, S * 4u + 3u);
		FLick Lk;
		Lk.Site = Key;
		// a lick: most are well under the flame length, a few reach past it
		Lk.H = static_cast<float>(L * FMath::Lerp(0.5, 1.2, H1 * H1));
		Lk.W = static_cast<float>(Wf * FMath::Lerp(0.8, 1.2, H2));
		Lk.I = static_cast<float>(I * FMath::Lerp(0.7, 1.0, H3));
		Lk.Life = static_cast<float>((0.3 + 0.3 * FMath::Sqrt(Lk.H)) * FMath::Lerp(0.75, 1.3, H2));
		// (a re-seed passes a fraction of the lick's own life: an absolute age past a short life left it fading)
		Lk.Age = static_cast<float>(bFrac ? Age * Lk.Life : FMath::Min(Age, 0.95 * Lk.Life));
		// licks start anywhere in a small patch around the fuel point
		Lk.Ox = X + (H3 - 0.5) * Wf;
		Lk.Oy = Y + (H1 - 0.5) * Wf;
		Lk.Gz = Gz;
		Lk.BaseUp = static_cast<float>(BaseUp);
		// lean: toward the spread (and the wind) - the whole tongue tilts (M_Flame's axis), and it
		// rises along that axis; a little per-lick jitter so a front is not a combed row
		Lk.Lean = static_cast<float>(LeanTan * FMath::Lerp(0.8, 1.15, H2));
		Lk.Lx = static_cast<float>(DirE);
		Lk.Ly = static_cast<float>(DirN);
		Lk.Seed = static_cast<float>(H1 * 0.7 + H3 * 0.3);
		Pool.Add(Lk);
	};

	// Fuel points: a fixed, hashed list per cell (the first n are used, so changing n with distance
	// only adds / drops points). Each knows when the front reaches it (arrival interpolated between
	// cell centres) and how fast the front moves there (the arrival gradient).
	NumSites = 0;
	double NearestD2 = TNumericLimits<double>::Max();
	NearestX = NearestY = 0.0;
	for (const FEmberFlameCell& C : Cells)
	{
		const double DistC = FMath::Sqrt(FMath::Square(C.X - CamX) + FMath::Square(C.Y - CamY));
		const double Spacing = FMath::Max(SiteM[C.Cls], 0.006 * DistC);   // ~ a few pixels apart far off
		const int32 N = FMath::Clamp(FMath::FloorToInt32(FMath::Square(CellM / Spacing)), 4, MaxSitesPerCell);
		double MaxA = C.A[0];
		for (int32 K = 1; K < 9; ++K)
		{
			MaxA = FMath::Max(MaxA, C.A[K]);
		}
		// the front left this neighbourhood over an hour ago: only the long burners matter
		const bool bBehind = SimS - MaxA > 3600.0;
		const int32 NRes = FMath::CeilToInt32(N * Persist[C.Cls]);
		const int32 NUse = bBehind ? NRes : N;
		for (int32 K = 0; K < NUse; ++K)
		{
			const double H1 = Hash01(C.Index, K * 6u + 1u);
			const double H2 = Hash01(C.Index, K * 6u + 2u);
			const double U = H1 - 0.5, V = H2 - 0.5;          // cell units, east / south of the centre
			// arrival and its gradient: bilinear in the quadrant of the 3 x 3 centres around the point
			const double Fx = 1.0 + U, Fy = 1.0 + V;
			const int32 I0 = Fx < 1.0 ? 0 : 1, J0 = Fy < 1.0 ? 0 : 1;
			const double Tx = Fx - I0, Ty = Fy - J0;
			const double A00 = C.A[J0 * 3 + I0], A10 = C.A[J0 * 3 + I0 + 1];
			const double A01 = C.A[(J0 + 1) * 3 + I0], A11 = C.A[(J0 + 1) * 3 + I0 + 1];
			const double Arr = FMath::Lerp(FMath::Lerp(A00, A10, Tx), FMath::Lerp(A01, A11, Tx), Ty);
			const double Since = SimS - Arr;
			if (Since < 0.0)
			{
				continue;                                     // the front is not here yet
			}
			const double Gx = FMath::Lerp(A10 - A00, A11 - A01, Ty) / CellM;
			const double Gy = FMath::Lerp(A01 - A00, A11 - A10, Tx) / CellM;
			const double Ros = FMath::Clamp(1.0 / FMath::Max(1e-6, FMath::Sqrt(Gx * Gx + Gy * Gy)), 0.002, 3.0);   // m/s
			const double Behind = Since * Ros;                // metres behind the front line
			// Where the source's channels speak (ADR 0010): its flame length is the lick length and its
			// crown class decides how many points torch (surface none, passive a quarter, active most);
			// under a crowning canopy the ground flames are capped (the source's number is the crown
			// flame, not a wall of ground fire). Elsewhere: the class rules of flames v2.
			const bool bSource = C.SourceFlameM >= 0.f;
			const double Share = bSource ? (C.SourceCrown == 2 ? SourceActiveShare : C.SourceCrown == 1 ? SourcePassiveShare : 0.0)
				: (C.Cls == 3 ? CrownShare : 0.0);
			const bool bCrown = Share > 0.0 && Hash01(C.Index, K * 6u + 5u) < Share;
			const double Band = (bCrown ? CrownBandM : BandM[C.Cls]) * FMath::Lerp(0.8, 1.4, static_cast<double>(C.Spread));
			// the front: flames rise fast at the line and die down across the band
			double I = SmoothStep(0.0, 0.12 * Band, Behind) * (1.0 - SmoothStep(0.5 * Band, Band, Behind));
			double Lm = bSource
				? (bCrown || C.SourceCrown <= 0 ? C.SourceFlameM : FMath::Min<double>(C.SourceFlameM, GroundUnderCrownM))
				: (bCrown ? CrownL : FlameL[C.Cls]) * FMath::Lerp(0.8, 1.3, static_cast<double>(C.Spread));
			Lm = FMath::Max(Lm, 0.15);
			bool bResidual = false;
			if (K < NRes && Behind >= 0.5 * Band)
			{
				// long burners (logs, stumps, a snag): small flames for 15 - 45 min after the front
				const double Tau = 900.0 * (1.0 + 2.0 * Hash01(C.Index, K * 6u + 4u));
				const double Res = 0.45 * FMath::Exp(-FMath::Max(0.0, Since - Band / Ros) / Tau);
				if (Res > I)
				{
					I = Res;
					Lm = FlameL[C.Cls] * FMath::Lerp(0.5, 1.0, Hash01(C.Index, K * 6u + 3u));
					bResidual = true;              // a long burner: on the ground, not in the crowns
				}
			}
			if (I < 0.06)
			{
				continue;
			}
			++NumSites;
			{
				const double Dn = FMath::Square(C.X + U * CellM - CamX) + FMath::Square(C.Y - V * CellM - CamY);
				if (I > 0.3 && Dn < NearestD2)
				{
					NearestD2 = Dn;
					NearestX = C.X + U * CellM;
					NearestY = C.Y - V * CellM;
				}
			}
			const double X = C.X + U * CellM;
			const double Y = C.Y - V * CellM;
			Lm *= FMath::Lerp(0.7, 1.2, Hash01(C.Index, K * 6u + 3u)) * FMath::Lerp(0.6, 1.0, I);
			// Flames v3 (Brad 2026-10-02: "in torching and crowns you'd see it in the tree tops"):
			// crown / torching licks start at the crown base (the pack's canopy base height; a quarter
			// of the canopy height where that is missing). The flame length is measured from the
			// GROUND (Brad: "Ground + Flame Height != Ground + Flame Height + Offset"), so a crown lick
			// runs from the crown base to ground + flame length; a flame that does not reach the crown
			// base is not torching - it burns on the ground at its own length. Without canopy data,
			// crown licks stay on the ground (v2).
			bool bCrownLick = bCrown && !bResidual;
			double BaseUp = 0.0;
			if (bCrownLick && C.CanopyHeightM > 2.f)
			{
				const double Ch = C.CanopyHeightM;
				const double Cb = C.CanopyBaseM > 0.5f ? FMath::Min<double>(C.CanopyBaseM, 0.8 * Ch) : 0.25 * Ch;
				const double Base = Cb * FMath::Lerp(0.85, 1.05, Hash01(C.Index, K * 6u + 4u));
				if (Lm > Base + 1.0)
				{
					BaseUp = Base;
					Lm -= Base;                                   // the top stays at ground + flame length
				}
				else
				{
					bCrownLick = false;                           // short of the crowns: a ground flame
				}
			}
			// Lean (Brad: "fire tends to get pushed by the wind ... lean the fire towards [the next cell
			// that burns]"): toward the local spread direction (the arrival gradient), more for a fast
			// front, more again in the crowns; the replay's wind, where it has one, pushes too.
			double DirE = 0.0, DirN = 0.0;
			{
				const double Gn = FMath::Sqrt(Gx * Gx + Gy * Gy);
				if (Gn > 1e-9)
				{
					DirE = Gx / Gn;
					DirN = -Gy / Gn;                              // Gy runs south (row order)
				}
				if (Ws > 0.5)
				{
					DirE += 0.7 * Dx;
					DirN += 0.7 * Dy;
					const double Dn = FMath::Sqrt(DirE * DirE + DirN * DirN);
					DirE = Dn > 1e-6 ? DirE / Dn : 0.0;
					DirN = Dn > 1e-6 ? DirN / Dn : 0.0;
				}
			}
			const double LeanTan = FMath::Clamp(0.3 + 0.55 * FMath::Sqrt(Ros) + 0.04 * Ws, 0.2, 1.2)
				* (bCrownLick ? 1.2 : 1.0) * ((DirE != 0.0 || DirN != 0.0) ? 1.0 : 0.0);
			// Fuller tongues (v3: "thin and transparent"): ~0.42 of their height (0.35 in the crowns);
			// far off they widen toward the point spacing so the line stays continuous.
			const double Wf = FMath::Min(FMath::Max((bCrownLick ? 0.35 : 0.42) * Lm, 0.5 * Spacing), 0.7 * Lm);
			// The body now occludes (M_Flame alpha-blended, v3), so stacked rows no longer add up to a
			// white sheet: only a mild thinning for deep bands, not the v2 additive share.
			const double Stack = FMath::Clamp(4.0 * Spacing / Band, 0.6, 1.0);
			I *= 0.9 * Stack;
			const uint64 Key = (static_cast<uint64>(C.Index) << 12) | static_cast<uint64>(K);
			const double LifeMean = 0.3 + 0.3 * FMath::Sqrt(0.8 * Lm);
			const double Rate = 2.0 / LifeMean;               // ~2 licks alive per point
			FSite* St = Sites.Find(Key);
			if (!St)
			{
				St = &Sites.Add(Key);
				const double Ph = Hash01(C.Index, K * 6u + 6u);
				St->Acc = Ph;
				if (bReset)
				{
					// a still / scrub: the point is already burning - two licks mid-life
					Spawn(Key, *St, X, Y, Lm, Wf, I, BaseUp, DirE, DirN, LeanTan, 0.05 + 0.45 * Ph, true);
					Spawn(Key, *St, X, Y, Lm, Wf, I, BaseUp, DirE, DirN, LeanTan, 0.5 + 0.45 * Ph, true);
				}
			}
			St->LastSeen = ClockS;
			St->Acc += Dt * Rate;
			while (St->Acc >= 1.0)
			{
				St->Acc -= 1.0;
				Spawn(Key, *St, X, Y, Lm, Wf, I, BaseUp, DirE, DirN, LeanTan, St->Acc / Rate);
			}
		}
	}
	for (auto It = Sites.CreateIterator(); It; ++It)
	{
		if (ClockS - It.Value().LastSeen > 2.0)
		{
			It.RemoveCurrent();
		}
	}

	// Lay the licks out: each from its own age - it grows, lifts off, leans downwind, fades.
	// v3: M_Flame is alpha-blended (the body occludes), so the cards go back to front.
	struct FOut { FTransform Xf; float D[DataFloats]; double Dist2; };
	TArray<FOut> Outs;
	Outs.Reserve(Pool.Num());
	for (const FLick& L : Pool)
	{
		const double Uf = L.Age / L.Life;
		const double Hh = L.H * (0.6 + 0.4 * SmoothStep(0.0, 0.35, Uf));   // grows, never shrinks
		const double Lift = 0.55 * L.H * Uf * Uf;                           // tears off and rises
		const double Up = Lift + 0.5 * Hh;
		const double X = L.Ox + L.Lx * Up * L.Lean;
		const double Y = L.Oy + L.Ly * Up * L.Lean;
		const double DistM = FMath::Sqrt(FMath::Square(X - CamX) + FMath::Square(Y - CamY));
		const double Far = 1.0 - FMath::Clamp((DistM - FadeStartM) / (RadiusM - FadeStartM), 0.0, 1.0);
		// a lick at the lens would fill the frame with a glow column: fade it out instead
		const double Near = SmoothStep(FMath::Max(2.0, 0.7 * Hh), FMath::Max(4.0, 1.5 * Hh), DistM);
		const double Alpha = L.I * SmoothStep(0.0, 0.1, Uf) * (1.0 - SmoothStep(0.55, 1.0, Uf)) * Far * Near;
		if (Alpha < 0.01)
		{
			continue;
		}
		const double Wf = L.W * (0.85 + 0.3 * Uf);
		// Base a little below the surface (the card's depth fade melts it into the ground); crown
		// licks start at their base height in the canopy (v3).
		const FVector Centre = Terrain->WorldToUE(X, Y, L.Gz + L.BaseUp - 0.08 * Hh + Up);
		FVector ToCam = CameraLoc - Centre;
		ToCam.Z = 0.0;
		if (!ToCam.Normalize())
		{
			ToCam = FVector::ForwardVector;
		}
		// The flame's axis leans (v3): east / north -> UE +X / -Y. The card contains that axis (local
		// X along it) and turns to the camera as far as it can (local Z = the view direction made
		// perpendicular to the axis); M_Flame draws the tongue along the same axis.
		const double LeanUx = L.Lx * L.Lean, LeanUy = -L.Ly * L.Lean;
		const FVector Axis = FVector(LeanUx, LeanUy, 1.0).GetSafeNormal();
		const double AxisLen = FMath::Sqrt(1.0 + L.Lean * L.Lean);
		FVector Normal = ToCam - Axis * FVector::DotProduct(ToCam, Axis);
		if (!Normal.Normalize())
		{
			Normal = ToCam;
		}
		const FQuat Q = FRotationMatrix::MakeFromZX(Normal, Axis).ToQuat();
		// The flame's shape is defined in world units from the centre (M_Flame); the card is
		// oversized so the tongue never meets its edge (a clipped card reads as a rectangle).
		FOut& O = Outs.AddDefaulted_GetRef();
		O.Xf = FTransform(Q, Centre, FVector(Hh * 1.3 * AxisLen, Wf * 2.0, 1.0));   // the engine plane is 1 m
		const float D[DataFloats] = {static_cast<float>(Alpha), L.Seed, static_cast<float>(Hh * 50.0),
			static_cast<float>(Wf * 50.0), static_cast<float>(Centre.X), static_cast<float>(Centre.Y),
			static_cast<float>(Centre.Z), static_cast<float>(LeanUx), static_cast<float>(LeanUy)};
		FMemory::Memcpy(O.D, D, sizeof(D));
		O.Dist2 = FVector::DistSquared(Centre, CameraLoc);
	}
	Outs.Sort([](const FOut& A, const FOut& B) { return A.Dist2 > B.Dist2; });
	TArray<FTransform> Xf;
	Xf.Reserve(Outs.Num());
	for (const FOut& O : Outs)
	{
		Xf.Add(O.Xf);
	}
	Cards->ClearInstances();
	Cards->AddInstances(Xf, false, true);
	for (int32 I = 0; I < Outs.Num(); ++I)
	{
		Cards->SetCustomData(I, TArrayView<const float>(Outs[I].D, DataFloats), false);
	}
	Cards->MarkRenderStateDirty();
	NumCells = Cells.Num();
	NumCards = Xf.Num();
}
