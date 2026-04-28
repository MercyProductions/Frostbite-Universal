#include "FrostbiteUniversalSdkBridge.h"

#include <algorithm>
#include <cwchar>

#include "SdkBindings.h"

namespace
{
    float ClampTimescale(float value)
    {
        if (value < 0.01f)
            return 0.01f;
        if (value > 20.0f)
            return 20.0f;
        return value;
    }

    template <std::size_t Count>
    void CopyWide(wchar_t (&destination)[Count], const std::wstring& source)
    {
        static_assert(Count > 0);
        destination[0] = L'\0';
        if (source.empty())
            return;

        const std::size_t length = std::min<std::size_t>(source.size(), Count - 1);
        std::wmemcpy(destination, source.c_str(), length);
        destination[length] = L'\0';
    }
}

AEGIS_BRIDGE_EXPORT std::uint32_t AEGIS_BRIDGE_CALL FrostbiteGame_GetActorModelCount()
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::RefreshActorModelCache();
    return Aegis::FrostbiteUniversalTemplate::Sdk::GetActorModelCount();
}

AEGIS_BRIDGE_EXPORT int AEGIS_BRIDGE_CALL FrostbiteGame_GetActorModelInfo(
    std::uint32_t index,
    FrostbiteActorModelInfo* outInfo)
{
    if (!outInfo)
        return 0;

    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();

    Aegis::FrostbiteUniversalTemplate::Sdk::ActorModel model;
    if (!Aegis::FrostbiteUniversalTemplate::Sdk::GetActorModel(index, model))
        return 0;

    *outInfo = {};
    outInfo->id = model.id;
    CopyWide(outInfo->actorName, model.actorName);
    CopyWide(outInfo->className, model.className);
    CopyWide(outInfo->modelName, model.modelName);
    CopyWide(outInfo->assetPath, model.assetPath);
    outInfo->position[0] = model.position.x;
    outInfo->position[1] = model.position.y;
    outInfo->position[2] = model.position.z;
    outInfo->rotationEuler[0] = model.rotationEuler.x;
    outInfo->rotationEuler[1] = model.rotationEuler.y;
    outInfo->rotationEuler[2] = model.rotationEuler.z;
    outInfo->scale[0] = model.scale.x;
    outInfo->scale[1] = model.scale.y;
    outInfo->scale[2] = model.scale.z;
    outInfo->boundsMin[0] = model.bounds.min.x;
    outInfo->boundsMin[1] = model.bounds.min.y;
    outInfo->boundsMin[2] = model.bounds.min.z;
    outInfo->boundsMax[0] = model.bounds.max.x;
    outInfo->boundsMax[1] = model.bounds.max.y;
    outInfo->boundsMax[2] = model.bounds.max.z;
    outInfo->size[0] = model.bounds.size.x;
    outInfo->size[1] = model.bounds.size.y;
    outInfo->size[2] = model.bounds.size.z;
    outInfo->radius = model.bounds.radius;
    outInfo->screenPosition[0] = model.screen.centerX;
    outInfo->screenPosition[1] = model.screen.centerY;
    outInfo->screenBoundsMin[0] = model.screen.minX;
    outInfo->screenBoundsMin[1] = model.screen.minY;
    outInfo->screenBoundsMax[0] = model.screen.maxX;
    outInfo->screenBoundsMax[1] = model.screen.maxY;
    outInfo->screenDepth = model.screen.depth;
    outInfo->flags = model.flags;
    if (model.screen.valid)
        outInfo->flags |= FrostbiteActorModel_HasScreenProjection;
    return 1;
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetTimescale(float timescale)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::SetTimescale(ClampTimescale(timescale));
}

AEGIS_BRIDGE_EXPORT float AEGIS_BRIDGE_CALL FrostbiteGame_GetTimescale()
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    return Aegis::FrostbiteUniversalTemplate::Sdk::GetTimescale();
}

AEGIS_BRIDGE_EXPORT int AEGIS_BRIDGE_CALL FrostbiteGame_ApplyUniversalFeatures(
    const FrostbiteUniversalFeatureState* state)
{
    if (!state)
        return 0;

    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    return Aegis::FrostbiteUniversalTemplate::Sdk::ApplyFeatureState(*state);
}

AEGIS_BRIDGE_EXPORT int AEGIS_BRIDGE_CALL FrostbiteGame_GetUniversalFeatureState(
    FrostbiteUniversalFeatureState* outState)
{
    if (!outState)
        return 0;

    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    return Aegis::FrostbiteUniversalTemplate::Sdk::ReadFeatureState(*outState) ? 1 : 0;
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetSkyboxTint(
    float r,
    float g,
    float b,
    float intensity)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::ApplySkyboxTint(r, g, b, intensity);
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetChams(
    int enabled,
    float r,
    float g,
    float b,
    float opacity)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::ApplyModelDebugTint(enabled != 0, r, g, b, opacity);
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetDebugMaterialTint(
    int enabled,
    float r,
    float g,
    float b,
    float opacity)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::ApplyModelDebugTint(enabled != 0, r, g, b, opacity);
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetFogTint(
    int enabled,
    float r,
    float g,
    float b,
    float density)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::ApplyFogTint(enabled != 0, r, g, b, density);
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetFov(float fovDegrees)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::ApplyFov(fovDegrees);
}

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetViewAngles(
    float pitch,
    float yaw,
    float roll)
{
    Aegis::FrostbiteUniversalTemplate::Sdk::InitializeIfNeeded();
    Aegis::FrostbiteUniversalTemplate::Sdk::ApplyViewAngles({ pitch, yaw, roll });
}
