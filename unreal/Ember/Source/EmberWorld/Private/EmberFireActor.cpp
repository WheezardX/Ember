#include "EmberFireActor.h"

#include "Engine/Texture2D.h"
#include "RenderingThread.h"
#include "TextureResource.h"

#include "EmberTerrainActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEmberFire, Log, All);

AEmberFireActor::AEmberFireActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

bool AEmberFireActor::Load(const FString& ReplayPath, AEmberTerrainActor* Terrain, FString& OutError)
{
	const emberworld::fire::ReplayInfo Info = emberworld::fire::read_replay(TCHAR_TO_UTF8(*ReplayPath));
	if (!Info.ok())
	{
		OutError = UTF8_TO_TCHAR(Info.error.c_str());
		return false;
	}
	const std::string Err = Stream.open(Info.stream_path);
	if (!Err.empty())
	{
		OutError = UTF8_TO_TCHAR(Err.c_str());
		return false;
	}
	Grid = Info.grid;
	ModelId = UTF8_TO_TCHAR(Info.model_id.c_str());
	T0Unix = Info.t0_unix;
	StartS = Stream.start_s();
	EndS = Stream.end_s();
	Nx = static_cast<int32>(Stream.header().nx);
	Ny = static_cast<int32>(Stream.header().ny);
	CellM = Grid.cell_m;
	if (!Terrain)
	{
		OutError = TEXT("fire needs the terrain's frame");
		return false;
	}
	// Grid top-left in UE; the grid runs +X east and +Y south, as the UE frame does.
	const FVector TopLeft = Terrain->WorldToUE(Grid.origin_x, Grid.origin_y, 0.0);
	Rect = FLinearColor(TopLeft.X, TopLeft.Y, Nx * CellM * 100.0, Ny * CellM * 100.0);

	Texture = UTexture2D::CreateTransient(Nx, Ny, PF_B8G8R8A8, TEXT("EmberFireState"));
	Texture->SRGB = false;                             // data, not colour
	Texture->Filter = TF_Bilinear;                     // soft 30 m cell edges
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->NeverStream = true;
	Texture->UpdateResource();
	Pixels.SetNumZeroed(Nx * Ny * 4);
	{
		// Where it's heading: the head is where the fire moves fastest (log 15 .. 300 m/h -> 0 .. 1).
		const std::vector<float> Rate = emberworld::fire::spread_rate_mh(Stream.final_arrival(),
			static_cast<uint32_t>(Nx), static_cast<uint32_t>(Ny), CellM);
		Spread.SetNumZeroed(Nx * Ny);
		for (int32 I = 0; I < Nx * Ny; ++I)
		{
			const double K = Rate[I] > 0.f ? FMath::Loge(Rate[I] / 15.0) / FMath::Loge(20.0) : 0.0;
			Spread[I] = static_cast<uint8>(FMath::Clamp(K, 0.0, 1.0) * 255.0 + 0.5);
		}
	}
	{
		// Does the stream report intensity? (the playback model writes 1 everywhere)
		const emberworld::fire::State End = Stream.at(EndS);
		for (size_t I = 0; I < End.intensity.size() && !bIntensityReported; ++I)
		{
			bIntensityReported = End.phase[I] >= emberworld::fire::Burning && End.intensity[I] >= 2;
		}
	}
	SpotsTotal = static_cast<int32>(Stream.spots().size());
	SpotsIgnitedTotal = 0;
	for (const emberworld::fire::Spot& S : Stream.spots())
	{
		SpotsIgnitedTotal += S.ignited;
	}
	UE_LOG(LogEmberFire, Log, TEXT("fire replay %s: model %s, %dx%d cells of %.0f m, t %d..%d s, %u ticks"),
		*ReplayPath, *ModelId, Nx, Ny, CellM, StartS, EndS, Stream.ticks());
	SetTime(StartS);
	return true;
}

