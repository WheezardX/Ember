#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

THIRD_PARTY_INCLUDES_START
#include "emberworld/firestate.h"
THIRD_PARTY_INCLUDES_END

#include "EmberFireActor.generated.h"

class AEmberTerrainActor;
class UTexture2D;

/** One smoke source (EmberSmokeActor): a 300 m bin of recently burning cells. */
struct FEmberSmokeSource
{
	double X = 0.0;          // strength-weighted centroid, metres (region CRS)
	double Y = 0.0;
	float Strength = 0.f;    // cell-equivalents, decaying with time since arrival
	float Burning = 0.f;     // burning cells only (flame light on the plume base)
	int32 Key = 0;           // bin index: stable across times (per-puff seeds)
	float Cluster = 0.f;     // Strength summed over the 5 x 5 bins around (1.5 km): plume height
	float Smoulder = 0.f;    // slow-decaying weight of the scar behind the front (days): low wisps
	float Heat = 0.f;        // observed interior heat (NIROPS) x today's activity: interior columns
	float HeatCluster = 0.f; // Heat summed over the 5 x 5 bins around: interior column height
};

/** One firebrand from the stream (HCP4 H4-4), launched within the last RecentBrandS of sim time. */
struct FEmberFirebrand
{
	double X0 = 0.0, Y0 = 0.0;   // launch cell centre, metres (region CRS)
	double X1 = 0.0, Y1 = 0.0;   // landing cell centre
	double AgeS = 0.0;           // sim seconds since launch
	double LandAgeS = 0.0;       // sim seconds since landing (< 0: still in the air)
	bool bIgnited = false;       // it started a spot fire
	uint32 Key = 0;              // index in the stream: stable per-brand seeds
};

/** A burning cell as rendered (HCP4 H4-5 flame cards). */
struct FEmberBurningCell
{
	double X = 0.0, Y = 0.0;     // cell centre, metres (region CRS)
	int32 Cls = 3;               // intensity class 1..3 (3 when the stream has none)
	double AgeS = 0.0;           // sim seconds since arrival
	float Spread = 0.f;          // 0..1 (FireTex A): the head is fast
	uint32 Index = 0;            // cell index: stable per-cell seeds
};

/**
 * The fire state player (EPIC_5_PLAN D1, D7). Loads an Epic 4 replay (+ its state stream) and,
 * for any sim time, writes the fire state into one texture over the replay's world grid:
 *   R  burning: 85 x intensity class (1 surface .. 3 crown fire), 0 = not burning
 *   G  burned (incl. burning): 64 + 63 x the class it burned at (127 / 190 / 253), 0 = not
 *      (HCP4: the class drives flame size, crown scorch vs crown fire, trunk char height; a
 *      stream without intensity - a playback - is drawn as class 3, the HCP3 look)
 *   B  sqrt(hours since arrival / 200): minute-scale near the front, ~8 days at 1
 *   A  spread (HCP4 H4-3, the head): local rate of spread from the arrival field, log scale,
 *      0 at <= 15 m/h .. 1 at >= 300 m/h (0 = never burned). Static per cell.
 * Terrain (M_Terrain) and trees (M_Veg) sample it by world position through FireTex / FireRect /
 * FireOn / FireTime; this actor owns the clock (scrub = SetTime).
 */
UCLASS()
class EMBERWORLD_API AEmberFireActor : public AActor
{
	GENERATED_BODY()

public:
	AEmberFireActor();

	/** Load `<name>.replay.json` and its stream; the terrain gives the world -> UE frame. */
	bool Load(const FString& ReplayPath, AEmberTerrainActor* InTerrain, FString& OutError);

	/** Show the state as of sim time T (seconds since the replay's t0). */
	void SetTime(double TSeconds);

	UTexture2D* GetTexture() const { return Texture; }
	/** The grid in UE cm: (x0, y0, width, height) - M_Terrain / M_Veg `FireRect`. */
	FLinearColor GetRect() const { return Rect; }

