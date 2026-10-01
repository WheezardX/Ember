#include "EmberGameMode.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "EmberEnvironment.h"
#include "EmberHarness.h"
#include "EmberHUD.h"
#include "EmberTerrainActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEmberGame, Log, All);

AEmberGameMode::AEmberGameMode()
{
	DefaultPawnClass = ASpectatorPawn::StaticClass();  // play mode swaps in AEmberFlyPawn
	HUDClass = AEmberHUD::StaticClass();               // play mode's fire timeline (draws nothing otherwise)
}

void AEmberGameMode::StartPlay()
{
	Super::StartPlay();
	UWorld* W = GetWorld();
	FActorSpawnParameters P;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	Environment = W->SpawnActor<AEmberEnvironment>(P);
	Environment->Build();

	FString Plan, WorldDir;
	if (FParse::Value(FCommandLine::Get(), TEXT("EmberRun="), Plan))
	{
		Harness = W->SpawnActor<AEmberHarness>(P);
		Harness->Start(Plan, Environment);
		return;
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("EmberWorld="), WorldDir))
	{
		Terrain = W->SpawnActor<AEmberTerrainActor>(P);
		FString Err;
		if (!Terrain->LoadRegion(WorldDir, -1, Err))
		{
			UE_LOG(LogEmberGame, Error, TEXT("EmberWorld load failed: %s"), *Err);
			return;
		}
		Terrain->SetBaseColor(FLinearColor(0.35f, 0.35f, 0.35f));
		// Start the spectator above the region centre looking north-ish and down.
		if (APlayerController* PC = W->GetFirstPlayerController())
		{
			if (APawn* Pawn = PC->GetPawn())
			{
				Pawn->SetActorLocation(FVector(0, 60000, 60000));
				PC->SetControlRotation(FRotator(-35, -90, 0));
			}
		}
		return;
	}
	UE_LOG(LogEmberGame, Warning, TEXT("No -EmberRun= or -EmberWorld= given; nothing to show."));
}
