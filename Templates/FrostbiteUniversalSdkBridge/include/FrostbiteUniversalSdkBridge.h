#pragma once

#include <cstdint>

#include "FrostbiteUniversal.h"

#if defined(_WIN32)
#define AEGIS_BRIDGE_EXPORT extern "C" __declspec(dllexport)
#define AEGIS_BRIDGE_CALL __stdcall
#else
#define AEGIS_BRIDGE_EXPORT extern "C"
#define AEGIS_BRIDGE_CALL
#endif

AEGIS_BRIDGE_EXPORT std::uint32_t AEGIS_BRIDGE_CALL FrostbiteGame_GetActorModelCount();
AEGIS_BRIDGE_EXPORT int AEGIS_BRIDGE_CALL FrostbiteGame_GetActorModelInfo(
    std::uint32_t index,
    FrostbiteActorModelInfo* outInfo);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetTimescale(float timescale);
AEGIS_BRIDGE_EXPORT float AEGIS_BRIDGE_CALL FrostbiteGame_GetTimescale();

AEGIS_BRIDGE_EXPORT int AEGIS_BRIDGE_CALL FrostbiteGame_ApplyUniversalFeatures(
    const FrostbiteUniversalFeatureState* state);

AEGIS_BRIDGE_EXPORT int AEGIS_BRIDGE_CALL FrostbiteGame_GetUniversalFeatureState(
    FrostbiteUniversalFeatureState* outState);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetSkyboxTint(
    float r,
    float g,
    float b,
    float intensity);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetChams(
    int enabled,
    float r,
    float g,
    float b,
    float opacity);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetDebugMaterialTint(
    int enabled,
    float r,
    float g,
    float b,
    float opacity);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetFogTint(
    int enabled,
    float r,
    float g,
    float b,
    float density);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetFov(float fovDegrees);

AEGIS_BRIDGE_EXPORT void AEGIS_BRIDGE_CALL FrostbiteGame_SetViewAngles(
    float pitch,
    float yaw,
    float roll);
