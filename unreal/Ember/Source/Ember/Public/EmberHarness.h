#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "EmberFireActor.h"

#include "EmberHarness.generated.h"

class AEmberTerrainActor;
class AEmberVegetationActor;
class AEmberGroundCoverActor;
class AEmberEnvironment;
class AEmberFireActor;
class AEmberSmokeActor;
class AEmberFirebrandActor;
class AEmberFlameActor;
class AEmberStarsActor;
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
	/** Play mode: the fire timeline bar (AEmberHUD calls this every frame). */
	void DrawTimeline(class UCanvas* Canvas);
	/** Play HUD: a compass in the upper right that turns with the view. */
	void DrawCompass(class UCanvas* Canvas);

private:
	struct FBookmark
	{
		FString Name;
		bool bFrac = true;
		FVector2D Target = FVector2D(0.5, 0.5);
		double DistanceM = 1000, YawDeg = 0, PitchDeg = -30, FovDeg = 60;
		FString Sun = TEXT("noon");
		double TargetZ = TNumericLimits<double>::Lowest();  // > Lowest: aim height override (m)
		// Absolute camera (8i R1, reference photos): stands at (CamX, CamY) in the world's CRS,
		// CamZ metres above the ground (bCamAgl) or at that altitude; yaw / pitch / fov as usual.
		bool bCamera = false;
		double CamX = 0, CamY = 0, CamZ = 1.7;
		bool bCamAgl = true;
		bool bExposure = false;    // true: this view's own exposure (EV100 bias)
		double ExposureBias = 0.0;
		bool bEvMin = false;       // true: this view's own auto-exposure floor (EV100)
		double EvMin = 0.0;
	};
	struct FOrbit
	{
		FString Name;
		FString Bookmark;          // start pose: target, distance, pitch, fov, sun, start yaw
		FString ToBookmark;        // non-empty: flyover, pose interpolated Bookmark -> ToBookmark
		double Degrees = 360.0;
		int32 Frames = 240;
		int32 Fps = 30;            // wind clock during the orbit: t = frame / fps
		double TFromS = -1.0;      // >= 0 with TToS: sim time sweeps TFromS -> TToS (timelapse)
		double TToS = -1.0;
		int32 WarmupFrames = 30;
		bool bCapture = true;      // false: a path for perf_orbit only, no video
	};
	struct FCapture
	{
		FString Name;
		FString Bookmark;
		int32 WarmupFrames = 30;
		double TimeS = -1.0;       // >= 0: fire sim time to show (replay scenarios)
	};
	enum class EState : uint8 { Idle, LoadWorld, Position, Warmup, Shoot, WaitShot, OrbitStart, OrbitWarmup, OrbitShoot, OrbitWait, PerfWarmup, Perf, Play, Done };

	bool LoadPlan(const FString& Path, FString& OutError);
	bool PlaceCamera(const FBookmark& B, FString& OutError);
	void OnScreenshot(int32 W, int32 H, const TArray<FColor>& Pixels);
	void Finish(int32 ExitCode, const FString& Error);
	void NextPhase();                                  // captures -> orbits -> perf -> finish
	// Play mode (`ember-dev play`): the scenario loads as for a capture run, then the view goes to
	// a free-fly pawn (AEmberFlyPawn) and the harness only animates wind / fire and draws help.
	void StartPlay();
	FString PerfOrbit;             // non-empty: the perf window flies this orbit (streaming / hitches)
	const struct FOrbit* FindPerfOrbit() const;
	void TickPlay(float DeltaSeconds);
	bool bPlay = false;
	FString PlayBookmark;
	bool bFirePlaying = false;
	bool bShowHelp = true;
	double PlayFireS = 0.0;       // replay time (s)
	double PlayRateH = 6.0;       // replay hours per real second while playing (plan: play_rate_h)
	double PlayStartS = 0.0;      // plan: play_start_s - where play mode opens (playing)
	bool bPlayStart = false;
	// play-mode frame cost of the fire, logged every 5 s (ms summed over PlayPerfFrames)
	int32 PlayPerfFrames = 0;
	double PerfFrameMs = 0.0, PerfFireMs = 0.0, PerfSmokeMs = 0.0, PerfFlamesMs = 0.0, PerfLastLog = 0.0;
	// Timeline bar (bottom of the screen): Tab shows the cursor; click / drag on the bar scrubs.
	bool bTimelineCursor = false;
	bool bScrubbing = false;
	bool bPlayingBeforeScrub = false;
	double StepHeldS = 0.0;       // how long , or . has been held (hold = continuous scrub)
	FBox2D TimelineRect = FBox2D(ForceInit);  // bar in viewport pixels (last layout)
	double TimeAtTimelineX(double X) const;
	double PlayClock = 0.0;       // wind / flicker clock
	int32 SunIndex = -1;
	UPROPERTY(Transient) TObjectPtr<class AEmberFlyPawn> FlyPawn;
	bool PlaceOrbitFrame(const FOrbit& O, int32 Frame, FString& OutError);
	/** The pose of an orbit/flyover frame (before any height override). */
	bool OrbitPose(const FOrbit& O, int32 Frame, FBookmark& Out, FString& OutError) const;
	void BookmarkTargetXY(const FBookmark& B, double& Wx, double& Wy) const;
	double CameraClearanceM = 3.0;   // the warmup lift keeps the camera this far over the ground
	double AbsCameraAglM = -1.0;     // >= 0: an absolute camera at this height over the rendered surface
	/** Flyovers: a smooth aim height per frame so the camera holds altitude like a drone instead
	 * of tracing every 10 m bump of the ground under the aim point. */
	void PrepareFlyoverHeights(const FOrbit& O);
	TArray<double> FlyZ;
	FString OrbitPath;  // camera.csv rows for the orbit being captured

	// plan
	FString Scenario, WorldDir, OutDir;
	int32 ResX = 1920, ResY = 1080, PerfFrames = 0;
	float ExposureBias = -2.f;
	float SkylightLeaking = 0.f;
	float HazeDensity = 0.0015f, HazeFalloff = 0.006f, HazeStartM = 200.f;   // item 5: haze / aerial perspective
	bool bAutoExposure = false;
	float ExposureEvMin = -4.f, ExposureEvMax = 4.f, AutoExposureBias = 0.f;
	int32 FixedLod = -1;          // >= 0: load that LOD only (fixtures); -1: stream around the camera
	double RefineFactor = 1.5;
	FString LookPath = TEXT("clay");
	bool bVegetation = false;
	bool bGroundCover = false;
	bool bWindFromReplay = false;
	bool bFireClasses = true;   // false: draw the stream as if it had no intensity (HCP4 A/B)
	bool bFireChannels = true;  // false: ignore the source's channels sidecar (External X2 A/B)
	double GroundCoverRadiusM = 60.0;
	bool bGroundRelief = false;
	double GroundReliefRadiusM = 56.0;
	double GroundReliefSpacingM = 0.3;
	UPROPERTY(Transient) TObjectPtr<class AEmberGroundRelief> Relief;
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
	FString ReplayPath;        // Epic 4 replay (.replay.json) -> fire state player
	bool bSmoke = true;        // smoke v0 plumes with the replay
	double SmokeWindMs = 8.0;
	bool bFirebrands = true;   // H4-4 ember showers / spot-fire glows with the replay
	bool bFlames = true;       // H4-5 eye-level flame cards with the replay
	bool bStars = true;        // the night sky (real stars for latitude / date, turned to the sun)
	double LatitudeDeg = 47.0;
	int64 SkyUnix = 0;         // the date shown without a replay
	TArray<AEmberFireActor::FProbe> FireProbes;
	FString PerfBookmark;  // "clay" or an absolute viz/looks/*.toml path
	TArray<FBookmark> Bookmarks;
	TArray<FCapture> Captures;
	TArray<FOrbit> Orbits;

	// run state
	EState State = EState::Idle;
	int32 CaptureIndex = 0;
	int32 OrbitIndex = 0;
	int32 OrbitFrame = 0;
	// Orbit capture timing (seconds, summed over the orbit; logged when it ends).
	double ProfStart = 0.0, ProfShotReq = 0.0, ProfShotWait = 0.0, ProfSave = 0.0, ProfPlace = 0.0;
	int32 ProfWaitTicks = 0;

	// Orbit video: raw BGRA frames piped to ffmpeg (GPU encoder, e.g. h264_nvenc) straight to
	// orbits/<name>.mp4. Without an ffmpeg path in the plan, frames are PNGs (ember-dev encodes).
	FString FfmpegPath;
	FString OrbitCodec = TEXT("h264_nvenc");
	FProcHandle EncProc;
	void* EncRead = nullptr;   // ffmpeg's stdin (child side)
	void* EncWrite = nullptr;  // ours
	bool StartOrbitVideo(const FString& Name, int32 Fps, int32 W, int32 H, FString& OutError);
	bool WriteOrbitFrame(FString& OutError);
	bool FinishOrbitVideo(FString& OutError);
	void AbortOrbitVideo();
	int32 FramesLeft = 0;
	// Warmup frames only count once shaders/assets have finished compiling (the first run after a
	// build otherwise captures grey fallback materials).
	bool CompilesPending();
	double CompileWaitStart = -1.0;
	bool bShotReady = false;
	int32 ShotW = 0, ShotH = 0;
	TArray<FColor> ShotPixels;
	FDelegateHandle ShotHandle;
	double StartSeconds = 0;
	TArray<FString> Written;

	UPROPERTY(Transient) TObjectPtr<AEmberTerrainActor> Terrain;
	UPROPERTY(Transient) TObjectPtr<AEmberVegetationActor> Vegetation;
	UPROPERTY(Transient) TObjectPtr<AEmberGroundCoverActor> Cover;
	UPROPERTY(Transient) TObjectPtr<AEmberEnvironment> Environment;
	UPROPERTY(Transient) TObjectPtr<AEmberFireActor> Fire;
	UPROPERTY(Transient) TObjectPtr<AEmberSmokeActor> Smoke;
	UPROPERTY(Transient) TObjectPtr<AEmberFirebrandActor> Firebrands;
	UPROPERTY(Transient) TObjectPtr<AEmberFlameActor> Flames;
	UPROPERTY(Transient) TObjectPtr<AEmberStarsActor> Stars;
	/** The star sky for the sun / date / smoke shown now (cheap when unchanged). */
	void UpdateStars();
	void SetFireTime(double SimS, double ClockS);
	UPROPERTY(Transient) TObjectPtr<ACameraActor> Camera;
};
