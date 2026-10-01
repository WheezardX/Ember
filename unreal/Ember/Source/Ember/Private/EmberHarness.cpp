#include "EmberHarness.h"

#include "EmberSmokeActor.h"
#include "EmberFirebrandActor.h"
#include "EmberFlameActor.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"
#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"

#include "EmberEnvironment.h"
#include "EmberFireActor.h"
#include "EmberFlyPawn.h"
#include "EmberGroundCoverActor.h"
#include "EmberGroundRelief.h"
#include "EmberSceneFacts.h"
#include "EmberTerrainActor.h"
#include "EmberVegetationActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEmberHarness, Log, All);

namespace
{
	/** "Sep 13 2017  14:05 PDT" for a Unix time, in US Pacific time (every world so far is in
	 *  Washington; per-world time zones come with the timeline). US DST: 2nd Sunday of March
	 *  02:00 local -> 1st Sunday of November 02:00 local (2007 rules). */
	FString PacificTime(int64 Unix)
	{
		const FDateTime Utc = FDateTime::FromUnixTimestamp(Unix);
		const int32 Y = Utc.GetYear();
		auto NthSunday = [](int32 Year, int32 Month, int32 N)
		{
			const FDateTime First(Year, Month, 1);
			const int32 Dow = static_cast<int32>(First.GetDayOfWeek());   // Monday = 0 .. Sunday = 6
			return 1 + (6 - Dow + 7) % 7 + 7 * (N - 1);
		};
		// DST starts 02:00 PST = 10:00 UTC; ends 02:00 PDT = 09:00 UTC
		const FDateTime DstOn(Y, 3, NthSunday(Y, 3, 2), 10);
		const FDateTime DstOff(Y, 11, NthSunday(Y, 11, 1), 9);
		const bool bDst = Utc >= DstOn && Utc < DstOff;
		const FDateTime L = Utc + FTimespan::FromHours(bDst ? -7.0 : -8.0);
		static const TCHAR* Months[] = {TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
			TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec")};
		return FString::Printf(TEXT("%s %d %d  %02d:%02d %s"), Months[L.GetMonth() - 1], L.GetDay(), L.GetYear(),
			L.GetHour(), L.GetMinute(), bDst ? TEXT("PDT") : TEXT("PST"));
	}
}

AEmberHarness::AEmberHarness()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
}

bool AEmberHarness::LoadPlan(const FString& Path, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = TEXT("cannot read run plan ") + Path;
		return false;
	}
	TSharedPtr<FJsonObject> J;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid())
	{
		OutError = TEXT("run plan is not valid JSON");
		return false;
	}
	if (J->GetStringField(TEXT("format")) != TEXT("ember-run-plan") || J->GetIntegerField(TEXT("version")) != 1)
	{
		OutError = TEXT("not an ember-run-plan v1");
		return false;
	}
	Scenario = J->GetStringField(TEXT("scenario"));
	WorldDir = J->GetStringField(TEXT("world"));
	OutDir = J->GetStringField(TEXT("out_dir"));
	const TArray<TSharedPtr<FJsonValue>>& Res = J->GetArrayField(TEXT("resolution"));
	ResX = static_cast<int32>(Res[0]->AsNumber());
	ResY = static_cast<int32>(Res[1]->AsNumber());
	PerfFrames = J->GetIntegerField(TEXT("perf_frames"));
	int32 Lod = -1;
	if (J->TryGetNumberField(TEXT("fixed_lod"), Lod))
	{
		FixedLod = Lod;
	}
	J->TryGetNumberField(TEXT("lod_refine_factor"), RefineFactor);
	J->TryGetStringField(TEXT("look"), LookPath);
	J->TryGetBoolField(TEXT("vegetation"), bVegetation);
	FString Mode;
	if (J->TryGetStringField(TEXT("mode"), Mode))
	{
		bPlay = Mode == TEXT("play");
	}
	J->TryGetStringField(TEXT("play_bookmark"), PlayBookmark);
	bPlayStart = J->TryGetNumberField(TEXT("play_start_s"), PlayStartS);  // null -> the replay's start
	J->TryGetNumberField(TEXT("play_rate_h"), PlayRateH);
	J->TryGetBoolField(TEXT("ground_cover"), bGroundCover);
	J->TryGetBoolField(TEXT("wind_from_replay"), bWindFromReplay);
	J->TryGetBoolField(TEXT("fire_classes"), bFireClasses);
	J->TryGetNumberField(TEXT("ground_cover_radius_m"), GroundCoverRadiusM);
	J->TryGetBoolField(TEXT("ground_relief"), bGroundRelief);
	J->TryGetNumberField(TEXT("ground_relief_radius_m"), GroundReliefRadiusM);
	J->TryGetNumberField(TEXT("ground_relief_spacing_m"), GroundReliefSpacingM);
	J->TryGetNumberField(TEXT("veg_radius_m"), VegRadiusM);
	J->TryGetNumberField(TEXT("veg_near_radius_m"), VegNearRadiusM);
	J->TryGetNumberField(TEXT("veg_mid_min_height_m"), VegMidMinHeightM);
	J->TryGetNumberField(TEXT("wind_strength"), WindStrength);
	J->TryGetNumberField(TEXT("wind_from_deg"), WindFromDeg);
	J->TryGetBoolField(TEXT("smoke"), bSmoke);
	J->TryGetNumberField(TEXT("smoke_wind_ms"), SmokeWindMs);
	J->TryGetBoolField(TEXT("firebrands"), bFirebrands);
	J->TryGetBoolField(TEXT("flames"), bFlames);
	J->TryGetBoolField(TEXT("veg_lineup"), bVegLineup);
	J->TryGetStringField(TEXT("perf_bookmark"), PerfBookmark);
	J->TryGetStringField(TEXT("perf_orbit"), PerfOrbit);
	J->TryGetStringField(TEXT("water_dir"), WaterDir);
	J->TryGetStringField(TEXT("ffmpeg"), FfmpegPath);
	J->TryGetStringField(TEXT("orbit_codec"), OrbitCodec);
	J->TryGetStringField(TEXT("replay"), ReplayPath);
	const TArray<TSharedPtr<FJsonValue>>* ProbeArr = nullptr;
	if (J->TryGetArrayField(TEXT("fire_probes"), ProbeArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *ProbeArr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			AEmberFireActor::FProbe Pr;
			Pr.Name = O->GetStringField(TEXT("name"));
			Pr.X = O->GetNumberField(TEXT("x"));
			Pr.Y = O->GetNumberField(TEXT("y"));
			FireProbes.Add(Pr);
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Cmds = nullptr;
	if (J->TryGetArrayField(TEXT("exec_cmds"), Cmds))
	{
		for (const TSharedPtr<FJsonValue>& V : *Cmds)
		{
			ExecCmds.Add(V->AsString());
		}
	}
	if (J->TryGetArrayField(TEXT("perf_exec_cmds"), Cmds))
	{
		for (const TSharedPtr<FJsonValue>& V : *Cmds)
		{
			PerfExecCmds.Add(V->AsString());
		}
	}
	double Ev = 0;
	if (J->TryGetNumberField(TEXT("exposure_bias"), Ev))
	{
		ExposureBias = static_cast<float>(Ev);
	}
	FString ExpMode;
	if (J->TryGetStringField(TEXT("exposure_mode"), ExpMode))
	{
		bAutoExposure = ExpMode == TEXT("auto");
	}
	double ExpV = 0;
	if (J->TryGetNumberField(TEXT("exposure_ev_min"), ExpV)) ExposureEvMin = static_cast<float>(ExpV);
	if (J->TryGetNumberField(TEXT("exposure_ev_max"), ExpV)) ExposureEvMax = static_cast<float>(ExpV);
	if (J->TryGetNumberField(TEXT("auto_exposure_bias"), ExpV)) AutoExposureBias = static_cast<float>(ExpV);
	double Leak = 0;
	if (J->TryGetNumberField(TEXT("skylight_leaking"), Leak))
	{
		SkylightLeaking = static_cast<float>(Leak);
	}
	if (J->TryGetNumberField(TEXT("haze_density"), ExpV)) HazeDensity = static_cast<float>(ExpV);
	if (J->TryGetNumberField(TEXT("haze_falloff"), ExpV)) HazeFalloff = static_cast<float>(ExpV);
	if (J->TryGetNumberField(TEXT("haze_start_m"), ExpV)) HazeStartM = static_cast<float>(ExpV);
	for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("bookmarks")))
	{
		const TSharedPtr<FJsonObject>& O = V->AsObject();
		FBookmark B;
		B.Name = O->GetStringField(TEXT("name"));
		const TArray<TSharedPtr<FJsonValue>>* T = nullptr;
		if (O->TryGetArrayField(TEXT("target_frac"), T) && T->Num() == 2)
		{
			B.bFrac = true;
		}
		else if (O->TryGetArrayField(TEXT("target_cell"), T) && T->Num() == 2)
		{
			B.bFrac = false;
		}
		else if (O->TryGetArrayField(TEXT("camera_xy"), T) && T->Num() == 2)
		{
			B.bCamera = true;
			B.CamX = (*T)[0]->AsNumber();
			B.CamY = (*T)[1]->AsNumber();
			double Z = 0.0;
			if (O->TryGetNumberField(TEXT("camera_alt_m"), Z))
			{
				B.bCamAgl = false;
				B.CamZ = Z;
			}
			else if (O->TryGetNumberField(TEXT("camera_agl_m"), Z))
			{
				B.CamZ = Z;
			}
		}
		else
		{
			OutError = TEXT("bookmark without target: ") + B.Name;
			return false;
		}
		if (!B.bCamera)
		{
			B.Target = FVector2D((*T)[0]->AsNumber(), (*T)[1]->AsNumber());
			B.DistanceM = O->GetNumberField(TEXT("distance_m"));
		}
		B.YawDeg = O->GetNumberField(TEXT("yaw_deg"));
		B.PitchDeg = O->GetNumberField(TEXT("pitch_deg"));
		B.FovDeg = O->GetNumberField(TEXT("fov_deg"));
		B.Sun = O->GetStringField(TEXT("sun"));
		double BEv = 0.0;
		if (O->TryGetNumberField(TEXT("exposure_bias"), BEv))  // null / absent = scenario exposure
		{
			B.bExposure = true;
			B.ExposureBias = BEv;
		}
		if (O->TryGetNumberField(TEXT("exposure_ev_min"), BEv))
		{
			B.bEvMin = true;
			B.EvMin = BEv;
		}
		Bookmarks.Add(B);
	}
	const TArray<TSharedPtr<FJsonValue>>* OrbitArr = nullptr;
	if (J->TryGetArrayField(TEXT("orbits"), OrbitArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *OrbitArr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			FOrbit Or;
			Or.Name = O->GetStringField(TEXT("name"));
			Or.Bookmark = O->GetStringField(TEXT("bookmark"));
			O->TryGetStringField(TEXT("to_bookmark"), Or.ToBookmark);
			O->TryGetNumberField(TEXT("fps"), Or.Fps);
			O->TryGetNumberField(TEXT("t_from_s"), Or.TFromS);
			O->TryGetNumberField(TEXT("t_to_s"), Or.TToS);
			Or.Degrees = O->GetNumberField(TEXT("degrees"));
			Or.Frames = O->GetIntegerField(TEXT("frames"));
			Or.WarmupFrames = O->GetIntegerField(TEXT("warmup_frames"));
			O->TryGetBoolField(TEXT("capture"), Or.bCapture);
			Orbits.Add(Or);
		}
	}
	for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("captures")))
	{
		const TSharedPtr<FJsonObject>& O = V->AsObject();
		FCapture C;
		C.Name = O->GetStringField(TEXT("name"));
		C.Bookmark = O->GetStringField(TEXT("bookmark"));
		C.WarmupFrames = O->GetIntegerField(TEXT("warmup_frames"));
		O->TryGetNumberField(TEXT("t_s"), C.TimeS);
		Captures.Add(C);
	}
	return true;
}

