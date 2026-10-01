#include "EmberFlyPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

#include "EmberTerrainActor.h"

AEmberFlyPawn::AEmberFlyPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	RootComponent = Camera;
	Camera->bConstrainAspectRatio = false;
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	// Inspection lamp: warm white, ~40 deg cone, 40 m reach, no shadows (cheap in dense foliage).
	// Bright enough to read under canopy at the auto exposure's forest-floor level.
	Lamp = CreateDefaultSubobject<USpotLightComponent>(TEXT("Lamp"));
	Lamp->SetupAttachment(Camera);
	Lamp->SetMobility(EComponentMobility::Movable);
	Lamp->SetIntensityUnits(ELightUnits::Candelas);
	Lamp->SetIntensity(400.f);
	Lamp->SetLightColor(FLinearColor(1.0f, 0.93f, 0.82f));
	Lamp->SetAttenuationRadius(4000.f);
	Lamp->SetInnerConeAngle(12.f);
	Lamp->SetOuterConeAngle(22.f);
	Lamp->SetCastShadows(false);
	Lamp->SetVisibility(false);
}

void AEmberFlyPawn::ToggleLamp()
{
	Lamp->SetVisibility(!Lamp->IsVisible());
}

bool AEmberFlyPawn::IsLampOn() const
{
	return Lamp && Lamp->IsVisible();
}

bool AEmberFlyPawn::GroundZ(const FVector& UE, double& OutUEZ) const
{
	if (!Terrain)
	{
		return false;
	}
	const emberworld::Frame& F = Terrain->GetFrame();
	double Gz = 0.0;
	const double Wx = F.anchor_x + UE.X / 100.0, Wy = F.anchor_y - UE.Y / 100.0;
	// the rendered triangles (smooth for walking); the DEM corner lookup where no tile is loaded
	if (!Terrain->SurfaceAt(Wx, Wy, Gz) && !Terrain->GroundHeightAt(Wx, Wy, Gz))
	{
		return false;
	}
	OutUEZ = Terrain->WorldToUE(0.0, 0.0, Gz).Z;
	return true;
}

void AEmberFlyPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}
	// Look: raw mouse deltas (no input-mapping assets: content-free project).
	float Mx = 0.f, My = 0.f;
	if (!PC->bShowMouseCursor)  // cursor out (Tab: the timeline) - the mouse points, it does not look
	{
		PC->GetInputMouseDelta(Mx, My);
	}
	FRotator R = PC->GetControlRotation();
	R.Yaw += Mx * LookSensitivity * 10.f;
	R.Pitch = FMath::Clamp(FRotator::NormalizeAxis(R.Pitch + My * LookSensitivity * 10.f), -89.f, 89.f);
	PC->SetControlRotation(R);

	auto Down = [&](const FKey& K) { return PC->IsInputKeyDown(K); };
	if (PC->WasInputKeyJustPressed(EKeys::G))
	{
		bWalk = !bWalk;
	}
	if (PC->WasInputKeyJustPressed(EKeys::MouseScrollUp))
	{
		SpeedScale = FMath::Min(SpeedScale * 1.25, 20.0);
	}
	if (PC->WasInputKeyJustPressed(EKeys::MouseScrollDown))
	{
		SpeedScale = FMath::Max(SpeedScale / 1.25, 0.05);
	}

	FVector Loc = GetActorLocation();
	double Gz = 0.0;
	bool bGround = GroundZ(Loc, Gz);
	if (!bGround && Terrain && Terrain->GetRegion())
	{
		// Off the map there is no ground below: measure against the region's lowest ground, or
		// the speed collapsed to its 4 m/s floor at altitude and the camera seemed frozen (Brad).
		Gz = Terrain->WorldToUE(0.0, 0.0, Terrain->GetRegion()->heightmap.z_min).Z;
		bGround = true;
	}
	AglM = bGround ? (Loc.Z - Gz) / 100.0 : -1.0;

	// Speed grows with height: ~4 m/s at eye level, ~0.8 x height above ~5 m (800 m/s at 1 km).
	const double Base = bWalk ? 4.0 : FMath::Max(4.0, 0.8 * FMath::Max(AglM, 0.0));
	double Mult = SpeedScale;
	if (Down(EKeys::LeftShift) || Down(EKeys::RightShift)) Mult *= 4.0;
	if (Down(EKeys::LeftControl) || Down(EKeys::RightControl)) Mult *= 0.25;
	SpeedMs = Base * Mult;

	const FRotator Yaw(0.f, R.Yaw, 0.f);
	const FVector Fwd = bWalk ? Yaw.Vector() : R.Vector();
	const FVector Right = FRotationMatrix(Yaw).GetScaledAxis(EAxis::Y);
	FVector Move = FVector::ZeroVector;
	if (Down(EKeys::W) || Down(EKeys::Up)) Move += Fwd;
	if (Down(EKeys::S) || Down(EKeys::Down)) Move -= Fwd;
	if (Down(EKeys::D) || Down(EKeys::Right)) Move += Right;
	if (Down(EKeys::A) || Down(EKeys::Left)) Move -= Right;
	if (!bWalk)
	{
		if (Down(EKeys::E) || Down(EKeys::SpaceBar)) Move += FVector::UpVector;
		if (Down(EKeys::Q) || Down(EKeys::C)) Move -= FVector::UpVector;
	}
	if (!Move.IsNearlyZero())
	{
		Loc += Move.GetSafeNormal() * SpeedMs * 100.0 * DeltaSeconds;
	}
	if (Terrain && Terrain->GetRegion())
	{
		// Stay within 2 km of the data: past it there is nothing to see and no way to tell where
		// the map went.
		const emberworld::Bounds E = Terrain->GetDataExtent();
		const emberworld::Frame& F = Terrain->GetFrame();
		const double Margin = 2000.0;
		const double Wx = FMath::Clamp(F.anchor_x + Loc.X / 100.0, E.min_x - Margin, E.max_x + Margin);
		const double Wy = FMath::Clamp(F.anchor_y - Loc.Y / 100.0, E.min_y - Margin, E.max_y + Margin);
		Loc.X = (Wx - F.anchor_x) * 100.0;
		Loc.Y = (F.anchor_y - Wy) * 100.0;
	}
	if (GroundZ(Loc, Gz))
	{
		if (bWalk)
		{
			Loc.Z = Gz + EyeM * 100.0;
		}
		else
		{
			Loc.Z = FMath::Max(Loc.Z, Gz + MinAglM * 100.0);
		}
	}
	SetActorLocation(Loc);
}
