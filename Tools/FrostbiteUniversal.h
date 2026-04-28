#pragma once

#include <Windows.h>
#include <cstdint>

#if defined(FROSTBITEUNIVERSAL_EXPORTS)
#define FROSTBITEUNIVERSAL_API extern "C" __declspec(dllexport)
#else
#define FROSTBITEUNIVERSAL_API extern "C" __declspec(dllimport)
#endif

enum FrostbiteModuleFlags : std::uint32_t
{
    FrostbiteModule_None = 0,
    FrostbiteModule_GameExecutable = 1u << 0,
    FrostbiteModule_EngineBuildInfo = 1u << 1,
    FrostbiteModule_RenderCore2 = 1u << 2,
    FrostbiteModule_DirectStorage = 1u << 3,
    FrostbiteModule_Oodle = 1u << 4,
    FrostbiteModule_PlatformSdk = 1u << 5,
    FrostbiteModule_AntiCheat = 1u << 6,
    FrostbiteModule_ThirdPartyRender = 1u << 7,
    FrostbiteModule_HasExports = 1u << 8
};

enum FrostbiteRuntimeFlags : std::uint32_t
{
    FrostbiteRuntime_None = 0,
    FrostbiteRuntime_IsFrostbiteProcess = 1u << 0,
    FrostbiteRuntime_HasDataDirectory = 1u << 1,
    FrostbiteRuntime_HasInitFs = 1u << 2,
    FrostbiteRuntime_HasLayoutToc = 1u << 3,
    FrostbiteRuntime_HasTocArchives = 1u << 4,
    FrostbiteRuntime_HasCasArchives = 1u << 5,
    FrostbiteRuntime_HasEngineBuildInfo = 1u << 6,
    FrostbiteRuntime_HasRenderCore2 = 1u << 7,
    FrostbiteRuntime_HasAntiCheatFiles = 1u << 8,
    FrostbiteRuntime_HasSharedImGui = 1u << 9
};

struct FrostbiteModuleInfo
{
    wchar_t name[MAX_PATH];
    wchar_t path[MAX_PATH];
    std::uintptr_t baseAddress;
    std::uint32_t imageSize;
    std::uint32_t flags;
};

struct FrostbiteRuntimeInfo
{
    wchar_t processName[MAX_PATH];
    wchar_t processPath[MAX_PATH];
    wchar_t gameRoot[MAX_PATH];
    wchar_t detectedTitle[128];
    std::uint32_t moduleCount;
    std::uint32_t frostbiteModuleCount;
    std::uint32_t tocFileCount;
    std::uint32_t casFileCount;
    std::uint32_t flags;
};

enum FrostbiteExportFlags : std::uint32_t
{
    FrostbiteExport_None = 0,
    FrostbiteExport_Forwarded = 1u << 0
};

struct FrostbiteExportInfo
{
    wchar_t moduleName[MAX_PATH];
    char name[256];
    std::uintptr_t address;
    std::uint32_t moduleIndex;
    std::uint32_t ordinal;
    std::uint32_t flags;
};

enum FrostbiteCatalogFlags : std::uint32_t
{
    FrostbiteCatalog_None = 0,
    FrostbiteCatalog_ActorSymbol = 1u << 0,
    FrostbiteCatalog_EntitySymbol = 1u << 1,
    FrostbiteCatalog_ModelSymbol = 1u << 2,
    FrostbiteCatalog_MeshSymbol = 1u << 3,
    FrostbiteCatalog_ModelAsset = 1u << 4,
    FrostbiteCatalog_MeshAsset = 1u << 5,
    FrostbiteCatalog_AnimationAsset = 1u << 6,
    FrostbiteCatalog_FromExport = 1u << 7,
    FrostbiteCatalog_FromFile = 1u << 8,
    FrostbiteCatalog_LimitReached = 1u << 9
};

struct FrostbiteCatalogInfo
{
    wchar_t name[256];
    wchar_t source[64];
    wchar_t path[MAX_PATH];
    std::uintptr_t address;
    std::uint32_t flags;
};

enum FrostbiteActorModelFlags : std::uint32_t
{
    FrostbiteActorModel_None = 0,
    FrostbiteActorModel_Actor = 1u << 0,
    FrostbiteActorModel_Model = 1u << 1,
    FrostbiteActorModel_Static = 1u << 2,
    FrostbiteActorModel_Dynamic = 1u << 3,
    FrostbiteActorModel_Visible = 1u << 4,
    FrostbiteActorModel_FromProvider = 1u << 5,
    FrostbiteActorModel_FromHostExport = 1u << 6,
    FrostbiteActorModel_FromManualAdd = 1u << 7,
    FrostbiteActorModel_HasScreenProjection = 1u << 8,
    FrostbiteActorModel_ViewTarget = 1u << 9
};

struct FrostbiteActorModelInfo
{
    std::uint64_t id;
    wchar_t actorName[128];
    wchar_t className[128];
    wchar_t modelName[128];
    wchar_t assetPath[MAX_PATH];
    float position[3];
    float rotationEuler[3];
    float scale[3];
    float boundsMin[3];
    float boundsMax[3];
    float size[3];
    float radius;
    float screenPosition[2];
    float screenBoundsMin[2];
    float screenBoundsMax[2];
    float screenDepth;
    std::uint32_t flags;
};