bool AEmberHarness::Start(const FString& PlanPath, AEmberEnvironment* Env)
{
	StartSeconds = FPlatformTime::Seconds();
	Environment = Env;
	FString Err;
	if (!LoadPlan(PlanPath, Err))
	{
		// No out dir known yet: log and exit non-zero; ember-dev reports the missing status.
		UE_LOG(LogEmberHarness, Error, TEXT("%s"), *Err);
		FPlatformMisc::RequestExitWithStatus(false, 3);
		return false;
	}
	UE_LOG(LogEmberHarness, Display, TEXT("Run plan: %s — world %s — %d captures -> %s"),
		*Scenario, *WorldDir, Captures.Num(), *OutDir);
	if (GEngine)
	{
		GEngine->bEnableOnScreenDebugMessages = bPlay;  // play mode draws its help text
	}
	if (Environment)
	{
		Environment->SetExposure(ExposureBias);
		if (bAutoExposure)
		{
			// captures: adapt at once (each still settles on the same value); play: natural pace
			Environment->SetAutoExposure(ExposureEvMin, ExposureEvMax, AutoExposureBias, bPlay ? 1.5f : 200.f);
		}
		Environment->SetSkylightLeaking(SkylightLeaking);
	}
	ShotHandle = UGameViewportClient::OnScreenshotCaptured().AddUObject(this, &AEmberHarness::OnScreenshot);	State = EState::LoadWorld;
	return true;
}

void AEmberHarness::EndPlay(const EEndPlayReason::Type Reason)
{
	AbortOrbitVideo();
	UGameViewportClient::OnScreenshotCaptured().Remove(ShotHandle);
	Super::EndPlay(Reason);
}

bool AEmberHarness::PlaceCamera(const FBookmark& B, FString& OutError)
{
	const emberworld::Region* R = Terrain ? Terrain->GetRegion() : nullptr;
	if (!R)
	{
		OutError = TEXT("no region loaded");
		return false;
	}
	const FRotator Rot(B.PitchDeg, B.YawDeg - 90.0, 0.0);  // compass -> UE yaw (X = east)
	FVector Loc;
	if (B.bCamera)
	{
		// Absolute camera (a reference photo's pose): exactly where it was asked for, only kept
		// out of the ground (0.3 m); eye height would be lifted by the 3 m rule below.
		double Gz = R->heightmap.z_min;
		Terrain->GroundHeightAt(B.CamX, B.CamY, Gz);
		const double Z = FMath::Max(B.bCamAgl ? Gz + B.CamZ : B.CamZ, Gz + 0.3);
		Loc = Terrain->WorldToUE(B.CamX, B.CamY, Z);
		CameraClearanceM = FMath::Min(3.0, Z - Gz);
		AbsCameraAglM = B.bCamAgl ? FMath::Max(0.3, B.CamZ) : -1.0;   // re-seated on the rendered surface in warmup
	}
	else
	{
		double Wx, Wy;
		BookmarkTargetXY(B, Wx, Wy);
		double Wz = R->heightmap.z_min;
		if (B.TargetZ > TNumericLimits<double>::Lowest())
		{
			Wz = B.TargetZ;
		}
		else
		{
			Terrain->GroundHeightAt(Wx, Wy, Wz);
		}
		const FVector Target = Terrain->WorldToUE(Wx, Wy, Wz);
		Loc = Target - Rot.Vector() * (B.DistanceM * 100.0);
		// Never inside the ground (flyovers cross ridges): keep 3 m over the rendered surface.
		const emberworld::Frame& F = Terrain->GetFrame();
		double Gz = 0.0;
		if (Terrain->GroundHeightAt(F.anchor_x + Loc.X / 100.0, F.anchor_y - Loc.Y / 100.0, Gz))
		{
			Loc.Z = FMath::Max(Loc.Z, Terrain->WorldToUE(0.0, 0.0, Gz + 3.0).Z);
		}
		CameraClearanceM = 3.0;
		AbsCameraAglM = -1.0;
	}

	if (!Camera)
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Camera = GetWorld()->SpawnActor<ACameraActor>(Loc, Rot, P);
		Camera->GetCameraComponent()->bConstrainAspectRatio = false;
		// fov_deg is the HORIZONTAL field of view at any aspect ratio (a photo's lens, 8i R1).
		// The engine default holds the vertical FOV of a 16:9 frame, so a square or 4:3 capture
		// came out ~40 % narrower than asked (found by the NAIP probe's alignment check).
		Camera->GetCameraComponent()->bOverrideAspectRatioAxisConstraint = true;
		Camera->GetCameraComponent()->AspectRatioAxisConstraint = EAspectRatioAxisConstraint::AspectRatio_MaintainXFOV;
	}
	Camera->SetActorLocationAndRotation(Loc, Rot);
	if (Smoke)
	{
		Smoke->Rebuild(Loc, Rot);  // camera-facing, back-to-front puffs
	}
	if (Firebrands)
	{
		Firebrands->Rebuild(Loc, Rot);
	}
	if (Flames)
	{
		Flames->Rebuild(Loc);
	}
	Camera->GetCameraComponent()->SetFieldOfView(B.FovDeg);
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->SetViewTarget(Camera);
	}
	if (Environment && bAutoExposure)  // auto: a view may lower (or raise) the metering floor
	{
		Environment->SetAutoExposure(B.bEvMin ? static_cast<float>(B.EvMin) : ExposureEvMin, ExposureEvMax,
		                             AutoExposureBias, bPlay ? 1.5f : 200.f);
	}
	else if (Environment)
	{
		Environment->SetExposure(B.bExposure ? static_cast<float>(B.ExposureBias) : ExposureBias);
	}
	if (Environment && !Environment->SetSun(B.Sun))
	{
		OutError = TEXT("unknown sun preset: ") + B.Sun;
		return false;
	}
	return true;
}

void AEmberHarness::OnScreenshot(int32 W, int32 H, const TArray<FColor>& Pixels)
{
	ShotW = W;
	ShotH = H;
	ShotPixels = Pixels;
	for (FColor& C : ShotPixels)
	{
		C.A = 255;
	}
	bShotReady = true;
}

