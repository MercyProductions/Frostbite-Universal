#include "SdkBindings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

#if __has_include("SDK/SDK.hpp")
#include "SDK/SDK.hpp"
#define AEGIS_TEMPLATE_HAS_GENERATED_SDK 1
#elif __has_include("SDK.hpp")
#include "SDK.hpp"
#define AEGIS_TEMPLATE_HAS_GENERATED_SDK 1
#else
#define AEGIS_TEMPLATE_HAS_GENERATED_SDK 0
#endif

#ifndef AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA
#define AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA 1
#endif

namespace Aegis::FrostbiteUniversalTemplate::Sdk
{
    namespace
    {
        constexpr float kBaseFrameDelta = 1.0f / 60.0f;

        std::once_flag g_initializeOnce;
        std::mutex g_stateMutex;
        std::atomic<float> g_timescale = 1.0f;
        FrostbiteUniversalFeatureState g_lastFeatureState = {};
        FeaturePreviewState g_previewState = {};
        RuntimeFeatureCallbacks g_callbacks = {};
        std::vector<ActorModel> g_actorModels;

        float Clamp01(float value)
        {
            if (value < 0.0f)
                return 0.0f;
            if (value > 1.0f)
                return 1.0f;
            return value;
        }

        float ClampTimescale(float value)
        {
            if (value < 0.01f)
                return 0.01f;
            if (value > 20.0f)
                return 20.0f;
            return value;
        }

        float ClampFov(float value)
        {
            if (value < 30.0f)
                return 30.0f;
            if (value > 140.0f)
                return 140.0f;
            return value;
        }

        Color4 MakeColor(float r, float g, float b, float a = 1.0f)
        {
            return {
                Clamp01(r),
                Clamp01(g),
                Clamp01(b),
                Clamp01(a)
            };
        }

        Vector3 Add(Vector3 left, Vector3 right)
        {
            return {
                left.x + right.x,
                left.y + right.y,
                left.z + right.z
            };
        }

        Vector3 Subtract(Vector3 left, Vector3 right)
        {
            return {
                left.x - right.x,
                left.y - right.y,
                left.z - right.z
            };
        }

        Vector3 Scale(Vector3 value, float scalar)
        {
            return {
                value.x * scalar,
                value.y * scalar,
                value.z * scalar
            };
        }

        float Length(Vector3 value)
        {
            return std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
        }

        Vector3 ComputeViewAnglesToTarget(Vector3 cameraPosition, Vector3 targetPosition)
        {
            constexpr float kRadiansToDegrees = 57.2957795f;
            const Vector3 delta = Subtract(targetPosition, cameraPosition);
            const float horizontal = std::sqrt((delta.x * delta.x) + (delta.z * delta.z));

            Vector3 angles = {};
            angles.x = -std::atan2(delta.y, horizontal) * kRadiansToDegrees;
            angles.y = std::atan2(delta.x, delta.z) * kRadiansToDegrees;
            angles.z = 0.0f;
            return angles;
        }

        Bounds3 MakeBoundsFromMinMax(Vector3 min, Vector3 max)
        {
            Bounds3 bounds = {};
            bounds.min = min;
            bounds.max = max;
            bounds.size = {
                std::max(0.0f, max.x - min.x),
                std::max(0.0f, max.y - min.y),
                std::max(0.0f, max.z - min.z)
            };
            bounds.center = Scale(Add(min, max), 0.5f);
            bounds.radius = Length(Scale(bounds.size, 0.5f));
            return bounds;
        }

        Bounds3 MakeBoundsFromCenterSize(Vector3 center, Vector3 size)
        {
            const Vector3 halfSize = Scale(size, 0.5f);
            return MakeBoundsFromMinMax(Subtract(center, halfSize), Add(center, halfSize));
        }

