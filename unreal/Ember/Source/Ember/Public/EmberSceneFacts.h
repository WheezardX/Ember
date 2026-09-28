#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "EmberSceneFacts.generated.h"

class FJsonObject;

/**
 * Scene facts (EPIC_5_PLAN A2): the machine-checkable half of every capture. Records
 * per-frame timings continuously; builds `ember-scene-facts` v1 JSON on request
 * (docs/viz/scene-facts.md). Console: `Ember.DumpFacts <file.json>`.
 */
UCLASS()
class EMBER_API UEmberSceneFactsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return false; }

	/** Facts at this instant. `Capture` names the capture point ("perf" for the perf window). */
	TSharedRef<FJsonObject> BuildFacts(const FString& Scenario, const FString& Capture) const;

	/** Start / stop a perf window; BuildFacts includes `perf` over the last window (or the
	 *  trailing 120 frames if none). */
	void BeginPerfWindow();
	void EndPerfWindow();

	static bool WriteJson(const TSharedRef<FJsonObject>& Obj, const FString& Path);

	/** Per-process dedicated VRAM in use (MB), -1 if unavailable. */
	static double QueryProcessVramMB();

	struct FFrameSample
	{
		float FrameMs, GameMs, RenderMs, GpuMs;
		int32 DrawCalls, Primitives;
	};

private:
	TArray<FFrameSample> Samples;   // ring (bounded)
	int32 WindowStart = INDEX_NONE;
	int32 WindowEnd = INDEX_NONE;
	int64 FrameCount = 0;
};
