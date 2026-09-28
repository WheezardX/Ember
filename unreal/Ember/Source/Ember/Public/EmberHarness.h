#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberHarness.generated.h"

class AEmberTerrainActor;
class AEmberVegetationActor;
class AEmberEnvironment;
class ACameraActor;

/**
 * The UE half of the agent iteration loop (EPIC_5_PLAN §2, A1/A2). Driven by a run plan
 * (`ember-run-plan` v1 JSON, written by `ember-dev run-scenario`, passed as -EmberRun=<path>):
 * load the world, then for each capture place the camera from its bookmark, render warmup
 * frames, grab the frame, write captures/<name>.png + facts/<name>.json; then an optional
 * perf window -> facts/perf.json; then run_status.json and exit.
 */
UCLASS()
class EMBER_API AEmberHarness : public AActor
{
	GENERATED_BODY()

public:
	AEmberHarness();

	bool Start(const FString& PlanPath, AEmberEnvironment* Env);
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	struct FBookmark
	{
		FString Name;
		bool bFrac = true;
		FVector2D Target = FVector2D(0.5, 0.5);
		double DistanceM = 1000, YawDeg = 0, PitchDeg = -30, FovDeg = 60;
		FString Sun = TEXT("noon");
	};
	struct FOrbit
	{
		FString Name;
		FString Bookmark;          // start pose: target, distance, pitch, fov, sun, start yaw
		FString ToBookmark;        // non-empty: flyover, pose interpolated Bookmark -> ToBookmark
		double Degrees = 360.0;
		int32 Frames = 240;
		int32 Fps = 30;            // wind clock during the orbit: t = frame / fps
		int32 WarmupFrames = 30;
	};
	struct FCapture
	{
		FString Name;
		FString Bookmark;
		int32 WarmupFrames = 30;
	};
	enum class EState : uint8 { Idle, LoadWorld, Position, Warmup, Shoot, WaitShot, OrbitStart, OrbitWarmup, OrbitShoot, OrbitWait, PerfWarmup, Perf, Done };

	bool LoadPlan(const FString& Path, FString& OutError);
	bool PlaceCamera(const FBookmark& B, FString& OutError);
	void OnScreenshot(int32 W, int32 H, const TArray<FColor>& Pixels);
	void Finish(int32 ExitCode, const FString& Error);
	void NextPhase();                                  // captures -> orbits -> perf -> finish
	bool PlaceOrbitFrame(const FOrbit& O, int32 Frame, FString& OutError);

	// plan
	FString Scenario, WorldDir, OutDir;
	int32 ResX = 1920, ResY = 1080, PerfFrames = 0;
	float ExposureBias = -2.f;
	int32 FixedLod = -1;          // >= 0: load that LOD only (fixtures); -1: stream around the camera
	double RefineFactor = 1.5;
	FString LookPath = TEXT("clay");
	bool bVegetation = false;
	double VegRadiusM = 1500.0;
	double VegNearRadiusM = 500.0;
	double VegMidMinHeightM = 12.0;
	double WindStrength = 6.0;
	double WindFromDeg = 270.0;
	bool bVegLineup = false;
	double PerfWindTime = 0.0;
	TArray<FString> ExecCmds;
	TArray<FString> PerfExecCmds;  // run as the perf window opens (e.g. ProfileGPU)
	FString WaterDir;
	FString PerfBookmark;  // "clay" or an absolute viz/looks/*.toml path
	TArray<FBookmark> Bookmarks;
	TArray<FCapture> Captures;
	TArray<FOrbit> Orbits;

	// run state
	EState State = EState::Idle;
	int32 CaptureIndex = 0;
	int32 OrbitIndex = 0;
	int32 OrbitFrame = 0;
	int32 FramesLeft = 0;
	bool bShotReady = false;
	int32 ShotW = 0, ShotH = 0;
	TArray<FColor> ShotPixels;
	FDelegateHandle ShotHandle;
	double StartSeconds = 0;
	TArray<FString> Written;

	UPROPERTY(Transient) TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient) TObjectPtr<AEmberVegetationActor> Vegetation;
	UPROPERTY(Transient) TObjectPtr<AEmberEnvironment> Environment;
	UPROPERTY(Transient) TObjectPtr<ACameraActor> Camera;
};