        ScreenRect MakeScreenRect(float centerX, float centerY, float width, float height, float depth)
        {
            ScreenRect rect = {};
            rect.centerX = centerX;
            rect.centerY = centerY;
            rect.minX = centerX - (width * 0.5f);
            rect.minY = centerY - (height * 0.5f);
            rect.maxX = centerX + (width * 0.5f);
            rect.maxY = centerY + (height * 0.5f);
            rect.depth = depth;
            rect.valid = true;
            return rect;
        }

        ActorModel MakeActorModel(
            std::uint64_t id,
            std::wstring actorName,
            std::wstring className,
            std::wstring modelName,
            std::wstring assetPath,
            Vector3 position,
            Vector3 size,
            ScreenRect screen,
            std::uint32_t flags)
        {
            ActorModel model = {};
            model.id = id;
            model.actorName = std::move(actorName);
            model.className = std::move(className);
            model.modelName = std::move(modelName);
            model.assetPath = std::move(assetPath);
            model.position = position;
            model.rotationEuler = {};
            model.scale = { 1.0f, 1.0f, 1.0f };
            model.bounds = MakeBoundsFromCenterSize(position, size);
            model.screen = screen;
            model.flags = flags;
            if (screen.valid)
                model.flags |= FrostbiteActorModel_HasScreenProjection;
            return model;
        }

        FrostbiteUniversalFeatureState MakeDefaultFeatureState()
        {
            FrostbiteUniversalFeatureState state = {};
            state.size = sizeof(FrostbiteUniversalFeatureState);
            state.enabledFlags = FrostbiteFeature_Timescale |
                FrostbiteFeature_SkyboxTint |
                FrostbiteFeature_FogTint;
            state.timescale = 1.0f;
            state.skyboxColor[0] = 0.20f;
            state.skyboxColor[1] = 0.52f;
            state.skyboxColor[2] = 1.00f;
            state.skyboxColor[3] = 1.00f;
            state.skyboxIntensity = 1.0f;
            state.skyboxRainbowSpeed = 0.35f;
            state.chamsColor[0] = 1.00f;
            state.chamsColor[1] = 0.10f;
            state.chamsColor[2] = 0.20f;
            state.chamsColor[3] = 1.00f;
            state.chamsOpacity = 0.70f;
            state.chamsRainbowSpeed = 0.45f;
            state.fogColor[0] = 0.45f;
            state.fogColor[1] = 0.65f;
            state.fogColor[2] = 1.00f;
            state.fogColor[3] = 1.00f;
            state.fogDensity = 0.015f;
            state.fovDegrees = 75.0f;
            state.viewTargetActorId = 0;
            state.viewTargetPosition[0] = 0.0f;
            state.viewTargetPosition[1] = 0.0f;
            state.viewTargetPosition[2] = 0.0f;
            state.viewAngles[0] = 0.0f;
            state.viewAngles[1] = 0.0f;
            state.viewAngles[2] = 0.0f;
            state.hasViewTarget = 0;
            return state;
        }

        void RecomputeTimingLocked()
        {
            g_previewState.timescale = ClampTimescale(g_previewState.timescale);
            g_previewState.baseFrameDelta = kBaseFrameDelta;
            g_previewState.scaledFrameDelta = kBaseFrameDelta * g_previewState.timescale;
            g_previewState.skyboxEffectiveRainbowSpeed =
                g_previewState.skyboxRainbowSpeed * g_previewState.timescale;
        }

