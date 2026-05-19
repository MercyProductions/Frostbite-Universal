# Frostbite Project Bridge Example

Use this only in a Frostbite project you own/control.

For a copyable SDK-based template, start with:

```text
Templates/FrostbiteUniversalSdkBridge/README.md
```

That template wraps the exports below and leaves the SDK-specific work in `SdkBindings.cpp`.

Export these functions from your game executable or owned bridge DLL so `INJECT_THIS_FrostbiteUniversal.dll` can auto-detect them after it is loaded through your local test/plugin flow.

The `FrostbiteGame_*` names are only the default names. If your owned project already uses different names, edit `FrostbiteUniversal_Bridge.ini` beside the Universal DLL:

```ini
[BridgeExports]
ModuleName=
GetActorModelCount=MyGame_GetActorModelCount
GetActorModelInfo=MyGame_GetActorModelInfo
SetTimescale=MyGame_SetTimescale
GetTimescale=MyGame_GetTimescale
ApplyFeatures=MyGame_ApplyUniversalFeatures
GetFeatureState=MyGame_GetUniversalFeatureState
SetSkyboxTint=MyGame_SetSkyboxTint
SetDebugMaterialTint=MyGame_SetDebugMaterialTint
SetFogTint=MyGame_SetFogTint
ExecuteConsoleCommand=MyGame_ExecuteConsoleCommand
```

