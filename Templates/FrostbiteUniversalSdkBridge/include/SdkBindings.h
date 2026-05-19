#pragma once

#include <cstdint>
#include <string>

#include "FrostbiteUniversal.h"

namespace Aegis::FrostbiteUniversalTemplate::Sdk
{
    struct Vector3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Bounds3
    {
        Vector3 min;
        Vector3 max;
        Vector3 size;
        Vector3 center;
        float radius = 0.0f;
    };

    struct ScreenRect
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        float depth = 0.0f;
        bool valid = false;
    };

    struct ActorModel
    {
        std::uint64_t id = 0;
        std::wstring actorName;
        std::wstring className;
        std::wstring modelName;
        std::wstring assetPath;
        Vector3 position;
        Vector3 rotationEuler;
        Vector3 scale = { 1.0f, 1.0f, 1.0f };
        Bounds3 bounds;
        ScreenRect screen;
        std::uint32_t flags = FrostbiteActorModel_Actor |
            FrostbiteActorModel_Model |
            FrostbiteActorModel_Dynamic |
            FrostbiteActorModel_Visible;
    };

    struct Color4
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct FeaturePreviewState
    {
        float timescale = 1.0f;
        float baseFrameDelta = 1.0f / 60.0f;
        float scaledFrameDelta = 1.0f / 60.0f;
        bool skyboxTintEnabled = false;
        bool skyboxRainbowEnabled = false;
        Color4 skyboxColor;
        float skyboxIntensity = 1.0f;
        float skyboxRainbowSpeed = 0.0f;
        float skyboxEffectiveRainbowSpeed = 0.0f;
        bool modelDebugTintEnabled = false;
        bool modelDebugRainbowEnabled = false;
        bool wireframeDebugEnabled = false;
        Color4 modelDebugColor;
        float modelDebugOpacity = 0.70f;
        bool fogTintEnabled = false;
        Color4 fogColor;
        float fogDensity = 0.015f;
        bool debugBoxesEnabled = false;
        bool snaplinesEnabled = false;
        bool fovOverrideEnabled = false;
        float fovDegrees = 75.0f;
        bool viewAnglePreviewEnabled = false;
        bool hasViewTarget = false;
        std::uint64_t viewTargetActorId = 0;
        Vector3 viewTargetPosition;
        Vector3 viewAngles;
    };

    struct RuntimeFeatureCallbacks
    {
        void* userData = nullptr;
        void (*setTimescale)(void* userData, float timescale) = nullptr;
        bool (*getTimescale)(void* userData, float* outTimescale) = nullptr;
        void (*setSkyboxTint)(void* userData, const Color4* color, float intensity) = nullptr;
        void (*setModelDebugTint)(void* userData, bool enabled, const Color4* color, float opacity) = nullptr;
        void (*setFogTint)(void* userData, bool enabled, const Color4* color, float density) = nullptr;
        void (*setFov)(void* userData, float fovDegrees) = nullptr;
        void (*setViewAngles)(void* userData, const Vector3* angles, const Vector3* targetPosition) = nullptr;
    };

    void InitializeIfNeeded();
    void SetRuntimeFeatureCallbacks(const RuntimeFeatureCallbacks& callbacks);
    void ClearRuntimeFeatureCallbacks();

    void RefreshActorModelCache();
    std::uint32_t GetActorModelCount();
    bool GetActorModel(std::uint32_t index, ActorModel& outModel);

    void SetTimescale(float timescale);
    float GetTimescale();

    void ApplySkyboxTint(float r, float g, float b, float intensity);
    void ApplyModelDebugTint(bool enabled, float r, float g, float b, float opacity);
    void ApplyFogTint(bool enabled, float r, float g, float b, float density);
    void ApplyFov(float fovDegrees);
    void ApplyViewAngles(const Vector3& angles);
    void ApplyViewAnglePreview(const Vector3& targetPosition, std::uint64_t actorId);

    int ApplyFeatureState(const FrostbiteUniversalFeatureState& state);
    bool ReadFeatureState(FrostbiteUniversalFeatureState& outState);
    FeaturePreviewState GetFeaturePreviewState();
    bool ExecuteConsoleCommand(const wchar_t* command);
}
