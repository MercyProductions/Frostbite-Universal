#include "OverlayHost.h"

#include "FrostbiteLog.h"
#include "FrostbiteUniversal.h"
#include "SharedImGuiBridge.h"

#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
#include <d3d11.h>
#include <dxgi.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr const wchar_t* kOverlayClassName = L"FrostbiteUniversalSelfHostedOverlay";
    constexpr int kDefaultWidth = 760;
    constexpr int kDefaultHeight = 540;
    constexpr DWORD kOverlayVisibleFrameMs = 66;
    constexpr DWORD kOverlayHiddenSleepMs = 50;
    constexpr DWORD kOverlayRepositionMs = 5000;
    constexpr DWORD kOverlayShowCooldownMs = 150;

    std::mutex g_overlayMutex;
    HANDLE g_overlayThread = nullptr;
    HANDLE g_overlayStopEvent = nullptr;
    DWORD g_overlayThreadId = 0;
    HWND g_overlayWindow = nullptr;
    HWND g_targetWindow = nullptr;
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_deviceContext = nullptr;
    IDXGISwapChain* g_swapChain = nullptr;
    ID3D11RenderTargetView* g_renderTargetView = nullptr;
    ImGuiContext* g_context = nullptr;
    bool g_running = false;
    bool g_lastVisible = true;
    std::uint64_t g_overlayFrameCount = 0;
    char g_moduleFilter[128] = {};
    char g_exportFilter[128] = {};
    char g_catalogFilter[128] = {};
    char g_actorModelFilter[128] = {};
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

        const std::uint32_t count = FrostbiteUniversal_GetActorModelCount();
        for (std::uint32_t index = 0; index < count; ++index)
        {
            FrostbiteActorModelInfo item = {};
            if (!FrostbiteUniversal_GetActorModelInfo(index, &item))
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

    bool CreateDevice(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 2;
        desc.BufferDesc.Width = 0;
        desc.BufferDesc.Height = 0;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
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
            D3D_DRIVER_TYPE_HARDWARE,
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

        if (FAILED(hr))
        {
            std::wstringstream message;
            message << L"Self-hosted overlay failed to create D3D11 device, HRESULT=0x" << std::hex << hr;
            FrostbiteUniversal::Log::Write(message.str());
            return false;
        }

        CreateRenderTarget();
        FrostbiteUniversal::Log::Write(L"Self-hosted overlay D3D11 device created");
        return true;
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
        ImGui::Begin("Frostbite Universal Auto Overlay", nullptr, ImGuiWindowFlags_NoCollapse);

        ImGui::TextUnformatted("Self-hosted overlay");
        ImGui::SameLine();
        ImGui::TextDisabled("F4 toggles visibility");

        if (ImGui::BeginTabBar("AutoOverlayTabs"))
        {
            if (ImGui::BeginTabItem("Status"))
            {
                LogTabEntry("Status");
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

            if (ImGui::BeginTabItem("Project"))
            {
                LogTabEntry("Project");
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
                flagCheckbox("FOV override", FrostbiteFeature_FovOverride);
                ImGui::SliderFloat("FOV degrees", &g_featureState.fovDegrees, 30.0f, 140.0f, "%.1f");
                flagCheckbox("View-angle preview", FrostbiteFeature_ViewAnglePreview);
                ImGui::SameLine();
                if (ImGui::Button("Target First Model"))
                {
                    const int count = FrostbiteUniversal_RefreshActorModelList();
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

                if (ImGui::Button("Refresh Actors/Models"))
                {
                    const int count = FrostbiteUniversal_RefreshActorModelList();
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

                    if (ImGui::BeginTable("AutoOverlayLiveActorModelTable", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 210.0f)))
                    {
                        ImGui::TableSetupColumn("ID");
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
                            ImGui::TextWrapped("%s", actorName.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextWrapped("%s", className.c_str());
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextWrapped("%s", modelName.c_str());
                            ImGui::TableSetColumnIndex(4);
                            ImGui::Text("%.2f, %.2f, %.2f", item.position[0], item.position[1], item.position[2]);
                            ImGui::TableSetColumnIndex(5);
                            ImGui::Text("%.2f, %.2f, %.2f", item.size[0], item.size[1], item.size[2]);
                            ImGui::TableSetColumnIndex(6);
                            ImGui::Text("%.2f", item.radius);
                            ImGui::TableSetColumnIndex(7);
                            ImGui::TextWrapped("%s", flags.c_str());
                            ImGui::TableSetColumnIndex(8);
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

            if (ImGui::BeginTabItem("Controls"))
            {
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
            if (g_device != nullptr && wParam != SIZE_MINIMIZED)
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

    DWORD WINAPI OverlayThreadProc(void*)
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
            ::DestroyWindow(g_overlayWindow);
            g_overlayWindow = nullptr;
            ::UnregisterClassW(kOverlayClassName, wc.hInstance);
            g_running = false;
            return 0;
        }

        g_context = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_context);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_overlayWindow);
        ImGui_ImplDX11_Init(g_device, g_deviceContext);

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
                            << L"; stopping overlay thread";
                }

                FrostbiteUniversal::Log::Write(failure.str());
                break;
            }

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

        ImGui::SetCurrentContext(g_context);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_context);
        g_context = nullptr;

        CleanupDevice();
        ::DestroyWindow(g_overlayWindow);
        g_overlayWindow = nullptr;
        ::UnregisterClassW(kOverlayClassName, wc.hInstance);

        FrostbiteUniversal::Log::Write(L"Self-hosted overlay thread leaving");
        g_running = false;
        return 0;
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

        Log::Write(L"Self-hosted overlay started");
        return true;
#else
        return false;
#endif
    }

    void Stop()
    {
#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
        HANDLE thread = nullptr;
        HANDLE stopEvent = nullptr;

        {
            std::lock_guard lock(g_overlayMutex);
            thread = g_overlayThread;
            stopEvent = g_overlayStopEvent;
            g_overlayThread = nullptr;
            g_overlayStopEvent = nullptr;
            g_overlayThreadId = 0;
        }

        if (stopEvent)
            ::SetEvent(stopEvent);

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