        void CopyFeatureStateToPreviewLocked(const FrostbiteUniversalFeatureState& state)
        {
            g_previewState.timescale = ClampTimescale(state.timescale);
            g_previewState.skyboxTintEnabled = (state.enabledFlags & FrostbiteFeature_SkyboxTint) != 0;
            g_previewState.skyboxRainbowEnabled = (state.enabledFlags & FrostbiteFeature_SkyboxRainbow) != 0;
            g_previewState.skyboxColor = MakeColor(
                state.skyboxColor[0],
                state.skyboxColor[1],
                state.skyboxColor[2],
                state.skyboxColor[3]);
            g_previewState.skyboxIntensity = std::max(0.0f, state.skyboxIntensity);
            g_previewState.skyboxRainbowSpeed = std::max(0.0f, state.skyboxRainbowSpeed);
            g_previewState.modelDebugTintEnabled = (state.enabledFlags & FrostbiteFeature_Chams) != 0;
            g_previewState.modelDebugRainbowEnabled = (state.enabledFlags & FrostbiteFeature_ChamsRainbow) != 0;
            g_previewState.wireframeDebugEnabled = (state.enabledFlags & FrostbiteFeature_WireframeDebug) != 0;
            g_previewState.modelDebugColor = MakeColor(
                state.chamsColor[0],
                state.chamsColor[1],
                state.chamsColor[2],
                state.chamsColor[3]);
            g_previewState.modelDebugOpacity = Clamp01(state.chamsOpacity);
            g_previewState.fogTintEnabled = (state.enabledFlags & FrostbiteFeature_FogTint) != 0;
            g_previewState.fogColor = MakeColor(
                state.fogColor[0],
                state.fogColor[1],
                state.fogColor[2],
                state.fogColor[3]);
            g_previewState.fogDensity = std::max(0.0f, state.fogDensity);
            g_previewState.debugBoxesEnabled = (state.enabledFlags & FrostbiteFeature_DebugBoxes) != 0;
            g_previewState.snaplinesEnabled = (state.enabledFlags & FrostbiteFeature_Snaplines) != 0;
            g_previewState.fovOverrideEnabled = (state.enabledFlags & FrostbiteFeature_FovOverride) != 0;
            g_previewState.fovDegrees = ClampFov(state.fovDegrees);
            g_previewState.viewAnglePreviewEnabled = (state.enabledFlags & FrostbiteFeature_ViewAnglePreview) != 0;
            g_previewState.hasViewTarget = state.hasViewTarget != 0;
            g_previewState.viewTargetActorId = state.viewTargetActorId;
            g_previewState.viewTargetPosition = {
                state.viewTargetPosition[0],
                state.viewTargetPosition[1],
                state.viewTargetPosition[2]
            };
            g_previewState.viewAngles = {
                state.viewAngles[0],
                state.viewAngles[1],
                state.viewAngles[2]
            };
            RecomputeTimingLocked();
        }

