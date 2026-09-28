#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "EmberGameMode.generated.h"

class AEmberEnvironment;
class AEmberHarness;
class AEmberTerrainActor;

/**
 * Entry point for every run mode. Content-free: the engine's Entry map is loaded and this
 * mode spawns everything at runtime.
 *   -EmberRun=<plan.json>   harness mode (captures + facts, then exit) — ember-dev run-scenario
 *   -EmberWorld=<region>    interactive: free-fly spectator over a Terrain region
 */
UCLASS()
class EMBER_API AEmberGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AEmberGameMode();
	virtual void StartPlay() override;

	UPROPERTY(Transient) TObjectPtr<AEmberEnvironment> Environment;
	UPROPERTY(Transient) TObjectPtr<AEmberHarness> Harness;
	UPROPERTY(Transient) TObjectPtr<AEmberTerrainActor> Terrain;
};
