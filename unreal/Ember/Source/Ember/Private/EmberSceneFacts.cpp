#include "EmberSceneFacts.h"

#include "Camera/PlayerCameraManager.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIStats.h"
#include "RenderTimer.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "EmberEnvironment.h"
#include "EmberFireActor.h"
#include "EmberSmokeActor.h"
#include "EmberTerrainActor.h"
#include "EmberGroundCoverActor.h"
#include "EmberVegetationActor.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
constexpr int32 MaxSamples = 4096;

double Percentile(TArray<float> V, double P)
{
	if (V.Num() == 0) return 0.0;
	V.Sort();
	const int32 I = FMath::Clamp(FMath::CeilToInt(P * V.Num()) - 1, 0, V.Num() - 1);
	return V[I];
}

TSharedRef<FJsonValue> Num(double D) { return MakeShared<FJsonValueNumber>(D); }

TArray<TSharedPtr<FJsonValue>> Vec(const FVector& V)
{
	return { Num(V.X), Num(V.Y), Num(V.Z) };
}
}  // namespace

TStatId UEmberSceneFactsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UEmberSceneFactsSubsystem, STATGROUP_Tickables);
}

void UEmberSceneFactsSubsystem::Tick(float DeltaTime)
{
	FFrameSample S;
	S.FrameMs = FApp::GetDeltaTime() * 1000.0;
	S.GameMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
	S.RenderMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	S.GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
	S.DrawCalls = GNumDrawCallsRHI[0];
	S.Primitives = GNumPrimitivesDrawnRHI[0];
	if (Samples.Num() >= MaxSamples)
	{
		// Keep it simple: drop the oldest half (windows are short relative to the ring).
		const int32 Drop = MaxSamples / 2;
		Samples.RemoveAt(0, Drop);
		if (WindowStart != INDEX_NONE) WindowStart = FMath::Max(0, WindowStart - Drop);
		if (WindowEnd != INDEX_NONE) WindowEnd = FMath::Max(0, WindowEnd - Drop);
	}
	Samples.Add(S);
	++FrameCount;
}

void UEmberSceneFactsSubsystem::BeginPerfWindow()
{
	WindowStart = Samples.Num();
	WindowEnd = INDEX_NONE;
}

void UEmberSceneFactsSubsystem::EndPerfWindow()
{
	WindowEnd = Samples.Num();
}

double UEmberSceneFactsSubsystem::QueryProcessVramMB()
{
#if PLATFORM_WINDOWS
	IDXGIFactory4* Factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&Factory))) || !Factory)
	{
		return -1.0;
	}
	// The adapter with the most dedicated memory is the render GPU on the runner (D2).
	double Best = -1.0;
	SIZE_T BestDedicated = 0;
	IDXGIAdapter1* A1 = nullptr;
	for (UINT i = 0; Factory->EnumAdapters1(i, &A1) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 Desc;
		A1->GetDesc1(&Desc);
		IDXGIAdapter3* A3 = nullptr;
		if (Desc.DedicatedVideoMemory > BestDedicated &&
			SUCCEEDED(A1->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&A3))))
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
			if (SUCCEEDED(A3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
			{
				BestDedicated = Desc.DedicatedVideoMemory;
				Best = Info.CurrentUsage / (1024.0 * 1024.0);
			}
			A3->Release();
		}
		A1->Release();
	}
	Factory->Release();
	return Best;
#else
	return -1.0;
#endif
}