void AEmberFireActor::SetTime(double TSeconds)
{
	if (!Texture)
	{
		return;
	}
	TimeS = TSeconds;
	State = Stream.at(static_cast<int32>(FMath::FloorToDouble(TSeconds)));
	Tick = State.tick;
	StreamBurned = State.metrics.burned;
	WindU = State.metrics.wind_u_cms / 100.0;
	WindV = State.metrics.wind_v_cms / 100.0;
	for (int64& C : CellsByClass)
	{
		C = 0;
	}
	const std::vector<int32_t>& Arrival = Stream.final_arrival();
	CellsBurning = 0;
	CellsHead = 0;
	CellsBurned = 0;
	const int32 N = Nx * Ny;
	const int32 Bx = (Nx + SmokeBinCells - 1) / SmokeBinCells;
	const int32 By = (Ny + SmokeBinCells - 1) / SmokeBinCells;
	struct FBin { double Sx = 0, Sy = 0, W = 0, Burning = 0; };
	TArray<FBin> Bins;
	Bins.SetNum(Bx * By);
	for (int32 I = 0; I < N; ++I)
	{
		const uint8 Phase = State.phase[I];
		// The front moves continuously: a cell ignites at its exact arrival time (written once in
		// the stream), not at the next tick record - timelapses do not step hour by hour.
		const bool bArrived = Arrival[I] >= 0 && Arrival[I] <= TSeconds;
		const bool bBurning = Phase == emberworld::fire::Burning || (bArrived && Phase == emberworld::fire::Unburned);
		const bool bBurned = Phase == emberworld::fire::Burned || bBurning;
		CellsBurning += bBurning;
		CellsBurned += bBurned;
		// Hours since arrival, square-root encoded: minute-scale at the front, ~8 days at 255.
		uint8 Age = 0;
		if (bBurned && Arrival[I] >= 0)
		{
			const double AgeH = FMath::Max(0.0, (TSeconds - Arrival[I]) / 3600.0);
			Age = static_cast<uint8>(FMath::Clamp(FMath::Sqrt(AgeH / 200.0) * 255.0, 0.0, 255.0));
			// Smoke comes off the front: weight by time since arrival, burning or not.
			const double W = FMath::Exp(-AgeH / SmokeDecayH);
			if (W > 0.01)
			{
				const int32 Cx = I % Nx;
				const int32 Cy = I / Nx;
				FBin& B = Bins[(Cy / SmokeBinCells) * Bx + Cx / SmokeBinCells];
				B.Sx += W * Cx;
				B.Sy += W * Cy;
				B.W += W;
				B.Burning += bBurning;
			}
		}
		// Intensity class 1..3 (a stream that does not report it is drawn as 3: the HCP3 look).
		const int32 Cls = bIntensityReported ? FMath::Clamp(static_cast<int32>(State.intensity[I]), 1, 3) : 3;
		if (bBurning)
		{
			++CellsByClass[Cls];
			CellsHead += Spread[I] >= 128;
		}
		uint8* P = &Pixels[I * 4];  // B G R A
		P[0] = Age;
		P[1] = bBurned ? static_cast<uint8>(64 + 63 * Cls) : 0;
		P[2] = bBurning ? static_cast<uint8>(85 * Cls) : 0;
		P[3] = Spread[I];
	}
	Firebrands.Reset();
	const std::vector<emberworld::fire::Spot>& Spots = Stream.spots();
	for (size_t K = 0; K < Spots.size(); ++K)
	{
		const emberworld::fire::Spot& S = Spots[K];
		const double Age = TSeconds - S.launch_s;
		if (Age < 0.0 || Age > RecentBrandS || S.src >= static_cast<uint32>(N) || S.dst >= static_cast<uint32>(N))
		{
			continue;
		}
		FEmberFirebrand B;
		B.X0 = Grid.origin_x + (S.src % Nx + 0.5) * CellM;
		B.Y0 = Grid.origin_y - (S.src / Nx + 0.5) * CellM;
		B.X1 = Grid.origin_x + (S.dst % Nx + 0.5) * CellM;
		B.Y1 = Grid.origin_y - (S.dst / Nx + 0.5) * CellM;
		B.AgeS = Age;
		B.LandAgeS = S.landed ? TSeconds - S.land_s : -1.0;
		B.bIgnited = S.ignited;
		B.Key = static_cast<uint32>(K);
		Firebrands.Add(B);
	}
	SmokeSources.Reset();
	for (int32 K = 0; K < Bins.Num(); ++K)
	{
		const FBin& B = Bins[K];
		if (B.W < 0.5)
		{
			continue;
		}
		FEmberSmokeSource S;
		S.X = Grid.origin_x + (B.Sx / B.W + 0.5) * CellM;
		S.Y = Grid.origin_y - (B.Sy / B.W + 0.5) * CellM;
		S.Strength = static_cast<float>(B.W);
		S.Burning = static_cast<float>(B.Burning);
		S.Key = K;
		const int32 Kx = K % Bx;
		const int32 Ky = K / Bx;
		double Cl = 0.0;
		for (int32 Dy = -2; Dy <= 2; ++Dy)
		{
			for (int32 Dx = -2; Dx <= 2; ++Dx)
			{
				const int32 X = Kx + Dx;
				const int32 Y = Ky + Dy;
				if (X >= 0 && Y >= 0 && X < Bx && Y < By)
				{
					Cl += Bins[Y * Bx + X].W;
				}
			}
		}
		S.Cluster = static_cast<float>(Cl);
		SmokeSources.Add(S);
	}
	// Upload the whole grid (a few MB); ordered before the next frame's rendering.
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Nx, Ny);
	uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Pixels.Num()));
	FMemory::Memcpy(Copy, Pixels.GetData(), Pixels.Num());
	Texture->UpdateTextureRegions(0, 1, Region, Nx * 4, 4, Copy,
		[](uint8* Data, const FUpdateTextureRegion2D* Regions)
		{
			FMemory::Free(Data);
			delete Regions;
		});
}