bool AEmberHarness::StartOrbitVideo(const FString& Name, int32 Fps, int32 W, int32 H, FString& OutError)
{
	const FString Mp4 = OutDir / TEXT("orbits") / (Name + TEXT(".mp4"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Mp4), true);
	// Raw frames in on stdin; the GPU encoder (NVENC) does the compression. Constant quality ~ crf 18;
	// yuv420p so every player takes it.
	const bool bNv = OrbitCodec.EndsWith(TEXT("_nvenc"));
	const FString Quality = bNv ? TEXT("-preset p5 -tune hq -rc vbr -cq 18 -b:v 0") : TEXT("-preset medium -crf 18");
	const FString Args = FString::Printf(
		TEXT("-y -loglevel error -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i - -c:v %s %s -pix_fmt yuv420p -movflags +faststart \"%s\""),
		W, H, FMath::Max(1, Fps), *OrbitCodec, *Quality, *Mp4);
	if (!FPlatformProcess::CreatePipe(EncRead, EncWrite, /*bWritePipeLocal*/ true))
	{
		OutError = TEXT("orbit video: could not create a pipe for ffmpeg");
		return false;
	}
	EncProc = FPlatformProcess::CreateProc(*FfmpegPath, *Args, false, true, true, nullptr, 0, nullptr,
		/*PipeWriteChild*/ nullptr, /*PipeReadChild (stdin)*/ EncRead);
	if (!EncProc.IsValid())
	{
		AbortOrbitVideo();
		OutError = TEXT("orbit video: could not start ") + FfmpegPath;
		return false;
	}
	UE_LOG(LogEmberHarness, Display, TEXT("orbit %s: streaming %dx%d frames to %s (%s)"), *Name, W, H, *Mp4, *OrbitCodec);
	return true;
}

bool AEmberHarness::WriteOrbitFrame(FString& OutError)
{
	const int32 Bytes = ShotPixels.Num() * sizeof(FColor);  // FColor is B, G, R, A in memory
	const uint8* Data = reinterpret_cast<const uint8*>(ShotPixels.GetData());
	int32 Done = 0;
	while (Done < Bytes)
	{
		int32 N = 0;
		if (!FPlatformProcess::WritePipe(EncWrite, Data + Done, Bytes - Done, &N) || N <= 0)
		{
			OutError = TEXT("orbit video: ffmpeg stopped reading frames (see the log for its error)");
			return false;
		}
		Done += N;
	}
	return true;
}

bool AEmberHarness::FinishOrbitVideo(FString& OutError)
{
	if (!EncProc.IsValid())
	{
		return true;
	}
	FPlatformProcess::ClosePipe(EncRead, EncWrite);  // EOF on ffmpeg's stdin: it finishes the file
	EncRead = EncWrite = nullptr;
	FPlatformProcess::WaitForProc(EncProc);
	int32 Code = -1;
	FPlatformProcess::GetProcReturnCode(EncProc, &Code);
	FPlatformProcess::CloseProc(EncProc);
	if (Code != 0)
	{
		OutError = FString::Printf(TEXT("orbit video: ffmpeg exited with %d"), Code);
		return false;
	}
	return true;
}

void AEmberHarness::AbortOrbitVideo()
{
	if (EncRead || EncWrite)
	{
		FPlatformProcess::ClosePipe(EncRead, EncWrite);
		EncRead = EncWrite = nullptr;
	}
	if (EncProc.IsValid())
	{
		FPlatformProcess::WaitForProc(EncProc);
		FPlatformProcess::CloseProc(EncProc);
	}
}

void AEmberHarness::Finish(int32 ExitCode, const FString& Error)
{
	AbortOrbitVideo();
	State = EState::Done;
	TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
	S->SetStringField(TEXT("format"), TEXT("ember-run-status"));
	S->SetNumberField(TEXT("version"), 1);
	S->SetStringField(TEXT("scenario"), Scenario);
	S->SetNumberField(TEXT("exit_code"), ExitCode);
	S->SetStringField(TEXT("error"), Error);
	S->SetNumberField(TEXT("elapsed_s"), FPlatformTime::Seconds() - StartSeconds);
	TArray<TSharedPtr<FJsonValue>> Files;
	for (const FString& F : Written)
	{
		Files.Add(MakeShared<FJsonValueString>(F));
	}
	S->SetArrayField(TEXT("written"), Files);
	if (!OutDir.IsEmpty())
	{
		UEmberSceneFactsSubsystem::WriteJson(S, OutDir / TEXT("run_status.json"));
	}
	if (!Error.IsEmpty())
	{
		UE_LOG(LogEmberHarness, Error, TEXT("Harness failed: %s"), *Error);
	}
	UE_LOG(LogEmberHarness, Display, TEXT("Harness done (exit %d) in %.1f s"), ExitCode, FPlatformTime::Seconds() - StartSeconds);
	FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(ExitCode));
}

void AEmberHarness::NextPhase()
{
	UEmberSceneFactsSubsystem* Facts = GetWorld()->GetSubsystem<UEmberSceneFactsSubsystem>();
	while (OrbitIndex < Orbits.Num() && !Orbits[OrbitIndex].bCapture)
	{
		++OrbitIndex;  // path-only orbits (perf_orbit) record nothing
	}
	if (CaptureIndex < Captures.Num())
	{
		State = EState::Position;
	}
	else if (OrbitIndex < Orbits.Num())
	{
		State = EState::OrbitStart;
	}
	else if (PerfFrames > 0 && State != EState::Perf && State != EState::PerfWarmup)
	{
		// Optional perf pose; settle (streaming, TSR) before the measured window.
		if (const FOrbit* O = FindPerfOrbit())
		{
			FString Err;
			PrepareFlyoverHeights(*O);
			if (!PlaceOrbitFrame(*O, 0, Err))
			{
				Finish(2, TEXT("perf orbit: ") + Err);
				return;
			}
		}
		else if (!PerfOrbit.IsEmpty())
		{
			Finish(2, TEXT("perf orbit not found: ") + PerfOrbit);
			return;
		}
		else if (!PerfBookmark.IsEmpty())
		{
			const FBookmark* B = Bookmarks.FindByPredicate([&](const FBookmark& X) { return X.Name == PerfBookmark; });
			FString Err;
			if (!B || !PlaceCamera(*B, Err))
			{
				Finish(2, TEXT("perf bookmark: ") + (B ? Err : PerfBookmark));
				return;
			}
		}
		FramesLeft = 60;
		if (Terrain)
		{
			Terrain->bSyncStreaming = false;  // perf measures the real (async) streaming
			Terrain->MorphSeconds = 0.6;      // ... with the play-mode geomorph cost included
		}
		if (Vegetation)
		{
			Vegetation->bSyncStreaming = false;
		}
		if (Cover)
		{
			Cover->bSyncStreaming = false;
		}
		GEngine->Exec(GetWorld(), TEXT("r.Shadow.Virtual.Cache 1"));  // perf measures the shipping path
		State = EState::PerfWarmup;
	}
	else
	{
		Finish(0, FString());
	}
}

void AEmberHarness::BookmarkTargetXY(const FBookmark& B, double& Wx, double& Wy) const
{
	if (B.bCamera)
	{
		Wx = B.CamX;   // absolute cameras have no target: where they stand
		Wy = B.CamY;
		return;
	}
	// Bookmarks address the valid data (the AOI), not the tile grid's padded extent.
	const emberworld::Region* R = Terrain->GetRegion();
	const emberworld::Bounds E = Terrain->GetDataExtent();
	if (B.bFrac)
	{
		Wx = E.min_x + B.Target.X * E.width();
		Wy = E.max_y - B.Target.Y * E.height();
	}
	else
	{
		// Cells are finest-LOD pixels, x east / y south from the data extent's NW corner.
		const double Px = R->tiles_at(R->finest_lod())[0]->content.width() / R->tile_px;
		Wx = E.min_x + (B.Target.X + 0.5) * Px;
		Wy = E.max_y - (B.Target.Y + 0.5) * Px;
	}
}