TSharedRef<FJsonObject> UEmberSceneFactsSubsystem::BuildFacts(const FString& Scenario, const FString& Capture) const
{
	UWorld* W = GetWorld();
	TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
	F->SetStringField(TEXT("format"), TEXT("ember-scene-facts"));
	F->SetNumberField(TEXT("version"), 1);
	F->SetStringField(TEXT("scenario"), Scenario);
	F->SetStringField(TEXT("capture"), Capture);
	F->SetNumberField(TEXT("frame"), static_cast<double>(FrameCount));
	F->SetStringField(TEXT("time_utc"), FDateTime::UtcNow().ToIso8601());

	// camera
	TSharedRef<FJsonObject> Cam = MakeShared<FJsonObject>();
	if (APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr)
	{
		if (PC->PlayerCameraManager)
		{
			Cam->SetArrayField(TEXT("location_cm"), Vec(PC->PlayerCameraManager->GetCameraLocation()));
			const FRotator R = PC->PlayerCameraManager->GetCameraRotation();
			Cam->SetArrayField(TEXT("rotation_pyr"), { Num(R.Pitch), Num(R.Yaw), Num(R.Roll) });
			Cam->SetNumberField(TEXT("fov_deg"), PC->PlayerCameraManager->GetFOVAngle());
		}
	}
	F->SetObjectField(TEXT("camera"), Cam);

	// world + tiles (the first terrain actor)
	TSharedRef<FJsonObject> World = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Tiles = MakeShared<FJsonObject>();
	for (TActorIterator<AEmberTerrainActor> It(W); It; ++It)
	{
		const AEmberTerrainActor* T = *It;
		if (const emberworld::Region* R = T->GetRegion())
		{
			World->SetStringField(TEXT("region"), T->RegionName);
			World->SetStringField(TEXT("crs"), UTF8_TO_TCHAR(R->crs.c_str()));
			World->SetNumberField(TEXT("tile_px"), R->tile_px);
			const emberworld::Frame& Fr = T->GetFrame();
			World->SetArrayField(TEXT("anchor_m"), { Num(Fr.anchor_x), Num(Fr.anchor_y), Num(Fr.anchor_z) });
			const emberworld::Bounds E = R->extent();
			World->SetArrayField(TEXT("extent_m"), { Num(E.min_x), Num(E.min_y), Num(E.max_x), Num(E.max_y) });
			const emberworld::Bounds& D = T->GetDataExtent();
			World->SetArrayField(TEXT("data_extent_m"), { Num(D.min_x), Num(D.min_y), Num(D.max_x), Num(D.max_y) });
		}
		Tiles->SetNumberField(TEXT("total"), T->TilesTotal);
		Tiles->SetNumberField(TEXT("loaded"), T->TilesLoaded);
		Tiles->SetNumberField(TEXT("triangles"), static_cast<double>(T->Triangles));
		Tiles->SetNumberField(TEXT("skirt_triangles"), static_cast<double>(T->SkirtTriangles));
		Tiles->SetNumberField(TEXT("nodata_corners"), static_cast<double>(T->NodataCorners));
		Tiles->SetNumberField(TEXT("load_ms"), T->LoadMs);
		Tiles->SetBoolField(TEXT("streaming"), T->IsStreaming());
		Tiles->SetNumberField(TEXT("stream_updates"), T->StreamUpdates);
		Tiles->SetNumberField(TEXT("last_stream_ms"), T->LastStreamMs);
		Tiles->SetStringField(TEXT("look"), T->LookName);
		Tiles->SetNumberField(TEXT("compose_ms"), T->ComposeMs);
		Tiles->SetNumberField(TEXT("water_tiles"), T->WaterTilesLoaded);
		Tiles->SetNumberField(TEXT("water_triangles"), static_cast<double>(T->WaterTriangles));
		TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
		for (const auto& KV : T->LodHistogram)
		{
			H->SetNumberField(FString::FromInt(KV.Key), KV.Value);
		}
		Tiles->SetObjectField(TEXT("lod_histogram"), H);
		break;
	}
	F->SetObjectField(TEXT("world"), World);
	F->SetObjectField(TEXT("tiles"), Tiles);

	TSharedRef<FJsonObject> Inst = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> BySp = MakeShared<FJsonObject>();
	Inst->SetNumberField(TEXT("total"), 0);
	Inst->SetBoolField(TEXT("enabled"), false);
	for (TActorIterator<AEmberGroundCoverActor> It(W); It; ++It)
	{
		Inst->SetNumberField(TEXT("ground_cover"), static_cast<double>(It->GetInstanceCount()));
		Inst->SetNumberField(TEXT("ground_cover_cells"), It->GetCellCount());
	}
	for (TActorIterator<AEmberVegetationActor> It(W); It; ++It)
	{
		Inst->SetBoolField(TEXT("enabled"), true);
		Inst->SetNumberField(TEXT("total"), static_cast<double>(It->InstancesTotal));
		Inst->SetNumberField(TEXT("tiles"), It->TilesLoaded);
		Inst->SetNumberField(TEXT("radius_m"), It->RadiusM);
		Inst->SetNumberField(TEXT("scatter_ms"), It->ScatterMs);
		Inst->SetNumberField(TEXT("ungrounded"), static_cast<double>(It->UngroundedInstances));
		Inst->SetNumberField(TEXT("no_surface"), static_cast<double>(It->NoSurfaceInstances));
		Inst->SetNumberField(TEXT("tiles_near"), It->TilesNear);
		Inst->SetNumberField(TEXT("cells_near"), It->CellsNear);
		Inst->SetNumberField(TEXT("near"), static_cast<double>(It->InstancesNear));
		Inst->SetNumberField(TEXT("near_radius_m"), It->NearRadiusM);
		Inst->SetNumberField(TEXT("mid_culled"), static_cast<double>(It->MidCulled));
		Inst->SetNumberField(TEXT("generated_species"), It->GeneratedSpecies);
		for (const auto& KV : It->BySpecies)
		{
			BySp->SetNumberField(KV.Key, static_cast<double>(KV.Value));
		}
		break;
	}
	Inst->SetObjectField(TEXT("by_species"), BySp);
	F->SetObjectField(TEXT("instances"), Inst);

	TSharedRef<FJsonObject> FireJ = MakeShared<FJsonObject>();
	FireJ->SetBoolField(TEXT("enabled"), false);
	for (TActorIterator<AEmberFireActor> It(W); It; ++It)
	{
		FireJ->SetBoolField(TEXT("enabled"), true);
		FireJ->SetStringField(TEXT("model"), It->ModelId);
		FireJ->SetNumberField(TEXT("t_s"), It->TimeS);
		FireJ->SetNumberField(TEXT("tick"), It->Tick);
		FireJ->SetStringField(TEXT("time_utc"), FDateTime::FromUnixTimestamp(It->T0Unix + static_cast<int64>(It->TimeS)).ToIso8601());
		FireJ->SetNumberField(TEXT("cells_burning"), static_cast<double>(It->CellsBurning));
		FireJ->SetNumberField(TEXT("cells_burned"), static_cast<double>(It->CellsBurned));
		FireJ->SetNumberField(TEXT("stream_burned"), static_cast<double>(It->StreamBurned));
		FireJ->SetNumberField(TEXT("burned_ha"), It->CellsBurned * It->CellM * It->CellM / 1e4);
		FireJ->SetNumberField(TEXT("start_s"), It->StartS);
		FireJ->SetNumberField(TEXT("end_s"), It->EndS);
		TSharedRef<FJsonObject> ProbesJ = MakeShared<FJsonObject>();
		for (const AEmberFireActor::FProbe& Pr : It->Probes)
		{
			ProbesJ->SetNumberField(Pr.Name, It->PhaseAt(Pr.X, Pr.Y));
		}
		FireJ->SetObjectField(TEXT("probes"), ProbesJ);
		break;
	}
	F->SetObjectField(TEXT("fire"), FireJ);

	TSharedRef<FJsonObject> SmokeJ = MakeShared<FJsonObject>();
	SmokeJ->SetBoolField(TEXT("enabled"), false);
	for (TActorIterator<AEmberSmokeActor> It(W); It; ++It)
	{
		SmokeJ->SetBoolField(TEXT("enabled"), true);
		SmokeJ->SetNumberField(TEXT("sources"), It->NumSources);
		SmokeJ->SetNumberField(TEXT("plumes"), It->NumPlumes);
		SmokeJ->SetNumberField(TEXT("puffs"), It->NumPuffs);
		SmokeJ->SetNumberField(TEXT("max_top_m"), It->MaxTopM);
		break;
	}
	F->SetObjectField(TEXT("smoke"), SmokeJ);

	TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
	for (TActorIterator<AEmberEnvironment> It(W); It; ++It)
	{
		Env->SetStringField(TEXT("sun"), It->SunPreset);
		Env->SetNumberField(TEXT("sun_azimuth_deg"), It->SunAzimuth);
		Env->SetNumberField(TEXT("sun_elevation_deg"), It->SunElevation);
		Env->SetNumberField(TEXT("exposure_bias"), It->ExposureBias);
		break;
	}
	F->SetObjectField(TEXT("environment"), Env);

	// render
	TSharedRef<FJsonObject> Render = MakeShared<FJsonObject>();
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint Sz = GEngine->GameViewport->Viewport->GetSizeXY();
		Render->SetArrayField(TEXT("resolution"), { Num(Sz.X), Num(Sz.Y) });
	}
	Render->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("none"));
	Render->SetStringField(TEXT("gpu"), GRHIAdapterName);
	const FFrameSample* Last = Samples.Num() ? &Samples.Last() : nullptr;
	Render->SetNumberField(TEXT("draw_calls"), Last ? Last->DrawCalls : 0);
	Render->SetNumberField(TEXT("primitives"), Last ? Last->Primitives : 0);
	Render->SetNumberField(TEXT("vram_mb"), QueryProcessVramMB());
	FTextureMemoryStats TMS;
	RHIGetTextureMemoryStats(TMS);
	Render->SetNumberField(TEXT("texture_mb"), (TMS.StreamingMemorySize + TMS.NonStreamingMemorySize) / (1024.0 * 1024.0));
	F->SetObjectField(TEXT("render"), Render);

	// perf over the window (or trailing 120 frames)
	int32 B = WindowStart, E = WindowEnd;
	if (B == INDEX_NONE)
	{
		E = Samples.Num();
		B = FMath::Max(0, E - 120);
	}
	else if (E == INDEX_NONE)
	{
		E = Samples.Num();
	}
	TArray<float> Fm, Gm, Rm, Gp;
	double DrawSum = 0, PrimSum = 0;
	for (int32 i = B; i < E; ++i)
	{
		Fm.Add(Samples[i].FrameMs);
		Gm.Add(Samples[i].GameMs);
		Rm.Add(Samples[i].RenderMs);
		Gp.Add(Samples[i].GpuMs);
		DrawSum += Samples[i].DrawCalls;
		PrimSum += Samples[i].Primitives;
	}
	TSharedRef<FJsonObject> Perf = MakeShared<FJsonObject>();
	const int32 N = Fm.Num();
	auto Avg = [](const TArray<float>& V) { double S = 0; for (float x : V) S += x; return V.Num() ? S / V.Num() : 0.0; };
	Perf->SetNumberField(TEXT("frames"), N);
	Perf->SetNumberField(TEXT("frame_ms_avg"), Avg(Fm));
	Perf->SetNumberField(TEXT("frame_ms_p50"), Percentile(Fm, 0.50));
	Perf->SetNumberField(TEXT("frame_ms_p95"), Percentile(Fm, 0.95));
	Perf->SetNumberField(TEXT("frame_ms_max"), Percentile(Fm, 1.0));
	{
		int32 H33 = 0, H50 = 0, H100 = 0;  // hitches: frames over 2x / 3x / 6x a 60 fps frame
		for (float Ms : Fm)
		{
			H33 += Ms > 33.3f;
			H50 += Ms > 50.f;
			H100 += Ms > 100.f;
		}
		Perf->SetNumberField(TEXT("hitches_33ms"), H33);
		Perf->SetNumberField(TEXT("hitches_50ms"), H50);
		Perf->SetNumberField(TEXT("hitches_100ms"), H100);
	}
	Perf->SetNumberField(TEXT("game_ms_avg"), Avg(Gm));
	Perf->SetNumberField(TEXT("render_ms_avg"), Avg(Rm));
	Perf->SetNumberField(TEXT("gpu_ms_avg"), Avg(Gp));
	Perf->SetNumberField(TEXT("gpu_ms_p95"), Percentile(Gp, 0.95));
	Perf->SetNumberField(TEXT("fps_avg"), Avg(Fm) > 0 ? 1000.0 / Avg(Fm) : 0.0);
	Perf->SetNumberField(TEXT("draw_calls_avg"), N ? DrawSum / N : 0.0);
	Perf->SetNumberField(TEXT("primitives_avg"), N ? PrimSum / N : 0.0);
	if (WindowStart != INDEX_NONE)
	{
		// Per-frame series of the measured window (spike patterns; bundle perf CSVs).
		auto Series = [](const TArray<float>& V)
		{
			TArray<TSharedPtr<FJsonValue>> A;
			for (float x : V) A.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(x * 100.0) / 100.0));
			return A;
		};
		Perf->SetArrayField(TEXT("frame_ms_series"), Series(Fm));
		Perf->SetArrayField(TEXT("game_ms_series"), Series(Gm));
		Perf->SetArrayField(TEXT("gpu_ms_series"), Series(Gp));
	}
	F->SetObjectField(TEXT("perf"), Perf);
	return F;
}

bool UEmberSceneFactsSubsystem::WriteJson(const TSharedRef<FJsonObject>& Obj, const FString& Path)
{
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	if (!FJsonSerializer::Serialize(Obj, Writer))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

static FAutoConsoleCommandWithWorldAndArgs GEmberDumpFactsCmd(
	TEXT("Ember.DumpFacts"),
	TEXT("Ember.DumpFacts <path.json> — write scene facts (ember-scene-facts v1) now."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World) return;
		UEmberSceneFactsSubsystem* S = World->GetSubsystem<UEmberSceneFactsSubsystem>();
		if (!S) return;
		const FString Path = Args.Num() ? Args[0] : FPaths::ProjectSavedDir() / TEXT("facts.json");
		UEmberSceneFactsSubsystem::WriteJson(S->BuildFacts(TEXT("console"), TEXT("console")), Path);
		UE_LOG(LogTemp, Display, TEXT("Ember.DumpFacts -> %s"), *Path);
	}));
