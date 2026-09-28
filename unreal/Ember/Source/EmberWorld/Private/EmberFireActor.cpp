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
	const std::vector<int32_t>& Arrival = Stream.final_arrival();
	CellsBurning = 0;
	CellsBurned = 0;
	const int32 N = Nx * Ny;
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
		}
		// Intensity 0..3 (0 = model does not report it; the playback model reports 1).
		const uint8 Flame = bBurning ? static_cast<uint8>(FMath::Clamp(150 + 35 * State.intensity[I], 0, 255)) : 0;
		uint8* P = &Pixels[I * 4];  // B G R A
		P[0] = Age;
		P[1] = bBurned ? 255 : 0;
		P[2] = Flame;
		P[3] = 255;
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

int32 AEmberFireActor::PhaseAt(double WorldX, double WorldY) const
{
	const int32 Cx = FMath::FloorToInt32((WorldX - Grid.origin_x) / CellM);
	const int32 Cy = FMath::FloorToInt32((Grid.origin_y - WorldY) / CellM);
	if (Cx < 0 || Cy < 0 || Cx >= Nx || Cy >= Ny || State.phase.empty())
	{
		return -1;
	}
	return State.phase[Cy * Nx + Cx];
}