void AEmberHarness::PrepareFlyoverHeights(const FOrbit& O)
{
	FlyZ.Reset();
	if (O.ToBookmark.IsEmpty() || !Terrain || !Terrain->GetRegion())
	{
		return;
	}
	// Ground under the aim point and under the camera, every frame (read from the region's tiles
	// on disk, independent of what is streamed).
	const int32 N = FMath::Max(2, O.Frames);
	TArray<double> Aim, Need;
	Aim.SetNum(N);
	Need.SetNum(N);
	const double Floor = Terrain->GetRegion()->heightmap.z_min;
	for (int32 F = 0; F < N; ++F)
	{
		FBookmark P;
		FString Err;
		if (!OrbitPose(O, F, P, Err))
		{
			FlyZ.Reset();
			return;
		}
		double Wx, Wy, Gt = Floor, Gc = Floor;
		BookmarkTargetXY(P, Wx, Wy);
		Terrain->GroundHeightAt(Wx, Wy, Gt);
		const double Pitch = FMath::DegreesToRadians(P.PitchDeg);
		const double Back = P.DistanceM * FMath::Cos(Pitch);
		const double Yaw = FMath::DegreesToRadians(P.YawDeg);
		Terrain->GroundHeightAt(Wx - Back * FMath::Sin(Yaw), Wy - Back * FMath::Cos(Yaw), Gc);
		Aim[F] = Gt;
		// Aim height that keeps the camera 30 m over the ground beneath it.
		Need[F] = Gc + 30.0 + P.DistanceM * FMath::Sin(Pitch);
	}
	// Moving average of the aim ground (+-3 s), raised to the moving max of the clearance need,
	// then smoothed again: slow, continuous altitude changes only.
	const int32 W = FMath::Clamp(O.Fps * 3, 5, N / 2);
	auto Window = [&](const TArray<double>& In, bool bMax)
	{
		TArray<double> Out;
		Out.SetNum(N);
		for (int32 F = 0; F < N; ++F)
		{
			double Acc = bMax ? TNumericLimits<double>::Lowest() : 0.0;
			int32 Count = 0;
			// Full window everywhere, ends padded with the end values: a window truncated at the
			// path's start moves at half speed, and the climb rate doubled when it began to slide.
			for (int32 K = F - W; K <= F + W; ++K)
			{
				const double V = In[FMath::Clamp(K, 0, N - 1)];
				Acc = bMax ? FMath::Max(Acc, V) : Acc + V;
				++Count;
			}
			Out[F] = bMax ? Acc : Acc / Count;
		}
		return Out;
	};
	const TArray<double> Smooth = Window(Aim, false);
	const TArray<double> Clear = Window(Need, true);
	TArray<double> Base;
	Base.SetNum(N);
	for (int32 F = 0; F < N; ++F)
	{
		Base[F] = FMath::Max(Smooth[F], Clear[F]);
	}
	FlyZ = Window(Window(Window(Base, true), false), false);  // box x box: no kinks in the climb rate
}

void AEmberHarness::SetFireTime(double SimS, double ClockS)
{
	if (!Fire)
	{
		return;
	}
	Fire->SetTime(SimS);
	if (bWindFromReplay)
	{
		// The tick's wind (stream metrics): the plume and the trees follow the fire's own wind.
		// Sway strength scales from the scenario's value, taken as the look at 5 m/s.
		const double Speed = Fire->WindSpeedMs(), From = Fire->WindFromDeg();
		if (Vegetation) Vegetation->SetWind(WindStrength * Speed / 5.0, From);
		if (Cover) Cover->SetWind(WindStrength * Speed / 5.0, From);
		if (Smoke) Smoke->SetWind(From, Speed);
	}
	if (Environment && Smoke)
	{
		// Smoke v1 pall: cells burned in the last ~day (e^-h/24). ~4,000 cell-equivalents (a
		// 900-acre day) is a heavy valley layer; the Aug 21 2026 run (~2,500 ac) saturates it.
		Environment->SetSmokePall(static_cast<float>(1.0 - FMath::Exp(-Fire->SmokeLoad / 9000.0)));
	}
	if (Smoke)
	{
		Smoke->SetSources(Fire->SmokeSources);
		Smoke->SetClock(ClockS);
		if (Camera)
		{
			// Stills set the time after placing the camera; PlaceCamera re-lays them on moves.
			Smoke->Rebuild(Camera->GetActorLocation(), Camera->GetActorRotation());
		}
	}
	if (Firebrands)
	{
		Firebrands->SetBrands(Fire->Firebrands);
		if (Camera)
		{
			Firebrands->Rebuild(Camera->GetActorLocation(), Camera->GetActorRotation());
		}
	}
	if (Flames)
	{
		Flames->SetFireTime(ClockS);
		if (Camera)
		{
			Flames->Rebuild(Camera->GetActorLocation());
		}
	}
	if (Terrain) Terrain->SetFireTime(ClockS);
	if (Vegetation) Vegetation->SetFireTime(ClockS);
	if (Cover) Cover->SetFireTime(ClockS);
}

bool AEmberHarness::PlaceOrbitFrame(const FOrbit& O, int32 Frame, FString& OutError)
{
	FBookmark At;
	if (!OrbitPose(O, Frame, At, OutError))
	{
		return false;
	}
	if (FlyZ.IsValidIndex(Frame))
	{
		At.TargetZ = FlyZ[Frame];
	}
	if (Vegetation)
	{
		Vegetation->SetWindTime(static_cast<double>(Frame) / FMath::Max(1, O.Fps));
	}
	if (Cover)
	{
		Cover->SetWindTime(static_cast<double>(Frame) / FMath::Max(1, O.Fps));
	}
	if (Fire)
	{
		// Timelapse: the sim clock sweeps with the frames (D7: the player owns time; flicker runs
		// on the render clock so flames move at real speed however fast the sim runs).
		const double U = static_cast<double>(Frame) / FMath::Max(1, O.Frames - 1);
		const double Sim = (O.TFromS >= 0.0 && O.TToS >= 0.0) ? FMath::Lerp(O.TFromS, O.TToS, U) : Fire->TimeS;
		SetFireTime(Sim, static_cast<double>(Frame) / FMath::Max(1, O.Fps));
	}
	return PlaceCamera(At, OutError);
}

bool AEmberHarness::OrbitPose(const FOrbit& O, int32 Frame, FBookmark& At, FString& OutError) const
{
	const FBookmark* B = Bookmarks.FindByPredicate([&](const FBookmark& X) { return X.Name == O.Bookmark; });
	if (!B)
	{
		OutError = TEXT("orbit ") + O.Name + TEXT(": unknown bookmark ") + O.Bookmark;
		return false;
	}
	const double T = static_cast<double>(Frame) / FMath::Max(1, O.Frames - (O.ToBookmark.IsEmpty() ? 0 : 1));
	At = *B;
	if (O.ToBookmark.IsEmpty())
	{
		At.YawDeg = B->YawDeg + O.Degrees * T;
	}
	else
	{
		const FBookmark* E = Bookmarks.FindByPredicate([&](const FBookmark& X) { return X.Name == O.ToBookmark; });
		if (!E || E->bFrac != B->bFrac)
		{
			OutError = TEXT("flyover ") + O.Name + TEXT(": bad to_bookmark ") + O.ToBookmark;
			return false;
		}
		const double S = FMath::SmoothStep(0.0, 1.0, T);  // ease in/out
		At.Target = FMath::Lerp(B->Target, E->Target, S);
		At.DistanceM = FMath::Lerp(B->DistanceM, E->DistanceM, S);
		At.YawDeg = B->YawDeg + FMath::FindDeltaAngleDegrees(B->YawDeg, E->YawDeg) * S;
		At.PitchDeg = FMath::Lerp(B->PitchDeg, E->PitchDeg, S);
		At.FovDeg = FMath::Lerp(B->FovDeg, E->FovDeg, S);
	}
	return true;
}

const AEmberHarness::FOrbit* AEmberHarness::FindPerfOrbit() const
{
	return PerfOrbit.IsEmpty() ? nullptr : Orbits.FindByPredicate([&](const FOrbit& X) { return X.Name == PerfOrbit; });
}

void AEmberHarness::StartPlay()
{
	// Start pose: the named bookmark, else the first capture's, else the first bookmark.
	FString Name = PlayBookmark;
	if (Name.IsEmpty() && Captures.Num())
	{
		Name = Captures[0].Bookmark;
	}
	const FBookmark* B = Bookmarks.FindByPredicate([&](const FBookmark& X) { return Name.IsEmpty() || X.Name == Name; });
	FString Err;
	if (B && !PlaceCamera(*B, Err))
	{
		UE_LOG(LogEmberHarness, Warning, TEXT("play: start bookmark: %s"), *Err);
	}
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC)
	{
		Finish(2, TEXT("play: no player controller"));
		return;
	}
	const FVector Loc = Camera ? Camera->GetActorLocation() : FVector(0, 0, 100000);
	const FRotator Rot = Camera ? Camera->GetActorRotation() : FRotator(-30, 0, 0);
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	FlyPawn = GetWorld()->SpawnActor<AEmberFlyPawn>(Loc, Rot, P);
	FlyPawn->Terrain = Terrain;
	if (Camera)
	{
		FlyPawn->Camera->SetFieldOfView(Camera->GetCameraComponent()->FieldOfView);
	}
	if (APawn* Old = PC->GetPawn())
	{
		PC->UnPossess();
		Old->Destroy();
	}
	PC->Possess(FlyPawn);
	PC->SetControlRotation(Rot);
	PC->SetViewTarget(FlyPawn);
	PC->SetInputMode(FInputModeGameOnly());
	PC->bShowMouseCursor = false;
	if (Fire)
	{
		// Open already playing from the scenario's play_start_s (Brad: it opened paused on the last
		// hour, so the rate keys seemed dead and P restarted three weeks before anything happens).
		PlayFireS = FMath::Clamp(bPlayStart ? PlayStartS : static_cast<double>(Fire->StartS),
			static_cast<double>(Fire->StartS), static_cast<double>(Fire->EndS));
		bFirePlaying = true;
		SetFireTime(PlayFireS, 0.0);
	}
	State = EState::Play;
	UE_LOG(LogEmberHarness, Display, TEXT("play: %s from bookmark '%s'"), *Scenario, B ? *B->Name : TEXT("(none)"));
}

