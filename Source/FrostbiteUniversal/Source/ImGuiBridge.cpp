#include "FrostbiteUniversal.h"
#include "FrostbiteLog.h"
#include "FrostbiteRuntime.h"
#include "SharedImGuiBridge.h"

#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
#include <d3d11.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <cmath>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    enum class RendererKind
    {
        None,
        Dx11,
        Dx12
    };

    std::mutex g_imguiMutex;
    ImGuiContext* g_context = nullptr;
    HWND g_hwnd = nullptr;
    RendererKind g_renderer = RendererKind::None;
    bool g_platformInitialized = false;
    bool g_visible = true;
    bool g_lastF4Down = false;
    bool g_showDemoWindow = false;
    bool g_autoRefresh = true;
    bool g_showOnlyFrostbiteModules = false;
    float g_autoRefreshSeconds = 2.0f;
    float g_overlayAlpha = 0.92f;
    DWORD g_lastAutoRefreshTick = 0;
    std::uint64_t g_renderCallCount = 0;
    std::uint64_t g_frameCount = 0;
    std::uint64_t g_toggleCount = 0;
    char g_moduleFilter[128] = {};
    char g_sdkSymbolFilter[128] = {};
    HANDLE g_hotkeyThread = nullptr;
    HANDLE g_hotkeyStopEvent = nullptr;

    void* g_dx11Device = nullptr;
    void* g_dx11Context = nullptr;

    void* g_dx12Device = nullptr;
    void* g_dx12Heap = nullptr;
    int g_dx12FramesInFlight = 0;
    int g_dx12RtvFormat = 0;
    std::uintptr_t g_dx12CpuDescriptor = 0;
    std::uint64_t g_dx12GpuDescriptor = 0;
    char g_catalogFilter[128] = {};
    char g_actorModelFilter[128] = {};
    float g_timescaleSlider = 1.0f;
    bool g_timescaleSliderInitialized = false;
    FrostbiteUniversalFeatureState g_featureState = {};
    bool g_featureStateInitialized = false;
    bool g_liveActorFrameRefresh = false;
    int g_liveActorRefreshIntervalMs = 1000;
    bool g_drawLikelyPlayersOnly = false;
    bool g_labelLikelyPlayerScores = true;
    float g_likelyPlayerThreshold = 55.0f;
    DWORD g_lastLiveActorRefreshTick = 0;
    int g_lastLiveActorRefreshCount = 0;
    std::uint64_t g_liveActorRefreshPasses = 0;

    const char* RendererName(RendererKind renderer)
    {
        switch (renderer)
        {
        case RendererKind::Dx11:
            return "DirectX 11";
        case RendererKind::Dx12:
            return "DirectX 12";
        default:
            return "none";
        }
    }

    void InitializeFeatureUiState()
    {
        if (g_featureStateInitialized)
            return;

        if (!FrostbiteUniversal_GetFeatureState(&g_featureState))
        {
            g_featureState = {};
            g_featureState.size = sizeof(FrostbiteUniversalFeatureState);
            g_featureState.enabledFlags = FrostbiteFeature_Timescale;
            g_featureState.timescale = 1.0f;
            g_featureState.skyboxColor[0] = 0.20f;
            g_featureState.skyboxColor[1] = 0.52f;
            g_featureState.skyboxColor[2] = 1.0f;
            g_featureState.skyboxColor[3] = 1.0f;
            g_featureState.skyboxIntensity = 1.0f;
            g_featureState.skyboxRainbowSpeed = 0.35f;
            g_featureState.chamsColor[0] = 1.0f;
            g_featureState.chamsColor[1] = 0.10f;
            g_featureState.chamsColor[2] = 0.20f;
            g_featureState.chamsColor[3] = 1.0f;
            g_featureState.chamsOpacity = 0.70f;
            g_featureState.chamsRainbowSpeed = 0.45f;
            g_featureState.fogColor[0] = 0.45f;
            g_featureState.fogColor[1] = 0.65f;
            g_featureState.fogColor[2] = 1.0f;
            g_featureState.fogColor[3] = 1.0f;
            g_featureState.fogDensity = 0.015f;
            g_featureState.fovDegrees = 75.0f;
            g_featureState.hasViewTarget = 0;
        }

        g_timescaleSlider = g_featureState.timescale;
        g_timescaleSliderInitialized = true;
        g_featureStateInitialized = true;
    }

    const wchar_t* RendererNameWide(RendererKind renderer)
    {
        switch (renderer)
        {
        case RendererKind::Dx11:
            return L"DirectX 11";
        case RendererKind::Dx12:
            return L"DirectX 12";
        default:
            return L"none";
        }
    }

    std::string WideToUtf8(const wchar_t* value)
    {
        if (!value || value[0] == L'\0')
            return "unknown";

        const int size = ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1)
            return "unknown";

        std::string result(static_cast<std::size_t>(size - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
        return result;
    }

    std::string GetLogPathUtf8()
    {
        wchar_t path[MAX_PATH] = {};
        FrostbiteUniversal_GetLogPath(path, static_cast<std::uint32_t>(std::size(path)));
        return WideToUtf8(path);
    }

    std::string ModuleFlagsToText(std::uint32_t flags)
    {
        std::string text;
        auto append = [&text](std::uint32_t mask, const char* name, std::uint32_t value) {
            if ((value & mask) == 0)
                return;

            if (!text.empty())
                text += ", ";
            text += name;
        };

        append(FrostbiteModule_GameExecutable, "game", flags);
        append(FrostbiteModule_EngineBuildInfo, "build-info", flags);
        append(FrostbiteModule_RenderCore2, "render", flags);
        append(FrostbiteModule_DirectStorage, "direct-storage", flags);
        append(FrostbiteModule_Oodle, "oodle", flags);
        append(FrostbiteModule_PlatformSdk, "platform-sdk", flags);
        append(FrostbiteModule_AntiCheat, "anti-cheat", flags);
        append(FrostbiteModule_ThirdPartyRender, "third-party-render", flags);
        append(FrostbiteModule_HasExports, "exports", flags);

        return text.empty() ? "none" : text;
    }

    std::string RuntimeFlagsToText(std::uint32_t flags)
    {
        std::string text;
        auto append = [&text](std::uint32_t mask, const char* name, std::uint32_t value) {
            if ((value & mask) == 0)
                return;

            if (!text.empty())
                text += ", ";
            text += name;
        };

        append(FrostbiteRuntime_IsFrostbiteProcess, "frostbite", flags);
        append(FrostbiteRuntime_HasDataDirectory, "data-dir", flags);
        append(FrostbiteRuntime_HasInitFs, "initfs", flags);
        append(FrostbiteRuntime_HasLayoutToc, "layout.toc", flags);
        append(FrostbiteRuntime_HasTocArchives, "toc", flags);
        append(FrostbiteRuntime_HasCasArchives, "cas", flags);
        append(FrostbiteRuntime_HasEngineBuildInfo, "build-info", flags);
        append(FrostbiteRuntime_HasRenderCore2, "render", flags);
        append(FrostbiteRuntime_HasAntiCheatFiles, "anti-cheat-files", flags);
        append(FrostbiteRuntime_HasSharedImGui, "imgui", flags);

        return text.empty() ? "none" : text;
    }

    std::string CatalogFlagsToText(std::uint32_t flags)
    {
        std::string text;
        auto append = [&text](std::uint32_t mask, const char* name, std::uint32_t value) {
            if ((value & mask) == 0)
                return;

            if (!text.empty())
                text += ", ";
            text += name;
        };

        append(FrostbiteCatalog_ActorSymbol, "actor-symbol", flags);
        append(FrostbiteCatalog_EntitySymbol, "entity-symbol", flags);
        append(FrostbiteCatalog_ModelSymbol, "model-symbol", flags);
        append(FrostbiteCatalog_MeshSymbol, "mesh-symbol", flags);
        append(FrostbiteCatalog_ModelAsset, "model-asset", flags);
        append(FrostbiteCatalog_MeshAsset, "mesh-asset", flags);
        append(FrostbiteCatalog_AnimationAsset, "animation-asset", flags);
        append(FrostbiteCatalog_FromExport, "export", flags);
        append(FrostbiteCatalog_FromFile, "file", flags);
        append(FrostbiteCatalog_LimitReached, "limit", flags);

        return text.empty() ? "none" : text;
    }

    std::string GeneratedSdkSymbolFlagsToText(std::uint32_t flags)
    {
        std::string text;
        auto append = [&text](std::uint32_t mask, const char* name, std::uint32_t value) {
            if ((value & mask) == 0)
                return;

            if (!text.empty())
                text += ", ";
            text += name;
        };

        append(FrostbiteGeneratedSdkSymbol_FunctionCandidate, "function", flags);
        append(FrostbiteGeneratedSdkSymbol_String, "string", flags);
        append(FrostbiteGeneratedSdkSymbol_PlayerLike, "player-like", flags);
        append(FrostbiteGeneratedSdkSymbol_ActorLike, "actor-like", flags);
        append(FrostbiteGeneratedSdkSymbol_EntityLike, "entity-like", flags);
        append(FrostbiteGeneratedSdkSymbol_ModelLike, "model-like", flags);
        append(FrostbiteGeneratedSdkSymbol_MeshLike, "mesh-like", flags);
        append(FrostbiteGeneratedSdkSymbol_ConsoleLike, "console-like", flags);
        append(FrostbiteGeneratedSdkSymbol_CameraLike, "camera-like", flags);
        append(FrostbiteGeneratedSdkSymbol_TransformLike, "transform-like", flags);
        append(FrostbiteGeneratedSdkSymbol_TimeLike, "time-like", flags);
        return text.empty() ? "none" : text;
    }

    std::string ActorModelFlagsToText(std::uint32_t flags)
    {
        std::string text;
        auto append = [&text](std::uint32_t mask, const char* name, std::uint32_t value) {
            if ((value & mask) == 0)
                return;

            if (!text.empty())
                text += ", ";
            text += name;
        };

        append(FrostbiteActorModel_Actor, "actor", flags);
        append(FrostbiteActorModel_Model, "model", flags);
        append(FrostbiteActorModel_Static, "static", flags);
        append(FrostbiteActorModel_Dynamic, "dynamic", flags);
        append(FrostbiteActorModel_Visible, "visible", flags);
        append(FrostbiteActorModel_FromProvider, "provider", flags);
        append(FrostbiteActorModel_FromHostExport, "host-export", flags);
        append(FrostbiteActorModel_FromManualAdd, "manual", flags);
        append(FrostbiteActorModel_HasScreenProjection, "screen", flags);
        append(FrostbiteActorModel_ViewTarget, "view-target", flags);
        append(FrostbiteActorModel_LikelyPlayer, "likely-player", flags);
        append(FrostbiteActorModel_LikelyNonPlayer, "likely-non-player", flags);

        return text.empty() ? "none" : text;
    }

    bool SetViewTargetFromFirstActor(FrostbiteUniversalFeatureState& state)
    {
        const std::uint32_t count = FrostbiteUniversal_GetActorModelCount();
        if (count == 0)
            return false;

        FrostbiteActorModelInfo item = {};
        if (!FrostbiteUniversal_GetActorModelInfo(0, &item))
            return false;

        constexpr float kRadiansToDegrees = 57.2957795f;
        const float dx = item.position[0];
        const float dy = item.position[1];
        const float dz = item.position[2];
        const float horizontal = std::sqrt((dx * dx) + (dz * dz));

        state.enabledFlags |= FrostbiteFeature_ViewAnglePreview;
        state.viewTargetActorId = item.id;
        state.viewTargetPosition[0] = item.position[0];
        state.viewTargetPosition[1] = item.position[1];
        state.viewTargetPosition[2] = item.position[2];
        state.viewAngles[0] = -std::atan2(dy, horizontal) * kRadiansToDegrees;
        state.viewAngles[1] = std::atan2(dx, dz) * kRadiansToDegrees;
        state.viewAngles[2] = 0.0f;
        state.hasViewTarget = 1;
        return true;
    }

    bool WantsLiveActorRefresh(const FrostbiteUniversalFeatureState& state, bool force)
    {
        const std::uint32_t liveVisualFlags =
            FrostbiteFeature_DebugBoxes |
            FrostbiteFeature_Snaplines |
            FrostbiteFeature_ViewAnglePreview;
        return force || ((state.enabledFlags & liveVisualFlags) != 0);
    }

    void RefreshLiveActorsForFrame(const FrostbiteUniversalFeatureState& state, bool force)
    {
        if (!g_liveActorFrameRefresh || !WantsLiveActorRefresh(state, force))
            return;

        const DWORD now = ::GetTickCount();
        const DWORD interval = static_cast<DWORD>(g_liveActorRefreshIntervalMs < 0 ? 0 : g_liveActorRefreshIntervalMs);
        if (interval > 0 &&
            g_lastLiveActorRefreshTick != 0 &&
            now - g_lastLiveActorRefreshTick < interval)
        {
            return;
        }

        g_lastLiveActorRefreshTick = now;
        g_lastLiveActorRefreshCount = FrostbiteUniversal_RefreshActorModelList();
        ++g_liveActorRefreshPasses;
    }

    DWORD LiveActorRefreshAgeMs()
    {
        if (g_lastLiveActorRefreshTick == 0)
            return 0;

        return ::GetTickCount() - g_lastLiveActorRefreshTick;
    }

    void DrawActorModelDebugOverlay(const FrostbiteUniversalFeatureState& state)
    {
        const bool drawBoxes = (state.enabledFlags & FrostbiteFeature_DebugBoxes) != 0;
        const bool drawSnaplines = (state.enabledFlags & FrostbiteFeature_Snaplines) != 0;
        if (!drawBoxes && !drawSnaplines)
            return;

        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        if (!drawList)
            return;

        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 snapOrigin(io.DisplaySize.x * 0.5f, io.DisplaySize.y);
        const ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            state.chamsColor[0],
            state.chamsColor[1],
            state.chamsColor[2],
            state.chamsOpacity > 0.05f ? state.chamsOpacity : 0.85f));
        const ImU32 snapColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            state.skyboxColor[0],
            state.skyboxColor[1],
            state.skyboxColor[2],
            0.70f));
        const ImU32 targetColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.92f, 0.20f, 1.0f));

        const std::uint32_t count = FrostbiteUniversal_GetActorModelCount();
        for (std::uint32_t index = 0; index < count; ++index)
        {
            FrostbiteActorModelInfo item = {};
            if (!FrostbiteUniversal_GetActorModelInfo(index, &item))
                continue;
            if (g_drawLikelyPlayersOnly && item.likelyPlayerScore < g_likelyPlayerThreshold)
                continue;
            if ((item.flags & FrostbiteActorModel_HasScreenProjection) == 0)
                continue;

            const ImVec2 minPoint(item.screenBoundsMin[0], item.screenBoundsMin[1]);
            const ImVec2 maxPoint(item.screenBoundsMax[0], item.screenBoundsMax[1]);
            const ImVec2 center(item.screenPosition[0], item.screenPosition[1]);

            if (drawBoxes)
                drawList->AddRect(minPoint, maxPoint, boxColor, 0.0f, 0, 2.0f);
            if (drawSnaplines)
                drawList->AddLine(snapOrigin, center, snapColor, 1.5f);
            if (g_labelLikelyPlayerScores && item.likelyPlayerScore >= g_likelyPlayerThreshold)
            {
                char scoreLabel[48] = {};
                sprintf_s(scoreLabel, "%.0f", item.likelyPlayerScore);
                drawList->AddText(ImVec2(maxPoint.x + 4.0f, minPoint.y), targetColor, scoreLabel);
            }
            if (state.hasViewTarget && item.id == state.viewTargetActorId)
            {
                drawList->AddCircle(center, 8.0f, targetColor, 24, 2.0f);
                drawList->AddLine(ImVec2(center.x - 12.0f, center.y), ImVec2(center.x + 12.0f, center.y), targetColor, 1.5f);
                drawList->AddLine(ImVec2(center.x, center.y - 12.0f), ImVec2(center.x, center.y + 12.0f), targetColor, 1.5f);
            }
        }
    }

    bool TextMatchesFilter(const std::string& value, const char* filter)
    {
        if (!filter || filter[0] == '\0')
            return true;

        std::string lowerValue = value;
        std::string lowerFilter = filter;
        for (char& ch : lowerValue)
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        for (char& ch : lowerFilter)
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));

        return lowerValue.find(lowerFilter) != std::string::npos;
    }

    void ApplyFrostbiteStyle()
    {
        ImGui::StyleColorsDark();

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 10.0f;
        style.ChildRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 8.0f;
        style.ScrollbarRounding = 10.0f;
        style.GrabRounding = 6.0f;
        style.TabRounding = 6.0f;
        style.WindowPadding = ImVec2(14.0f, 12.0f);
        style.FramePadding = ImVec2(10.0f, 6.0f);
        style.ItemSpacing = ImVec2(9.0f, 7.0f);
        style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);
        style.ScrollbarSize = 12.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.040f, 0.050f, 0.96f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.060f, 0.074f, 0.92f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.050f, 0.064f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.42f, 0.08f, 0.12f, 0.55f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.090f, 0.100f, 0.120f, 0.95f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.07f, 0.10f, 0.95f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.48f, 0.08f, 0.13f, 0.95f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.040f, 0.045f, 0.055f, 1.0f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.045f, 0.055f, 1.0f);
        colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.20f, 0.28f, 1.0f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.88f, 0.16f, 0.22f, 1.0f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.28f, 0.35f, 1.0f);
        colors[ImGuiCol_Button] = ImVec4(0.16f, 0.055f, 0.075f, 0.95f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.08f, 0.12f, 0.95f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.66f, 0.10f, 0.16f, 1.0f);
        colors[ImGuiCol_Header] = ImVec4(0.22f, 0.065f, 0.09f, 0.85f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.42f, 0.08f, 0.13f, 0.90f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.62f, 0.10f, 0.16f, 0.95f);
        colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.09f, 0.11f, 0.95f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.38f, 0.08f, 0.12f, 0.95f);
        colors[ImGuiCol_TabActive] = ImVec4(0.20f, 0.06f, 0.08f, 1.0f);
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.08f, 0.10f, 1.0f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.045f, 0.050f, 0.060f, 0.88f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.065f, 0.070f, 0.084f, 0.88f);
    }

    void DrawMetricCard(const char* label, const char* value, const ImVec4& accent)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.070f, 0.075f, 0.090f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.55f));
        ImGui::BeginChild(label, ImVec2(0.0f, 58.0f), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(accent, "%s", label);
        ImGui::TextWrapped("%s", value ? value : "unknown");
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    void DrawStatusPill(const char* label, bool healthy)
    {
        const ImVec4 color = healthy
            ? ImVec4(0.26f, 0.95f, 0.52f, 1.0f)
            : ImVec4(1.0f, 0.23f, 0.30f, 1.0f);
        ImGui::TextColored(color, "%s: %s", label, healthy ? "ready" : "offline");
    }

    void DrawGeneratedSdkRuntimeStatus()
    {
        FrostbiteGeneratedSdkInfo sdkInfo = {};
        const bool loaded = FrostbiteUniversal_GetGeneratedSdkInfo(&sdkInfo) != 0;

        ImGui::Text("Generated SDK cache: %s", loaded ? "loaded" : "not loaded");
        if (loaded)
        {
            ImGui::Text("Reload generation: %u", sdkInfo.reloadGeneration);
            ImGui::Text("Generated files: %u  Bytes: %llu",
                sdkInfo.generatedFileCount,
                static_cast<unsigned long long>(sdkInfo.generatedBytes));
            ImGui::Text("Manifest: %u games / %u files", sdkInfo.manifestGameCount, sdkInfo.manifestFileCount);
            ImGui::Text("Runtime candidates: %u functions", sdkInfo.runtimeFunctionCandidateCount);
            ImGui::Text("Process snapshot: %u modules / %u symbol groups", sdkInfo.processModuleCount, sdkInfo.processSymbolCount);
            ImGui::Text("Strings: %u values / %u xrefs", sdkInfo.stringCount, sdkInfo.stringXrefCount);
            ImGui::Text("Classified SDK symbols: %u", sdkInfo.generatedSymbolCount);
            ImGui::TextWrapped("Output: %s", WideToUtf8(sdkInfo.outputDir).c_str());
        }
        else
        {
            ImGui::TextDisabled("No generated SDK folder has been loaded into the runtime cache yet.");
        }

        if (ImGui::Button("Dump Current Process SDK"))
            FrostbiteUniversal_StartSdkDump(nullptr);

        ImGui::SameLine();
        if (ImGui::Button("Reload Generated SDK Cache"))
            FrostbiteUniversal_ReloadGeneratedSdk(nullptr);
    }

    void DrawGeneratedSdkSymbolPanel()
    {
        FrostbiteGeneratedSdkInfo sdkInfo = {};
        const bool loaded = FrostbiteUniversal_GetGeneratedSdkInfo(&sdkInfo) != 0;
        const std::uint32_t symbolCount = FrostbiteUniversal_GetGeneratedSdkSymbolCount();

        ImGui::Text("Generated SDK symbols: %u", symbolCount);
        ImGui::SameLine();
        if (ImGui::Button("Dump Current Process SDK"))
            FrostbiteUniversal_StartSdkDump(nullptr);

        ImGui::SameLine();
        if (ImGui::Button("Reload SDK Cache"))
            FrostbiteUniversal_ReloadGeneratedSdk(nullptr);

        if (loaded)
        {
            ImGui::Text("Functions: %u  Strings: %u  Xrefs: %u  Classified: %u",
                sdkInfo.runtimeFunctionCandidateCount,
                sdkInfo.stringCount,
                sdkInfo.stringXrefCount,
                sdkInfo.generatedSymbolCount);
            ImGui::TextWrapped("Source: %s", WideToUtf8(sdkInfo.outputDir).c_str());
        }
        else
        {
            ImGui::TextWrapped("No current-process generated SDK cache is loaded yet. Run a dump from this process, then reload.");
        }

        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputText("SDK symbol filter", g_sdkSymbolFilter, static_cast<int>(std::size(g_sdkSymbolFilter)));
        ImGui::SameLine();
        ImGui::TextDisabled("Try: player, soldier, entity, transform, camera, console, fov");

        if (ImGui::BeginTable("GeneratedSdkSymbols", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 280.0f)))
        {
            ImGui::TableSetupColumn("Source");
            ImGui::TableSetupColumn("Score");
            ImGui::TableSetupColumn("Flags");
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Module");
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();

            for (std::uint32_t index = 0; index < symbolCount; ++index)
            {
                FrostbiteGeneratedSdkSymbolInfo symbol = {};
                if (!FrostbiteUniversal_GetGeneratedSdkSymbolInfo(index, &symbol))
                    continue;

                const std::string source = WideToUtf8(symbol.source);
                const std::string category = WideToUtf8(symbol.category);
                const std::string moduleName = WideToUtf8(symbol.moduleName);
                const std::string name = WideToUtf8(symbol.name);
                const std::string address = WideToUtf8(symbol.addressHex);
                const std::string detail = WideToUtf8(symbol.detail);
                const std::string flags = GeneratedSdkSymbolFlagsToText(symbol.flags);

                if (!TextMatchesFilter(source, g_sdkSymbolFilter) &&
                    !TextMatchesFilter(category, g_sdkSymbolFilter) &&
                    !TextMatchesFilter(moduleName, g_sdkSymbolFilter) &&
                    !TextMatchesFilter(name, g_sdkSymbolFilter) &&
                    !TextMatchesFilter(address, g_sdkSymbolFilter) &&
                    !TextMatchesFilter(detail, g_sdkSymbolFilter) &&
                    !TextMatchesFilter(flags, g_sdkSymbolFilter))
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(source.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", symbol.score);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", flags.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextWrapped("%s", category.c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextWrapped("%s", name.c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(address.c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::TextWrapped("%s", moduleName.c_str());
                ImGui::TableSetColumnIndex(7);
                ImGui::TextWrapped("%s", detail.c_str());
            }

            ImGui::EndTable();
        }
    }

    void ToggleVisibleUnlocked(const wchar_t* source)
    {
        g_visible = !g_visible;
        ++g_toggleCount;

        std::wstringstream message;
        message << L"ImGui visibility toggled by " << source << L": " << (g_visible ? L"visible" : L"hidden");
        FrostbiteUniversal::Log::Write(message.str());
    }

    void LogRenderStateUnlocked(const wchar_t* source)
    {
        std::wstringstream message;
        message << source
                << L": visible=" << (g_visible ? L"true" : L"false")
                << L", renderer=" << RendererNameWide(g_renderer)
                << L", renderCalls=" << g_renderCallCount
                << L", renderedFrames=" << g_frameCount;
        FrostbiteUniversal::Log::Write(message.str());

        if (g_renderCallCount == 0)
        {
            FrostbiteUniversal::Log::Write(
                L"ImGui is not rendering yet: no DX11/DX12 render export calls have reached FrostbiteUniversal.");
        }
        else if (g_renderer == RendererKind::None)
        {
            FrostbiteUniversal::Log::Write(
                L"ImGui render export was called, but no renderer backend initialized. Check null device/context/descriptor logs above.");
        }
    }

    void PollF4ToggleUnlocked()
    {
        const bool f4Down = (::GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        if (f4Down && !g_lastF4Down)
            ToggleVisibleUnlocked(L"F4");

        g_lastF4Down = f4Down;
    }

    DWORD WINAPI HotkeyThreadProc(void*)
    {
        FrostbiteUniversal::Log::Write(L"ImGui F4 hotkey monitor thread entered");

        bool lastF4Down = false;
        while (::WaitForSingleObject(g_hotkeyStopEvent, 25) == WAIT_TIMEOUT)
        {
            const bool f4Down = (::GetAsyncKeyState(VK_F4) & 0x8000) != 0;
            if (f4Down && !lastF4Down)
            {
                std::lock_guard lock(g_imguiMutex);
                ToggleVisibleUnlocked(L"background F4 monitor");
                LogRenderStateUnlocked(L"F4 monitor diagnostic");
            }

            lastF4Down = f4Down;
        }

        FrostbiteUniversal::Log::Write(L"ImGui F4 hotkey monitor thread leaving");
        return 0;
    }

    void AutoRefreshRuntimeUnlocked()
    {
        if (!g_autoRefresh)
            return;

        const DWORD now = ::GetTickCount();
        const DWORD interval = static_cast<DWORD>((g_autoRefreshSeconds < 0.25f ? 0.25f : g_autoRefreshSeconds) * 1000.0f);
        if (g_lastAutoRefreshTick != 0 && now - g_lastAutoRefreshTick < interval)
            return;

        g_lastAutoRefreshTick = now;
        FrostbiteUniversal_Refresh();
    }

    void SetContext()
    {
        if (g_context)
            ImGui::SetCurrentContext(g_context);
    }

    void ShutdownRendererBackendUnlocked()
    {
        SetContext();

        if (g_renderer == RendererKind::Dx11)
            ImGui_ImplDX11_Shutdown();
        else if (g_renderer == RendererKind::Dx12)
            ImGui_ImplDX12_Shutdown();

        g_renderer = RendererKind::None;
        g_dx11Device = nullptr;
        g_dx11Context = nullptr;
        g_dx12Device = nullptr;
        g_dx12Heap = nullptr;
        g_dx12FramesInFlight = 0;
        g_dx12RtvFormat = 0;
        g_dx12CpuDescriptor = 0;
        g_dx12GpuDescriptor = 0;
    }

    void ShutdownAllUnlocked()
    {
        ShutdownRendererBackendUnlocked();

        SetContext();
        if (g_platformInitialized)
        {
            ImGui_ImplWin32_Shutdown();
            g_platformInitialized = false;
        }

        if (g_context)
        {
            ImGui::DestroyContext(g_context);
            g_context = nullptr;
        }

        g_hwnd = nullptr;
        FrostbiteUniversal::Log::Write(L"ImGui bridge shutdown");
    }

    bool EnsureContextUnlocked()
    {
        if (g_context)
        {
            SetContext();
            return true;
        }

        g_context = ImGui::CreateContext();
        if (!g_context)
        {
            FrostbiteUniversal::Log::Write(L"ImGui context creation failed");
            return false;
        }

        SetContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ApplyFrostbiteStyle();
        FrostbiteUniversal::Log::Write(L"ImGui context created");
        return true;
    }

    bool EnsurePlatformUnlocked(HWND hwnd)
    {
        if (!hwnd)
        {
            FrostbiteUniversal::Log::Write(L"ImGui platform init failed: hwnd is null");
            return false;
        }

        if (g_platformInitialized && hwnd == g_hwnd)
            return true;

        if (g_platformInitialized)
            ShutdownAllUnlocked();

        if (!EnsureContextUnlocked())
            return false;

        if (!ImGui_ImplWin32_Init(hwnd))
        {
            FrostbiteUniversal::Log::Write(L"ImGui Win32 backend init failed");
            return false;
        }

        g_hwnd = hwnd;
        g_platformInitialized = true;
        FrostbiteUniversal::Log::Write(L"ImGui Win32 backend initialized");
        return true;
    }

    bool EnsureDx11Unlocked(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (!device || !context)
        {
            FrostbiteUniversal::Log::Write(L"ImGui DX11 init failed: device or context is null");
            return false;
        }

        if (!EnsurePlatformUnlocked(hwnd))
            return false;

        if (g_renderer == RendererKind::Dx11 && g_dx11Device == device && g_dx11Context == context)
            return true;

        ShutdownRendererBackendUnlocked();
        SetContext();

        if (!ImGui_ImplDX11_Init(device, context))
        {
            FrostbiteUniversal::Log::Write(L"ImGui DX11 backend init failed");
            return false;
        }

        g_renderer = RendererKind::Dx11;
        g_dx11Device = device;
        g_dx11Context = context;
        FrostbiteUniversal::Log::Write(L"ImGui DX11 backend initialized");
        return true;
    }

    bool EnsureDx12Unlocked(
        HWND hwnd,
        ID3D12Device* device,
        int framesInFlight,
        DXGI_FORMAT rtvFormat,
        ID3D12DescriptorHeap* srvDescriptorHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor,
        D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor)
    {
        if (!device || !srvDescriptorHeap || cpuDescriptor.ptr == 0 || gpuDescriptor.ptr == 0)
        {
            FrostbiteUniversal::Log::Write(L"ImGui DX12 init failed: device, heap, or descriptor handle is null");
            return false;
        }

        if (!EnsurePlatformUnlocked(hwnd))
            return false;

        framesInFlight = framesInFlight > 0 ? framesInFlight : 3;

        if (g_renderer == RendererKind::Dx12 &&
            g_dx12Device == device &&
            g_dx12Heap == srvDescriptorHeap &&
            g_dx12FramesInFlight == framesInFlight &&
            g_dx12RtvFormat == static_cast<int>(rtvFormat) &&
            g_dx12CpuDescriptor == cpuDescriptor.ptr &&
            g_dx12GpuDescriptor == gpuDescriptor.ptr)
        {
            return true;
        }

        ShutdownRendererBackendUnlocked();
        SetContext();

        if (!ImGui_ImplDX12_Init(device, framesInFlight, rtvFormat, srvDescriptorHeap, cpuDescriptor, gpuDescriptor))
        {
            FrostbiteUniversal::Log::Write(L"ImGui DX12 backend init failed");
            return false;
        }

        g_renderer = RendererKind::Dx12;
        g_dx12Device = device;
        g_dx12Heap = srvDescriptorHeap;
        g_dx12FramesInFlight = framesInFlight;
        g_dx12RtvFormat = static_cast<int>(rtvFormat);
        g_dx12CpuDescriptor = cpuDescriptor.ptr;
        g_dx12GpuDescriptor = gpuDescriptor.ptr;
        FrostbiteUniversal::Log::Write(L"ImGui DX12 backend initialized");
        return true;
    }

    void DrawOverlayUnlocked()
    {
        if (!g_visible)
            return;

        InitializeFeatureUiState();
        RefreshLiveActorsForFrame(g_featureState, false);
        DrawActorModelDebugOverlay(g_featureState);

        FrostbiteRuntimeInfo info = {};
        FrostbiteUniversal_GetRuntimeInfo(&info);
        const std::string processName = WideToUtf8(info.processName);
        const std::string processPath = WideToUtf8(info.processPath);
        const std::string gameRoot = WideToUtf8(info.gameRoot);
        const std::string detectedTitle = WideToUtf8(info.detectedTitle);
        const std::string runtimeFlags = RuntimeFlagsToText(info.flags);
        const std::string logPath = GetLogPathUtf8();

        ImGui::SetNextWindowSize(ImVec2(690.0f, 460.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(g_overlayAlpha);
        if (!ImGui::Begin("Frostbite Universal", &g_visible, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted("Frostbite Universal");
        ImGui::SameLine();
        ImGui::TextDisabled("F4 toggles this panel");
        ImGui::Separator();
        DrawStatusPill("Runtime", FrostbiteUniversal_IsFrostbiteProcess() != 0);
        ImGui::SameLine();
        DrawStatusPill("Actor bridge", FrostbiteUniversal_HasActorModelBridge() != 0);
        ImGui::SameLine();
        DrawStatusPill("Feature bridge", FrostbiteUniversal_HasFeatureBridge() != 0);

        if (ImGui::BeginTabBar("FrostbiteUniversalTabs"))
        {
            if (ImGui::BeginTabItem("Runtime"))
            {
                char rendererValue[96] = {};
                sprintf_s(rendererValue, "%s / %llu calls", RendererName(g_renderer), static_cast<unsigned long long>(g_renderCallCount));
                char moduleValue[96] = {};
                sprintf_s(moduleValue, "%u total / %u Frostbite", info.moduleCount, info.frostbiteModuleCount);
                char archiveValue[96] = {};
                sprintf_s(archiveValue, "%u TOC / %u CAS", info.tocFileCount, info.casFileCount);
                char catalogValue[96] = {};
                sprintf_s(catalogValue, "%u entries", FrostbiteUniversal_GetCatalogCount());

                if (ImGui::BeginTable("OverviewCards", 4, ImGuiTableFlags_SizingStretchSame))
                {
                    ImGui::TableNextColumn();
                    DrawMetricCard("Renderer", rendererValue, ImVec4(1.0f, 0.20f, 0.28f, 1.0f));
                    ImGui::TableNextColumn();
                    DrawMetricCard("Modules", moduleValue, ImVec4(0.95f, 0.58f, 0.18f, 1.0f));
                    ImGui::TableNextColumn();
                    DrawMetricCard("Archives", archiveValue, ImVec4(0.42f, 0.70f, 1.0f, 1.0f));
                    ImGui::TableNextColumn();
                    DrawMetricCard("Catalog", catalogValue, ImVec4(0.42f, 0.95f, 0.58f, 1.0f));
                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::Text("Renderer: %s", RendererName(g_renderer));
                ImGui::Text("Render calls: %llu", static_cast<unsigned long long>(g_renderCallCount));
                ImGui::Text("Rendered frames: %llu", static_cast<unsigned long long>(g_frameCount));
                ImGui::Text("F4 toggles: %llu", static_cast<unsigned long long>(g_toggleCount));
                ImGui::Separator();
                ImGui::Text("Process: %s", processName.c_str());
                ImGui::Text("Detected title: %s", detectedTitle.c_str());
                ImGui::Text("Detected Frostbite: %s", FrostbiteUniversal_IsFrostbiteProcess() ? "yes" : "no");
                ImGui::Text("Modules: %u", info.moduleCount);
                ImGui::Text("Frostbite modules: %u", info.frostbiteModuleCount);
                ImGui::Text("TOC files: %u", info.tocFileCount);
                ImGui::Text("CAS files: %u", info.casFileCount);
                ImGui::Text("Actor/model catalog: %u", FrostbiteUniversal_GetCatalogCount());
                ImGui::Text("Live actor/model bridge: %s", FrostbiteUniversal_HasActorModelBridge() ? "connected" : "not connected");
                ImGui::Text("Timescale bridge: %s", FrostbiteUniversal_HasTimescaleBridge() ? "connected" : "not connected");
                ImGui::Text("Requested timescale: %.2f", FrostbiteUniversal_GetTimescale());
                ImGui::Text("Flags: 0x%08X", info.flags);
                ImGui::TextWrapped("Flag names: %s", runtimeFlags.c_str());
                DrawGeneratedSdkRuntimeStatus();

                if (ImGui::Button("Refresh Runtime"))
                    FrostbiteUniversal_Refresh();

                ImGui::SameLine();
                if (ImGui::Button("Write Report"))
                    FrostbiteUniversal_WriteRuntimeReport(nullptr);

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Adapter"))
            {
                FrostbiteCapabilityInfo capability = {};
                FrostbiteAdapterTiming adapterTiming = {};
                FrostbiteUniversal_GetCapabilityInfo(&capability);
                FrostbiteUniversal_GetAdapterTiming(&adapterTiming);

                ImGui::Text("Renderer backend: %s", WideToUtf8(capability.rendererBackend).c_str());
                ImGui::Text("Provider timing: entities %.3f ms | matrix %.3f ms | viewport %.3f ms",
                    adapterTiming.entityProviderMs,
                    adapterTiming.matrixProviderMs,
                    adapterTiming.viewportProviderMs);
                ImGui::Text("Entities: %u | projected %u | clipped %u | frame %llu",
                    adapterTiming.entityCount,
                    adapterTiming.projectedCount,
                    adapterTiming.clippedCount,
                    static_cast<unsigned long long>(adapterTiming.frameId));
                ImGui::TextWrapped("Details: %s", WideToUtf8(capability.details).c_str());

                if (ImGui::Button("Update Providers"))
                    FrostbiteUniversal_UpdateProviders();

                ImGui::SameLine();
                if (ImGui::Button("Print Entities"))
                    FrostbiteUniversal_PrintCurrentEntities();

                ImGui::SameLine();
                if (ImGui::Button("Write Snapshot"))
                {
                    wchar_t tempPath[MAX_PATH] = {};
                    if (::GetTempPathW(MAX_PATH, tempPath) != 0)
                    {
                        std::wstring path = tempPath;
                        if (!path.empty() && path.back() != L'\\')
                            path.push_back(L'\\');
                        path += L"FrostbiteUniversal_Snapshot.json";
                        FrostbiteUniversal_WriteSnapshotJson(path.c_str());
                    }
                }

                auto drawCapability = [](const char* name, int value, const char* detail) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(name);
                    ImGui::TableSetColumnIndex(1);
                    if (value)
                        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.55f, 1.0f), "Pass");
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Warn");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", detail);
                };

                if (ImGui::BeginTable("FrostbiteAdapterCapabilityTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 260.0f)))
                {
                    ImGui::TableSetupColumn("Capability");
                    ImGui::TableSetupColumn("State");
                    ImGui::TableSetupColumn("Details");
                    ImGui::TableHeadersRow();
                    drawCapability("Frostbite detected", capability.frostbiteDetected, "Runtime module/filesystem heuristics identify Frostbite.");
                    drawCapability("Data directory", capability.dataDirectoryFound, "Data folder present near the process.");
                    drawCapability("InitFS", capability.initFsFound, "initfs evidence found.");
                    drawCapability("TOC/CAS archives", capability.tocArchivesFound || capability.casArchivesFound, "Frostbite archive evidence found.");
                    drawCapability("Render core", capability.renderCoreFound, "Frostbite render module or D3D backend evidence found.");
                    drawCapability("Exports", capability.exportsFound, "Named PE exports available for resolver/reporting.");
                    drawCapability("ImGui", capability.imguiAvailable, "Shared ImGui compiled into the DLL.");
                    drawCapability("Overlay running", capability.overlayRunning, "Self-hosted overlay or in-process ImGui path is active.");
                    drawCapability("Entity provider", capability.entityProviderRegistered, "Provider or host bridge has supplied actor/model access.");
                    drawCapability("Matrix provider", capability.viewProjectionProviderRegistered, "View-projection provider registered.");
                    drawCapability("Viewport provider", capability.viewportProviderRegistered, "Viewport provider registered.");
                    drawCapability("Viewport valid", capability.viewportValid, "Viewport dimensions are finite and positive.");
                    drawCapability("Matrix valid", capability.matrixValid, "View-projection matrix is finite and nonzero.");
                    drawCapability("W2S projection", capability.w2sProjectionWorking, "Synthetic world point can be projected.");
                    drawCapability("Snapshot ready", capability.snapshotReady, "Entity/actor/model snapshot cache has entries.");
                    drawCapability("SDK cache", capability.generatedSdkLoaded, "Generated SDK metadata reloaded into runtime memory.");
                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Modules"))
            {
                ImGui::InputText("Filter", g_moduleFilter, static_cast<int>(std::size(g_moduleFilter)));
                ImGui::Checkbox("Only Frostbite-ish modules", &g_showOnlyFrostbiteModules);

                if (ImGui::BeginTable("ModuleTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 285.0f)))
                {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Base");
                    ImGui::TableSetupColumn("Size");
                    ImGui::TableSetupColumn("Flags");
                    ImGui::TableSetupColumn("Path");
                    ImGui::TableHeadersRow();

                    for (std::uint32_t index = 0; index < info.moduleCount; ++index)
                    {
                        FrostbiteModuleInfo module = {};
                        if (!FrostbiteUniversal_GetModuleInfo(index, &module))
                            continue;

                        const std::string name = WideToUtf8(module.name);
                        const std::string path = WideToUtf8(module.path);
                        const std::string flags = ModuleFlagsToText(module.flags);
                        const bool frostbiteish = (module.flags & (FrostbiteModule_GameExecutable | FrostbiteModule_EngineBuildInfo | FrostbiteModule_RenderCore2 | FrostbiteModule_DirectStorage | FrostbiteModule_Oodle)) != 0;

                        if (g_showOnlyFrostbiteModules && !frostbiteish)
                            continue;

                        if (!TextMatchesFilter(name, g_moduleFilter) && !TextMatchesFilter(path, g_moduleFilter) && !TextMatchesFilter(flags, g_moduleFilter))
                            continue;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("0x%p", reinterpret_cast<void*>(module.baseAddress));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%u", module.imageSize);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextWrapped("%s", flags.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextWrapped("%s", path.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Overlay"))
            {
                InitializeFeatureUiState();

                wchar_t hookStatusWide[256] = {};
                FrostbiteUniversal_GetOwnedProjectHookStatus(hookStatusWide, static_cast<std::uint32_t>(std::size(hookStatusWide)));
                const std::string hookStatus = WideToUtf8(hookStatusWide);

                auto flagCheckbox = [](const char* label, std::uint32_t flag) {
                    bool enabled = (g_featureState.enabledFlags & flag) != 0;
                    if (ImGui::Checkbox(label, &enabled))
                    {
                        if (enabled)
                            g_featureState.enabledFlags |= flag;
                        else
                            g_featureState.enabledFlags &= ~flag;
                    }
                };

                ImGui::Text("Actor/model bridge: %s", FrostbiteUniversal_HasActorModelBridge() ? "connected" : "not connected");
                ImGui::Text("Timescale bridge: %s", FrostbiteUniversal_HasTimescaleBridge() ? "connected" : "not connected");
                ImGui::Text("Feature bridge: %s", FrostbiteUniversal_HasFeatureBridge() ? "connected" : "local state only");
                ImGui::TextWrapped("Hook status: %s", hookStatus.c_str());

                if (ImGui::Button("Install MinHook Hooks"))
                    FrostbiteUniversal_InstallOwnedProjectHooks();

                ImGui::SameLine();
                if (ImGui::Button("Enable All Visuals"))
                {
                    g_featureState.enabledFlags =
                        FrostbiteFeature_Timescale |
                        FrostbiteFeature_SkyboxTint |
                        FrostbiteFeature_SkyboxRainbow |
                        FrostbiteFeature_Chams |
                        FrostbiteFeature_ChamsRainbow |
                        FrostbiteFeature_FogTint |
                        FrostbiteFeature_WireframeDebug |
                        FrostbiteFeature_DebugBoxes |
                        FrostbiteFeature_Snaplines |
                        FrostbiteFeature_FovOverride;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    FrostbiteUniversal_ApplyFeatureState();
                }

                ImGui::SameLine();
                if (ImGui::Button("Apply Features"))
                {
                    g_featureState.timescale = g_timescaleSlider;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    FrostbiteUniversal_ApplyFeatureState();
                }

                ImGui::Separator();

                flagCheckbox("Timescale override", FrostbiteFeature_Timescale);
                ImGui::SliderFloat("Timescale", &g_timescaleSlider, 0.01f, 5.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::Button("Apply"))
                {
                    g_featureState.timescale = g_timescaleSlider;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    FrostbiteUniversal_SetTimescale(g_timescaleSlider);
                }

                ImGui::SameLine();
                if (ImGui::Button("Reset 1.0x"))
                {
                    g_timescaleSlider = 1.0f;
                    g_featureState.timescale = g_timescaleSlider;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    FrostbiteUniversal_SetTimescale(g_timescaleSlider);
                }

                ImGui::SameLine();
                if (ImGui::Button("Sync"))
                {
                    if (FrostbiteUniversal_SyncTimescaleFromHost())
                    {
                        g_timescaleSlider = FrostbiteUniversal_GetTimescale();
                        g_featureState.timescale = g_timescaleSlider;
                    }
                }

                ImGui::Separator();
                flagCheckbox("Skybox tint", FrostbiteFeature_SkyboxTint);
                ImGui::SameLine();
                flagCheckbox("Rainbow skybox", FrostbiteFeature_SkyboxRainbow);
                ImGui::ColorEdit4("Skybox color", g_featureState.skyboxColor);
                ImGui::SliderFloat("Skybox intensity", &g_featureState.skyboxIntensity, 0.0f, 10.0f, "%.2f");
                ImGui::SliderFloat("Skybox rainbow speed", &g_featureState.skyboxRainbowSpeed, 0.0f, 10.0f, "%.2f");

                flagCheckbox("Debug material tint", FrostbiteFeature_Chams);
                ImGui::SameLine();
                flagCheckbox("Rainbow material tint", FrostbiteFeature_ChamsRainbow);
                ImGui::SameLine();
                flagCheckbox("Wireframe/debug material", FrostbiteFeature_WireframeDebug);
                ImGui::ColorEdit4("Material tint color", g_featureState.chamsColor);
                ImGui::SliderFloat("Material tint opacity", &g_featureState.chamsOpacity, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("Material tint rainbow speed", &g_featureState.chamsRainbowSpeed, 0.0f, 10.0f, "%.2f");

                flagCheckbox("Fog tint", FrostbiteFeature_FogTint);
                ImGui::ColorEdit4("Fog color", g_featureState.fogColor);
                ImGui::SliderFloat("Fog density", &g_featureState.fogDensity, 0.0f, 2.0f, "%.3f");

                ImGui::Separator();
                flagCheckbox("Draw model boxes", FrostbiteFeature_DebugBoxes);
                ImGui::SameLine();
                flagCheckbox("Draw snaplines", FrostbiteFeature_Snaplines);
                ImGui::SameLine();
                ImGui::Checkbox("Likely players only", &g_drawLikelyPlayersOnly);
                ImGui::SameLine();
                ImGui::Checkbox("Score labels", &g_labelLikelyPlayerScores);
                ImGui::SliderFloat("Likely player threshold", &g_likelyPlayerThreshold, 0.0f, 100.0f, "%.0f");
                flagCheckbox("FOV override", FrostbiteFeature_FovOverride);
                ImGui::SliderFloat("FOV degrees", &g_featureState.fovDegrees, 30.0f, 140.0f, "%.1f");
                flagCheckbox("View-angle preview", FrostbiteFeature_ViewAnglePreview);
                ImGui::SameLine();
                if (ImGui::Button("Target First Model"))
                {
                    if (FrostbiteUniversal_RefreshActorModelList() > 0 &&
                        SetViewTargetFromFirstActor(g_featureState))
                    {
                        FrostbiteUniversal_SetFeatureState(&g_featureState);
                        FrostbiteUniversal_ApplyFeatureState();
                    }
                }
                if (g_featureState.hasViewTarget)
                {
                    ImGui::Text("Target ID: %llu  Pos: %.2f, %.2f, %.2f  View: %.1f, %.1f, %.1f",
                        static_cast<unsigned long long>(g_featureState.viewTargetActorId),
                        g_featureState.viewTargetPosition[0],
                        g_featureState.viewTargetPosition[1],
                        g_featureState.viewTargetPosition[2],
                        g_featureState.viewAngles[0],
                        g_featureState.viewAngles[1],
                        g_featureState.viewAngles[2]);
                }

                ImGui::Separator();

                ImGui::Checkbox("Refresh live actors every rendered frame", &g_liveActorFrameRefresh);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(150.0f);
                ImGui::SliderInt("Min refresh ms", &g_liveActorRefreshIntervalMs, 250, 5000);
                RefreshLiveActorsForFrame(g_featureState, true);
                ImGui::Text("Live refresh: %d entries, %lu ms old, %llu passes",
                    g_lastLiveActorRefreshCount,
                    static_cast<unsigned long>(LiveActorRefreshAgeMs()),
                    static_cast<unsigned long long>(g_liveActorRefreshPasses));

                if (ImGui::Button("Refresh Actors/Models"))
                {
                    const int count = FrostbiteUniversal_RefreshActorModelList();
                    g_lastLiveActorRefreshTick = ::GetTickCount();
                    g_lastLiveActorRefreshCount = count;
                    ++g_liveActorRefreshPasses;
                }

                ImGui::SameLine();
                if (ImGui::Button("Clear Local List"))
                    FrostbiteUniversal_ClearActorModelList();

                const std::uint32_t actorModelCount = FrostbiteUniversal_GetActorModelCount();
                ImGui::Text("Live actor/model entries: %u", actorModelCount);
                ImGui::InputText("Actor/model filter", g_actorModelFilter, static_cast<int>(std::size(g_actorModelFilter)));

                if (ImGui::BeginTable("LiveActorModelTable", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 255.0f)))
                {
                    ImGui::TableSetupColumn("ID");
                    ImGui::TableSetupColumn("Score");
                    ImGui::TableSetupColumn("Actor");
                    ImGui::TableSetupColumn("Class");
                    ImGui::TableSetupColumn("Model");
                    ImGui::TableSetupColumn("Position");
                    ImGui::TableSetupColumn("Size");
                    ImGui::TableSetupColumn("Radius");
                    ImGui::TableSetupColumn("Flags");
                    ImGui::TableSetupColumn("Asset");
                    ImGui::TableHeadersRow();

                    for (std::uint32_t index = 0; index < actorModelCount; ++index)
                    {
                        FrostbiteActorModelInfo item = {};
                        if (!FrostbiteUniversal_GetActorModelInfo(index, &item))
                            continue;

                        const std::string actorName = WideToUtf8(item.actorName);
                        const std::string className = WideToUtf8(item.className);
                        const std::string modelName = WideToUtf8(item.modelName);
                        const std::string assetPath = WideToUtf8(item.assetPath);
                        const std::string flags = ActorModelFlagsToText(item.flags);

                        if (g_drawLikelyPlayersOnly && item.likelyPlayerScore < g_likelyPlayerThreshold)
                            continue;

                        if (!TextMatchesFilter(actorName, g_actorModelFilter) &&
                            !TextMatchesFilter(className, g_actorModelFilter) &&
                            !TextMatchesFilter(modelName, g_actorModelFilter) &&
                            !TextMatchesFilter(assetPath, g_actorModelFilter) &&
                            !TextMatchesFilter(flags, g_actorModelFilter))
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%llu", static_cast<unsigned long long>(item.id));
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%.0f", item.likelyPlayerScore);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextWrapped("%s", actorName.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextWrapped("%s", className.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextWrapped("%s", modelName.c_str());
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("%.2f, %.2f, %.2f", item.position[0], item.position[1], item.position[2]);
                        ImGui::TableSetColumnIndex(6);
                        ImGui::Text("%.2f, %.2f, %.2f", item.size[0], item.size[1], item.size[2]);
                        ImGui::TableSetColumnIndex(7);
                        ImGui::Text("%.2f", item.radius);
                        ImGui::TableSetColumnIndex(8);
                        ImGui::TextWrapped("%s", flags.c_str());
                        ImGui::TableSetColumnIndex(9);
                        ImGui::TextWrapped("%s", assetPath.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Catalog"))
            {
                const std::uint32_t catalogCount = FrostbiteUniversal_GetCatalogCount();
                ImGui::Text("Actor/model-like entries: %u", catalogCount);
                ImGui::InputText("Catalog filter", g_catalogFilter, static_cast<int>(std::size(g_catalogFilter)));

                if (ImGui::Button("Refresh Catalog"))
                    FrostbiteUniversal_Refresh();

                ImGui::SameLine();
                if (ImGui::Button("Write Catalog Report"))
                    FrostbiteUniversal_WriteCatalogReport(nullptr);

                ImGui::TextWrapped("This catalog comes from loaded export names and model-like files in the app folder. It is not a live actor memory list.");

                if (ImGui::BeginTable("CatalogTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 280.0f)))
                {
                    ImGui::TableSetupColumn("Source");
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Path/Module");
                    ImGui::TableSetupColumn("Address");
                    ImGui::TableSetupColumn("Flags");
                    ImGui::TableHeadersRow();

                    for (std::uint32_t index = 0; index < catalogCount; ++index)
                    {
                        FrostbiteCatalogInfo item = {};
                        if (!FrostbiteUniversal_GetCatalogInfo(index, &item))
                            continue;

                        const std::string source = WideToUtf8(item.source);
                        const std::string name = WideToUtf8(item.name);
                        const std::string path = WideToUtf8(item.path);
                        const std::string flags = CatalogFlagsToText(item.flags);

                        if (!TextMatchesFilter(source, g_catalogFilter) &&
                            !TextMatchesFilter(name, g_catalogFilter) &&
                            !TextMatchesFilter(path, g_catalogFilter) &&
                            !TextMatchesFilter(flags, g_catalogFilter))
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(source.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextWrapped("%s", name.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextWrapped("%s", path.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("0x%p", reinterpret_cast<void*>(item.address));
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextWrapped("%s", flags.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("SDK Symbols"))
            {
                DrawGeneratedSdkSymbolPanel();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Diagnostics"))
            {
                ImGui::TextWrapped("Runtime reports are written with the current process/module snapshot.");
                if (ImGui::Button("Write Runtime Report"))
                    FrostbiteUniversal_WriteRuntimeReport(nullptr);

                ImGui::SameLine();
                if (ImGui::Button("Write Catalog Report"))
                    FrostbiteUniversal_WriteCatalogReport(nullptr);

                ImGui::SameLine();
                if (ImGui::Button("Refresh Then Write"))
                {
                    FrostbiteUniversal_Refresh();
                    FrostbiteUniversal_WriteRuntimeReport(nullptr);
                    FrostbiteUniversal_WriteCatalogReport(nullptr);
                }

                ImGui::Separator();
                ImGui::TextWrapped("Log path:");
                ImGui::TextWrapped("%s", logPath.c_str());
                if (ImGui::Button("Copy Log Path"))
                    ImGui::SetClipboardText(logPath.c_str());

                ImGui::Separator();
                ImGui::TextWrapped("Process path:");
                ImGui::TextWrapped("%s", processPath.c_str());
                ImGui::TextWrapped("Game root:");
                ImGui::TextWrapped("%s", gameRoot.c_str());

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings"))
            {
                ImGui::Checkbox("Auto-refresh runtime", &g_autoRefresh);
                ImGui::SliderFloat("Refresh seconds", &g_autoRefreshSeconds, 0.25f, 10.0f, "%.2f");
                ImGui::SliderFloat("Overlay alpha", &g_overlayAlpha, 0.35f, 1.0f, "%.2f");
                ImGui::Checkbox("Show ImGui demo window", &g_showDemoWindow);

                ImGui::Separator();
                ImGui::TextWrapped("This panel is diagnostic only. It does not patch gameplay, entities, rendering materials, or simulation speed.");
                ImGui::TextWrapped("If the panel disappears, press F4 while the render callback is active.");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        if (g_showDemoWindow)
            ImGui::ShowDemoWindow(&g_showDemoWindow);
    }
}
#endif

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasSharedImGui()
{
    return FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI ? 1 : 0;
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ImGuiSetVisible(int visible)
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    std::lock_guard lock(g_imguiMutex);
    g_visible = visible != 0;
#else
    (void)visible;
#endif
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiIsVisible()
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    std::lock_guard lock(g_imguiMutex);
    return g_visible ? 1 : 0;
#else
    return 0;
#endif
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiStartHotkeyMonitor()
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    std::lock_guard lock(g_imguiMutex);
    if (g_hotkeyThread)
        return 1;

    g_hotkeyStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hotkeyStopEvent)
    {
        FrostbiteUniversal::Log::Write(L"ImGui F4 hotkey monitor failed: CreateEventW failed");
        return 0;
    }

    g_hotkeyThread = ::CreateThread(nullptr, 0, HotkeyThreadProc, nullptr, 0, nullptr);
    if (!g_hotkeyThread)
    {
        FrostbiteUniversal::Log::Write(L"ImGui F4 hotkey monitor failed: CreateThread failed");
        ::CloseHandle(g_hotkeyStopEvent);
        g_hotkeyStopEvent = nullptr;
        return 0;
    }

    FrostbiteUniversal::Log::Write(L"ImGui F4 hotkey monitor started");
    return 1;
#else
    return 0;
#endif
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ImGuiStopHotkeyMonitor()
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    HANDLE thread = nullptr;
    HANDLE stopEvent = nullptr;

    {
        std::lock_guard lock(g_imguiMutex);
        thread = g_hotkeyThread;
        stopEvent = g_hotkeyStopEvent;
        g_hotkeyThread = nullptr;
        g_hotkeyStopEvent = nullptr;
    }

    if (stopEvent)
        ::SetEvent(stopEvent);

    if (thread)
    {
        ::WaitForSingleObject(thread, 1000);
        ::CloseHandle(thread);
    }

    if (stopEvent)
        ::CloseHandle(stopEvent);

    FrostbiteUniversal::Log::Write(L"ImGui F4 hotkey monitor stopped");
#endif
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ImGuiShutdown()
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    FrostbiteUniversal_ImGuiStopHotkeyMonitor();
    std::lock_guard lock(g_imguiMutex);
    ShutdownAllUnlocked();
#endif
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiRenderDx11(HWND hwnd, void* d3d11Device, void* d3d11DeviceContext, void* renderTargetView)
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    std::lock_guard lock(g_imguiMutex);
    const std::uint64_t previousRenderCalls = g_renderCallCount;
    ++g_renderCallCount;
    if (previousRenderCalls == 0)
        FrostbiteUniversal::Log::Write(L"ImGui DX11 render export received");

    auto* device = static_cast<ID3D11Device*>(d3d11Device);
    auto* context = static_cast<ID3D11DeviceContext*>(d3d11DeviceContext);
    auto* targetView = static_cast<ID3D11RenderTargetView*>(renderTargetView);

    if (!EnsureDx11Unlocked(hwnd, device, context))
        return 0;

    SetContext();
    PollF4ToggleUnlocked();
    AutoRefreshRuntimeUnlocked();

    if (targetView)
        context->OMSetRenderTargets(1, &targetView, nullptr);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawOverlayUnlocked();
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ++g_frameCount;
    if (g_frameCount == 1 || (g_frameCount % 300) == 0)
        LogRenderStateUnlocked(L"ImGui DX11 render heartbeat");
    return 1;
#else
    (void)hwnd;
    (void)d3d11Device;
    (void)d3d11DeviceContext;
    (void)renderTargetView;
    return 0;
#endif
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiRenderDx12(
    HWND hwnd,
    void* d3d12Device,
    void* graphicsCommandList,
    int framesInFlight,
    int rtvFormat,
    void* srvDescriptorHeap,
    std::uintptr_t fontSrvCpuDescriptor,
    std::uint64_t fontSrvGpuDescriptor)
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    std::lock_guard lock(g_imguiMutex);
    const std::uint64_t previousRenderCalls = g_renderCallCount;
    ++g_renderCallCount;
    if (previousRenderCalls == 0)
        FrostbiteUniversal::Log::Write(L"ImGui DX12 render export received");

    auto* device = static_cast<ID3D12Device*>(d3d12Device);
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(graphicsCommandList);
    auto* heap = static_cast<ID3D12DescriptorHeap*>(srvDescriptorHeap);

    if (!commandList)
    {
        FrostbiteUniversal::Log::Write(L"ImGui DX12 render failed: command list is null");
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptor = {};
    cpuDescriptor.ptr = fontSrvCpuDescriptor;

    D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptor = {};
    gpuDescriptor.ptr = fontSrvGpuDescriptor;

    if (!EnsureDx12Unlocked(hwnd, device, framesInFlight, static_cast<DXGI_FORMAT>(rtvFormat), heap, cpuDescriptor, gpuDescriptor))
        return 0;

    SetContext();
    PollF4ToggleUnlocked();
    AutoRefreshRuntimeUnlocked();

    commandList->SetDescriptorHeaps(1, &heap);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawOverlayUnlocked();
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
    ++g_frameCount;
    if (g_frameCount == 1 || (g_frameCount % 300) == 0)
        LogRenderStateUnlocked(L"ImGui DX12 render heartbeat");
    return 1;
#else
    (void)hwnd;
    (void)d3d12Device;
    (void)graphicsCommandList;
    (void)framesInFlight;
    (void)rtvFormat;
    (void)srvDescriptorHeap;
    (void)fontSrvCpuDescriptor;
    (void)fontSrvGpuDescriptor;
    return 0;
#endif
}

FROSTBITEUNIVERSAL_API LRESULT FrostbiteUniversal_ImGuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
    std::lock_guard lock(g_imguiMutex);
    if (msg == WM_KEYDOWN && wParam == VK_F4 && (lParam & (1L << 30)) == 0)
    {
        ToggleVisibleUnlocked(L"WndProc F4");
        LogRenderStateUnlocked(L"WndProc F4 diagnostic");
        return 1;
    }

    if (!g_context)
        return 0;

    SetContext();
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
#else
    (void)hwnd;
    (void)msg;
    (void)wParam;
    (void)lParam;
    return 0;
#endif
}