using FrostbiteActorModelProviderCallback = std::uint32_t(__stdcall*)(
    FrostbiteActorModelInfo* outItems,
    std::uint32_t maxItems,
    void* userData);

using FrostbiteTimescaleCallback = void(__stdcall*)(
    float timescale,
    void* userData);

enum FrostbiteUniversalFeatureFlags : std::uint32_t
{
    FrostbiteFeature_None = 0,
    FrostbiteFeature_Timescale = 1u << 0,
    FrostbiteFeature_SkyboxTint = 1u << 1,
    FrostbiteFeature_SkyboxRainbow = 1u << 2,
    FrostbiteFeature_Chams = 1u << 3,
    FrostbiteFeature_ChamsRainbow = 1u << 4,
    FrostbiteFeature_FogTint = 1u << 5,
    FrostbiteFeature_WireframeDebug = 1u << 6,
    FrostbiteFeature_DebugBoxes = 1u << 7,
    FrostbiteFeature_Snaplines = 1u << 8,
    FrostbiteFeature_FovOverride = 1u << 9,
    FrostbiteFeature_ViewAnglePreview = 1u << 10
};

struct FrostbiteUniversalFeatureState
{
    std::uint32_t size;
    std::uint32_t enabledFlags;
    float timescale;
    float skyboxColor[4];
    float skyboxIntensity;
    float skyboxRainbowSpeed;
    float chamsColor[4];
    float chamsOpacity;
    float chamsRainbowSpeed;
    float fogColor[4];
    float fogDensity;
    float fovDegrees;
    std::uint64_t viewTargetActorId;
    float viewTargetPosition[3];
    float viewAngles[3];
    std::uint32_t hasViewTarget;
};

using FrostbiteFeatureApplyCallback = int(__stdcall*)(
    const FrostbiteUniversalFeatureState* state,
    void* userData);

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_Initialize();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_Refresh();
FROSTBITEUNIVERSAL_API void FrostbiteUniversal_Shutdown();

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_IsInitialized();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_IsFrostbiteProcess();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasSharedImGui();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_OpenConsole();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetLogPath(wchar_t* outPath, std::uint32_t outPathLength);

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ImGuiSetVisible(int visible);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiIsVisible();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiStartHotkeyMonitor();
FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ImGuiStopHotkeyMonitor();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_OverlayStart();
FROSTBITEUNIVERSAL_API void FrostbiteUniversal_OverlayStop();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_OverlayIsRunning();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_RunUniversalValidation();
FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ImGuiShutdown();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiRenderDx11(HWND hwnd, void* d3d11Device, void* d3d11DeviceContext, void* renderTargetView);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ImGuiRenderDx12(
    HWND hwnd,
    void* d3d12Device,
    void* graphicsCommandList,
    int framesInFlight,
    int rtvFormat,
    void* srvDescriptorHeap,
    std::uintptr_t fontSrvCpuDescriptor,
    std::uint64_t fontSrvGpuDescriptor);
FROSTBITEUNIVERSAL_API LRESULT FrostbiteUniversal_ImGuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetRuntimeInfo(FrostbiteRuntimeInfo* outInfo);
FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetModuleCount();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetModuleInfo(std::uint32_t index, FrostbiteModuleInfo* outInfo);
FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetExportCount();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetExportInfo(std::uint32_t index, FrostbiteExportInfo* outInfo);
FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetCatalogCount();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetCatalogInfo(std::uint32_t index, FrostbiteCatalogInfo* outInfo);

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ClearActorModelList();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_AddActorModelInfo(const FrostbiteActorModelInfo* info);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetActorModelProvider(FrostbiteActorModelProviderCallback callback, void* userData);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_RefreshActorModelList();
FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetActorModelCount();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetActorModelInfo(std::uint32_t index, FrostbiteActorModelInfo* outInfo);

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetTimescaleCallback(FrostbiteTimescaleCallback callback, void* userData);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetTimescale(float timescale);
FROSTBITEUNIVERSAL_API float FrostbiteUniversal_GetTimescale();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SyncTimescaleFromHost();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasTimescaleBridge();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasActorModelBridge();

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetFeatureApplyCallback(FrostbiteFeatureApplyCallback callback, void* userData);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetFeatureState(const FrostbiteUniversalFeatureState* state);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetFeatureState(FrostbiteUniversalFeatureState* outState);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ApplyFeatureState();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasFeatureBridge();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_InstallOwnedProjectHooks();
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetOwnedProjectHookStatus(wchar_t* outStatus, std::uint32_t outStatusLength);

// If moduleName is null, empty, or "*", every loaded module is searched.
FROSTBITEUNIVERSAL_API void* FrostbiteUniversal_GetExport(const wchar_t* moduleName, const char* exportName);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteRuntimeReport(const wchar_t* reportPath);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteExportReport(const wchar_t* reportPath);
FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteCatalogReport(const wchar_t* reportPath);