void AEmberFireActor::BurningNear(double X, double Y, double RadiusM, TArray<FEmberBurningCell>& Out) const
{
	Out.Reset();
	if (State.phase.empty())
	{
		return;
	}
	const double Cx = (X - Grid.origin_x) / CellM - 0.5;
	const double Cy = (Grid.origin_y - Y) / CellM - 0.5;
	const int32 Rc = FMath::CeilToInt32(RadiusM / CellM);
	const int32 X0 = FMath::Max(0, FMath::FloorToInt32(Cx) - Rc), X1 = FMath::Min(Nx - 1, FMath::CeilToInt32(Cx) + Rc);
	const int32 Y0 = FMath::Max(0, FMath::FloorToInt32(Cy) - Rc), Y1 = FMath::Min(Ny - 1, FMath::CeilToInt32(Cy) + Rc);
	const std::vector<int32_t>& Arrival = Stream.final_arrival();
	const double R2 = FMath::Square(RadiusM / CellM);
	for (int32 Iy = Y0; Iy <= Y1; ++Iy)
	{
		for (int32 Ix = X0; Ix <= X1; ++Ix)
		{
			if (FMath::Square(Ix - Cx) + FMath::Square(Iy - Cy) > R2)
			{
				continue;
			}
			const int32 I = Iy * Nx + Ix;
			const uint8 Phase = State.phase[I];
			const bool bArrived = Arrival[I] >= 0 && Arrival[I] <= TimeS;
			// As SetTime draws it: a cell burns from its exact arrival time.
			if (!(Phase == emberworld::fire::Burning || (bArrived && Phase == emberworld::fire::Unburned)))
			{
				continue;
			}
			FEmberBurningCell C;
			C.X = Grid.origin_x + (Ix + 0.5) * CellM;
			C.Y = Grid.origin_y - (Iy + 0.5) * CellM;
			C.Cls = bIntensityReported ? FMath::Clamp(static_cast<int32>(State.intensity[I]), 1, 3) : 3;
			C.AgeS = Arrival[I] >= 0 ? FMath::Max(0.0, TimeS - Arrival[I]) : 0.0;
			C.Spread = Spread.IsValidIndex(I) ? Spread[I] / 255.f : 0.f;
			C.Index = static_cast<uint32>(I);
			Out.Add(C);
		}
	}
}

int32 AEmberFireActor::PhaseAt(double WorldX, double WorldY) const
{
	const int32 Cx = FMath::FloorToInt32((WorldX - Grid.origin_x) / CellM);
	const int32 Cy = FMath::FloorToInt32((Grid.origin_y - WorldY) / CellM);
	if (Cx < 0 || Cy < 0 || Cx >= Nx || Cy >= Ny || State.phase.empty())
	{
		return -1;
	}
	// As rendered: a cell burns from its exact arrival time (see SetTime).
	const int32 I = Cy * Nx + Cx;
	const int32 A = Stream.final_arrival()[I];
	if (State.phase[I] == emberworld::fire::Unburned && A >= 0 && A <= TimeS)
	{
		return emberworld::fire::Burning;
	}
	return State.phase[I];
}
