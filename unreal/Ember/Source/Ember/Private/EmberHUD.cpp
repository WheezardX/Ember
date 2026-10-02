#include "EmberHUD.h"

#include "Engine/World.h"

#include "EmberGameMode.h"
#include "EmberHarness.h"

void AEmberHUD::DrawHUD()
{
	Super::DrawHUD();
	const AEmberGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AEmberGameMode>() : nullptr;
	if (GM && GM->Harness && Canvas)
	{
		GM->Harness->DrawTimeline(Canvas);
		GM->Harness->DrawCompass(Canvas);
	}
}