```cpp
#include <cmath>

#include "FrostbiteUniversal.h"

extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteGame_GetActorModelCount()
{
    return static_cast<std::uint32_t>(YourActorList.size());
}

extern "C" __declspec(dllexport)
int __stdcall FrostbiteGame_GetActorModelInfo(std::uint32_t index, FrostbiteActorModelInfo* outInfo)
{
    if (!outInfo || index >= YourActorList.size())
        return 0;

    const auto& actor = YourActorList[index];
    ZeroMemory(outInfo, sizeof(*outInfo));

    outInfo->id = actor.Id;
    wcscpy_s(outInfo->actorName, actor.Name.c_str());
    wcscpy_s(outInfo->className, actor.ClassName.c_str());
    wcscpy_s(outInfo->modelName, actor.ModelName.c_str());
    wcscpy_s(outInfo->assetPath, actor.AssetPath.c_str());
    outInfo->position[0] = actor.Position.x;
    outInfo->position[1] = actor.Position.y;
    outInfo->position[2] = actor.Position.z;
    outInfo->rotationEuler[0] = actor.RotationEuler.x;
    outInfo->rotationEuler[1] = actor.RotationEuler.y;
    outInfo->rotationEuler[2] = actor.RotationEuler.z;
    outInfo->scale[0] = actor.Scale.x;
    outInfo->scale[1] = actor.Scale.y;
    outInfo->scale[2] = actor.Scale.z;
    outInfo->boundsMin[0] = actor.BoundsMin.x;
    outInfo->boundsMin[1] = actor.BoundsMin.y;
    outInfo->boundsMin[2] = actor.BoundsMin.z;
    outInfo->boundsMax[0] = actor.BoundsMax.x;
    outInfo->boundsMax[1] = actor.BoundsMax.y;
    outInfo->boundsMax[2] = actor.BoundsMax.z;
    outInfo->size[0] = actor.BoundsMax.x - actor.BoundsMin.x;
    outInfo->size[1] = actor.BoundsMax.y - actor.BoundsMin.y;
    outInfo->size[2] = actor.BoundsMax.z - actor.BoundsMin.z;
    outInfo->radius = sqrtf(
        (outInfo->size[0] * 0.5f) * (outInfo->size[0] * 0.5f) +
        (outInfo->size[1] * 0.5f) * (outInfo->size[1] * 0.5f) +
        (outInfo->size[2] * 0.5f) * (outInfo->size[2] * 0.5f));
    outInfo->flags = FrostbiteActorModel_Actor |
        FrostbiteActorModel_Model |
        FrostbiteActorModel_Dynamic |
        FrostbiteActorModel_Visible;

    return 1;
}

extern "C" __declspec(dllexport)
void __stdcall FrostbiteGame_SetTimescale(float timescale)
{
    timescale = timescale < 0.01f ? 0.01f : timescale;
    timescale = timescale > 20.0f ? 20.0f : timescale;
    YourEngineOrWorld->SetTimeScale(timescale);
}

extern "C" __declspec(dllexport)
float __stdcall FrostbiteGame_GetTimescale()
{
    return YourEngineOrWorld->GetTimeScale();
}

extern "C" __declspec(dllexport)
int __stdcall FrostbiteGame_ApplyUniversalFeatures(const FrostbiteUniversalFeatureState* state)
{
    if (!state)
        return 0;

    if ((state->enabledFlags & FrostbiteFeature_Timescale) != 0)
        YourEngineOrWorld->SetTimeScale(state->timescale);

    if ((state->enabledFlags & (FrostbiteFeature_SkyboxTint | FrostbiteFeature_SkyboxRainbow)) != 0)
        YourSkySystem->SetTint(state->skyboxColor[0], state->skyboxColor[1], state->skyboxColor[2], state->skyboxIntensity);

    // FrostbiteFeature_Chams is the legacy ABI flag name for debug material tint.
    if ((state->enabledFlags & (FrostbiteFeature_Chams | FrostbiteFeature_ChamsRainbow | FrostbiteFeature_WireframeDebug)) != 0)
        YourDebugRenderer->SetModelTint(state->chamsColor[0], state->chamsColor[1], state->chamsColor[2], state->chamsOpacity);

    if ((state->enabledFlags & FrostbiteFeature_FogTint) != 0)
        YourWorldRenderer->SetFogTint(state->fogColor[0], state->fogColor[1], state->fogColor[2], state->fogDensity);

    if ((state->enabledFlags & FrostbiteFeature_FovOverride) != 0)
        YourDebugCamera->SetFovDegrees(state->fovDegrees);

    if ((state->enabledFlags & FrostbiteFeature_ViewAnglePreview) != 0 && state->hasViewTarget)
        YourDebugCamera->SetPreviewViewAngles(state->viewAngles[0], state->viewAngles[1], state->viewAngles[2]);

    return 1;
}

extern "C" __declspec(dllexport)
int __stdcall FrostbiteGame_GetUniversalFeatureState(FrostbiteUniversalFeatureState* outState)
{
    if (!outState)
        return 0;

    ZeroMemory(outState, sizeof(*outState));
    outState->size = sizeof(*outState);
    outState->timescale = YourEngineOrWorld->GetTimeScale();
    outState->fovDegrees = YourDebugCamera->GetFovDegrees();
    return 1;
}

extern "C" __declspec(dllexport)
void __stdcall FrostbiteGame_SetSkyboxTint(float r, float g, float b, float intensity)
{
    YourSkySystem->SetTint(r, g, b, intensity);
}

extern "C" __declspec(dllexport)
void __stdcall FrostbiteGame_SetDebugMaterialTint(int enabled, float r, float g, float b, float opacity)
{
    YourDebugRenderer->SetModelTintEnabled(enabled != 0);
    YourDebugRenderer->SetModelTint(r, g, b, opacity);
}

extern "C" __declspec(dllexport)
void __stdcall FrostbiteGame_SetFogTint(int enabled, float r, float g, float b, float density)
{
    YourWorldRenderer->SetFogTintEnabled(enabled != 0);
    YourWorldRenderer->SetFogTint(r, g, b, density);
}

extern "C" __declspec(dllexport)
int __stdcall FrostbiteGame_ExecuteConsoleCommand(const wchar_t* command)
{
    if (!command || command[0] == L'\0')
        return 0;

    return YourDebugConsole->Execute(command) ? 1 : 0;
}
```

After injection, open the Universal ImGui **Project** tab:

- `Refresh Actors/Models` pulls the current list.
- `Timescale` applies through your configured set-timescale export.
- `Sync` reads through your configured get-timescale export.
- `Apply Features` pushes `FrostbiteUniversalFeatureState` through your configured apply-features export.
- `Install MinHook Hooks` hooks your configured get-timescale and get-feature-state exports if your game exports them with the signatures above.

The DLL only reads what your project intentionally exposes through this bridge.
It detects the bridge exports during load, but it does not call them until you press one of the Project tab buttons or call the matching Universal API yourself.