void AEmberHarness::TickPlay(float DeltaSeconds)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !FlyPawn)
	{
		return;
	}
	PlayClock += DeltaSeconds;
	if (Vegetation) Vegetation->SetWindTime(PlayClock);
	if (Cover) Cover->SetWindTime(PlayClock);
	if (PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		FPlatformMisc::RequestExit(false);
		return;
	}
	if (PC->WasInputKeyJustPressed(EKeys::H))
	{
		bShowHelp = !bShowHelp;
	}
	if (PC->WasInputKeyJustPressed(EKeys::L))
	{
		FlyPawn->ToggleLamp();
	}
	static const TCHAR* Suns[] = {TEXT("dawn"), TEXT("morning"), TEXT("noon"), TEXT("afternoon"), TEXT("dusk")};
	const FKey SunKeys[] = {EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five};
	for (int32 i = 0; i < 5; ++i)
	{
		if (PC->WasInputKeyJustPressed(SunKeys[i]) && Environment)
		{
			Environment->SetSun(Suns[i]);
			SunIndex = i;
		}
	}
	if (Fire)
	{
		bool bChanged = false;
		if (PC->WasInputKeyJustPressed(EKeys::P))
		{
			bFirePlaying = !bFirePlaying;
			if (bFirePlaying && PlayFireS >= Fire->EndS)
			{
				PlayFireS = Fire->StartS;
			}
		}
		// Step , . by the current rate (Brad: the rate only changed playback, so going back was an
		// hour at a time): a tap moves max(1 h, rate x 1 s); held past 0.3 s it scrubs continuously
		// at the rate - backwards too.
		{
			const bool bFwd = PC->IsInputKeyDown(EKeys::Period), bBack = PC->IsInputKeyDown(EKeys::Comma);
			const double StepS = FMath::Max(1.0, PlayRateH) * 3600.0;
			if (PC->WasInputKeyJustPressed(EKeys::Period)) { PlayFireS += StepS; bChanged = true; StepHeldS = 0.0; }
			if (PC->WasInputKeyJustPressed(EKeys::Comma)) { PlayFireS -= StepS; bChanged = true; StepHeldS = 0.0; }
			if (bFwd != bBack)
			{
				StepHeldS += DeltaSeconds;
				if (StepHeldS > 0.3)
				{
					PlayFireS += (bFwd ? 1.0 : -1.0) * PlayRateH * 3600.0 * DeltaSeconds;
					bChanged = true;
				}
			}
			else
			{
				StepHeldS = 0.0;
			}
		}
		// Timeline bar: Tab shows the cursor; click / drag on the bar sets the time (playback pauses
		// while dragging and resumes after).
		if (PC->WasInputKeyJustPressed(EKeys::Tab))
		{
			bTimelineCursor = !bTimelineCursor;
			PC->bShowMouseCursor = bTimelineCursor;
			if (bTimelineCursor)
			{
				FInputModeGameAndUI M;
				M.SetHideCursorDuringCapture(false);
				PC->SetInputMode(M);
			}
			else
			{
				PC->SetInputMode(FInputModeGameOnly());
			}
		}
		{
			float Mx = 0.f, My = 0.f;
			const bool bMouse = bTimelineCursor && PC->GetMousePosition(Mx, My);
			const FBox2D Hit = TimelineRect.ExpandBy(FVector2D(0.0, 10.0));
			if (bMouse && PC->WasInputKeyJustPressed(EKeys::LeftMouseButton) && TimelineRect.bIsValid
				&& Hit.IsInside(FVector2D(Mx, My)))
			{
				bScrubbing = true;
				bPlayingBeforeScrub = bFirePlaying;
				bFirePlaying = false;
			}
			if (bScrubbing)
			{
				if (bMouse && PC->IsInputKeyDown(EKeys::LeftMouseButton))
				{
					PlayFireS = TimeAtTimelineX(Mx);
					bChanged = true;
				}
				else
				{
					bScrubbing = false;
					bFirePlaying = bPlayingBeforeScrub;
				}
			}
		}
		// Day jumps: a 1,600 h replay is not walked an hour at a time
		if (PC->WasInputKeyJustPressed(EKeys::PageUp) || PC->WasInputKeyJustPressed(EKeys::N)) { PlayFireS += 86400.0; bChanged = true; }
		if (PC->WasInputKeyJustPressed(EKeys::PageDown) || PC->WasInputKeyJustPressed(EKeys::B)) { PlayFireS -= 86400.0; bChanged = true; }
		if (PC->WasInputKeyJustPressed(EKeys::Home)) { PlayFireS = bPlayStart ? PlayStartS : Fire->StartS; bChanged = true; }
		// Rate: [ ] (Brad: "the playback speed keys don't work") plus - = and the numpad - +, in
		// case the bracket keys do not reach the client on a keyboard layout / overlay.
		const bool bFaster = PC->WasInputKeyJustPressed(EKeys::RightBracket) || PC->WasInputKeyJustPressed(EKeys::Equals)
			|| PC->WasInputKeyJustPressed(EKeys::Add);
		const bool bSlower = PC->WasInputKeyJustPressed(EKeys::LeftBracket) || PC->WasInputKeyJustPressed(EKeys::Hyphen)
			|| PC->WasInputKeyJustPressed(EKeys::Subtract);
		if (bFaster || bSlower)
		{
			PlayRateH = bFaster ? FMath::Min(PlayRateH * 2.0, 96.0) : FMath::Max(PlayRateH / 2.0, 0.125);
			UE_LOG(LogEmberHarness, Display, TEXT("fire playback rate %.3g h/s"), PlayRateH);
		}
		if (bFirePlaying)
		{
			PlayFireS += PlayRateH * 3600.0 * DeltaSeconds;
			bChanged = true;
			if (PlayFireS >= Fire->EndS)
			{
				bFirePlaying = false;
			}
		}
		PlayFireS = FMath::Clamp(PlayFireS, static_cast<double>(Fire->StartS), static_cast<double>(Fire->EndS));
		if (bChanged)
		{
			SetFireTime(PlayFireS, PlayClock);
		}
		else
		{
			if (Terrain) Terrain->SetFireTime(PlayClock);  // flames keep flickering
			if (Flames) Flames->SetFireTime(PlayClock);
			if (Vegetation) Vegetation->SetFireTime(PlayClock);
			if (Cover) Cover->SetFireTime(PlayClock);
		}
		if (Smoke)
		{
			Smoke->SetClock(PlayClock);
			Smoke->Rebuild(FlyPawn->GetActorLocation(), PC->GetControlRotation());
		}
		if (Firebrands)
		{
			Firebrands->Rebuild(FlyPawn->GetActorLocation(), PC->GetControlRotation());
		}
		if (Flames)
		{
			Flames->Rebuild(FlyPawn->GetActorLocation());
		}
	}
	if (GEngine)
	{
		// On-screen debug messages draw newest key on top: add the bottom line first.
		static const TCHAR* Help[] = {
			TEXT("  Esc       quit"),
			TEXT("  H         hide / show this help"),
			TEXT("  L         lamp on / off (inspect dense foliage)"),
			TEXT("  [ ]  - =  fire rate  /2  x2  (playback speed and the , . step)"),
			TEXT("  Tab       cursor: click / drag the timeline bar to set the fire time"),
			TEXT("  Home      fire time back to the start"),
			TEXT("  B  N      fire time  -1 day  +1 day   (also PgDn PgUp)"),
			TEXT("  ,  .      fire time  back / forward one rate step (tap), scrub at the rate (hold)"),
			TEXT("  P         play / pause the fire"),
			TEXT("  1 - 5     sun: dawn  morning  noon  afternoon  dusk"),
			TEXT("  G         walk (eye height, follows the ground) / fly"),
			TEXT("  wheel     base speed      Shift x4   Ctrl x0.25"),
			TEXT("  E Space   up       Q C   down"),
			TEXT("  WASD      move     mouse   look"),
			TEXT("CONTROLS"),
		};
		const int32 NHelp = UE_ARRAY_COUNT(Help);
		for (int32 I = 0; I < NHelp; ++I)
		{
			if (bShowHelp)
			{
				GEngine->AddOnScreenDebugMessage(9100 + I, 0.f, FColor(210, 210, 200), Help[I]);
			}
			else
			{
				GEngine->RemoveOnScreenDebugMessage(9100 + I);
			}
		}
		if (Fire)
		{
			// Fire clock (Brad, 8g): the date and local time of day being shown, and how long since
			// ignition - "t = 503 h" alone said neither.
			const int64 Unix = Fire->T0Unix + static_cast<int64>(PlayFireS);
			const FString Local = PacificTime(Unix);
			// Counted from the stream's t0 = the incident start (Jolly: Aug 11 2017 17:00 PDT); a
			// model run's own first arrival can be weeks later (the CA run starts on day 19).
			const double SinceH = PlayFireS / 3600.0;
			const int32 Day = FMath::FloorToInt32(SinceH / 24.0) + 1;
			const FString Clock = FString::Printf(TEXT("FIRE   %s   day %d   (%s%.0f h since the fire started)%s"),
				*Local, Day, SinceH < 0 ? TEXT("") : TEXT("+"), SinceH,
				*FString::Printf(TEXT("   %s  rate %.3g h/s"), bFirePlaying ? TEXT("PLAYING") : TEXT("paused"), PlayRateH));
			GEngine->AddOnScreenDebugMessage(9002, 0.f, FColor(255, 190, 120), Clock);
		}
		const FString Status = FString::Printf(TEXT("%s   %s   %.0f m above ground   %.1f m/s (x%.2f)%s   %.0f fps"),
			*Scenario, FlyPawn->IsWalking() ? TEXT("WALK") : TEXT("FLY"), FlyPawn->GetAglM(), FlyPawn->GetSpeedMs(),
			FlyPawn->SpeedScale, FlyPawn->IsLampOn() ? TEXT("   LAMP") : TEXT(""),
			DeltaSeconds > 0.f ? 1.0 / DeltaSeconds : 0.0);
		GEngine->AddOnScreenDebugMessage(9001, 0.f, FColor::White, Status);
	}
}