        void CopyPreviewToFeatureStateLocked()
        {
            g_lastFeatureState.size = sizeof(FrostbiteUniversalFeatureState);
            g_lastFeatureState.timescale = g_previewState.timescale;

            if (g_previewState.skyboxTintEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_SkyboxTint;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_SkyboxTint;

            if (g_previewState.skyboxRainbowEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_SkyboxRainbow;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_SkyboxRainbow;

            if (g_previewState.modelDebugTintEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_Chams;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_Chams;

            if (g_previewState.modelDebugRainbowEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_ChamsRainbow;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_ChamsRainbow;

            if (g_previewState.wireframeDebugEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_WireframeDebug;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_WireframeDebug;

            if (g_previewState.fogTintEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_FogTint;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_FogTint;

            if (g_previewState.debugBoxesEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_DebugBoxes;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_DebugBoxes;

            if (g_previewState.snaplinesEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_Snaplines;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_Snaplines;

            if (g_previewState.fovOverrideEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_FovOverride;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_FovOverride;

            if (g_previewState.viewAnglePreviewEnabled)
                g_lastFeatureState.enabledFlags |= FrostbiteFeature_ViewAnglePreview;
            else
                g_lastFeatureState.enabledFlags &= ~FrostbiteFeature_ViewAnglePreview;

            g_lastFeatureState.skyboxColor[0] = g_previewState.skyboxColor.r;
            g_lastFeatureState.skyboxColor[1] = g_previewState.skyboxColor.g;
            g_lastFeatureState.skyboxColor[2] = g_previewState.skyboxColor.b;
            g_lastFeatureState.skyboxColor[3] = g_previewState.skyboxColor.a;
            g_lastFeatureState.skyboxIntensity = g_previewState.skyboxIntensity;
            g_lastFeatureState.skyboxRainbowSpeed = g_previewState.skyboxRainbowSpeed;
            g_lastFeatureState.chamsColor[0] = g_previewState.modelDebugColor.r;
            g_lastFeatureState.chamsColor[1] = g_previewState.modelDebugColor.g;
            g_lastFeatureState.chamsColor[2] = g_previewState.modelDebugColor.b;
            g_lastFeatureState.chamsColor[3] = g_previewState.modelDebugColor.a;
            g_lastFeatureState.chamsOpacity = g_previewState.modelDebugOpacity;
            g_lastFeatureState.chamsRainbowSpeed =
                g_previewState.modelDebugRainbowEnabled ? g_lastFeatureState.chamsRainbowSpeed : 0.0f;
            g_lastFeatureState.fogColor[0] = g_previewState.fogColor.r;
            g_lastFeatureState.fogColor[1] = g_previewState.fogColor.g;
            g_lastFeatureState.fogColor[2] = g_previewState.fogColor.b;
            g_lastFeatureState.fogColor[3] = g_previewState.fogColor.a;
            g_lastFeatureState.fogDensity = g_previewState.fogDensity;
            g_lastFeatureState.fovDegrees = ClampFov(g_previewState.fovDegrees);
            g_lastFeatureState.viewTargetActorId = g_previewState.viewTargetActorId;
            g_lastFeatureState.viewTargetPosition[0] = g_previewState.viewTargetPosition.x;
            g_lastFeatureState.viewTargetPosition[1] = g_previewState.viewTargetPosition.y;
            g_lastFeatureState.viewTargetPosition[2] = g_previewState.viewTargetPosition.z;
            g_lastFeatureState.viewAngles[0] = g_previewState.viewAngles.x;
            g_lastFeatureState.viewAngles[1] = g_previewState.viewAngles.y;
            g_lastFeatureState.viewAngles[2] = g_previewState.viewAngles.z;
            g_lastFeatureState.hasViewTarget = g_previewState.hasViewTarget ? 1u : 0u;
        }

        void FormatWide(wchar_t* buffer, std::size_t count, const wchar_t* format, double value)
        {
            if (!buffer || count == 0)
                return;
            std::swprintf(buffer, count, format, value);
            buffer[count - 1] = L'\0';
        }

        void RefreshActorModelCacheLocked()
        {
#if AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA
            wchar_t timeName[128] = {};
            wchar_t skyName[128] = {};
            wchar_t fogName[128] = {};
            FormatWide(timeName, std::size(timeName), L"Template_TimeSystem_x%.2f", g_previewState.timescale);
            FormatWide(skyName, std::size(skyName), L"Template_Skybox_Intensity_%.2f", g_previewState.skyboxIntensity);
            FormatWide(fogName, std::size(fogName), L"Template_Fog_Density_%.3f", g_previewState.fogDensity);

            g_actorModels.clear();
            g_actorModels.push_back(MakeActorModel(
                1001,
                timeName,
                L"Sdk::TimeSystem",
                L"runtime/timescale",
                L"GeneratedSDK::Runtime::TimeSystem",
                { 0.0f, g_previewState.scaledFrameDelta, 0.0f },
                { 1.0f, g_previewState.timescale, 1.0f },
                MakeScreenRect(120.0f, 210.0f, 64.0f, 82.0f, 0.2f),
                FrostbiteActorModel_Actor | FrostbiteActorModel_Dynamic |
                    FrostbiteActorModel_FromProvider | FrostbiteActorModel_Visible));

            g_actorModels.push_back(MakeActorModel(
                2001,
                skyName,
                L"Sdk::VisualEnvironment",
                L"environment/skybox_tint",
                L"GeneratedSDK::Environment::Skybox",
                {
                    g_previewState.skyboxColor.r,
                    g_previewState.skyboxColor.g,
                    g_previewState.skyboxColor.b
                },
                {
                    8.0f * std::max(0.25f, g_previewState.skyboxIntensity),
                    4.0f,
                    8.0f * std::max(0.25f, g_previewState.skyboxIntensity)
                },
                MakeScreenRect(390.0f, 170.0f, 140.0f, 94.0f, 0.45f),
                FrostbiteActorModel_Model | FrostbiteActorModel_Static |
                    FrostbiteActorModel_FromProvider | FrostbiteActorModel_Visible));

            g_actorModels.push_back(MakeActorModel(
                3001,
                fogName,
                L"Sdk::FogComponent",
                L"environment/fog_tint",
                L"GeneratedSDK::Environment::Fog",
                {
                    g_previewState.fogColor.r,
                    g_previewState.fogColor.g,
                    g_previewState.fogColor.b
                },
                {
                    20.0f,
                    std::max(1.0f, g_previewState.fogDensity * 500.0f),
                    20.0f
                },
                MakeScreenRect(560.0f, 300.0f, 116.0f, 74.0f, 0.65f),
                FrostbiteActorModel_Model | FrostbiteActorModel_Static |
                    FrostbiteActorModel_FromProvider | FrostbiteActorModel_Visible));

            if (g_previewState.hasViewTarget)
            {
                for (ActorModel& model : g_actorModels)
                {
                    if (model.id == g_previewState.viewTargetActorId)
                        model.flags |= FrostbiteActorModel_ViewTarget;
                }
            }
#else
            // TODO:
            // Replace this block with your generated SDK traversal.
            //
            // Example shape:
            //
            // g_actorModels.clear();
            // auto* world = YourSdk::GetGameWorld();
            // if (!world)
            //     return;
            //
            // for (auto* entity : world->Entities())
            // {
            //     if (!entity)
            //         continue;
            //
            //     auto* transform = entity->GetTransformComponent();
            //     auto* render = entity->GetRenderComponent();
            //     auto* model = render ? render->GetModel() : nullptr;
            //     if (!transform || !model)
            //         continue;
            //
            //     ActorModel item = {};
            //     item.id = entity->GetStableId();
            //     item.actorName = entity->GetNameW();
            //     item.className = entity->GetTypeNameW();
            //     item.modelName = model->GetNameW();
            //     item.assetPath = model->GetAssetPathW();
            //     item.position = {
            //         transform->WorldPosition().x,
            //         transform->WorldPosition().y,
            //         transform->WorldPosition().z
            //     };
            //     item.rotationEuler = {
            //         transform->WorldRotationEuler().x,
            //         transform->WorldRotationEuler().y,
            //         transform->WorldRotationEuler().z
            //     };
            //     item.scale = {
            //         transform->WorldScale().x,
            //         transform->WorldScale().y,
            //         transform->WorldScale().z
            //     };
            //     item.bounds = MakeBoundsFromMinMax(
            //         { model->WorldBoundsMin().x, model->WorldBoundsMin().y, model->WorldBoundsMin().z },
            //         { model->WorldBoundsMax().x, model->WorldBoundsMax().y, model->WorldBoundsMax().z });
            //     item.flags = FrostbiteActorModel_Actor | FrostbiteActorModel_Model |
            //         FrostbiteActorModel_Dynamic | FrostbiteActorModel_Visible |
            //         FrostbiteActorModel_FromProvider;
            //     g_actorModels.push_back(std::move(item));
            // }
#endif
        }

        RuntimeFeatureCallbacks GetCallbacksSnapshot()
        {
            std::lock_guard lock(g_stateMutex);
            return g_callbacks;
        }
    }

    void InitializeIfNeeded()
    {
        std::call_once(g_initializeOnce, []()
        {
            std::lock_guard lock(g_stateMutex);
            g_lastFeatureState = MakeDefaultFeatureState();
            CopyFeatureStateToPreviewLocked(g_lastFeatureState);
            g_timescale.store(g_previewState.timescale);
            RefreshActorModelCacheLocked();

#if AEGIS_TEMPLATE_HAS_GENERATED_SDK
            // TODO:
            // Connect your generated SDK here.
            //
            // Examples:
            // - Cache a pointer/reference to your ClientGameContext or GameWorld wrapper.
            // - Resolve the world/entity/component list your SDK exposes.
            // - Resolve your time/tick system wrapper.
            // - Resolve your visual environment, sky, fog, or render-debug wrapper.
            //
            // Keep expensive scans out of per-frame functions. Resolve once here, then
            // refresh only when your owned project changes level/world state.
#endif
        });
    }

    void SetRuntimeFeatureCallbacks(const RuntimeFeatureCallbacks& callbacks)
    {
        InitializeIfNeeded();
        std::lock_guard lock(g_stateMutex);
        g_callbacks = callbacks;
    }

    void ClearRuntimeFeatureCallbacks()
    {
        InitializeIfNeeded();
        std::lock_guard lock(g_stateMutex);
        g_callbacks = {};
    }

    void RefreshActorModelCache()
    {
        InitializeIfNeeded();
        std::lock_guard lock(g_stateMutex);
        RefreshActorModelCacheLocked();
    }

    std::uint32_t GetActorModelCount()
    {
        InitializeIfNeeded();
        std::lock_guard lock(g_stateMutex);

        // TODO:
        // Replace this fallback with your SDK actor/entity/component count.
        return static_cast<std::uint32_t>(g_actorModels.size());
    }

    bool GetActorModel(std::uint32_t index, ActorModel& outModel)
    {
        InitializeIfNeeded();
        std::lock_guard lock(g_stateMutex);

        // TODO:
        // Replace this fallback with your SDK actor/entity/component lookup.
        if (index >= g_actorModels.size())
            return false;

        outModel = g_actorModels[index];
        return true;
    }

    void SetTimescale(float timescale)
    {
        InitializeIfNeeded();
        const float clampedTimescale = ClampTimescale(timescale);
        RuntimeFeatureCallbacks callbacks = {};

        {
            std::lock_guard lock(g_stateMutex);
            g_timescale.store(clampedTimescale);
            g_previewState.timescale = clampedTimescale;
            g_lastFeatureState.enabledFlags |= FrostbiteFeature_Timescale;
            RecomputeTimingLocked();
            CopyPreviewToFeatureStateLocked();
            RefreshActorModelCacheLocked();
            callbacks = g_callbacks;
        }

        if (callbacks.setTimescale)
            callbacks.setTimescale(callbacks.userData, clampedTimescale);

        // TODO:
        // If you do not use callbacks, replace this fallback with your owned SDK call:
        //
        // auto* timeSystem = YourSdk::GetTimeSystem();
        // if (timeSystem)
        //     timeSystem->SetTimeScale(clampedTimescale);
    }

    float GetTimescale()
    {
        InitializeIfNeeded();

        const RuntimeFeatureCallbacks callbacks = GetCallbacksSnapshot();
        float liveTimescale = g_timescale.load();
        if (callbacks.getTimescale && callbacks.getTimescale(callbacks.userData, &liveTimescale))
        {
            liveTimescale = ClampTimescale(liveTimescale);
            std::lock_guard lock(g_stateMutex);
            g_timescale.store(liveTimescale);
            g_previewState.timescale = liveTimescale;
            RecomputeTimingLocked();
            CopyPreviewToFeatureStateLocked();
            RefreshActorModelCacheLocked();
        }

        return liveTimescale;
    }

    void ApplySkyboxTint(float r, float g, float b, float intensity)
    {
        InitializeIfNeeded();
        RuntimeFeatureCallbacks callbacks = {};
        Color4 color = {};
        float clampedIntensity = 0.0f;

        {
            std::lock_guard lock(g_stateMutex);
            g_previewState.skyboxTintEnabled = true;
            g_previewState.skyboxColor = MakeColor(r, g, b);
            g_previewState.skyboxIntensity = std::max(0.0f, intensity);
            RecomputeTimingLocked();
            CopyPreviewToFeatureStateLocked();
            RefreshActorModelCacheLocked();
            callbacks = g_callbacks;
            color = g_previewState.skyboxColor;
            clampedIntensity = g_previewState.skyboxIntensity;
        }

        if (callbacks.setSkyboxTint)
            callbacks.setSkyboxTint(callbacks.userData, &color, clampedIntensity);

        // TODO:
        // If you do not use callbacks, map this to your owned visual environment:
        //
        // auto* sky = YourSdk::GetVisualEnvironment();
        // if (sky)
        //     sky->SetSkyboxTint(color.r, color.g, color.b, clampedIntensity);
    }

    void ApplyModelDebugTint(bool enabled, float r, float g, float b, float opacity)
    {
        InitializeIfNeeded();
        RuntimeFeatureCallbacks callbacks = {};
        Color4 color = {};
        float clampedOpacity = 0.0f;

        {
            std::lock_guard lock(g_stateMutex);
            g_previewState.modelDebugTintEnabled = enabled;
            if (!enabled)
            {
                g_previewState.modelDebugRainbowEnabled = false;
                g_previewState.wireframeDebugEnabled = false;
            }
            g_previewState.modelDebugColor = MakeColor(r, g, b);
            g_previewState.modelDebugOpacity = Clamp01(opacity);
            RecomputeTimingLocked();
            CopyPreviewToFeatureStateLocked();
            RefreshActorModelCacheLocked();
            callbacks = g_callbacks;
            color = g_previewState.modelDebugColor;
            clampedOpacity = g_previewState.modelDebugOpacity;
        }

        if (callbacks.setModelDebugTint)
            callbacks.setModelDebugTint(callbacks.userData, enabled, &color, clampedOpacity);

        // TODO:
        // If you do not use callbacks, map this to your owned editor/debug
        // material visualization system.
    }

    void ApplyFogTint(bool enabled, float r, float g, float b, float density)
    {
        InitializeIfNeeded();
        RuntimeFeatureCallbacks callbacks = {};
        Color4 color = {};
        float clampedDensity = 0.0f;

        {
            std::lock_guard lock(g_stateMutex);
            g_previewState.fogTintEnabled = enabled;
            g_previewState.fogColor = MakeColor(r, g, b);
            g_previewState.fogDensity = std::max(0.0f, density);
            RecomputeTimingLocked();
            CopyPreviewToFeatureStateLocked();
            RefreshActorModelCacheLocked();
            callbacks = g_callbacks;
            color = g_previewState.fogColor;
            clampedDensity = g_previewState.fogDensity;
        }

        if (callbacks.setFogTint)
            callbacks.setFogTint(callbacks.userData, enabled, &color, clampedDensity);

        // TODO:
        // If you do not use callbacks, map this to your owned fog, atmosphere,
        // or environment settings.
    }

    void ApplyFov(float fovDegrees)
    {
        InitializeIfNeeded();
        RuntimeFeatureCallbacks callbacks = {};
        float clampedFov = ClampFov(fovDegrees);

        {
            std::lock_guard lock(g_stateMutex);
            g_previewState.fovOverrideEnabled = true;
            g_previewState.fovDegrees = clampedFov;
            CopyPreviewToFeatureStateLocked();
            callbacks = g_callbacks;
        }

        if (callbacks.setFov)
            callbacks.setFov(callbacks.userData, clampedFov);

        // TODO:
        // If you do not use callbacks, map this to your owned camera/view system:
        //
        // auto* camera = YourSdk::GetActiveCamera();
        // if (camera)
        //     camera->SetVerticalFovDegrees(clampedFov);
    }

    void ApplyViewAngles(const Vector3& angles)
    {
        InitializeIfNeeded();
        RuntimeFeatureCallbacks callbacks = {};
        Vector3 targetPosition = {};
        bool hasTarget = false;

        {
            std::lock_guard lock(g_stateMutex);
            g_previewState.viewAnglePreviewEnabled = true;
            g_previewState.viewAngles = angles;
            targetPosition = g_previewState.viewTargetPosition;
            hasTarget = g_previewState.hasViewTarget;
            CopyPreviewToFeatureStateLocked();
            callbacks = g_callbacks;
        }

        if (callbacks.setViewAngles)
            callbacks.setViewAngles(callbacks.userData, &angles, hasTarget ? &targetPosition : nullptr);
    }

    void ApplyViewAnglePreview(const Vector3& targetPosition, std::uint64_t actorId)
    {
        InitializeIfNeeded();
        RuntimeFeatureCallbacks callbacks = {};
        Vector3 angles = {};

        {
            std::lock_guard lock(g_stateMutex);
            g_previewState.viewAnglePreviewEnabled = true;
            g_previewState.hasViewTarget = true;
            g_previewState.viewTargetActorId = actorId;
            g_previewState.viewTargetPosition = targetPosition;
            g_previewState.viewAngles = ComputeViewAnglesToTarget({}, targetPosition);
            angles = g_previewState.viewAngles;
            CopyPreviewToFeatureStateLocked();
            callbacks = g_callbacks;
        }

        if (callbacks.setViewAngles)
            callbacks.setViewAngles(callbacks.userData, &angles, &targetPosition);

        // TODO:
        // If you do not use callbacks, map this to your owned editor/debug
        // camera system. This template computes a preview look-at angle from
        // a camera at world origin; replace that with your real active camera.
    }

    int ApplyFeatureState(const FrostbiteUniversalFeatureState& state)
    {
        InitializeIfNeeded();
        FrostbiteUniversalFeatureState normalized = state;
        normalized.size = sizeof(FrostbiteUniversalFeatureState);
        normalized.timescale = ClampTimescale(normalized.timescale);

        {
            std::lock_guard lock(g_stateMutex);
            g_lastFeatureState = normalized;
            CopyFeatureStateToPreviewLocked(g_lastFeatureState);
            g_timescale.store(g_previewState.timescale);
            RefreshActorModelCacheLocked();
        }

        if ((normalized.enabledFlags & FrostbiteFeature_Timescale) != 0)
            SetTimescale(normalized.timescale);

        if ((normalized.enabledFlags & (FrostbiteFeature_SkyboxTint | FrostbiteFeature_SkyboxRainbow)) != 0)
        {
            ApplySkyboxTint(
                normalized.skyboxColor[0],
                normalized.skyboxColor[1],
                normalized.skyboxColor[2],
                normalized.skyboxIntensity);
        }

        ApplyModelDebugTint(
            (normalized.enabledFlags & (FrostbiteFeature_Chams | FrostbiteFeature_ChamsRainbow | FrostbiteFeature_WireframeDebug)) != 0,
            normalized.chamsColor[0],
            normalized.chamsColor[1],
            normalized.chamsColor[2],
            normalized.chamsOpacity);

        ApplyFogTint(
            (normalized.enabledFlags & FrostbiteFeature_FogTint) != 0,
            normalized.fogColor[0],
            normalized.fogColor[1],
            normalized.fogColor[2],
            normalized.fogDensity);

        if ((normalized.enabledFlags & FrostbiteFeature_FovOverride) != 0)
            ApplyFov(normalized.fovDegrees);

        if ((normalized.enabledFlags & FrostbiteFeature_ViewAnglePreview) != 0 && normalized.hasViewTarget)
        {
            ApplyViewAnglePreview(
                {
                    normalized.viewTargetPosition[0],
                    normalized.viewTargetPosition[1],
                    normalized.viewTargetPosition[2]
                },
                normalized.viewTargetActorId);
        }

        return 1;
    }

    bool ReadFeatureState(FrostbiteUniversalFeatureState& outState)
    {
        InitializeIfNeeded();
        const float liveTimescale = GetTimescale();

        std::lock_guard lock(g_stateMutex);
        outState = g_lastFeatureState;
        outState.size = sizeof(FrostbiteUniversalFeatureState);
        outState.timescale = liveTimescale;
        return true;
    }

    FeaturePreviewState GetFeaturePreviewState()
    {
        InitializeIfNeeded();
        const float liveTimescale = GetTimescale();

        std::lock_guard lock(g_stateMutex);
        g_previewState.timescale = liveTimescale;
        RecomputeTimingLocked();
        return g_previewState;
    }
}