	// Facts
	double TimeS = 0.0;
	uint32 Tick = 0;
	int64 CellsBurning = 0;
	int64 CellsBurned = 0;       // burned or burning (the scar so far)
	int64 StreamBurned = 0;      // the stream's own metric at this tick
	int64 T0Unix = 0;
	int32 StartS = 0;
	int32 EndS = 0;	FString ModelId;
	bool bIntensityReported = false;  // the stream carries intensity classes (>= 2 somewhere)
	bool bUseIntensity = true;        // set before Load; false draws it as if it had none (A/B)
	// The tick's grid-mean 10 m wind (stream metrics): velocity, the air moves TOWARD (u, v).
	double WindU = 0.0, WindV = 0.0;  // m/s, east / north
	double WindSpeedMs() const { return FMath::Sqrt(WindU * WindU + WindV * WindV); }
	/** Compass bearing the wind blows FROM (what SetWind takes). */
	double WindFromDeg() const { return FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(-WindU, -WindV)) + 360.0, 360.0); }
	int64 CellsByClass[4] = {0, 0, 0, 0};  // burning cells per intensity class (facts)
	int64 CellsHead = 0;         // burning cells spreading >= HeadRateMh (the head, facts)
	static constexpr double HeadRateMh = 67.0;   // spread A >= 0.5
	double CellM = 30.0;
	int32 Nx = 0;
	int32 Ny = 0;

	/** Firebrands launched in the last RecentBrandS (rebuilt by SetTime), oldest first. */
	TArray<FEmberFirebrand> Firebrands;
	static constexpr double RecentBrandS = 1800.0;
	int32 SpotsTotal = 0;        // firebrands in the whole run (facts)
	int32 SpotsIgnitedTotal = 0;

	/** Smoke sources for the current time (rebuilt by SetTime). */
	TArray<FEmberSmokeSource> SmokeSources;
	static constexpr int32 SmokeBinCells = 10;       // 300 m bins on the 30 m grid
	static constexpr double SmokeDecayH = 1.5;       // cells smoke e^-(h since arrival)/1.5
	// Smoulder (reference: Three Queens 2026 NIROPS IR - scattered heat over 50-90 % of the scar
	// for weeks, isolated heat points climbing to 1,200 as the fire died): a weak, slow tail.
	static constexpr double SmoulderDecayH = 120.0;  // e-folding 5 days
	static constexpr double SmoulderK = 0.03;        // per cell, vs 1.0 at the flaming front
	/** Recent smoke (cell-equivalents, e^-h/24 since arrival): the valley smoke layer / pall. */
	double SmokeLoad = 0.0;

	/** Observed heat (ember.incidents.ir_heat, `<pack>.heat.json` beside the world pack): the NIROPS
	 * heat classes of the last IR flight at or before the shown time. A perimeter playback only knows
	 * when each cell burned; the IR says where it was still hot, often for weeks (Three Queens 2026). */
	bool bObservedHeat = false;
	FString HeatFlightUtc;        // flight shown (facts); empty = none within HeatMaxAgeH
	double HeatAgeH = -1.0;
	int64 HeatCells[4] = {0, 0, 0, 0};  // cells per class shown: 1 isolated, 2 scattered, 3 intense
	/** IR flight times (unix s, ascending) - the play timeline marks them. */
	const TArray<int64>& GetHeatFlightsUnix() const { return HeatUnix; }
	/** Burned area (ha) at each whole hour since t0, from the final arrival field: the timeline's growth curve. */
	TArray<float> GrowthHa;
	static constexpr double HeatMaxAgeH = 72.0;
	static constexpr double HeatDecayH = 48.0;

	/** Burning cells (as rendered) whose centres lie within RadiusM of (X, Y), metres. */
	void BurningNear(double X, double Y, double RadiusM, TArray<FEmberBurningCell>& Out) const;

	/** The phase shown at a world point (metres, region CRS); -1 outside the grid. Probes. */
	int32 PhaseAt(double WorldX, double WorldY) const;

	struct FProbe
	{
		FString Name;
		double X = 0.0;
		double Y = 0.0;
	};
	/** Scenario fire probes, reported in scene facts as fire.probes.<name> = PhaseAt. */
	TArray<FProbe> Probes;

private:
	emberworld::fire::Stream Stream;
	emberworld::fire::GridGeo Grid;
	emberworld::fire::State State;
	TArray<uint8> Pixels;
	TArray<uint8> Spread;          // FireTex A per cell (see above)
	TArray<uint8> HeatGrids;       // flights x Nx x Ny classes (row 0 north)
	TArray<int64> HeatUnix;        // per flight, ascending
	TArray<FString> HeatUtc;
	void LoadObservedHeat(const FString& ReplayPath);
	FLinearColor Rect = FLinearColor(0, 0, 1, 1);

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> Texture;
};