double AEmberHarness::TimeAtTimelineX(double X) const
{
	if (!Fire || !TimelineRect.bIsValid)
	{
		return PlayFireS;
	}
	const double U = FMath::Clamp((X - TimelineRect.Min.X) / FMath::Max(1.0, TimelineRect.Max.X - TimelineRect.Min.X), 0.0, 1.0);
	return Fire->StartS + U * (Fire->EndS - Fire->StartS);
}

void AEmberHarness::DrawTimeline(UCanvas* Canvas)
{
	if (State != EState::Play || !Fire || !Canvas || Fire->EndS <= Fire->StartS || !GEngine)
	{
		return;
	}
	// Bottom bar over the whole replay: burned-area growth as a filled curve, a tick per local
	// midnight (labelled weekly), the IR night flights above the bar, the playhead with its date.
	const double W = Canvas->ClipX, H = Canvas->ClipY;
	const double X0 = 60.0, X1 = W - 60.0, BarH = 28.0, Y1 = H - 46.0, Y0 = Y1 - BarH;
	TimelineRect = FBox2D(FVector2D(X0, Y0), FVector2D(X1, Y1));
	const double T0 = Fire->StartS, T1 = Fire->EndS;
	auto XAt = [&](double T) { return X0 + (T - T0) / (T1 - T0) * (X1 - X0); };
	UFont* Font = GEngine->GetSmallFont();

	auto Box = [&](double Ax, double Ay, double Bx, double By, const FLinearColor& C)
	{
		FCanvasTileItem T(FVector2D(Ax, Ay), FVector2D(Bx - Ax, By - Ay), C);
		T.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(T);
	};
	auto Line = [&](double Ax, double Ay, double Bx, double By, const FLinearColor& C, float Thick)
	{
		FCanvasLineItem L(FVector2D(Ax, Ay), FVector2D(Bx, By));
		L.SetColor(C);
		L.LineThickness = Thick;
		Canvas->DrawItem(L);
	};
	auto Text = [&](double X, double Y, const FString& S, const FLinearColor& C, bool bCentre)
	{
		FCanvasTextItem T(FVector2D(X, Y), FText::FromString(S), Font, C);
		T.bCentreX = bCentre;
		T.EnableShadow(FLinearColor::Black);
		Canvas->DrawItem(T);
	};

	Box(X0 - 6, Y0 - 22, X1 + 6, Y1 + 24, FLinearColor(0.f, 0.f, 0.f, 0.45f));
	Box(X0, Y0, X1, Y1, FLinearColor(0.08f, 0.08f, 0.09f, 0.85f));
	// Growth: burned area so far, scaled to the final area (a column every 2 px)
	const TArray<float>& G = Fire->GrowthHa;
	const float MaxHa = G.Num() ? FMath::Max(1.f, G.Last()) : 1.f;
	for (double X = X0; X < X1; X += 2.0)
	{
		const double T = T0 + (X - X0) / (X1 - X0) * (T1 - T0);
		const int32 Hr = FMath::Clamp(static_cast<int32>(T / 3600.0), 0, FMath::Max(0, G.Num() - 1));
		const double F = G.Num() ? G[Hr] / MaxHa : 0.0;
		if (F > 0.0)
		{
			const bool bPast = T <= PlayFireS;
			Box(X, Y1 - F * BarH, X + 2.0, Y1, bPast ? FLinearColor(0.95f, 0.45f, 0.12f, 0.9f) : FLinearColor(0.55f, 0.3f, 0.15f, 0.45f));
		}
	}
	// Local midnights (UTC-7 in fire season) and weekly date labels
	static const TCHAR* Months[] = {TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"),
		TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec")};
	const int64 LocalOff = -7 * 3600;
	const int64 FirstMid = ((Fire->T0Unix + LocalOff) / 86400 + 1) * 86400 - LocalOff;  // unix of the first local midnight
	for (int64 U = FirstMid, N = 0; U - Fire->T0Unix <= T1; U += 86400, ++N)
	{
		const double X = XAt(static_cast<double>(U - Fire->T0Unix));
		const FDateTime D = FDateTime::FromUnixTimestamp(U + LocalOff);
		const bool bLabel = D.GetDayOfWeek() == EDayOfWeek::Monday;
		Line(X, Y1, X, Y1 + (bLabel ? 7.0 : 4.0), FLinearColor(0.7f, 0.7f, 0.7f, 0.8f), 1.f);
		if (bLabel)
		{
			Text(X, Y1 + 8.0, FString::Printf(TEXT("%s %d"), Months[D.GetMonth() - 1], D.GetDay()), FLinearColor(0.8f, 0.8f, 0.8f), true);
		}
	}
	// IR flights (NIROPS): cyan ticks above the bar
	for (const int64 U : Fire->GetHeatFlightsUnix())
	{
		const double X = XAt(static_cast<double>(U - Fire->T0Unix));
		Line(X, Y0 - 5.0, X, Y0, FLinearColor(0.3f, 0.85f, 1.f, 0.9f), 1.f);
	}
	// Playhead + its date / time and the burned area
	const double Px = XAt(PlayFireS);
	Line(Px, Y0 - 8.0, Px, Y1 + 3.0, FLinearColor::White, 2.f);
	const int32 Hr = FMath::Clamp(static_cast<int32>(PlayFireS / 3600.0), 0, FMath::Max(0, G.Num() - 1));
	const FString Head = FString::Printf(TEXT("%s   %.0f h   %s ac"), *PacificTime(Fire->T0Unix + static_cast<int64>(PlayFireS)),
		PlayFireS / 3600.0, *FText::AsNumber(FMath::RoundToInt(G.Num() ? G[Hr] * 2.4710538f : 0.f)).ToString());
	Text(FMath::Clamp(Px, X0 + 120.0, X1 - 120.0), Y0 - 22.0, Head, FLinearColor(1.f, 0.85f, 0.6f), true);
	Text(X0, Y0 - 22.0, bTimelineCursor ? TEXT("Tab: back to flying") : TEXT("Tab: timeline cursor"), FLinearColor(0.6f, 0.6f, 0.6f), false);
	// Hover readout while the cursor is out
	if (bTimelineCursor)
	{
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			float Mx = 0.f, My = 0.f;
			if (PC->GetMousePosition(Mx, My) && TimelineRect.ExpandBy(FVector2D(0.0, 10.0)).IsInside(FVector2D(Mx, My)))
			{
				const double T = TimeAtTimelineX(Mx);
				Line(Mx, Y0, Mx, Y1, FLinearColor(1.f, 1.f, 1.f, 0.4f), 1.f);
				Text(Mx, Y0 - 40.0, PacificTime(Fire->T0Unix + static_cast<int64>(T)), FLinearColor(0.85f, 0.85f, 0.85f), true);
			}
		}
	}
}

bool AEmberHarness::CompilesPending()
{
	const int32 Shaders = GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0;
	// Terrain streams asynchronously: a capture waits until every wanted tile is in.
	const int32 Assets = FAssetCompilingManager::Get().GetNumRemainingAssets()
		+ ((Terrain && Terrain->IsStreamingBusy()) ? 1 : 0)
		+ ((Vegetation && !Vegetation->bSyncStreaming && Vegetation->IsStreamingBusy()) ? 1 : 0)
		+ ((Cover && Cover->IsStreamingBusy()) ? 1 : 0);
	const double Now = FPlatformTime::Seconds();
	if (Shaders + Assets == 0)
	{
		if (CompileWaitStart >= 0.0)
		{
			UE_LOG(LogEmberHarness, Display, TEXT("compiles done after %.1f s; warmup continues"), Now - CompileWaitStart);
			CompileWaitStart = -1.0;
		}
		return false;
	}
	if (CompileWaitStart < 0.0)
	{
		CompileWaitStart = Now;
		UE_LOG(LogEmberHarness, Display, TEXT("waiting for %d shader job(s) and %d asset(s) / terrain streaming before capturing"), Shaders, Assets);
	}
	if (Now - CompileWaitStart > 900.0)
	{
		UE_LOG(LogEmberHarness, Warning, TEXT("still compiling after 900 s (%d shaders, %d assets): capturing anyway"), Shaders, Assets);
		return false;
	}
	return true;
}

