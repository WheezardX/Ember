#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "EmberHUD.generated.h"

/** Canvas overlay for play mode: hands the canvas to the harness (the fire timeline bar). */
UCLASS()
class EMBER_API AEmberHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
