#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberHarness.generated.h"

class AEmberTerrainActor;
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
	struct FCapture
	{
		FString Name;
		FString Bookmark;
		int32 WarmupFrames = 30;
	};
	enum class EState : uint8 { Idle, LoadWorld, Position, Warmup, Shoot, WaitShot, Perf, Done };

	bool LoadPlan(const FString& Path, FString& OutError);
	bool PlaceCamera(const FBookmark& B, FString& OutError);
	void OnScreenshot(int32 W, int32 H, const TArray<FColor>& Pixels);
	void Finish(int32 ExitCode, const FString& Error);

	// plan
	FString Scenario, WorldDir, OutDir;
	int32 ResX = 1920, ResY = 1080, PerfFrames = 0;
	float ExposureBias = -2.f;
	TArray<FBookmark> Bookmarks;
	TArray<FCapture> Captures;

	// run state
	EState State = EState::Idle;
	int32 CaptureIndex = 0;
	int32 FramesLeft = 0;
	bool bShotReady = false;
	int32 ShotW = 0, ShotH = 0;
	TArray<FColor> ShotPixels;
	FDelegateHandle ShotHandle;
	double StartSeconds = 0;
	TArray<FString> Written;

	UPROPERTY(Transient) TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient) TObjectPtr<AEmberEnvironment> Environment;
	UPROPERTY(Transient) TObjectPtr<ACameraActor> Camera;
};
