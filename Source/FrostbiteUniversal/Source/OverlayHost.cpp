#include "OverlayHost.h"

#include "FrostbiteLog.h"
#include "FrostbiteUniversal.h"
#include "SharedImGuiBridge.h"

#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
#include <d3d11.h>
#include <dxgi.h>
#include <windowsx.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr const wchar_t* kOverlayClassName = L"FrostbiteUniversalSelfHostedOverlay";
    constexpr int kDefaultWidth = 760;
    constexpr int kDefaultHeight = 540;
    constexpr DWORD kOverlayVisibleFrameMs = 1;
    constexpr DWORD kOverlayHiddenSleepMs = 50;
    constexpr DWORD kOverlayRepositionMs = 5000;
    constexpr DWORD kOverlayShowCooldownMs = 150;
    constexpr int kDefaultLiveActorRefreshMs = 1000;
    constexpr int kMinLiveActorRefreshMs = 250;
    constexpr int kMaxLiveActorRefreshMs = 5000;
    constexpr std::uint32_t kMaxDebugDrawActorModels = 512;

    std::mutex g_overlayMutex;
    HANDLE g_overlayThread = nullptr;
    HANDLE g_actorRefreshThread = nullptr;
    HANDLE g_overlayStopEvent = nullptr;
    DWORD g_overlayThreadId = 0;
    DWORD g_actorRefreshThreadId = 0;
    HWND g_overlayWindow = nullptr;
    HWND g_targetWindow = nullptr;
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_deviceContext = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_renderTargetView = nullptr;
    ImGuiContext* g_context = nullptr;
    bool g_running = false;
    bool g_lastVisible = true;
    bool g_forceWarpDevice = false;
    bool g_nativeFallbackMode = false;
    int g_nativeFallbackTab = 0;
    std::uint64_t g_overlayFrameCount = 0;
    char g_moduleFilter[128] = {};
    char g_exportFilter[128] = {};
    char g_catalogFilter[128] = {};
    char g_sdkSymbolFilter[128] = {};
    char g_actorModelFilter[128] = {};
    char g_consoleCommand[512] = {};
    float g_timescaleSlider = 1.0f;
    bool g_timescaleSliderInitialized = false;
    FrostbiteUniversalFeatureState g_featureState = {};
    bool g_featureStateInitialized = false;
    int g_validationFlashFrames = 0;
    int g_lastValidationResult = -1;
    int g_cachedActorBridge = -1;
    int g_cachedTimescaleBridge = -1;
    int g_cachedFeatureBridge = -1;
    char g_cachedHookStatus[256] = "not refreshed";
    char g_lastActionStatus[256] = "No action yet. Press Refresh Bridge Status first.";
    bool g_showLiveActorModelTable = false;
    bool g_drawLikelyPlayersOnly = false;
    bool g_labelLikelyPlayerScores = true;
    float g_likelyPlayerThreshold = 55.0f;
    int g_consolePresetIndex = 0;
    std::vector<std::string> g_consoleHistory;
    std::atomic_bool g_liveActorAutoRefresh = false;
    std::atomic_int g_liveActorRefreshIntervalMs = kDefaultLiveActorRefreshMs;
    std::atomic<DWORD> g_lastLiveActorRefreshTick = 0;
    std::atomic_int g_lastLiveActorRefreshCount = 0;
    std::atomic<std::uint64_t> g_liveActorRefreshPasses = 0;
    std::atomic_bool g_actorRefreshInFlight = false;
    const char* g_lastLoggedTab = "";

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

    std::wstring Utf8ToWide(const char* value)
    {
        if (!value || value[0] == '\0')
            return {};

        const int size = ::MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
        if (size <= 1)
            return {};

        std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, value, -1, result.data(), size);
        return result;
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

    void RefreshBridgeStatusCache()
    {
        g_cachedActorBridge = FrostbiteUniversal_HasActorModelBridge();
        g_cachedTimescaleBridge = FrostbiteUniversal_HasTimescaleBridge();
        g_cachedFeatureBridge = FrostbiteUniversal_HasFeatureBridge();

        wchar_t hookStatusWide[256] = {};
        if (FrostbiteUniversal_GetOwnedProjectHookStatus(hookStatusWide, static_cast<std::uint32_t>(std::size(hookStatusWide))))
        {
            const std::string hookStatus = WideToUtf8(hookStatusWide);
            strncpy_s(g_cachedHookStatus, hookStatus.c_str(), _TRUNCATE);
        }

        if (!g_cachedActorBridge && !g_cachedTimescaleBridge && !g_cachedFeatureBridge)
            strncpy_s(g_lastActionStatus, "No configured bridge exports found. Feature buttons are local-only until your owned game exposes the bridge.", _TRUNCATE);
        else
            strncpy_s(g_lastActionStatus, "Bridge status refreshed. Connected controls can now call into the owned game bridge.", _TRUNCATE);
    }

    const char* BridgeText(int value)
    {
        if (value < 0)
            return "not refreshed";

        return value ? "connected" : "not connected";
    }

    void LogTabEntry(const char* tabName)
    {
        if (!tabName || g_lastLoggedTab == tabName || strcmp(g_lastLoggedTab, tabName) == 0)
            return;

        g_lastLoggedTab = tabName;

        std::wstring wideTabName;
        for (const char* ch = tabName; *ch; ++ch)
            wideTabName.push_back(static_cast<wchar_t>(*ch));

        std::wstringstream message;
        message << L"Self-hosted overlay tab entered: " << wideTabName;
        FrostbiteUniversal::Log::Write(message.str());
    }

    void SetActionStatus(const char* message)
    {
        strncpy_s(g_lastActionStatus, message ? message : "unknown action", _TRUNCATE);
    }

    void SetActionStatus(const char* prefix, int result)
    {
        char message[256] = {};
        sprintf_s(message, "%s: %s", prefix ? prefix : "Action", result ? "sent to connected bridge" : "local-only, no connected game bridge");
        SetActionStatus(message);
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

        const std::uint32_t available = FrostbiteUniversal_GetActorModelCount();
        const std::uint32_t count = available < kMaxDebugDrawActorModels ? available : kMaxDebugDrawActorModels;
        if (count == 0)
            return;

        std::vector<FrostbiteActorModelInfo> actors(count);
        const std::uint32_t copied = FrostbiteUniversal_CopyActorModelList(actors.data(), count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (index >= copied)
                break;

            const FrostbiteActorModelInfo& item = actors[index];
            if (item.id == 0)
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

    int ClampLiveActorRefreshInterval(int value)
    {
        return (std::max)(kMinLiveActorRefreshMs, (std::min)(value, kMaxLiveActorRefreshMs));
    }

    int RefreshActorModelCacheNow()
    {
        bool expected = false;
        if (!g_actorRefreshInFlight.compare_exchange_strong(expected, true, std::memory_order_acquire))
            return g_lastLiveActorRefreshCount.load(std::memory_order_relaxed);

        struct RefreshInFlightReset
        {
            std::atomic_bool& value;
            ~RefreshInFlightReset()
            {
                value.store(false, std::memory_order_release);
            }
        } reset{g_actorRefreshInFlight};

        try
        {
            const int count = FrostbiteUniversal_RefreshActorModelList();
            g_lastLiveActorRefreshTick.store(::GetTickCount(), std::memory_order_relaxed);
            g_lastLiveActorRefreshCount.store(count, std::memory_order_relaxed);
            g_liveActorRefreshPasses.fetch_add(1, std::memory_order_relaxed);
            return count;
        }
        catch (const std::exception& ex)
        {
            std::wstring message = L"Actor/model cache refresh caught C++ exception: ";
            message += Utf8ToWide(ex.what());
            FrostbiteUniversal::Log::Write(message);
        }
        catch (...)
        {
            FrostbiteUniversal::Log::Write(L"Actor/model cache refresh caught unknown C++ exception");
        }

        return g_lastLiveActorRefreshCount.load(std::memory_order_relaxed);
    }

    DWORD LiveActorRefreshAgeMs()
    {
        const DWORD lastTick = g_lastLiveActorRefreshTick.load(std::memory_order_relaxed);
        if (lastTick == 0)
            return 0;

        return ::GetTickCount() - lastTick;
    }

    DWORD WINAPI ActorModelRefreshThreadBody(LPVOID parameter)
    {
        HANDLE stopEvent = reinterpret_cast<HANDLE>(parameter);
        FrostbiteUniversal::Log::Write(L"Self-hosted overlay actor/model cache refresh thread started; auto-refresh is opt-in for stability");

        while (stopEvent)
        {
            const DWORD interval = static_cast<DWORD>(
                ClampLiveActorRefreshInterval(g_liveActorRefreshIntervalMs.load(std::memory_order_relaxed)));
            const DWORD waitResult = ::WaitForSingleObject(stopEvent, interval);
            if (waitResult != WAIT_TIMEOUT)
                break;

            if (!g_liveActorAutoRefresh.load(std::memory_order_relaxed))
                continue;

            RefreshActorModelCacheNow();
        }

        FrostbiteUniversal::Log::Write(L"Self-hosted overlay actor/model cache refresh thread leaving");
        return 0;
    }

    DWORD ActorModelRefreshThreadWithCppGuards(LPVOID parameter)
    {
        try
        {
            return ActorModelRefreshThreadBody(parameter);
        }
        catch (const std::exception& ex)
        {
            std::wstring message = L"Actor/model refresh thread caught C++ exception: ";
            message += Utf8ToWide(ex.what());
            FrostbiteUniversal::Log::Write(message);
            return 0;
        }
        catch (...)
        {
            FrostbiteUniversal::Log::Write(L"Actor/model refresh thread caught unknown C++ exception");
            return 0;
        }
    }

    DWORD WINAPI ActorModelRefreshThreadProc(LPVOID parameter)
    {
        DWORD exceptionCode = 0;
        __try
        {
            return ActorModelRefreshThreadWithCppGuards(parameter);
        }
        __except ((exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            wchar_t message[160] = {};
            swprintf_s(message, L"Actor/model refresh thread caught SEH exception 0x%08X", exceptionCode);
            FrostbiteUniversal::Log::Write(message);
            return 0;
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
        {
            const int result = FrostbiteUniversal_StartSdkDump(nullptr);
            SetActionStatus(result ? "Current-process SDK dump started" : "SDK dump is already running or failed to start");
        }

        ImGui::SameLine();
        if (ImGui::Button("Reload Generated SDK Cache"))
        {
            const int result = FrostbiteUniversal_ReloadGeneratedSdk(nullptr);
            SetActionStatus(result ? "Generated SDK cache reloaded" : "No generated SDK cache folder found");
        }
    }

    std::string ToLowerAscii(std::string value)
    {
        for (char& ch : value)
            ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        return value;
    }

    bool ParseToggleValue(const std::string& value, bool& outEnabled)
    {
        const std::string lower = ToLowerAscii(value);
        if (lower == "1" || lower == "on" || lower == "true" || lower == "yes" || lower == "enable" || lower == "enabled")
        {
            outEnabled = true;
            return true;
        }

        if (lower == "0" || lower == "off" || lower == "false" || lower == "no" || lower == "disable" || lower == "disabled")
        {
            outEnabled = false;
            return true;
        }

        return false;
    }

    void PushConsoleHistory(const std::string& line)
    {
        if (line.empty())
            return;

        g_consoleHistory.push_back(line);
        if (g_consoleHistory.size() > 96)
            g_consoleHistory.erase(g_consoleHistory.begin(), g_consoleHistory.begin() + (g_consoleHistory.size() - 96));
    }

    void SetFeatureFlag(std::uint32_t flag, bool enabled)
    {
        if (enabled)
            g_featureState.enabledFlags |= flag;
        else
            g_featureState.enabledFlags &= ~flag;

        FrostbiteUniversal_SetFeatureState(&g_featureState);
        FrostbiteUniversal_ApplyFeatureState();
    }

    void ExecuteOverlayConsoleCommand(const char* commandText)
    {
        const std::string command = commandText ? commandText : "";
        if (command.empty())
            return;

        PushConsoleHistory("> " + command);

        std::istringstream stream(command);
        std::string verb;
        stream >> verb;
        verb = ToLowerAscii(verb);

        if (verb == "help")
        {
            PushConsoleHistory("Commands: open_console, refresh, validate, reload_sdk, hooks, bridge, timescale <v>, boxes <on/off>, snaplines <on/off>, likely <on/off>, likely_threshold <0-100>, fov <degrees>, send <command>");
            return;
        }

        if (verb == "open_console")
        {
            const int result = FrostbiteUniversal_OpenConsole();
            PushConsoleHistory(result ? "OS console opened." : "OS console open failed.");
            SetActionStatus(result ? "Console opened" : "Console open failed");
            return;
        }

        if (verb == "refresh")
        {
            const int count = RefreshActorModelCacheNow();
            char line[96] = {};
            sprintf_s(line, "Actor/model cache refreshed: %d entries.", count);
            PushConsoleHistory(line);
            return;
        }

        if (verb == "validate")
        {
            g_lastValidationResult = FrostbiteUniversal_RunUniversalValidation();
            g_validationFlashFrames = 240;
            PushConsoleHistory(g_lastValidationResult ? "Validation passed." : "Validation completed with warnings.");
            return;
        }

        if (verb == "sdk_dump")
        {
            const int result = FrostbiteUniversal_StartSdkDump(nullptr);
            PushConsoleHistory(result ? "SDK dump started." : "SDK dump did not start.");
            return;
        }

        if (verb == "reload_sdk")
        {
            const int result = FrostbiteUniversal_ReloadGeneratedSdk(nullptr);
            PushConsoleHistory(result ? "Generated SDK cache reloaded." : "No generated SDK cache folder found.");
            return;
        }

        if (verb == "hooks")
        {
            const int result = FrostbiteUniversal_InstallOwnedProjectHooks();
            PushConsoleHistory(result ? "Owned-project hooks installed." : "No owned-project hook target was installed.");
            return;
        }

        if (verb == "bridge")
        {
            RefreshBridgeStatusCache();
            PushConsoleHistory("Bridge status refreshed.");
            return;
        }

        if (verb == "timescale")
        {
            float value = g_timescaleSlider;
            stream >> value;
            g_timescaleSlider = value;
            g_featureState.timescale = value;
            SetFeatureFlag(FrostbiteFeature_Timescale, true);
            PushConsoleHistory("Timescale command applied to Universal state.");
            return;
        }

        if (verb == "boxes" || verb == "snaplines" || verb == "likely")
        {
            std::string value;
            stream >> value;
            bool enabled = false;
            if (!ParseToggleValue(value, enabled))
            {
                PushConsoleHistory("Expected on/off.");
                return;
            }

            if (verb == "boxes")
                SetFeatureFlag(FrostbiteFeature_DebugBoxes, enabled);
            else if (verb == "snaplines")
                SetFeatureFlag(FrostbiteFeature_Snaplines, enabled);
            else
                g_drawLikelyPlayersOnly = enabled;

            PushConsoleHistory(enabled ? "Toggle enabled." : "Toggle disabled.");
            return;
        }

        if (verb == "likely_threshold")
        {
            float value = g_likelyPlayerThreshold;
            stream >> value;
            g_likelyPlayerThreshold = (std::max)(0.0f, (std::min)(value, 100.0f));
            PushConsoleHistory("Likely-player threshold updated.");
            return;
        }

        if (verb == "fov")
        {
            float value = g_featureState.fovDegrees;
            stream >> value;
            g_featureState.fovDegrees = (std::max)(30.0f, (std::min)(value, 140.0f));
            SetFeatureFlag(FrostbiteFeature_FovOverride, true);
            PushConsoleHistory("FOV command applied to Universal state.");
            return;
        }

        std::string bridgeCommand = command;
        if (verb == "send")
        {
            std::getline(stream, bridgeCommand);
            while (!bridgeCommand.empty() && bridgeCommand.front() == ' ')
                bridgeCommand.erase(bridgeCommand.begin());
        }

        const std::wstring wideCommand = Utf8ToWide(bridgeCommand.c_str());
        const int result = FrostbiteUniversal_ExecuteConsoleCommand(wideCommand.c_str());
        PushConsoleHistory(result ? "Command sent to owned-project bridge." : "No owned-project console bridge handled the command.");
    }

    void DrawConsolePanel()
    {
        static const char* kPresets[] = {
            "help",
            "open_console",
            "refresh",
            "validate",
            "reload_sdk",
            "bridge",
            "hooks",
            "boxes on",
            "snaplines on",
            "likely on",
            "likely_threshold 55",
            "timescale 1.0",
            "fov 90",
            "send "
        };

        if (ImGui::Button("Open OS Console"))
        {
            const int result = FrostbiteUniversal_OpenConsole();
            PushConsoleHistory(result ? "OS console opened." : "OS console open failed.");
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("Preset", &g_consolePresetIndex, kPresets, static_cast<int>(std::size(kPresets))))
            strncpy_s(g_consoleCommand, kPresets[g_consolePresetIndex], _TRUNCATE);

        ImGui::SetNextItemWidth(-90.0f);
        const bool submitted = ImGui::InputText("Command", g_consoleCommand, static_cast<int>(std::size(g_consoleCommand)), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Send") || submitted)
        {
            ExecuteOverlayConsoleCommand(g_consoleCommand);
            g_consoleCommand[0] = '\0';
        }

        if (ImGui::BeginChild("FrostbiteUniversalConsoleHistory", ImVec2(0.0f, 220.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
        {
            for (const std::string& line : g_consoleHistory)
                ImGui::TextWrapped("%s", line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    void DrawGeneratedSdkSymbolPanel()
    {
        FrostbiteGeneratedSdkInfo sdkInfo = {};
        const bool loaded = FrostbiteUniversal_GetGeneratedSdkInfo(&sdkInfo) != 0;
        const std::uint32_t symbolCount = FrostbiteUniversal_GetGeneratedSdkSymbolCount();

        ImGui::Text("Generated SDK symbols: %u", symbolCount);
        ImGui::SameLine();
        if (ImGui::Button("Dump Current Process SDK"))
        {
            const int result = FrostbiteUniversal_StartSdkDump(nullptr);
            SetActionStatus(result ? "Current-process SDK dump started" : "SDK dump is already running or failed to start");
        }

        ImGui::SameLine();
        if (ImGui::Button("Reload SDK Cache"))
        {
            const int result = FrostbiteUniversal_ReloadGeneratedSdk(nullptr);
            SetActionStatus(result ? "Generated SDK cache reloaded" : "No current-process generated SDK cache found");
        }

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

        if (ImGui::BeginTable("AutoOverlayGeneratedSdkSymbols", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 330.0f)))
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

    RECT MakeRect(int x, int y, int width, int height)
    {
        RECT rect = {};
        rect.left = x;
        rect.top = y;
        rect.right = x + width;
        rect.bottom = y + height;
        return rect;
    }

    bool HitRect(const RECT& rect, int x, int y)
    {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }

    void DrawNativeText(HDC dc, int x, int& y, const std::wstring& text, COLORREF color = RGB(220, 225, 235))
    {
        RECT rect = MakeRect(x, y, 700, 22);
        ::SetTextColor(dc, color);
        ::DrawTextW(dc, text.c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += 24;
    }

    void DrawNativeButton(HDC dc, const RECT& rect, const wchar_t* label, bool active = false)
    {
        HBRUSH brush = ::CreateSolidBrush(active ? RGB(92, 28, 36) : RGB(42, 45, 55));
        ::FillRect(dc, &rect, brush);
        ::DeleteObject(brush);

        HPEN pen = ::CreatePen(PS_SOLID, 1, active ? RGB(255, 72, 88) : RGB(120, 125, 140));
        HGDIOBJ oldPen = ::SelectObject(dc, pen);
        HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);

        RECT textRect = rect;
        ::SetTextColor(dc, RGB(235, 238, 245));
        ::DrawTextW(dc, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void DrawNativeFallbackPanel(HWND hwnd)
    {
        PAINTSTRUCT ps = {};
        HDC dc = ::BeginPaint(hwnd, &ps);
        if (!dc)
            return;

        RECT client = {};
        ::GetClientRect(hwnd, &client);
        HBRUSH background = ::CreateSolidBrush(RGB(15, 17, 22));
        ::FillRect(dc, &client, background);
        ::DeleteObject(background);

        ::SetBkMode(dc, TRANSPARENT);
        HFONT font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = ::SelectObject(dc, font);

        RECT titleRect = MakeRect(18, 14, client.right - 36, 26);
        ::SetTextColor(dc, RGB(255, 72, 88));
        ::DrawTextW(dc, L"Frostbite Universal - Native Fallback Menu", -1, &titleRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

        const wchar_t* tabs[] = { L"Runtime", L"Overlay", L"Console", L"SDK", L"Settings" };
        for (int index = 0; index < 5; ++index)
            DrawNativeButton(dc, MakeRect(18 + (index * 128), 48, 118, 30), tabs[index], g_nativeFallbackTab == index);

        int y = 94;
        FrostbiteRuntimeInfo info = {};
        FrostbiteUniversal_GetRuntimeInfo(&info);

        if (g_nativeFallbackTab == 0)
        {
            DrawNativeText(dc, 20, y, L"Renderer: native Win32/GDI fallback because the self-hosted D3D swapchain is device-removed.", RGB(255, 205, 110));
            DrawNativeText(dc, 20, y, std::wstring(L"Process: ") + info.processName);
            DrawNativeText(dc, 20, y, std::wstring(L"Game root: ") + info.gameRoot);

            wchar_t metrics[256] = {};
            swprintf_s(metrics, L"Modules: %u total / %u Frostbite    Exports: %u    Catalog: %u    TOC/CAS: %u/%u",
                info.moduleCount,
                info.frostbiteModuleCount,
                FrostbiteUniversal_GetExportCount(),
                FrostbiteUniversal_GetCatalogCount(),
                info.tocFileCount,
                info.casFileCount);
            DrawNativeText(dc, 20, y, metrics);

            FrostbiteGeneratedSdkInfo sdkInfo = {};
            if (FrostbiteUniversal_GetGeneratedSdkInfo(&sdkInfo))
            {
                wchar_t sdkLine[256] = {};
                swprintf_s(sdkLine, L"Generated SDK cache: files=%u functions=%u strings=%u xrefs=%u",
                    sdkInfo.generatedFileCount,
                    sdkInfo.runtimeFunctionCandidateCount,
                    sdkInfo.stringCount,
                    sdkInfo.stringXrefCount);
                DrawNativeText(dc, 20, y, sdkLine);
                wchar_t symbolLine[160] = {};
                swprintf_s(symbolLine, L"Classified SDK symbols: %u", sdkInfo.generatedSymbolCount);
                DrawNativeText(dc, 20, y, symbolLine);
                DrawNativeText(dc, 20, y, std::wstring(L"SDK output: ") + sdkInfo.outputDir);
            }
            else
            {
                DrawNativeText(dc, 20, y, L"Generated SDK cache: no current-process dump loaded", RGB(255, 160, 120));
            }

            DrawNativeButton(dc, MakeRect(20, 318, 130, 32), L"Refresh Runtime");
            DrawNativeButton(dc, MakeRect(162, 318, 130, 32), L"Reload SDK");
            DrawNativeButton(dc, MakeRect(304, 318, 130, 32), L"Dump SDK");
            DrawNativeButton(dc, MakeRect(446, 318, 130, 32), L"Copy Log Path");
        }
        else if (g_nativeFallbackTab == 1)
        {
            DrawNativeText(dc, 20, y, L"Overlay cache controls");
            DrawNativeText(dc, 20, y, std::wstring(L"Actor bridge: ") + Utf8ToWide(BridgeText(g_cachedActorBridge)));
            DrawNativeText(dc, 20, y, std::wstring(L"Feature bridge: ") + Utf8ToWide(BridgeText(g_cachedFeatureBridge)));
            wchar_t cacheLine[180] = {};
            swprintf_s(cacheLine, L"Actor/model cache: %d entries, %llu refresh passes, auto-refresh %s",
                g_lastLiveActorRefreshCount.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(g_liveActorRefreshPasses.load(std::memory_order_relaxed)),
                g_liveActorAutoRefresh.load(std::memory_order_relaxed) ? L"on" : L"off");
            DrawNativeText(dc, 20, y, cacheLine);
            DrawNativeText(dc, 20, y, std::wstring(L"Last action: ") + Utf8ToWide(g_lastActionStatus));

            DrawNativeButton(dc, MakeRect(20, 210, 140, 32), L"Refresh Actors");
            DrawNativeButton(dc, MakeRect(172, 210, 150, 32), g_liveActorAutoRefresh.load(std::memory_order_relaxed) ? L"Auto Refresh: On" : L"Auto Refresh: Off");
            DrawNativeButton(dc, MakeRect(334, 210, 130, 32), L"Bridge Status");
        }
        else if (g_nativeFallbackTab == 2)
        {
            DrawNativeText(dc, 20, y, L"Console bridge");
            DrawNativeText(dc, 20, y, L"The native fallback keeps controls available, but text command editing needs the ImGui renderer path.");
            DrawNativeText(dc, 20, y, L"Use Open OS Console for logs and bridge diagnostics.");
            DrawNativeButton(dc, MakeRect(20, 176, 150, 32), L"Open OS Console");
            DrawNativeButton(dc, MakeRect(182, 176, 130, 32), L"Validate");
        }
        else if (g_nativeFallbackTab == 3)
        {
            const std::uint32_t symbolCount = FrostbiteUniversal_GetGeneratedSdkSymbolCount();
            wchar_t countLine[160] = {};
            swprintf_s(countLine, L"Generated SDK classified symbols: %u", symbolCount);
            DrawNativeText(dc, 20, y, countLine);
            DrawNativeText(dc, 20, y, L"Top current-process dump candidates:");

            const std::uint32_t limit = (std::min)(symbolCount, 10u);
            for (std::uint32_t index = 0; index < limit; ++index)
            {
                FrostbiteGeneratedSdkSymbolInfo symbol = {};
                if (!FrostbiteUniversal_GetGeneratedSdkSymbolInfo(index, &symbol))
                    continue;

                wchar_t line[680] = {};
                swprintf_s(line, L"%u. [%s/%s] score=%u flags=0x%X %s @ %s",
                    index + 1,
                    symbol.source,
                    symbol.category,
                    symbol.score,
                    symbol.flags,
                    symbol.name,
                    symbol.addressHex);
                DrawNativeText(dc, 20, y, line);
            }

            DrawNativeButton(dc, MakeRect(20, 384, 130, 32), L"Reload SDK");
            DrawNativeButton(dc, MakeRect(162, 384, 130, 32), L"Dump SDK");
        }
        else
        {
            DrawNativeText(dc, 20, y, L"Settings");
            DrawNativeText(dc, 20, y, L"D3D self-hosted UI failed with DXGI device removed; this window is intentionally not using D3D.");
            DrawNativeText(dc, 20, y, L"The in-game ImGui path still requires FrostbiteUniversal_ImGuiRenderDx11/Dx12 to be called from a render path.");
            DrawNativeText(dc, 20, y, std::wstring(L"Last action: ") + Utf8ToWide(g_lastActionStatus));
            DrawNativeButton(dc, MakeRect(20, 192, 130, 32), L"Hide Menu");
            DrawNativeButton(dc, MakeRect(162, 192, 130, 32), L"Copy Log Path");
        }

        ::SelectObject(dc, oldFont);
        ::EndPaint(hwnd, &ps);
    }

    void SetNativeClipboardText(HWND hwnd, const wchar_t* text)
    {
        if (!text || !::OpenClipboard(hwnd))
            return;

        ::EmptyClipboard();
        const std::size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory)
        {
            void* destination = ::GlobalLock(memory);
            if (destination)
            {
                memcpy(destination, text, bytes);
                ::GlobalUnlock(memory);
                ::SetClipboardData(CF_UNICODETEXT, memory);
                memory = nullptr;
            }
        }

        if (memory)
            ::GlobalFree(memory);

        ::CloseClipboard();
    }

    void HandleNativeFallbackClick(HWND hwnd, int x, int y)
    {
        for (int index = 0; index < 5; ++index)
        {
            if (HitRect(MakeRect(18 + (index * 128), 48, 118, 30), x, y))
            {
                g_nativeFallbackTab = index;
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
        }

        if (g_nativeFallbackTab == 0)
        {
            if (HitRect(MakeRect(20, 318, 130, 32), x, y))
            {
                FrostbiteUniversal_Refresh();
                SetActionStatus("Runtime refreshed from native fallback");
            }
            else if (HitRect(MakeRect(162, 318, 130, 32), x, y))
            {
                const int result = FrostbiteUniversal_ReloadGeneratedSdk(nullptr);
                SetActionStatus(result ? "Generated SDK cache reloaded" : "No current-process generated SDK cache found");
            }
            else if (HitRect(MakeRect(304, 318, 130, 32), x, y))
            {
                const int result = FrostbiteUniversal_StartSdkDump(nullptr);
                SetActionStatus(result ? "Current-process SDK dump started" : "SDK dump is already running or failed to start");
            }
            else if (HitRect(MakeRect(446, 318, 130, 32), x, y))
            {
                wchar_t path[MAX_PATH] = {};
                FrostbiteUniversal_GetLogPath(path, static_cast<std::uint32_t>(std::size(path)));
                SetNativeClipboardText(hwnd, path);
                SetActionStatus("Log path copied");
            }
        }
        else if (g_nativeFallbackTab == 1)
        {
            if (HitRect(MakeRect(20, 210, 140, 32), x, y))
            {
                const int count = RefreshActorModelCacheNow();
                char status[128] = {};
                sprintf_s(status, "Native fallback actor refresh: %d entries", count);
                SetActionStatus(status);
            }
            else if (HitRect(MakeRect(172, 210, 150, 32), x, y))
            {
                const bool next = !g_liveActorAutoRefresh.load(std::memory_order_relaxed);
                g_liveActorAutoRefresh.store(next, std::memory_order_relaxed);
                SetActionStatus(next ? "Actor auto-refresh enabled" : "Actor auto-refresh disabled");
            }
            else if (HitRect(MakeRect(334, 210, 130, 32), x, y))
            {
                RefreshBridgeStatusCache();
            }
        }
        else if (g_nativeFallbackTab == 2)
        {
            if (HitRect(MakeRect(20, 176, 150, 32), x, y))
                FrostbiteUniversal_OpenConsole();
            else if (HitRect(MakeRect(182, 176, 130, 32), x, y))
                FrostbiteUniversal_RunUniversalValidation();
        }
        else if (g_nativeFallbackTab == 3)
        {
            if (HitRect(MakeRect(20, 384, 130, 32), x, y))
            {
                const int result = FrostbiteUniversal_ReloadGeneratedSdk(nullptr);
                SetActionStatus(result ? "Generated SDK cache reloaded" : "No current-process generated SDK cache found");
            }
            else if (HitRect(MakeRect(162, 384, 130, 32), x, y))
            {
                const int result = FrostbiteUniversal_StartSdkDump(nullptr);
                SetActionStatus(result ? "Current-process SDK dump started" : "SDK dump is already running or failed to start");
            }
        }
        else
        {
            if (HitRect(MakeRect(20, 192, 130, 32), x, y))
                FrostbiteUniversal_ImGuiSetVisible(0);
            else if (HitRect(MakeRect(162, 192, 130, 32), x, y))
            {
                wchar_t path[MAX_PATH] = {};
                FrostbiteUniversal_GetLogPath(path, static_cast<std::uint32_t>(std::size(path)));
                SetNativeClipboardText(hwnd, path);
                SetActionStatus("Log path copied");
            }
        }

        ::InvalidateRect(hwnd, nullptr, FALSE);
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (g_swapChain && SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (g_renderTargetView)
        {
            g_renderTargetView->Release();
            g_renderTargetView = nullptr;
        }
    }

    void CleanupDevice();

    bool CreateDeviceWithDriver(HWND hwnd, D3D_DRIVER_TYPE driverType, const wchar_t* driverLabel, HRESULT& outHr)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 2;
        desc.BufferDesc.Width = 0;
        desc.BufferDesc.Height = 0;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.Flags = 0;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = hwnd;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createFlags = 0;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };

        const HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr,
            driverType,
            nullptr,
            createFlags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &desc,
            &g_swapChain,
            &g_device,
            &featureLevel,
            &g_deviceContext);
        outHr = hr;

        if (FAILED(hr))
            return false;

        CreateRenderTarget();
        std::wstringstream message;
        message << L"Self-hosted overlay D3D11 device created using "
                << (driverLabel ? driverLabel : L"unknown")
                << L" driver";
        FrostbiteUniversal::Log::Write(message.str());
        return true;
    }

    bool CreateDevice(HWND hwnd)
    {
        if (g_forceWarpDevice)
        {
            HRESULT forcedWarpHr = S_OK;
            if (CreateDeviceWithDriver(hwnd, D3D_DRIVER_TYPE_WARP, L"WARP-forced", forcedWarpHr))
                return true;

            std::wstringstream forcedFailure;
            forcedFailure << L"Self-hosted overlay forced WARP D3D11 device failed, HRESULT=0x" << std::hex << forcedWarpHr;
            FrostbiteUniversal::Log::Write(forcedFailure.str());
            return false;
        }

        HRESULT hardwareHr = S_OK;
        if (CreateDeviceWithDriver(hwnd, D3D_DRIVER_TYPE_HARDWARE, L"hardware", hardwareHr))
            return true;

        std::wstringstream hardwareFailure;
        hardwareFailure << L"Self-hosted overlay hardware D3D11 device failed, HRESULT=0x" << std::hex << hardwareHr
                        << L"; trying WARP fallback";
        FrostbiteUniversal::Log::Write(hardwareFailure.str());

        CleanupDevice();
        HRESULT warpHr = S_OK;
        if (CreateDeviceWithDriver(hwnd, D3D_DRIVER_TYPE_WARP, L"WARP", warpHr))
            return true;

        std::wstringstream warpFailure;
        warpFailure << L"Self-hosted overlay failed to create D3D11 device, hardware HRESULT=0x" << std::hex << hardwareHr
                    << L", WARP HRESULT=0x" << warpHr;
        FrostbiteUniversal::Log::Write(warpFailure.str());
        return false;
    }

    void CleanupDevice()
    {
        CleanupRenderTarget();

        if (g_swapChain)
        {
            g_swapChain->Release();
            g_swapChain = nullptr;
        }

        if (g_deviceContext)
        {
            g_deviceContext->Release();
            g_deviceContext = nullptr;
        }

        if (g_device)
        {
            g_device->Release();
            g_device = nullptr;
        }
    }

    BOOL CALLBACK FindProcessWindowProc(HWND hwnd, LPARAM lParam)
    {
        DWORD windowProcessId = 0;
        ::GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (windowProcessId != ::GetCurrentProcessId())
            return TRUE;

        if (hwnd == g_overlayWindow || !::IsWindowVisible(hwnd) || ::GetWindow(hwnd, GW_OWNER))
            return TRUE;

        RECT rect = {};
        if (!::GetWindowRect(hwnd, &rect))
            return TRUE;

        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width < 320 || height < 200)
            return TRUE;

        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE;
    }

    HWND FindTargetWindow()
    {
        HWND result = nullptr;
        ::EnumWindows(FindProcessWindowProc, reinterpret_cast<LPARAM>(&result));
        return result;
    }

    RECT GetOverlayRect()
    {
        RECT rect = {};
        HWND target = FindTargetWindow();
        g_targetWindow = target;

        if (target && ::GetWindowRect(target, &rect))
        {
            rect.left += 36;
            rect.top += 36;
            rect.right = rect.left + kDefaultWidth;
            rect.bottom = rect.top + kDefaultHeight;
            return rect;
        }

        rect.left = 120;
        rect.top = 120;
        rect.right = rect.left + kDefaultWidth;
        rect.bottom = rect.top + kDefaultHeight;
        return rect;
    }

    void PositionOverlay(bool visible)
    {
        RECT rect = GetOverlayRect();
        const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible ? 0 : SWP_HIDEWINDOW);
        ::SetWindowPos(
            g_overlayWindow,
            HWND_TOPMOST,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            flags);
    }

    void DrawOverlay()
    {
        InitializeFeatureUiState();
        DrawActorModelDebugOverlay(g_featureState);

        FrostbiteRuntimeInfo info = {};
        FrostbiteUniversal_GetRuntimeInfo(&info);

        const std::string processName = WideToUtf8(info.processName);
        const std::string gameRoot = WideToUtf8(info.gameRoot);

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(736.0f, 506.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Frostbite Universal", nullptr, ImGuiWindowFlags_NoCollapse);

        ImGui::TextUnformatted("Runtime overlay");
        ImGui::SameLine();
        ImGui::TextDisabled("self-hosted fallback, F4 toggles visibility");
        ImGui::Separator();
        DrawStatusPill("Runtime", FrostbiteUniversal_IsFrostbiteProcess() != 0);
        ImGui::SameLine();
        DrawStatusPill("Actor bridge", g_cachedActorBridge == 1);
        ImGui::SameLine();
        DrawStatusPill("Feature bridge", g_cachedFeatureBridge == 1);

        if (ImGui::BeginTabBar("AutoOverlayTabs"))
        {
            if (ImGui::BeginTabItem("Runtime"))
            {
                LogTabEntry("Runtime");
                char moduleValue[96] = {};
                sprintf_s(moduleValue, "%u total / %u Frostbite", info.moduleCount, info.frostbiteModuleCount);
                char exportValue[96] = {};
                sprintf_s(exportValue, "%u named exports", FrostbiteUniversal_GetExportCount());
                char catalogValue[96] = {};
                sprintf_s(catalogValue, "%u entries", FrostbiteUniversal_GetCatalogCount());
                char archiveValue[96] = {};
                sprintf_s(archiveValue, "%u TOC / %u CAS", info.tocFileCount, info.casFileCount);

                if (ImGui::BeginTable("AutoOverlayStatusCards", 4, ImGuiTableFlags_SizingStretchSame))
                {
                    ImGui::TableNextColumn();
                    DrawMetricCard("Modules", moduleValue, ImVec4(1.0f, 0.20f, 0.28f, 1.0f));
                    ImGui::TableNextColumn();
                    DrawMetricCard("Exports", exportValue, ImVec4(0.95f, 0.58f, 0.18f, 1.0f));
                    ImGui::TableNextColumn();
                    DrawMetricCard("Catalog", catalogValue, ImVec4(0.42f, 0.70f, 1.0f, 1.0f));
                    ImGui::TableNextColumn();
                    DrawMetricCard("Archives", archiveValue, ImVec4(0.42f, 0.95f, 0.58f, 1.0f));
                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::Text("Process: %s", processName.c_str());
                ImGui::Text("Detected Frostbite: %s", FrostbiteUniversal_IsFrostbiteProcess() ? "yes" : "no");
                ImGui::Text("Overlay frames: %llu", static_cast<unsigned long long>(g_overlayFrameCount));
                ImGui::Text("Target window: 0x%p", g_targetWindow);
                ImGui::Separator();
                ImGui::Text("Modules: %u", info.moduleCount);
                ImGui::Text("Frostbite modules: %u", info.frostbiteModuleCount);
                ImGui::Text("Named exports: %u", FrostbiteUniversal_GetExportCount());
                ImGui::Text("Actor/model catalog: %u", FrostbiteUniversal_GetCatalogCount());
                ImGui::Text("Live actor/model bridge: %s", BridgeText(g_cachedActorBridge));
                ImGui::Text("Timescale bridge: %s", BridgeText(g_cachedTimescaleBridge));
                ImGui::Text("Feature bridge: %s", BridgeText(g_cachedFeatureBridge));
                ImGui::Text("Requested timescale: %.2f", FrostbiteUniversal_GetTimescale());
                ImGui::Text("TOC files: %u", info.tocFileCount);
                ImGui::Text("CAS files: %u", info.casFileCount);
                ImGui::Text("Flags: 0x%08X", info.flags);
                DrawGeneratedSdkRuntimeStatus();

                if (g_validationFlashFrames > 0)
                {
                    --g_validationFlashFrames;
                    const ImVec4 color = g_lastValidationResult == 1
                        ? ImVec4(0.22f, 0.95f, 0.45f, 1.0f)
                        : ImVec4(1.0f, 0.60f, 0.25f, 1.0f);
                    ImGui::TextColored(color, "Universal validation: %s", g_lastValidationResult == 1 ? "passed" : "completed with warnings");
                }

                if (ImGui::Button("Run Universal Validation"))
                {
                    g_lastValidationResult = FrostbiteUniversal_RunUniversalValidation();
                    g_validationFlashFrames = 240;
                }

                ImGui::SameLine();
                if (ImGui::Button("Refresh Runtime"))
                    FrostbiteUniversal_Refresh();

                ImGui::SameLine();
                if (ImGui::Button("Refresh Bridge Status"))
                    RefreshBridgeStatusCache();

                ImGui::SameLine();
                if (ImGui::Button("Write Runtime Report"))
                    FrostbiteUniversal_WriteRuntimeReport(nullptr);

                ImGui::Separator();
                ImGui::TextWrapped("Game root: %s", gameRoot.c_str());
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Adapter"))
            {
                LogTabEntry("Adapter");
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
                {
                    FrostbiteUniversal_UpdateProviders();
                    SetActionStatus("Update Providers: provider snapshot refreshed");
                }

                ImGui::SameLine();
                if (ImGui::Button("Print Entities"))
                {
                    FrostbiteUniversal_PrintCurrentEntities();
                    SetActionStatus("Print Entities: wrote current actor/model list to console/debug output");
                }

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
                        const int result = FrostbiteUniversal_WriteSnapshotJson(path.c_str());
                        SetActionStatus(result ? "Write Snapshot: saved to temp FrostbiteUniversal_Snapshot.json" : "Write Snapshot: failed");
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

                if (ImGui::BeginTable("AutoOverlayAdapterCapabilityTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 285.0f)))
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

            if (ImGui::BeginTabItem("Overlay"))
            {
                LogTabEntry("Overlay");
                InitializeFeatureUiState();

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

                ImGui::Text("Actor/model bridge: %s", BridgeText(g_cachedActorBridge));
                ImGui::Text("Timescale bridge: %s", BridgeText(g_cachedTimescaleBridge));
                ImGui::Text("Feature bridge: %s", BridgeText(g_cachedFeatureBridge));
                ImGui::TextWrapped("Hook status: %s", g_cachedHookStatus);
                ImGui::TextWrapped("Last action: %s", g_lastActionStatus);

                if (ImGui::Button("Refresh Bridge Status"))
                    RefreshBridgeStatusCache();

                ImGui::SameLine();
                if (ImGui::Button("Install MinHook Hooks"))
                {
                    const int result = FrostbiteUniversal_InstallOwnedProjectHooks();
                    RefreshBridgeStatusCache();
                    SetActionStatus("Install MinHook Hooks", result);
                }

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
                    const int result = FrostbiteUniversal_ApplyFeatureState();
                    SetActionStatus("Enable All Visuals", result);
                }

                ImGui::SameLine();
                if (ImGui::Button("Apply Features"))
                {
                    g_featureState.timescale = g_timescaleSlider;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    const int result = FrostbiteUniversal_ApplyFeatureState();
                    SetActionStatus("Apply Features", result);
                }

                ImGui::Separator();

                flagCheckbox("Timescale override", FrostbiteFeature_Timescale);
                ImGui::SliderFloat("Timescale", &g_timescaleSlider, 0.01f, 5.0f, "%.2f");
                ImGui::SameLine();
                if (ImGui::Button("Apply"))
                {
                    g_featureState.timescale = g_timescaleSlider;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    const int result = FrostbiteUniversal_SetTimescale(g_timescaleSlider);
                    SetActionStatus("Apply Timescale", result);
                }

                ImGui::SameLine();
                if (ImGui::Button("Reset 1.0x"))
                {
                    g_timescaleSlider = 1.0f;
                    g_featureState.timescale = g_timescaleSlider;
                    FrostbiteUniversal_SetFeatureState(&g_featureState);
                    const int result = FrostbiteUniversal_SetTimescale(g_timescaleSlider);
                    SetActionStatus("Reset Timescale", result);
                }

                ImGui::SameLine();
                if (ImGui::Button("Sync"))
                {
                    if (FrostbiteUniversal_SyncTimescaleFromHost())
                    {
                        g_timescaleSlider = FrostbiteUniversal_GetTimescale();
                        g_featureState.timescale = g_timescaleSlider;
                        SetActionStatus("Sync: read timescale from connected bridge");
                    }
                    else
                    {
                        SetActionStatus("Sync: local-only, no connected timescale getter");
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
                    const int count = RefreshActorModelCacheNow();
                    if (count > 0 && SetViewTargetFromFirstActor(g_featureState))
                    {
                        FrostbiteUniversal_SetFeatureState(&g_featureState);
                        const int result = FrostbiteUniversal_ApplyFeatureState();
                        SetActionStatus("Target First Model", result);
                    }
                    else
                    {
                        SetActionStatus("Target First Model: no actor/model rows available");
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

                bool autoRefresh = g_liveActorAutoRefresh.load(std::memory_order_relaxed);
                if (ImGui::Checkbox("Auto-refresh cached actors/models", &autoRefresh))
                    g_liveActorAutoRefresh.store(autoRefresh, std::memory_order_relaxed);

                ImGui::SameLine();
                int refreshInterval = ClampLiveActorRefreshInterval(g_liveActorRefreshIntervalMs.load(std::memory_order_relaxed));
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::SliderInt("Refresh interval ms", &refreshInterval, kMinLiveActorRefreshMs, kMaxLiveActorRefreshMs))
                    g_liveActorRefreshIntervalMs.store(ClampLiveActorRefreshInterval(refreshInterval), std::memory_order_relaxed);
                ImGui::Text("Live refresh: %d entries, %lu ms old, %llu passes",
                    g_lastLiveActorRefreshCount.load(std::memory_order_relaxed),
                    static_cast<unsigned long>(LiveActorRefreshAgeMs()),
                    static_cast<unsigned long long>(g_liveActorRefreshPasses.load(std::memory_order_relaxed)));

                if (ImGui::Button("Refresh Actors/Models"))
                {
                    const int count = RefreshActorModelCacheNow();
                    g_showLiveActorModelTable = true;
                    char status[256] = {};
                    sprintf_s(status, "Refresh Actors/Models: %d entries%s", count, g_cachedActorBridge == 1 ? " from bridge/local list" : " local-only, no connected actor bridge");
                    SetActionStatus(status);
                }

                ImGui::SameLine();
                if (ImGui::Button("Clear Local List"))
                {
                    FrostbiteUniversal_ClearActorModelList();
                    g_showLiveActorModelTable = false;
                }

                ImGui::SameLine();
                if (ImGui::Button(g_showLiveActorModelTable ? "Hide List" : "Show List"))
                    g_showLiveActorModelTable = !g_showLiveActorModelTable;

                const std::uint32_t actorModelCount = FrostbiteUniversal_GetActorModelCount();
                ImGui::Text("Live actor/model entries: %u", actorModelCount);

                if (g_showLiveActorModelTable)
                {
                    ImGui::InputText("Actor/model filter", g_actorModelFilter, static_cast<int>(std::size(g_actorModelFilter)));

                    if (ImGui::BeginTable("AutoOverlayLiveActorModelTable", 10, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 210.0f)))
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
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Modules"))
            {
                ImGui::InputText("Filter", g_moduleFilter, static_cast<int>(std::size(g_moduleFilter)));

                if (ImGui::BeginTable("AutoOverlayModuleTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 375.0f)))
                {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Base");
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

                        if (!TextMatchesFilter(name, g_moduleFilter) && !TextMatchesFilter(path, g_moduleFilter) && !TextMatchesFilter(flags, g_moduleFilter))
                            continue;

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("0x%p", reinterpret_cast<void*>(module.baseAddress));
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextWrapped("%s", flags.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextWrapped("%s", path.c_str());
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

                ImGui::TextWrapped("Safe diagnostic catalog: export names plus model-like files from the injected app folder. No actor memory walking.");

                if (ImGui::BeginTable("AutoOverlayCatalogTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 330.0f)))
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

            if (ImGui::BeginTabItem("Exports"))
            {
                const std::uint32_t exportCount = FrostbiteUniversal_GetExportCount();
                ImGui::Text("Named exports cataloged: %u", exportCount);
                ImGui::InputText("Export filter", g_exportFilter, static_cast<int>(std::size(g_exportFilter)));

                if (ImGui::Button("Write Export Report"))
                    FrostbiteUniversal_WriteExportReport(nullptr);

                ImGui::SameLine();
                if (ImGui::Button("Run Validation"))
                {
                    g_lastValidationResult = FrostbiteUniversal_RunUniversalValidation();
                    g_validationFlashFrames = 240;
                }

                if (ImGui::BeginTable("AutoOverlayExportTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 335.0f)))
                {
                    ImGui::TableSetupColumn("Module");
                    ImGui::TableSetupColumn("Export");
                    ImGui::TableSetupColumn("Address");
                    ImGui::TableSetupColumn("Ordinal");
                    ImGui::TableSetupColumn("Flags");
                    ImGui::TableHeadersRow();

                    for (std::uint32_t index = 0; index < exportCount; ++index)
                    {
                        FrostbiteExportInfo exportInfo = {};
                        if (!FrostbiteUniversal_GetExportInfo(index, &exportInfo))
                            continue;

                        const std::string moduleName = WideToUtf8(exportInfo.moduleName);
                        const std::string exportName = exportInfo.name;
                        const std::string flags = (exportInfo.flags & FrostbiteExport_Forwarded) ? "forwarded" : "none";

                        if (!TextMatchesFilter(moduleName, g_exportFilter) &&
                            !TextMatchesFilter(exportName, g_exportFilter) &&
                            !TextMatchesFilter(flags, g_exportFilter))
                        {
                            continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(moduleName.c_str());
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(exportName.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("0x%p", reinterpret_cast<void*>(exportInfo.address));
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%u", exportInfo.ordinal);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(flags.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Console"))
            {
                LogTabEntry("Console");
                DrawConsolePanel();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("SDK Symbols"))
            {
                LogTabEntry("SDK Symbols");
                DrawGeneratedSdkSymbolPanel();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings"))
            {
                LogTabEntry("Settings");
                ImGui::TextWrapped("This overlay owns its own DX11 render loop, so it can show even when no game swapchain render callback is wired yet.");
                ImGui::TextWrapped("For true in-backbuffer rendering, keep using FrostbiteUniversal_ImGuiRenderDx11 or FrostbiteUniversal_ImGuiRenderDx12 from a present/render path.");
                ImGui::TextWrapped("The export catalog lists named PE exports only. It does not include stripped private/internal functions and does not toggle game or engine functions.");

                if (ImGui::Button("Hide Overlay"))
                    FrostbiteUniversal_ImGuiSetVisible(0);

                ImGui::SameLine();
                if (ImGui::Button("Copy Log Path"))
                {
                    wchar_t path[MAX_PATH] = {};
                    FrostbiteUniversal_GetLogPath(path, static_cast<std::uint32_t>(std::size(path)));
                    ImGui::SetClipboardText(WideToUtf8(path).c_str());
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    bool RenderOverlayFrameGuarded(DWORD* exceptionCode, HRESULT* presentResult)
    {
        if (!exceptionCode || !presentResult)
            return false;

        *exceptionCode = 0;
        *presentResult = S_OK;

        __try
        {
            ImGui::SetCurrentContext(g_context);
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            DrawOverlay();

            ImGui::Render();
            const float clearColor[4] = { 0.055f, 0.060f, 0.070f, 1.0f };
            g_deviceContext->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
            g_deviceContext->ClearRenderTargetView(g_renderTargetView, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            *presentResult = g_swapChain->Present(1, 0);
            return SUCCEEDED(*presentResult);
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsDeviceLostResult(HRESULT hr)
    {
        return hr == DXGI_ERROR_DEVICE_REMOVED ||
               hr == DXGI_ERROR_DEVICE_RESET ||
               hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ||
               hr == DXGI_ERROR_DEVICE_HUNG;
    }

    bool RecoverRenderDeviceGuarded(DWORD* exceptionCode)
    {
        if (!exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            FrostbiteUniversal::Log::Write(L"Self-hosted overlay attempting D3D11 device recovery");
            g_forceWarpDevice = true;
            ImGui::SetCurrentContext(g_context);
            ImGui_ImplDX11_Shutdown();
            CleanupDevice();

            if (!CreateDevice(g_overlayWindow))
                return false;

            ImGui_ImplDX11_Init(g_device, g_deviceContext);
            FrostbiteUniversal::Log::Write(L"Self-hosted overlay D3D11 device recovery completed");
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void EnterNativeFallbackMode(const wchar_t* reason)
    {
        if (g_nativeFallbackMode)
            return;

        std::wstring message = L"Self-hosted overlay switching to native fallback menu";
        if (reason && reason[0] != L'\0')
        {
            message += L": ";
            message += reason;
        }
        FrostbiteUniversal::Log::Write(message);

        if (g_context)
        {
            ImGui::SetCurrentContext(g_context);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(g_context);
            g_context = nullptr;
        }

        CleanupDevice();
        g_nativeFallbackMode = true;
        if (g_overlayWindow)
        {
            ::SetWindowTextW(g_overlayWindow, L"Frostbite Universal Native Fallback");
            ::InvalidateRect(g_overlayWindow, nullptr, TRUE);
        }
    }

    bool ShowOverlayWindowGuarded(bool visible, DWORD* exceptionCode)
    {
        if (!exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            ::ShowWindow(g_overlayWindow, visible ? SW_SHOWNA : SW_HIDE);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool PositionOverlayGuarded(bool visible, DWORD* exceptionCode)
    {
        if (!exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            PositionOverlay(visible);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (g_nativeFallbackMode)
        {
            switch (msg)
            {
            case WM_PAINT:
                DrawNativeFallbackPanel(hwnd);
                return 0;

            case WM_LBUTTONUP:
                HandleNativeFallbackClick(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;

            default:
                break;
            }
        }

        if (g_context)
        {
            DWORD exceptionCode = 0;
            __try
            {
                ImGui::SetCurrentContext(g_context);
                if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
                    return TRUE;
            }
            __except ((exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
            {
                wchar_t message[160] = {};
                swprintf_s(message, L"Self-hosted overlay WndProc caught SEH exception 0x%08X", exceptionCode);
                FrostbiteUniversal::Log::Write(message);
            }
        }

        switch (msg)
        {
        case WM_SIZE:
            if (!g_nativeFallbackMode && g_device != nullptr && g_swapChain != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_CLOSE:
            FrostbiteUniversal_ImGuiSetVisible(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    DWORD WINAPI OverlayThreadBody(void*)
    {
        FrostbiteUniversal::Log::Write(L"Self-hosted overlay thread entered");

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = OverlayWndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = kOverlayClassName;
        ::RegisterClassExW(&wc);

        RECT rect = GetOverlayRect();
        g_overlayWindow = ::CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kOverlayClassName,
            L"Frostbite Universal Overlay",
            WS_POPUP,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            wc.hInstance,
            nullptr);

        if (!g_overlayWindow)
        {
            FrostbiteUniversal::Log::Write(L"Self-hosted overlay failed: CreateWindowExW failed");
            ::UnregisterClassW(kOverlayClassName, wc.hInstance);
            g_running = false;
            return 0;
        }

        if (!CreateDevice(g_overlayWindow))
        {
            g_nativeFallbackMode = true;
            ::SetWindowTextW(g_overlayWindow, L"Frostbite Universal Native Fallback");
            FrostbiteUniversal::Log::Write(L"Self-hosted overlay starting in native fallback menu because D3D device creation failed");
        }
        else
        {
            g_nativeFallbackMode = false;
            g_context = ImGui::CreateContext();
            ImGui::SetCurrentContext(g_context);
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            ApplyFrostbiteStyle();
            ImGui_ImplWin32_Init(g_overlayWindow);
            ImGui_ImplDX11_Init(g_device, g_deviceContext);
        }

        g_lastVisible = FrostbiteUniversal_ImGuiIsVisible() != 0;
        DWORD showException = 0;
        if (!ShowOverlayWindowGuarded(g_lastVisible, &showException))
        {
            wchar_t message[160] = {};
            swprintf_s(message, L"Self-hosted overlay initial ShowWindow caught SEH exception 0x%08X", showException);
            FrostbiteUniversal::Log::Write(message);
        }
        ::UpdateWindow(g_overlayWindow);
        DWORD positionException = 0;
        if (!PositionOverlayGuarded(g_lastVisible, &positionException))
        {
            wchar_t message[160] = {};
            swprintf_s(message, L"Self-hosted overlay initial position caught SEH exception 0x%08X", positionException);
            FrostbiteUniversal::Log::Write(message);
        }
        RefreshBridgeStatusCache();
        FrostbiteUniversal::Log::Write(L"Self-hosted overlay window created");

        DWORD lastPositionTick = 0;
        DWORD lastShowTick = 0;
        DWORD lastRecoveryTick = 0;
        int consecutivePresentFailures = 0;
        MSG msg = {};
        while (::WaitForSingleObject(g_overlayStopEvent, 0) == WAIT_TIMEOUT)
        {
            const DWORD frameStart = ::GetTickCount();
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }

            const bool visible = FrostbiteUniversal_ImGuiIsVisible() != 0;
            const DWORD now = ::GetTickCount();
            if (visible != g_lastVisible)
            {
                g_lastVisible = visible;
                DWORD visibilityException = 0;
                if (!ShowOverlayWindowGuarded(visible, &visibilityException))
                {
                    wchar_t message[160] = {};
                    swprintf_s(message, L"Self-hosted overlay ShowWindow caught SEH exception 0x%08X", visibilityException);
                    FrostbiteUniversal::Log::Write(message);
                    break;
                }

                lastShowTick = now;
                FrostbiteUniversal::Log::Write(visible
                    ? L"Self-hosted overlay shown"
                    : L"Self-hosted overlay hidden");
            }

            if (visible && (lastPositionTick == 0 || now - lastPositionTick > kOverlayRepositionMs))
            {
                DWORD repositionException = 0;
                if (!PositionOverlayGuarded(true, &repositionException))
                {
                    wchar_t message[160] = {};
                    swprintf_s(message, L"Self-hosted overlay position caught SEH exception 0x%08X", repositionException);
                    FrostbiteUniversal::Log::Write(message);
                    break;
                }

                lastPositionTick = now;
            }

            if (!visible)
            {
                ::Sleep(kOverlayHiddenSleepMs);
                continue;
            }

            if (lastShowTick != 0 && now - lastShowTick < kOverlayShowCooldownMs)
            {
                ::Sleep(kOverlayShowCooldownMs - (now - lastShowTick));
                continue;
            }

            if (g_nativeFallbackMode)
            {
                ::InvalidateRect(g_overlayWindow, nullptr, FALSE);
                ::UpdateWindow(g_overlayWindow);
                ::Sleep(33);
                continue;
            }

            DWORD renderException = 0;
            HRESULT presentResult = S_OK;
            if (!RenderOverlayFrameGuarded(&renderException, &presentResult))
            {
                std::wstringstream failure;
                if (renderException != 0)
                {
                    failure << L"Self-hosted overlay render loop caught SEH exception 0x"
                            << std::hex << renderException
                            << L"; stopping overlay thread";
                }
                else
                {
                    failure << L"Self-hosted overlay Present failed, HRESULT=0x"
                            << std::hex << presentResult
                            << L"; attempting recovery";
                }

                FrostbiteUniversal::Log::Write(failure.str());

                ++consecutivePresentFailures;
                const bool canRecover =
                    renderException == 0 &&
                    IsDeviceLostResult(presentResult) &&
                    consecutivePresentFailures <= 5 &&
                    (lastRecoveryTick == 0 || now - lastRecoveryTick >= 250);

                if (canRecover)
                {
                    DWORD recoveryException = 0;
                    lastRecoveryTick = now;
                    if (RecoverRenderDeviceGuarded(&recoveryException))
                    {
                        ::Sleep(50);
                        continue;
                    }

                    std::wstringstream recoveryFailure;
                    if (recoveryException != 0)
                    {
                        recoveryFailure << L"Self-hosted overlay device recovery caught SEH exception 0x"
                                        << std::hex << recoveryException;
                    }
                    else
                    {
                        recoveryFailure << L"Self-hosted overlay device recovery failed";
                    }
                    FrostbiteUniversal::Log::Write(recoveryFailure.str());
                }

                if (consecutivePresentFailures > 5)
                    FrostbiteUniversal::Log::Write(L"Self-hosted overlay exceeded D3D recovery limit; entering native fallback menu");
                else if (renderException != 0)
                    FrostbiteUniversal::Log::Write(L"Self-hosted overlay cannot recover from render SEH exception; entering native fallback menu");
                else
                    FrostbiteUniversal::Log::Write(L"Self-hosted overlay Present failure was not recoverable; entering native fallback menu");
                EnterNativeFallbackMode(L"D3D Present/device recovery failed");
                consecutivePresentFailures = 0;
                ::Sleep(50);
                continue;
            }

            consecutivePresentFailures = 0;
            ++g_overlayFrameCount;

            if (g_overlayFrameCount == 1 || (g_overlayFrameCount % 300) == 0)
            {
                std::wstringstream heartbeat;
                heartbeat << L"Self-hosted overlay heartbeat: frames=" << g_overlayFrameCount
                          << L", targetWindow=0x" << std::hex << reinterpret_cast<std::uintptr_t>(g_targetWindow);
                FrostbiteUniversal::Log::Write(heartbeat.str());
            }

            const DWORD frameElapsed = ::GetTickCount() - frameStart;
            if (frameElapsed < kOverlayVisibleFrameMs)
                ::Sleep(kOverlayVisibleFrameMs - frameElapsed);
        }

        if (g_context)
        {
            ImGui::SetCurrentContext(g_context);
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext(g_context);
            g_context = nullptr;
        }

        CleanupDevice();
        ::DestroyWindow(g_overlayWindow);
        g_overlayWindow = nullptr;
        ::UnregisterClassW(kOverlayClassName, wc.hInstance);

        FrostbiteUniversal::Log::Write(L"Self-hosted overlay thread leaving");
        g_running = false;
        return 0;
    }

    DWORD OverlayThreadWithCppGuards(void* parameter)
    {
        try
        {
            return OverlayThreadBody(parameter);
        }
        catch (const std::exception& ex)
        {
            std::wstring message = L"Self-hosted overlay thread caught C++ exception: ";
            message += Utf8ToWide(ex.what());
            FrostbiteUniversal::Log::Write(message);
            g_running = false;
            return 0;
        }
        catch (...)
        {
            FrostbiteUniversal::Log::Write(L"Self-hosted overlay thread caught unknown C++ exception");
            g_running = false;
            return 0;
        }
    }

    DWORD WINAPI OverlayThreadProc(void* parameter)
    {
        DWORD exceptionCode = 0;
        __try
        {
            return OverlayThreadWithCppGuards(parameter);
        }
        __except ((exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            wchar_t message[160] = {};
            swprintf_s(message, L"Self-hosted overlay thread caught SEH exception 0x%08X", exceptionCode);
            FrostbiteUniversal::Log::Write(message);
            g_running = false;
            return 0;
        }
    }
}
#endif

namespace FrostbiteUniversal::OverlayHost
{
    bool Start()
    {
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
        std::lock_guard lock(g_overlayMutex);
        if (g_overlayThread)
            return true;

        g_overlayStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_overlayStopEvent)
        {
            Log::Write(L"Self-hosted overlay failed: CreateEventW failed");
            return false;
        }

        g_running = true;
        g_overlayThread = ::CreateThread(nullptr, 0, OverlayThreadProc, nullptr, 0, &g_overlayThreadId);
        if (!g_overlayThread)
        {
            Log::Write(L"Self-hosted overlay failed: CreateThread failed");
            ::CloseHandle(g_overlayStopEvent);
            g_overlayStopEvent = nullptr;
            g_running = false;
            return false;
        }

        g_actorRefreshThread = ::CreateThread(nullptr, 0, ActorModelRefreshThreadProc, g_overlayStopEvent, 0, &g_actorRefreshThreadId);
        if (!g_actorRefreshThread)
        {
            g_actorRefreshThreadId = 0;
            Log::Write(L"Self-hosted overlay actor/model cache refresh thread was not started");
        }

        Log::Write(L"Self-hosted overlay started");
        Log::Write(L"Self-hosted overlay render path is cache-only for actor/model drawing");
        return true;
#else
        return false;
#endif
    }

    void Stop()
    {
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
        HANDLE thread = nullptr;
        HANDLE actorRefreshThread = nullptr;
        HANDLE stopEvent = nullptr;

        {
            std::lock_guard lock(g_overlayMutex);
            thread = g_overlayThread;
            actorRefreshThread = g_actorRefreshThread;
            stopEvent = g_overlayStopEvent;
            g_overlayThread = nullptr;
            g_actorRefreshThread = nullptr;
            g_overlayStopEvent = nullptr;
            g_overlayThreadId = 0;
            g_actorRefreshThreadId = 0;
        }

        if (stopEvent)
            ::SetEvent(stopEvent);

        if (actorRefreshThread)
        {
            ::WaitForSingleObject(actorRefreshThread, 2000);
            ::CloseHandle(actorRefreshThread);
        }

        if (thread)
        {
            ::WaitForSingleObject(thread, 2000);
            ::CloseHandle(thread);
        }

        if (stopEvent)
            ::CloseHandle(stopEvent);

        Log::Write(L"Self-hosted overlay stopped");
#endif
    }

    bool IsRunning()
    {
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
        return g_running;
#else
        return false;
#endif
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_OverlayStart()
{
    return FrostbiteUniversal::OverlayHost::Start() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_OverlayStop()
{
    FrostbiteUniversal::OverlayHost::Stop();
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_OverlayIsRunning()
{
    return FrostbiteUniversal::OverlayHost::IsRunning() ? 1 : 0;
}