void AEmberHarness::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UEmberSceneFactsSubsystem* Facts = GetWorld()->GetSubsystem<UEmberSceneFactsSubsystem>();
	FString Err;

	switch (State)
	{
	case EState::Idle:
	case EState::Done:
		return;

	case EState::Play:
		TickPlay(DeltaSeconds);
		return;

	case EState::LoadWorld:
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Terrain = GetWorld()->SpawnActor<AEmberTerrainActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
		Terrain->RefineFactor = RefineFactor;
		Terrain->bSyncStreaming = !bPlay;  // captures: reproducible; play + perf: async
		if (!WaterDir.IsEmpty() && !Terrain->SetWaterDir(WaterDir, Err))
		{
			Finish(2, TEXT("water: ") + Err);
			return;
		}
		if (LookPath != TEXT("clay") && !Terrain->SetLook(LookPath, Err))
		{
			Finish(2, TEXT("look: ") + Err);
			return;
		}
		// far-field canopy impression takes over where the drawn trees end (none drawn: everywhere)
		Terrain->CanopyFarStartM = bVegetation ? 0.85 * VegRadiusM : 0.0;
		if (!Terrain->LoadRegion(WorldDir, FixedLod, Err))
		{
			Finish(2, TEXT("world load failed: ") + Err);
			return;
		}
		Terrain->SetBaseColor(FLinearColor(0.35f, 0.35f, 0.35f));
		Terrain->MorphSeconds = bPlay ? 0.6 : 0.0;   // geomorph tile swaps in play; captures pop (deterministic)
		if (Environment && Terrain->GetRegion())
		{
			// haze pools in the valleys: the fog's reference height is the region's lowest ground
			Environment->SetHaze(HazeDensity, HazeFalloff, HazeStartM,
				Terrain->WorldToUE(0.0, 0.0, Terrain->GetRegion()->heightmap.z_min).Z);
		}
		if (bVegetation)
		{
			Vegetation = GetWorld()->SpawnActor<AEmberVegetationActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
			Vegetation->RadiusM = VegRadiusM;
			Vegetation->bSyncStreaming = !bPlay;
			Vegetation->NearRadiusM = VegNearRadiusM;
			Vegetation->MidMinHeightM = VegMidMinHeightM;
			if (!Vegetation->Init(Terrain, Err))
			{
				Finish(2, TEXT("vegetation: ") + Err);
				return;
			}
			Vegetation->SetWind(WindStrength, WindFromDeg);
			Vegetation->SetWindTime(0.0);
			if (bVegLineup)
			{
				const FBookmark* B = Captures.Num() ? Bookmarks.FindByPredicate([&](const FBookmark& X) { return X.Name == Captures[0].Bookmark; }) : nullptr;
				if (!B || !B->bFrac)
				{
					Finish(2, TEXT("veg_lineup needs a first capture with a target_frac bookmark"));
					return;
				}
				const emberworld::Bounds E = Terrain->GetDataExtent();
				// Row across the view: perpendicular to the camera's compass heading.
				Vegetation->SpawnLineup(E.min_x + B->Target.X * E.width(), E.max_y - B->Target.Y * E.height(), B->YawDeg + 90.0);
			}
		}
		if (bGroundCover)
		{
			if (LookPath == TEXT("clay"))
			{
				Finish(2, TEXT("ground_cover needs a look (its [cover] rules)"));
				return;
			}
			Cover = GetWorld()->SpawnActor<AEmberGroundCoverActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
			Cover->RadiusM = GroundCoverRadiusM;
			Cover->bSyncStreaming = !bPlay;
			Cover->Vegetation = Vegetation;   // hung-up trees anchor on the drawn trees (null: none)
			if (!Cover->Init(Terrain, LookPath, Err))
			{
				Finish(2, TEXT("ground cover: ") + Err);
				return;
			}
			Cover->SetWind(WindStrength, WindFromDeg);
			Cover->SetWindTime(0.0);
		}
		if (bGroundRelief && Terrain)
		{
			Relief = GetWorld()->SpawnActor<AEmberGroundRelief>(FVector::ZeroVector, FRotator::ZeroRotator, P);
			Relief->Init(Terrain, GroundReliefRadiusM, GroundReliefSpacingM);
			if (Cover)
			{
				// items stand on the relief (same function, parameters and lift), not under it
				Cover->bRelief = true;
				Cover->ReliefParams = Relief->Params;
				Cover->ReliefLiftM = Relief->LiftM;
			}
		}
		if (!ReplayPath.IsEmpty())
		{
			Fire = GetWorld()->SpawnActor<AEmberFireActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
			Fire->bUseIntensity = bFireClasses;
			if (!Fire->Load(ReplayPath, Terrain, Err))
			{
				Finish(2, TEXT("replay: ") + Err);
				return;
			}
			Fire->Probes = FireProbes;
			Terrain->SetFireClasses(Fire->bIntensityReported);
			Terrain->SetFire(Fire->GetTexture(), Fire->GetRect());
			if (Vegetation)
			{
				Vegetation->SetFire(Fire->GetTexture(), Fire->GetRect());
			}
			if (Cover)
			{
				Cover->SetFire(Fire->GetTexture(), Fire->GetRect());
			}
			if (bSmoke)
			{
				Smoke = GetWorld()->SpawnActor<AEmberSmokeActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
				if (!Smoke->Init(Terrain, Err))
				{
					Finish(2, TEXT("smoke: ") + Err);
					return;
				}
				Smoke->SetWind(WindFromDeg, SmokeWindMs);
			}
			if (bFirebrands)
			{
				Firebrands = GetWorld()->SpawnActor<AEmberFirebrandActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
				if (!Firebrands->Init(Terrain, Err))
				{
					Finish(2, TEXT("firebrands: ") + Err);
					return;
				}
			}
			if (bFlames)
			{
				Flames = GetWorld()->SpawnActor<AEmberFlameActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
				if (!Flames->Init(Terrain, Fire, Err))
				{
					Finish(2, TEXT("flames: ") + Err);
					return;
				}
			}
			SetFireTime(Fire->EndS, 0.0);  // default: the final footprint
		}
		for (const FString& Cmd : ExecCmds)
		{
			UE_LOG(LogEmberHarness, Display, TEXT("exec: %s"), *Cmd);
			GEngine->Exec(GetWorld(), *Cmd);
		}
		CaptureIndex = 0;
		OrbitIndex = 0;
		if (bPlay)
		{
			StartPlay();
			return;
		}
		NextPhase();
		return;
	}

	case EState::Position:
	{
		const FCapture& C = Captures[CaptureIndex];
		const FBookmark* B = Bookmarks.FindByPredicate([&](const FBookmark& X) { return X.Name == C.Bookmark; });
		if (!B)
		{
			Finish(2, TEXT("unknown bookmark ") + C.Bookmark);
			return;
		}
		if (!PlaceCamera(*B, Err))
		{
			Finish(2, Err);
			return;
		}
		if (Vegetation)
		{
			Vegetation->SetWindTime(0.0);  // stills: frozen wind clock (pixel-deterministic goldens)
		}
		// Stills: no virtual shadow map cache. Its pages keep whatever was drawn while tiles were
		// still streaming, so canopy shadows differed run to run (S_ground_tq 0.99 -> 0.9995 SSIM
		// between runs without it). Play and perf windows keep the cache (it is a big GPU saving).
		GEngine->Exec(GetWorld(), TEXT("r.Shadow.Virtual.Cache 0"));
		if (Cover)
		{
			Cover->SetWindTime(0.0);
		}
		if (Fire && C.TimeS >= 0.0)
		{
			SetFireTime(C.TimeS, 0.0);     // stills: frozen flicker clock too
		}
		FramesLeft = FMath::Max(1, C.WarmupFrames);
		State = EState::Warmup;
		return;
	}

	case EState::Warmup:
		// Placement measured the ground on whatever LOD was loaded then; the finer tiles that
		// stream in around the camera can sit above it (a close-up ended up underground: black).
		// Raise-only, so cameras that were already clear never move.
		if (Camera && Terrain)
		{
			const emberworld::Frame& F = Terrain->GetFrame();
			FVector Loc = Camera->GetActorLocation();
			double Gz = 0.0;
			double Sz = 0.0;
			if (AbsCameraAglM >= 0.0 && Terrain->SurfaceAt(F.anchor_x + Loc.X / 100.0, F.anchor_y - Loc.Y / 100.0, Sz))
			{
				// An absolute camera at eye height stands on the RENDERED surface: the disk DEM's
				// nearest corner is metres off on a steep slope (a 32 deg camera ended up underground).
				const double Want = Terrain->WorldToUE(0.0, 0.0, Sz + AbsCameraAglM).Z;
				if (FMath::Abs(Want - Loc.Z) > 1.0)
				{
					Loc.Z = Want;
					Camera->SetActorLocation(Loc);
				}
			}
			else if (Terrain->GroundHeightAt(F.anchor_x + Loc.X / 100.0, F.anchor_y - Loc.Y / 100.0, Gz))
			{
				const double MinZ = Terrain->WorldToUE(0.0, 0.0, Gz + CameraClearanceM).Z;
				if (Loc.Z < MinZ)
				{
					UE_LOG(LogEmberHarness, Display, TEXT("camera lifted %.2f m clear of the streamed-in ground"), (MinZ - Loc.Z) / 100.0);
					Loc.Z = MinZ;
					Camera->SetActorLocation(Loc);
				}
			}
		}
		if (CompilesPending())
		{
			// Warmup counts QUIET frames: frames spent before a late stream / compile do not let
			// TSR, Lumen and auto exposure settle on the final content (run-to-run differences).
			FramesLeft = FMath::Max(1, Captures[CaptureIndex].WarmupFrames);
			return;
		}
		if (--FramesLeft <= 0)
		{
			State = EState::Shoot;
		}
		return;

	case EState::Shoot:
		bShotReady = false;
		FScreenshotRequest::RequestScreenshot(false);
		FramesLeft = 120;  // timeout in frames
		State = EState::WaitShot;
		return;

	case EState::WaitShot:
	{
		if (!bShotReady)
		{
			if (--FramesLeft <= 0)
			{
				Finish(2, TEXT("screenshot timed out: ") + Captures[CaptureIndex].Name);
			}
			return;
		}
		const FCapture& C = Captures[CaptureIndex];
		const FString Png = OutDir / TEXT("captures") / (C.Name + TEXT(".png"));
		const FImageView Img(ShotPixels.GetData(), ShotW, ShotH);
		if (!FImageUtils::SaveImageByExtension(*Png, Img))
		{
			Finish(2, TEXT("failed to write ") + Png);
			return;
		}
		Written.Add(Png);
		if (Facts)
		{
			const FString FJ = OutDir / TEXT("facts") / (C.Name + TEXT(".json"));
			UEmberSceneFactsSubsystem::WriteJson(Facts->BuildFacts(Scenario, C.Name), FJ);
			Written.Add(FJ);
		}
		UE_LOG(LogEmberHarness, Display, TEXT("Captured %s (%dx%d)"), *C.Name, ShotW, ShotH);
		++CaptureIndex;
		NextPhase();
		return;
	}

	case EState::OrbitStart:
	{
		const FOrbit& O = Orbits[OrbitIndex];
		OrbitFrame = 0;
		ProfStart = FPlatformTime::Seconds();
		ProfShotWait = ProfSave = ProfPlace = 0.0;
		ProfWaitTicks = 0;
		PrepareFlyoverHeights(O);
		if (!PlaceOrbitFrame(O, 0, Err))
		{
			Finish(2, Err);
			return;
		}
		FramesLeft = FMath::Max(1, O.WarmupFrames);
		State = EState::OrbitWarmup;
		return;
	}

	case EState::OrbitWarmup:
		if (CompilesPending())
		{
			FramesLeft = FMath::Max(1, Orbits[OrbitIndex].WarmupFrames);  // quiet frames, as stills
			return;
		}
		if (--FramesLeft <= 0)
		{
			if (Facts)
			{
				const FString FJ = OutDir / TEXT("facts") / (TEXT("orbit_") + Orbits[OrbitIndex].Name + TEXT(".json"));
				UEmberSceneFactsSubsystem::WriteJson(Facts->BuildFacts(Scenario, TEXT("orbit_") + Orbits[OrbitIndex].Name), FJ);
				Written.Add(FJ);
			}
			State = EState::OrbitShoot;
		}
		return;

	case EState::OrbitShoot:
		if (Terrain && Terrain->IsStreamingBusy())
		{
			return;  // each orbit frame waits for its tiles (videos never show half-loaded ground)
		}
		bShotReady = false;
		ProfShotReq = FPlatformTime::Seconds();
		FScreenshotRequest::RequestScreenshot(false);
		FramesLeft = 120;
		State = EState::OrbitWait;
		return;

	case EState::OrbitWait:
	{
		const FOrbit& O = Orbits[OrbitIndex];
		++ProfWaitTicks;
		if (!bShotReady)
		{
			if (--FramesLeft <= 0)
			{
				Finish(2, FString::Printf(TEXT("orbit %s frame %d: screenshot timed out"), *O.Name, OrbitFrame));
			}
			return;
		}
		const double TSave0 = FPlatformTime::Seconds();
		ProfShotWait += TSave0 - ProfShotReq;
		if (!FfmpegPath.IsEmpty())
		{
			if (OrbitFrame == 0 && !StartOrbitVideo(O.Name, O.Fps, ShotW, ShotH, Err))
			{
				Finish(2, Err);
				return;
			}
			if (!WriteOrbitFrame(Err))
			{
				Finish(2, FString::Printf(TEXT("orbit %s frame %d: %s"), *O.Name, OrbitFrame, *Err));
				return;
			}
		}
		else
		{
			const FString Png = OutDir / TEXT("frames") / O.Name / FString::Printf(TEXT("f%05d.png"), OrbitFrame);
			if (!FImageUtils::SaveImageByExtension(*Png, FImageView(ShotPixels.GetData(), ShotW, ShotH)))
			{
				Finish(2, TEXT("failed to write ") + Png);
				return;
			}
		}
		ProfSave += FPlatformTime::Seconds() - TSave0;
		if (Camera)
		{
			const FVector L = Camera->GetActorLocation();
			OrbitPath += FString::Printf(TEXT("%d,%.1f,%.1f,%.1f\n"), OrbitFrame, L.X, L.Y, L.Z);
		}
		if (++OrbitFrame < O.Frames)
		{
			// Move now; the next tick requests the frame rendered from the new pose.
			const double TPlace0 = FPlatformTime::Seconds();
			if (!PlaceOrbitFrame(O, OrbitFrame, Err))
			{
				Finish(2, Err);
				return;
			}
			ProfPlace += FPlatformTime::Seconds() - TPlace0;
			State = EState::OrbitShoot;
			return;
		}
		{
			const double Wall = FPlatformTime::Seconds() - ProfStart;
			const double N = FMath::Max(1, O.Frames);
			UE_LOG(LogEmberHarness, Display,
				TEXT("orbit %s timing: %d frames in %.1f s = %.0f ms/frame | screenshot wait %.0f ms (%.1f ticks) | frame out (%s) %.0f ms | place (fire+smoke+camera) %.0f ms"),
				*O.Name, O.Frames, Wall, 1000.0 * Wall / N, 1000.0 * ProfShotWait / N, ProfWaitTicks / N,
				FfmpegPath.IsEmpty() ? TEXT("png") : *OrbitCodec, 1000.0 * ProfSave / N, 1000.0 * ProfPlace / N);
		}
		if (!FinishOrbitVideo(Err))
		{
			Finish(2, FString::Printf(TEXT("orbit %s: %s"), *O.Name, *Err));
			return;
		}
		// Camera path (UE cm) per frame: review / smoothness checks.
		FFileHelper::SaveStringToFile(TEXT("frame,x_cm,y_cm,z_cm\n") + OrbitPath,
			*(OutDir / TEXT("frames") / O.Name / TEXT("camera.csv")));
		OrbitPath.Reset();
		UE_LOG(LogEmberHarness, Display, TEXT("Orbit %s: %d frames"), *O.Name, O.Frames);
		Written.Add(OutDir / TEXT("frames") / O.Name);
		++OrbitIndex;
		NextPhase();
		return;
	}

	case EState::PerfWarmup:
		if (CompilesPending())
		{
			return;
		}
		if (--FramesLeft <= 0)
		{
			FramesLeft = PerfFrames;
			if (Facts) Facts->BeginPerfWindow();
			for (const FString& Cmd : PerfExecCmds)
			{
				UE_LOG(LogEmberHarness, Display, TEXT("perf exec: %s"), *Cmd);
				GEngine->Exec(GetWorld(), *Cmd);
			}
			State = EState::Perf;
		}
		return;

	case EState::Perf:
		if (const FOrbit* O = FindPerfOrbit())
		{
			// one pose per frame along the path: the frames measured include its streaming
			const int32 I = PerfFrames - FramesLeft;
			const int32 F = FMath::Clamp(static_cast<int32>(static_cast<int64>(I) * (O->Frames - 1) / FMath::Max(1, PerfFrames - 1)), 0, O->Frames - 1);
			if (!PlaceOrbitFrame(*O, F, Err))
			{
				Finish(2, TEXT("perf orbit: ") + Err);
				return;
			}
		}
		if (Vegetation)
		{
			PerfWindTime += DeltaSeconds;  // perf measures the real sway cost
			Vegetation->SetWindTime(PerfWindTime);
		}
		if (Cover)
		{
			Cover->SetWindTime(PerfWindTime);
		}
		if (--FramesLeft <= 0)
		{
			if (Facts)
			{
				Facts->EndPerfWindow();
				const FString FJ = OutDir / TEXT("facts") / TEXT("perf.json");
				UEmberSceneFactsSubsystem::WriteJson(Facts->BuildFacts(Scenario, TEXT("perf")), FJ);
				Written.Add(FJ);
			}
			Finish(0, FString());
		}
		return;
	}
}
