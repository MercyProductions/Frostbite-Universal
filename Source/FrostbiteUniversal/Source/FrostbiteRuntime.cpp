#include "FrostbiteRuntime.h"
#include "FrostbiteLog.h"

#include "SharedImGuiBridge.h"

#include <MinHook.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
    }

    std::string WideToUtf8(const std::wstring& value)
    {
        if (value.empty())
            return {};

        const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
            return {};

        std::string out(static_cast<std::size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};

        const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0)
            return {};

        std::wstring out(static_cast<std::size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
        return out;
    }

    bool StartsWith(const std::wstring& value, const wchar_t* prefix)
    {
        return value.rfind(prefix, 0) == 0;
    }

    bool EndsWith(const std::wstring& value, const wchar_t* suffix)
    {
        const std::wstring suffixValue = suffix;
        return value.size() >= suffixValue.size() &&
               value.compare(value.size() - suffixValue.size(), suffixValue.size(), suffixValue) == 0;
    }

    std::wstring FileNameFromPath(const std::wstring& path)
    {
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    std::wstring DirectoryFromPath(const std::wstring& path)
    {
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
    }

    template <std::size_t N>
    void CopyWide(wchar_t (&dest)[N], const std::wstring& value)
    {
        static_assert(N > 0);
        wcsncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    template <std::size_t N>
    void CopyNarrow(char (&dest)[N], const std::string& value)
    {
        static_assert(N > 0);
        strncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    std::wstring GetCurrentProcessPath()
    {
        wchar_t path[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    bool HasAnyExport(HMODULE module)
    {
        if (!module)
            return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        return nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress != 0;
    }

    std::uint32_t ClassifyModule(const std::wstring& name, HMODULE module)
    {
        const std::wstring lowerName = ToLower(name);
        std::uint32_t flags = FrostbiteModule_None;

        if (EndsWith(lowerName, L".exe") &&
            lowerName != L"eaanticheat.gameservicelauncher.exe" &&
            lowerName.find(L"installer") == std::wstring::npos)
        {
            flags |= FrostbiteModule_GameExecutable;
        }

        if (lowerName == L"engine.buildinfo.dll")
            flags |= FrostbiteModule_EngineBuildInfo;

        if ((lowerName.find(L"engine.render") != std::wstring::npos &&
             (lowerName.find(L"dx11") != std::wstring::npos ||
              lowerName.find(L"dx12") != std::wstring::npos ||
              lowerName.find(L"pcdx") != std::wstring::npos)) ||
            lowerName == L"renderdevice.dll" ||
            lowerName == L"renderdevice_dx11.dll" ||
            lowerName == L"renderdevice_dx12.dll")
        {
            flags |= FrostbiteModule_RenderCore2;
        }

        if (lowerName == L"dstorage.dll" || lowerName == L"dstoragecore.dll")
            flags |= FrostbiteModule_DirectStorage;

        if (StartsWith(lowerName, L"oo2") && EndsWith(lowerName, L".dll"))
            flags |= FrostbiteModule_Oodle;

        if (StartsWith(lowerName, L"steam_api") ||
            StartsWith(lowerName, L"sl.") ||
            StartsWith(lowerName, L"nvngx_") ||
            StartsWith(lowerName, L"libxess") ||
            StartsWith(lowerName, L"amd_ags") ||
            lowerName == L"gfsdk_aftermath_lib.x64.dll")
        {
            flags |= FrostbiteModule_PlatformSdk | FrostbiteModule_ThirdPartyRender;
        }

        if (lowerName.find(L"eaanticheat") != std::wstring::npos ||
            lowerName.find(L"javelin") != std::wstring::npos)
        {
            flags |= FrostbiteModule_AntiCheat;
        }

        if (HasAnyExport(module))
            flags |= FrostbiteModule_HasExports;

        return flags;
    }

    std::vector<FrostbiteUniversal::ModuleRecord> EnumerateProcessModules()
    {
        std::vector<FrostbiteUniversal::ModuleRecord> modules;
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, ::GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE)
            return modules;

        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);

        if (::Module32FirstW(snapshot, &entry))
        {
            do
            {
                FrostbiteUniversal::ModuleRecord record;
                record.name = entry.szModule;
                record.path = entry.szExePath;
                record.handle = entry.hModule;
                record.baseAddress = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                record.imageSize = entry.modBaseSize;
                record.flags = ClassifyModule(record.name, record.handle);
                modules.emplace_back(std::move(record));
            } while (::Module32NextW(snapshot, &entry));
        }

        ::CloseHandle(snapshot);

        std::sort(modules.begin(), modules.end(), [](const auto& lhs, const auto& rhs) {
            return ToLower(lhs.name) < ToLower(rhs.name);
        });

        return modules;
    }

    bool ModuleMatches(const FrostbiteUniversal::ModuleRecord& module, const wchar_t* moduleName)
    {
        if (!moduleName || moduleName[0] == L'\0' || wcscmp(moduleName, L"*") == 0)
            return true;

        const std::wstring wanted = ToLower(moduleName);
        const std::wstring moduleBaseName = ToLower(module.name);
        const std::wstring modulePath = ToLower(module.path);

        return moduleBaseName == wanted ||
               modulePath == wanted ||
               moduleBaseName.find(wanted) != std::wstring::npos ||
               modulePath.find(wanted) != std::wstring::npos;
    }

    std::wstring GuessTitleFromProcessName(const std::wstring& processName)
    {
        std::wstring title = processName;
        const std::wstring lower = ToLower(title);
        if (EndsWith(lower, L".exe"))
            title.resize(title.size() - 4);
        return title.empty() ? L"unknown" : title;
    }

    std::wstring DefaultReportPath()
    {
        wchar_t tempPath[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, tempPath) == 0)
            return L"FrostbiteUniversal_Report.txt";

        std::wstring path = tempPath;
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');

        path += L"FrostbiteUniversal_Report.txt";
        return path;
    }

    std::wstring DefaultExportReportPath()
    {
        wchar_t tempPath[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, tempPath) == 0)
            return L"FrostbiteUniversal_ExportReport.md";

        std::wstring path = tempPath;
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');

        path += L"FrostbiteUniversal_ExportReport.md";
        return path;
    }

    std::wstring DefaultCatalogReportPath()
    {
        wchar_t tempPath[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, tempPath) == 0)
            return L"FrostbiteUniversal_ActorModelCatalog.md";

        std::wstring path = tempPath;
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');

        path += L"FrostbiteUniversal_ActorModelCatalog.md";
        return path;
    }

    std::uint32_t CatalogFlagsFromText(const std::wstring& lowerText, bool fromExport)
    {
        std::uint32_t flags = fromExport ? FrostbiteCatalog_FromExport : FrostbiteCatalog_FromFile;

        if (lowerText.find(L"actor") != std::wstring::npos)
            flags |= FrostbiteCatalog_ActorSymbol;

        if (lowerText.find(L"entity") != std::wstring::npos)
            flags |= FrostbiteCatalog_EntitySymbol;

        if (lowerText.find(L"model") != std::wstring::npos)
            flags |= fromExport ? FrostbiteCatalog_ModelSymbol : FrostbiteCatalog_ModelAsset;

        if (lowerText.find(L"mesh") != std::wstring::npos ||
            lowerText.find(L"geometry") != std::wstring::npos)
        {
            flags |= fromExport ? FrostbiteCatalog_MeshSymbol : FrostbiteCatalog_MeshAsset;
        }

        if (!fromExport &&
            (lowerText.find(L"anim") != std::wstring::npos ||
             lowerText.find(L"skeleton") != std::wstring::npos ||
             lowerText.find(L"rig") != std::wstring::npos))
        {
            flags |= FrostbiteCatalog_AnimationAsset;
        }

        return flags;
    }

    bool HasCatalogMeaning(std::uint32_t flags)
    {
        const std::uint32_t interesting =
            FrostbiteCatalog_ActorSymbol |
            FrostbiteCatalog_EntitySymbol |
            FrostbiteCatalog_ModelSymbol |
            FrostbiteCatalog_MeshSymbol |
            FrostbiteCatalog_ModelAsset |
            FrostbiteCatalog_MeshAsset |
            FrostbiteCatalog_AnimationAsset;
        return (flags & interesting) != 0;
    }

    bool IsCatalogAssetExtension(const std::wstring& lowerExtension)
    {
        return lowerExtension == L".fbx" ||
               lowerExtension == L".obj" ||
               lowerExtension == L".mesh" ||
               lowerExtension == L".model" ||
               lowerExtension == L".skinnedmesh" ||
               lowerExtension == L".skeleton" ||
               lowerExtension == L".animation" ||
               lowerExtension == L".anim" ||
               lowerExtension == L".ebx";
    }

    std::wstring RelativeOrFullPath(const fs::path& path, const fs::path& root)
    {
        std::error_code ec;
        const fs::path relative = fs::relative(path, root, ec);
        return ec ? path.wstring() : relative.wstring();
    }

    bool CatalogRecordExists(const std::vector<FrostbiteUniversal::CatalogRecord>& records, const std::wstring& source, const std::wstring& path, const std::wstring& name)
    {
        for (const FrostbiteUniversal::CatalogRecord& record : records)
        {
            if (record.source == source && record.path == path && record.name == name)
                return true;
        }
        return false;
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
        append(FrostbiteCatalog_FromExport, "from-export", flags);
        append(FrostbiteCatalog_FromFile, "from-file", flags);
        append(FrostbiteCatalog_LimitReached, "limit-reached", flags);

        return text.empty() ? "none" : text;
    }

    using HostGetActorModelCountFn = std::uint32_t(__stdcall*)();
    using HostGetActorModelInfoFn = int(__stdcall*)(std::uint32_t index, FrostbiteActorModelInfo* outInfo);
    using HostSetTimescaleFn = void(__stdcall*)(float timescale);
    using HostGetTimescaleFn = float(__stdcall*)();
    using HostApplyFeaturesFn = int(__stdcall*)(const FrostbiteUniversalFeatureState* state);
    using HostGetFeatureStateFn = int(__stdcall*)(FrostbiteUniversalFeatureState* outState);
    using HostSetSkyboxTintFn = void(__stdcall*)(float r, float g, float b, float intensity);
    using HostSetChamsFn = void(__stdcall*)(int enabled, float r, float g, float b, float opacity);
    using HostSetFogTintFn = void(__stdcall*)(int enabled, float r, float g, float b, float density);

    struct HostBridgeExportNames
    {
        std::wstring moduleName;
        std::string getActorModelCount = "FrostbiteGame_GetActorModelCount";
        std::string getActorModelInfo = "FrostbiteGame_GetActorModelInfo";
        std::string setTimescale = "FrostbiteGame_SetTimescale";
        std::string getTimescale = "FrostbiteGame_GetTimescale";
        std::string applyFeatures = "FrostbiteGame_ApplyUniversalFeatures";
        std::string getFeatureState = "FrostbiteGame_GetUniversalFeatureState";
        std::string setSkyboxTint = "FrostbiteGame_SetSkyboxTint";
        std::string setChams = "FrostbiteGame_SetChams";
        std::string setFogTint = "FrostbiteGame_SetFogTint";
    };

    std::mutex g_projectBridgeMutex;
    std::vector<FrostbiteActorModelInfo> g_actorModelList;
    FrostbiteActorModelProviderCallback g_actorModelProvider = nullptr;
    void* g_actorModelProviderUserData = nullptr;
    FrostbiteTimescaleCallback g_timescaleCallback = nullptr;
    void* g_timescaleCallbackUserData = nullptr;
    FrostbiteFeatureApplyCallback g_featureApplyCallback = nullptr;
    void* g_featureApplyCallbackUserData = nullptr;
    float g_localTimescale = 1.0f;
    FrostbiteUniversalFeatureState g_featureState = {};
    bool g_hostBridgeResolved = false;
    bool g_bridgeExportNamesLoaded = false;
    HostBridgeExportNames g_bridgeExportNames;
    HostGetActorModelCountFn g_hostGetActorModelCount = nullptr;
    HostGetActorModelInfoFn g_hostGetActorModelInfo = nullptr;
    HostSetTimescaleFn g_hostSetTimescale = nullptr;
    HostGetTimescaleFn g_hostGetTimescale = nullptr;
    HostApplyFeaturesFn g_hostApplyFeatures = nullptr;
    HostGetFeatureStateFn g_hostGetFeatureState = nullptr;
    HostSetSkyboxTintFn g_hostSetSkyboxTint = nullptr;
    HostSetChamsFn g_hostSetChams = nullptr;
    HostSetFogTintFn g_hostSetFogTint = nullptr;
    HMODULE g_loadedConfiguredBridgeModule = nullptr;
    HostGetTimescaleFn g_originalHookedHostGetTimescale = nullptr;
    HostGetFeatureStateFn g_originalHookedHostGetFeatureState = nullptr;
    bool g_minhookInitialized = false;
    bool g_timescaleHookInstalled = false;
    bool g_featureStateHookInstalled = false;
    std::wstring g_hookStatus = L"MinHook not installed";

    float ClampTimescale(float value)
    {
        if (!(value > 0.0f))
            return 1.0f;

        if (value < 0.01f)
            return 0.01f;

        if (value > 20.0f)
            return 20.0f;

        return value;
    }

    float Clamp01(float value)
    {
        if (!(value >= 0.0f))
            return 0.0f;

        if (value > 1.0f)
            return 1.0f;

        return value;
    }

    float ClampRange(float value, float minValue, float maxValue)
    {
        if (!(value >= minValue))
            return minValue;

        if (value > maxValue)
            return maxValue;

        return value;
    }

    FrostbiteUniversalFeatureState MakeDefaultFeatureState()
    {
        FrostbiteUniversalFeatureState state = {};
        state.size = sizeof(FrostbiteUniversalFeatureState);
        state.enabledFlags = FrostbiteFeature_Timescale;
        state.timescale = 1.0f;
        state.skyboxColor[0] = 0.20f;
        state.skyboxColor[1] = 0.52f;
        state.skyboxColor[2] = 1.0f;
        state.skyboxColor[3] = 1.0f;
        state.skyboxIntensity = 1.0f;
        state.skyboxRainbowSpeed = 0.35f;
        state.chamsColor[0] = 1.0f;
        state.chamsColor[1] = 0.10f;
        state.chamsColor[2] = 0.20f;
        state.chamsColor[3] = 1.0f;
        state.chamsOpacity = 0.70f;
        state.chamsRainbowSpeed = 0.45f;
        state.fogColor[0] = 0.45f;
        state.fogColor[1] = 0.65f;
        state.fogColor[2] = 1.0f;
        state.fogColor[3] = 1.0f;
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

    void NormalizeFeatureState(FrostbiteUniversalFeatureState& state)
    {
        if (state.size == 0 || state.size > sizeof(FrostbiteUniversalFeatureState))
            state.size = sizeof(FrostbiteUniversalFeatureState);

        state.timescale = ClampTimescale(state.timescale);
        state.skyboxColor[0] = Clamp01(state.skyboxColor[0]);
        state.skyboxColor[1] = Clamp01(state.skyboxColor[1]);
        state.skyboxColor[2] = Clamp01(state.skyboxColor[2]);
        state.skyboxColor[3] = Clamp01(state.skyboxColor[3]);
        state.skyboxIntensity = ClampRange(state.skyboxIntensity, 0.0f, 10.0f);
        state.skyboxRainbowSpeed = ClampRange(state.skyboxRainbowSpeed, 0.0f, 10.0f);
        state.chamsColor[0] = Clamp01(state.chamsColor[0]);
        state.chamsColor[1] = Clamp01(state.chamsColor[1]);
        state.chamsColor[2] = Clamp01(state.chamsColor[2]);
        state.chamsColor[3] = Clamp01(state.chamsColor[3]);
        state.chamsOpacity = Clamp01(state.chamsOpacity);
        state.chamsRainbowSpeed = ClampRange(state.chamsRainbowSpeed, 0.0f, 10.0f);
        state.fogColor[0] = Clamp01(state.fogColor[0]);
        state.fogColor[1] = Clamp01(state.fogColor[1]);
        state.fogColor[2] = Clamp01(state.fogColor[2]);
        state.fogColor[3] = Clamp01(state.fogColor[3]);
        state.fogDensity = ClampRange(state.fogDensity, 0.0f, 2.0f);
        state.fovDegrees = ClampRange(state.fovDegrees, 30.0f, 140.0f);
        state.hasViewTarget = state.hasViewTarget ? 1u : 0u;
    }

    void EnsureFeatureStateInitializedLocked()
    {
        if (g_featureState.size == sizeof(FrostbiteUniversalFeatureState))
            return;

        g_featureState = MakeDefaultFeatureState();
        g_localTimescale = g_featureState.timescale;
    }

    void NormalizeActorModelInfo(FrostbiteActorModelInfo& info)
    {
        info.actorName[std::size(info.actorName) - 1] = L'\0';
        info.className[std::size(info.className) - 1] = L'\0';
        info.modelName[std::size(info.modelName) - 1] = L'\0';
        info.assetPath[std::size(info.assetPath) - 1] = L'\0';
    }

    std::wstring GetUniversalDllDirectory()
    {
        HMODULE module = nullptr;
        if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetUniversalDllDirectory),
                &module))
        {
            return {};
        }

        wchar_t path[MAX_PATH] = {};
        if (!::GetModuleFileNameW(module, path, MAX_PATH))
            return {};

        return DirectoryFromPath(path);
    }

    std::wstring ReadBridgeIniValue(const fs::path& iniPath, const wchar_t* key)
    {
        constexpr wchar_t kMissingValue[] = L"__FROSTBITE_UNIVERSAL_MISSING__";
        wchar_t value[512] = {};
        ::GetPrivateProfileStringW(
            L"BridgeExports",
            key,
            kMissingValue,
            value,
            static_cast<DWORD>(std::size(value)),
            iniPath.c_str());

        if (wcscmp(value, kMissingValue) == 0)
            return {};

        return value;
    }

    void AssignConfiguredExportName(std::string& destination, const fs::path& iniPath, const wchar_t* key)
    {
        const std::wstring value = ReadBridgeIniValue(iniPath, key);
        if (value.empty())
            return;

        destination = WideToUtf8(value);
    }

    void LoadBridgeExportConfigLocked()
    {
        if (g_bridgeExportNamesLoaded)
            return;

        g_bridgeExportNamesLoaded = true;

        const std::wstring directory = GetUniversalDllDirectory();
        if (directory.empty())
        {
            FrostbiteUniversal::Log::Write(L"Project bridge export config: using default export names; DLL directory was unavailable");
            return;
        }

        const fs::path iniPath = fs::path(directory) / L"FrostbiteUniversal_Bridge.ini";
        std::error_code ec;
        const bool configExists = fs::exists(iniPath, ec);
        if (!configExists)
        {
            FrostbiteUniversal::Log::Write(L"Project bridge export config: using default FrostbiteGame_* export names");
            return;
        }

        const std::wstring moduleName = ReadBridgeIniValue(iniPath, L"ModuleName");
        if (!moduleName.empty())
            g_bridgeExportNames.moduleName = moduleName;

        AssignConfiguredExportName(g_bridgeExportNames.getActorModelCount, iniPath, L"GetActorModelCount");
        AssignConfiguredExportName(g_bridgeExportNames.getActorModelInfo, iniPath, L"GetActorModelInfo");
        AssignConfiguredExportName(g_bridgeExportNames.setTimescale, iniPath, L"SetTimescale");
        AssignConfiguredExportName(g_bridgeExportNames.getTimescale, iniPath, L"GetTimescale");
        AssignConfiguredExportName(g_bridgeExportNames.applyFeatures, iniPath, L"ApplyFeatures");
        AssignConfiguredExportName(g_bridgeExportNames.getFeatureState, iniPath, L"GetFeatureState");
        AssignConfiguredExportName(g_bridgeExportNames.setSkyboxTint, iniPath, L"SetSkyboxTint");
        AssignConfiguredExportName(g_bridgeExportNames.setChams, iniPath, L"SetChams");
        AssignConfiguredExportName(g_bridgeExportNames.setChams, iniPath, L"SetDebugMaterialTint");
        AssignConfiguredExportName(g_bridgeExportNames.setFogTint, iniPath, L"SetFogTint");

        std::wstringstream message;
        message << L"Project bridge export config loaded: " << iniPath.wstring()
                << L", module="
                << (g_bridgeExportNames.moduleName.empty() ? L"<main exe>" : g_bridgeExportNames.moduleName);
        FrostbiteUniversal::Log::Write(message.str());
    }

    FARPROC GetConfiguredProcAddress(HMODULE module, const std::string& exportName)
    {
        if (!module || exportName.empty())
            return nullptr;

        return ::GetProcAddress(module, exportName.c_str());
    }

    HMODULE LoadConfiguredBridgeModuleLocked()
    {
        if (g_bridgeExportNames.moduleName.empty())
            return nullptr;

        if (g_loadedConfiguredBridgeModule)
            return g_loadedConfiguredBridgeModule;

        const std::wstring directory = GetUniversalDllDirectory();
        if (directory.empty())
        {
            FrostbiteUniversal::Log::Write(L"Project bridge module was not auto-loaded because the Universal DLL directory was unavailable");
            return nullptr;
        }

        const fs::path bridgePath = fs::path(directory) / g_bridgeExportNames.moduleName;
        std::error_code ec;
        if (!fs::exists(bridgePath, ec))
        {
            std::wstringstream message;
            message << L"Project bridge module was not auto-loaded; file was not found beside Universal: "
                    << bridgePath.wstring();
            FrostbiteUniversal::Log::Write(message.str());
            return nullptr;
        }

        g_loadedConfiguredBridgeModule = ::LoadLibraryW(bridgePath.c_str());
        if (!g_loadedConfiguredBridgeModule)
        {
            std::wstringstream message;
            message << L"Project bridge LoadLibrary failed for " << bridgePath.wstring()
                    << L" (GetLastError=" << ::GetLastError() << L")";
            FrostbiteUniversal::Log::Write(message.str());
            return nullptr;
        }

        std::wstringstream message;
        message << L"Project bridge module auto-loaded: " << bridgePath.wstring();
        FrostbiteUniversal::Log::Write(message.str());
        return g_loadedConfiguredBridgeModule;
    }

    void ResolveHostBridgeExportsLocked()
    {
        if (g_hostBridgeResolved)
            return;

        LoadBridgeExportConfigLocked();
        g_hostBridgeResolved = true;
        HMODULE host = g_bridgeExportNames.moduleName.empty()
            ? ::GetModuleHandleW(nullptr)
            : ::GetModuleHandleW(g_bridgeExportNames.moduleName.c_str());
        if (!host && !g_bridgeExportNames.moduleName.empty())
            host = LoadConfiguredBridgeModuleLocked();

        if (!host)
        {
            std::wstringstream message;
            message << L"Project bridge module was not loaded: "
                    << (g_bridgeExportNames.moduleName.empty() ? L"<main exe>" : g_bridgeExportNames.moduleName);
            FrostbiteUniversal::Log::Write(message.str());
            return;
        }

        g_hostGetActorModelCount = reinterpret_cast<HostGetActorModelCountFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.getActorModelCount));
        g_hostGetActorModelInfo = reinterpret_cast<HostGetActorModelInfoFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.getActorModelInfo));
        g_hostSetTimescale = reinterpret_cast<HostSetTimescaleFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.setTimescale));
        g_hostGetTimescale = reinterpret_cast<HostGetTimescaleFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.getTimescale));
        g_hostApplyFeatures = reinterpret_cast<HostApplyFeaturesFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.applyFeatures));
        g_hostGetFeatureState = reinterpret_cast<HostGetFeatureStateFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.getFeatureState));
        g_hostSetSkyboxTint = reinterpret_cast<HostSetSkyboxTintFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.setSkyboxTint));
        g_hostSetChams = reinterpret_cast<HostSetChamsFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.setChams));
        g_hostSetFogTint = reinterpret_cast<HostSetFogTintFn>(
            GetConfiguredProcAddress(host, g_bridgeExportNames.setFogTint));

        if (g_hostGetActorModelCount && g_hostGetActorModelInfo)
            FrostbiteUniversal::Log::Write(L"Project bridge auto-detected actor/model host exports");

        if (g_hostSetTimescale || g_hostGetTimescale)
            FrostbiteUniversal::Log::Write(L"Project bridge auto-detected timescale host exports");

        if (g_hostApplyFeatures || g_hostGetFeatureState || g_hostSetSkyboxTint || g_hostSetChams || g_hostSetFogTint)
            FrostbiteUniversal::Log::Write(L"Project bridge auto-detected feature host exports");
    }

    bool HasActorBridgeLocked()
    {
        ResolveHostBridgeExportsLocked();
        return g_actorModelProvider != nullptr ||
               (g_hostGetActorModelCount != nullptr && g_hostGetActorModelInfo != nullptr);
    }

    bool HasTimescaleBridgeLocked()
    {
        ResolveHostBridgeExportsLocked();
        return g_timescaleCallback != nullptr ||
               g_hostSetTimescale != nullptr ||
               g_hostGetTimescale != nullptr ||
               g_timescaleHookInstalled;
    }

    bool HasFeatureBridgeLocked()
    {
        ResolveHostBridgeExportsLocked();
        return g_featureApplyCallback != nullptr ||
               g_hostApplyFeatures != nullptr ||
               g_hostGetFeatureState != nullptr ||
               g_hostSetSkyboxTint != nullptr ||
               g_hostSetChams != nullptr ||
               g_hostSetFogTint != nullptr ||
               g_featureStateHookInstalled;
    }

    float __stdcall HookedHostGetTimescale()
    {
        std::lock_guard lock(g_projectBridgeMutex);
        EnsureFeatureStateInitializedLocked();
        return g_localTimescale;
    }

    int __stdcall HookedHostGetFeatureState(FrostbiteUniversalFeatureState* outState)
    {
        if (!outState)
            return 0;

        std::lock_guard lock(g_projectBridgeMutex);
        EnsureFeatureStateInitializedLocked();
        *outState = g_featureState;
        return 1;
    }

    std::wstring MinHookStatusToWide(MH_STATUS status)
    {
        const char* text = MH_StatusToString(status);
        if (!text)
            return L"unknown";

        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (size <= 1)
            return L"unknown";

        std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
        return out;
    }

    bool EnsureMinHookInitializedLocked()
    {
        if (g_minhookInitialized)
            return true;

        const MH_STATUS status = MH_Initialize();
        if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED)
        {
            g_minhookInitialized = true;
            return true;
        }

        std::wstringstream message;
        message << L"MinHook initialize failed: " << MinHookStatusToWide(status);
        g_hookStatus = message.str();
        FrostbiteUniversal::Log::Write(g_hookStatus);
        return false;
    }

    bool InstallMinHookForExportLocked(void* target, void* detour, void** original, const wchar_t* name)
    {
        if (!target || !detour)
            return false;

        MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            std::wstringstream message;
            message << L"MinHook create failed for " << name << L": " << MinHookStatusToWide(status);
            FrostbiteUniversal::Log::Write(message.str());
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            std::wstringstream message;
            message << L"MinHook enable failed for " << name << L": " << MinHookStatusToWide(status);
            FrostbiteUniversal::Log::Write(message.str());
            return false;
        }

        return true;
    }

    bool SehCallActorModelProviderRaw(
        FrostbiteActorModelProviderCallback provider,
        FrostbiteActorModelInfo* outItems,
        std::uint32_t maxItems,
        void* userData,
        std::uint32_t* outCount,
        DWORD* exceptionCode)
    {
        if (!provider || !outCount || !exceptionCode)
            return false;

        *outCount = 0;
        *exceptionCode = 0;
        __try
        {
            *outCount = provider(outItems, maxItems, userData);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            *outCount = 0;
            return false;
        }
    }

    bool SehCallHostActorCountRaw(HostGetActorModelCountFn function, std::uint32_t* outCount, DWORD* exceptionCode)
    {
        if (!function || !outCount || !exceptionCode)
            return false;

        *outCount = 0;
        *exceptionCode = 0;
        __try
        {
            *outCount = function();
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            *outCount = 0;
            return false;
        }
    }

    bool SehCallHostActorInfoRaw(
        HostGetActorModelInfoFn function,
        std::uint32_t index,
        FrostbiteActorModelInfo* outInfo,
        int* outResult,
        DWORD* exceptionCode)
    {
        if (!function || !outInfo || !outResult || !exceptionCode)
            return false;

        *outResult = 0;
        *exceptionCode = 0;
        __try
        {
            *outResult = function(index, outInfo);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            *outResult = 0;
            return false;
        }
    }

    bool SehCallHostSetTimescaleRaw(HostSetTimescaleFn function, float timescale, DWORD* exceptionCode)
    {
        if (!function || !exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            function(timescale);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehCallHostGetTimescaleRaw(HostGetTimescaleFn function, float* outTimescale, DWORD* exceptionCode)
    {
        if (!function || !outTimescale || !exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            *outTimescale = function();
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehCallTimescaleCallbackRaw(FrostbiteTimescaleCallback callback, float timescale, void* userData, DWORD* exceptionCode)
    {
        if (!callback || !exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            callback(timescale, userData);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehCallFeatureApplyCallbackRaw(
        FrostbiteFeatureApplyCallback callback,
        const FrostbiteUniversalFeatureState* state,
        void* userData,
        int* outResult,
        DWORD* exceptionCode)
    {
        if (!callback || !state || !outResult || !exceptionCode)
            return false;

        *outResult = 0;
        *exceptionCode = 0;
        __try
        {
            *outResult = callback(state, userData);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            *outResult = 0;
            return false;
        }
    }

    bool SehCallHostApplyFeaturesRaw(
        HostApplyFeaturesFn function,
        const FrostbiteUniversalFeatureState* state,
        int* outResult,
        DWORD* exceptionCode)
    {
        if (!function || !state || !outResult || !exceptionCode)
            return false;

        *outResult = 0;
        *exceptionCode = 0;
        __try
        {
            *outResult = function(state);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            *outResult = 0;
            return false;
        }
    }

    bool SehCallHostSetSkyboxTintRaw(HostSetSkyboxTintFn function, const FrostbiteUniversalFeatureState* state, DWORD* exceptionCode)
    {
        if (!function || !state || !exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            function(state->skyboxColor[0], state->skyboxColor[1], state->skyboxColor[2], state->skyboxIntensity);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehCallHostSetChamsRaw(HostSetChamsFn function, const FrostbiteUniversalFeatureState* state, DWORD* exceptionCode)
    {
        if (!function || !state || !exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            function((state->enabledFlags & (FrostbiteFeature_Chams | FrostbiteFeature_ChamsRainbow | FrostbiteFeature_WireframeDebug)) != 0,
                state->chamsColor[0],
                state->chamsColor[1],
                state->chamsColor[2],
                state->chamsOpacity);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehCallHostSetFogTintRaw(HostSetFogTintFn function, const FrostbiteUniversalFeatureState* state, DWORD* exceptionCode)
    {
        if (!function || !state || !exceptionCode)
            return false;

        *exceptionCode = 0;
        __try
        {
            function((state->enabledFlags & FrostbiteFeature_FogTint) != 0,
                state->fogColor[0],
                state->fogColor[1],
                state->fogColor[2],
                state->fogDensity);
            return true;
        }
        __except ((*exceptionCode = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeCallActorModelProvider(
        FrostbiteActorModelProviderCallback provider,
        FrostbiteActorModelInfo* outItems,
        std::uint32_t maxItems,
        void* userData,
        std::uint32_t& outCount)
    {
        DWORD exceptionCode = 0;
        if (!SehCallActorModelProviderRaw(provider, outItems, maxItems, userData, &outCount, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Project actor/model provider threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            outCount = 0;
            return false;
        }

        return true;
    }

    bool SafeCallHostActorCount(HostGetActorModelCountFn function, std::uint32_t& outCount)
    {
        DWORD exceptionCode = 0;
        if (!SehCallHostActorCountRaw(function, &outCount, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured actor/model count bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            outCount = 0;
            return false;
        }

        return true;
    }

    bool SafeCallHostActorInfo(HostGetActorModelInfoFn function, std::uint32_t index, FrostbiteActorModelInfo& outInfo)
    {
        int result = 0;
        DWORD exceptionCode = 0;
        if (!SehCallHostActorInfoRaw(function, index, &outInfo, &result, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured actor/model info bridge export threw SEH exception 0x" << std::hex << exceptionCode
                        << L" at index " << std::dec << index;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return result != 0;
    }

    bool SafeCallHostSetTimescale(HostSetTimescaleFn function, float timescale)
    {
        DWORD exceptionCode = 0;
        if (!SehCallHostSetTimescaleRaw(function, timescale, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured set-timescale bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return true;
    }

    bool SafeCallHostGetTimescale(HostGetTimescaleFn function, float& outTimescale)
    {
        DWORD exceptionCode = 0;
        if (!SehCallHostGetTimescaleRaw(function, &outTimescale, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured get-timescale bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return true;
    }

    bool SafeCallTimescaleCallback(FrostbiteTimescaleCallback callback, float timescale, void* userData)
    {
        DWORD exceptionCode = 0;
        if (!SehCallTimescaleCallbackRaw(callback, timescale, userData, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Project timescale callback threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return true;
    }

    bool SafeCallFeatureApplyCallback(FrostbiteFeatureApplyCallback callback, const FrostbiteUniversalFeatureState& state, void* userData)
    {
        int result = 0;
        DWORD exceptionCode = 0;
        if (!SehCallFeatureApplyCallbackRaw(callback, &state, userData, &result, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Project feature apply callback threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return result != 0;
    }

    bool SafeCallHostApplyFeatures(HostApplyFeaturesFn function, const FrostbiteUniversalFeatureState& state)
    {
        int result = 0;
        DWORD exceptionCode = 0;
        if (!SehCallHostApplyFeaturesRaw(function, &state, &result, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured apply-features bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return result != 0;
    }

    bool SafeCallHostSetSkyboxTint(HostSetSkyboxTintFn function, const FrostbiteUniversalFeatureState& state)
    {
        DWORD exceptionCode = 0;
        if (!SehCallHostSetSkyboxTintRaw(function, &state, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured set-skybox-tint bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return true;
    }

    bool SafeCallHostSetChams(HostSetChamsFn function, const FrostbiteUniversalFeatureState& state)
    {
        DWORD exceptionCode = 0;
        if (!SehCallHostSetChamsRaw(function, &state, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured debug material tint bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return true;
    }

    bool SafeCallHostSetFogTint(HostSetFogTintFn function, const FrostbiteUniversalFeatureState& state)
    {
        DWORD exceptionCode = 0;
        if (!SehCallHostSetFogTintRaw(function, &state, &exceptionCode))
        {
            if (exceptionCode != 0)
            {
                std::wstringstream message;
                message << L"Configured set-fog-tint bridge export threw SEH exception 0x" << std::hex << exceptionCode;
                FrostbiteUniversal::Log::Write(message.str());
            }

            return false;
        }

        return true;
    }

    bool FindFileRecursive(const fs::path& root, const std::wstring& fileName, std::size_t maxVisited = 4096)
    {
        if (!fs::exists(root))
            return false;

        const std::wstring wanted = ToLower(fileName);
        std::error_code ec;
        std::size_t visited = 0;

        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
        {
            if (ec || ++visited > maxVisited)
                break;

            if (!entry.is_regular_file())
                continue;

            if (ToLower(entry.path().filename().wstring()) == wanted)
                return true;
        }

        return false;
    }

    bool CountFilesWithExtensionRecursive(const fs::path& root, const std::wstring& extension, std::uint32_t& count, std::size_t maxVisited = 20000)
    {
        if (!fs::exists(root))
            return false;

        std::error_code ec;
        std::size_t visited = 0;

        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
        {
            if (ec || ++visited > maxVisited)
                break;

            if (!entry.is_regular_file())
                continue;

            if (ToLower(entry.path().extension().wstring()) == extension)
                ++count;
        }

        return count > 0;
    }

    bool IsValidImageRange(std::uintptr_t base, std::uint32_t imageSize, std::uint32_t rva, std::size_t size)
    {
        if (imageSize == 0 || base == 0)
            return false;

        return rva <= imageSize && size <= imageSize - rva;
    }

    const char* ReadExportString(std::uintptr_t base, std::uint32_t imageSize, std::uint32_t rva)
    {
        if (!IsValidImageRange(base, imageSize, rva, 1))
            return nullptr;

        const char* value = reinterpret_cast<const char*>(base + rva);
        const std::size_t maxLength = imageSize - rva;
        for (std::size_t i = 0; i < maxLength; ++i)
        {
            if (value[i] == '\0')
                return value;
        }

        return nullptr;
    }

    bool ShouldCatalogExports(const FrostbiteUniversal::ModuleRecord& module)
    {
        if ((module.flags & FrostbiteModule_AntiCheat) != 0)
            return false;

        const std::uint32_t interesting =
            FrostbiteModule_GameExecutable |
            FrostbiteModule_EngineBuildInfo |
            FrostbiteModule_RenderCore2 |
            FrostbiteModule_DirectStorage |
            FrostbiteModule_Oodle |
            FrostbiteModule_PlatformSdk |
            FrostbiteModule_ThirdPartyRender;

        return (module.flags & FrostbiteModule_HasExports) != 0 &&
               (module.flags & interesting) != 0;
    }
}

namespace FrostbiteUniversal
{
    Runtime& GetRuntime()
    {
        static Runtime runtime;
        return runtime;
    }

    bool Runtime::Initialize()
    {
        Log::Write(L"Runtime initialize requested");
        std::lock_guard lock(m_mutex);
        RebuildLocked();
        m_initialized = true;

        const bool detected = (m_runtimeFlags & FrostbiteRuntime_IsFrostbiteProcess) != 0;
        Log::Write(detected
            ? L"Runtime initialize completed: Frostbite detected"
            : L"Runtime initialize completed: Frostbite not detected");
        return detected;
    }

    bool Runtime::Refresh()
    {
        Log::Write(L"Runtime refresh requested");
        std::lock_guard lock(m_mutex);
        RebuildLocked();
        m_initialized = true;

        const bool detected = (m_runtimeFlags & FrostbiteRuntime_IsFrostbiteProcess) != 0;
        Log::Write(detected
            ? L"Runtime refresh completed: Frostbite detected"
            : L"Runtime refresh completed: Frostbite not detected");
        return detected;
    }

    void Runtime::Shutdown()
    {
        std::lock_guard lock(m_mutex);
        m_modules.clear();
        m_exports.clear();
        m_catalog.clear();
        m_frostbiteModuleCount = 0;
        m_tocFileCount = 0;
        m_casFileCount = 0;
        m_runtimeFlags = FrostbiteRuntime_None;
        m_initialized = false;
    }

    bool Runtime::IsInitialized() const
    {
        std::lock_guard lock(m_mutex);
        return m_initialized;
    }

    bool Runtime::IsFrostbiteProcess() const
    {
        return (m_runtimeFlags & FrostbiteRuntime_IsFrostbiteProcess) != 0;
    }

    bool Runtime::GetInfo(FrostbiteRuntimeInfo& outInfo) const
    {
        std::lock_guard lock(m_mutex);
        ZeroMemory(&outInfo, sizeof(outInfo));

        CopyWide(outInfo.processName, m_processName);
        CopyWide(outInfo.processPath, m_processPath);
        CopyWide(outInfo.gameRoot, m_gameRoot);
        CopyWide(outInfo.detectedTitle, m_detectedTitle);
        outInfo.moduleCount = static_cast<std::uint32_t>(m_modules.size());
        outInfo.frostbiteModuleCount = m_frostbiteModuleCount;
        outInfo.tocFileCount = m_tocFileCount;
        outInfo.casFileCount = m_casFileCount;
        outInfo.flags = m_runtimeFlags;
        return m_initialized;
    }

    bool Runtime::GetModuleInfo(std::uint32_t index, FrostbiteModuleInfo& outInfo) const
    {
        std::lock_guard lock(m_mutex);
        if (index >= m_modules.size())
            return false;

        const ModuleRecord& module = m_modules[index];
        ZeroMemory(&outInfo, sizeof(outInfo));
        CopyWide(outInfo.name, module.name);
        CopyWide(outInfo.path, module.path);
        outInfo.baseAddress = module.baseAddress;
        outInfo.imageSize = module.imageSize;
        outInfo.flags = module.flags;
        return true;
    }

    bool Runtime::GetExportInfo(std::uint32_t index, FrostbiteExportInfo& outInfo) const
    {
        std::lock_guard lock(m_mutex);
        if (index >= m_exports.size())
            return false;

        const ExportRecord& record = m_exports[index];
        ZeroMemory(&outInfo, sizeof(outInfo));
        CopyWide(outInfo.moduleName, record.moduleName);
        CopyNarrow(outInfo.name, record.name);
        outInfo.address = record.address;
        outInfo.moduleIndex = record.moduleIndex;
        outInfo.ordinal = record.ordinal;
        outInfo.flags = record.flags;
        return true;
    }

    std::uint32_t Runtime::GetModuleCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<std::uint32_t>(m_modules.size());
    }

    std::uint32_t Runtime::GetExportCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<std::uint32_t>(m_exports.size());
    }

    bool Runtime::GetCatalogInfo(std::uint32_t index, FrostbiteCatalogInfo& outInfo) const
    {
        std::lock_guard lock(m_mutex);
        if (index >= m_catalog.size())
            return false;

        const CatalogRecord& record = m_catalog[index];
        ZeroMemory(&outInfo, sizeof(outInfo));
        CopyWide(outInfo.name, record.name);
        CopyWide(outInfo.source, record.source);
        CopyWide(outInfo.path, record.path);
        outInfo.address = record.address;
        outInfo.flags = record.flags;
        return true;
    }

    std::uint32_t Runtime::GetCatalogCount() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<std::uint32_t>(m_catalog.size());
    }

    void* Runtime::GetExport(const wchar_t* moduleName, const char* exportName) const
    {
        if (!exportName || exportName[0] == '\0')
            return nullptr;

        std::lock_guard lock(m_mutex);
        for (const ModuleRecord& module : m_modules)
        {
            if (!ModuleMatches(module, moduleName))
                continue;

            if ((module.flags & FrostbiteModule_AntiCheat) != 0)
                continue;

            if (FARPROC proc = ::GetProcAddress(module.handle, exportName))
                return reinterpret_cast<void*>(proc);
        }

        return nullptr;
    }

    bool Runtime::WriteReport(const wchar_t* reportPath) const
    {
        std::lock_guard lock(m_mutex);
        const std::wstring path = reportPath && reportPath[0] ? reportPath : DefaultReportPath();
        Log::Write(L"Writing runtime report: " + path);

        std::wofstream report(path, std::ios::trunc);
        if (!report)
        {
            Log::Write(L"Runtime report failed to open: " + path);
            return false;
        }

        report << L"FrostbiteUniversal runtime report\n";
        report << L"Process: " << m_processName << L"\n";
        report << L"Path: " << m_processPath << L"\n";
        report << L"Game root: " << m_gameRoot << L"\n";
        report << L"Detected title: " << m_detectedTitle << L"\n";
        report << L"Module count: " << m_modules.size() << L"\n";
        report << L"Frostbite module count: " << m_frostbiteModuleCount << L"\n";
        report << L"TOC files: " << m_tocFileCount << L"\n";
        report << L"CAS files: " << m_casFileCount << L"\n";
        report << L"Runtime flags: 0x" << std::hex << m_runtimeFlags << std::dec << L"\n\n";

        for (const ModuleRecord& module : m_modules)
        {
            report << L"[" << module.name << L"]\n";
            report << L"  Path: " << module.path << L"\n";
            report << L"  Base: 0x" << std::hex << module.baseAddress << std::dec << L"\n";
            report << L"  Size: " << module.imageSize << L"\n";
            report << L"  Flags: 0x" << std::hex << module.flags << std::dec << L"\n\n";
        }

        return true;
    }

    bool Runtime::WriteExportReport(const wchar_t* reportPath) const
    {
        std::lock_guard lock(m_mutex);
        const std::wstring path = reportPath && reportPath[0] ? reportPath : DefaultExportReportPath();
        Log::Write(L"Writing export report: " + path);

        std::ofstream report(path, std::ios::binary | std::ios::trunc);
        if (!report)
        {
            Log::Write(L"Export report failed to open: " + path);
            return false;
        }

        report << "# FrostbiteUniversal Export Catalog\n\n";
        report << "This is a named PE export catalog from loaded game/engine-related modules. ";
        report << "It does not include stripped internal functions and it does not hook, patch, or toggle game code.\n\n";
        report << "- Process: " << WideToUtf8(m_processName) << "\n";
        report << "- Modules: " << m_modules.size() << "\n";
        report << "- Named exports cataloged: " << m_exports.size() << "\n\n";
        report << "| Module | Export | Address | Ordinal | Flags |\n";
        report << "| --- | --- | ---: | ---: | --- |\n";

        for (const ExportRecord& record : m_exports)
        {
            report << "| `" << WideToUtf8(record.moduleName) << "` | `"
                   << record.name << "` | `0x" << std::hex << record.address << std::dec << "` | "
                   << record.ordinal << " | "
                   << ((record.flags & FrostbiteExport_Forwarded) ? "`forwarded`" : "`none`")
                   << " |\n";
        }

        return true;
    }

    bool Runtime::WriteCatalogReport(const wchar_t* reportPath) const
    {
        std::lock_guard lock(m_mutex);
        const std::wstring path = reportPath && reportPath[0] ? reportPath : DefaultCatalogReportPath();
        Log::Write(L"Writing actor/model catalog report: " + path);

        std::ofstream report(path, std::ios::binary | std::ios::trunc);
        if (!report)
        {
            Log::Write(L"Actor/model catalog report failed to open: " + path);
            return false;
        }

        report << "# FrostbiteUniversal Actor/Model Catalog\n\n";
        report << "This is a diagnostic catalog from loaded module exports and model-like files in the injected app folder. ";
        report << "It is not a live entity memory list and it does not hook, patch, or read private actor arrays.\n\n";
        report << "- Process: " << WideToUtf8(m_processName) << "\n";
        report << "- Game root: " << WideToUtf8(m_gameRoot) << "\n";
        report << "- Catalog entries: " << m_catalog.size() << "\n\n";
        report << "| Source | Name | Path/Module | Address | Flags |\n";
        report << "| --- | --- | --- | ---: | --- |\n";

        for (const CatalogRecord& record : m_catalog)
        {
            report << "| `" << WideToUtf8(record.source) << "` | `"
                   << WideToUtf8(record.name) << "` | `"
                   << WideToUtf8(record.path) << "` | `0x"
                   << std::hex << record.address << std::dec << "` | `"
                   << CatalogFlagsToText(record.flags) << "` |\n";
        }

        return true;
    }

    void Runtime::RebuildLocked()
    {
        m_processPath = GetCurrentProcessPath();
        m_processName = FileNameFromPath(m_processPath);
        m_gameRoot = DirectoryFromPath(m_processPath);
        m_detectedTitle = GuessTitleFromProcessName(m_processName);
        m_modules = EnumerateProcessModules();
        m_tocFileCount = 0;
        m_casFileCount = 0;
        m_runtimeFlags = FrostbiteRuntime_None;

        InspectGameRootLocked();
        UpdateRuntimeFlagsLocked();
        EnumerateExportsLocked();
        BuildActorModelCatalogLocked();

        std::wstringstream summary;
        summary << L"Runtime rebuilt: process=" << m_processName
                << L", modules=" << m_modules.size()
                << L", frostbiteModules=" << m_frostbiteModuleCount
                << L", namedExports=" << m_exports.size()
                << L", catalog=" << m_catalog.size()
                << L", toc=" << m_tocFileCount
                << L", cas=" << m_casFileCount
                << L", flags=0x" << std::hex << m_runtimeFlags;
        Log::Write(summary.str());
    }

    void Runtime::InspectGameRootLocked()
    {
        const fs::path root = m_gameRoot;
        const fs::path dataRoot = root / L"Data";

        if (fs::exists(dataRoot))
            m_runtimeFlags |= FrostbiteRuntime_HasDataDirectory;

        if (fs::exists(dataRoot / L"initfs_Win32") ||
            fs::exists(root / L"initfs_Win32") ||
            FindFileRecursive(dataRoot, L"initfs_Win32"))
        {
            m_runtimeFlags |= FrostbiteRuntime_HasInitFs;
        }

        if (fs::exists(dataRoot / L"layout.toc") ||
            fs::exists(root / L"layout.toc") ||
            FindFileRecursive(dataRoot, L"layout.toc"))
        {
            m_runtimeFlags |= FrostbiteRuntime_HasLayoutToc;
        }

        if (fs::exists(root / L"EAAntiCheat.cfg") ||
            fs::exists(root / L"EAAntiCheat.GameServiceLauncher.exe") ||
            fs::exists(root / L"EAAntiCheat.GameServiceLauncher.dll"))
        {
            m_runtimeFlags |= FrostbiteRuntime_HasAntiCheatFiles;
        }

        if (!fs::exists(dataRoot))
            return;

        if (CountFilesWithExtensionRecursive(dataRoot, L".toc", m_tocFileCount))
            m_runtimeFlags |= FrostbiteRuntime_HasTocArchives;

        if (CountFilesWithExtensionRecursive(dataRoot, L".cas", m_casFileCount))
            m_runtimeFlags |= FrostbiteRuntime_HasCasArchives;
    }

    void Runtime::UpdateRuntimeFlagsLocked()
    {
        m_frostbiteModuleCount = 0;

        for (const ModuleRecord& module : m_modules)
        {
            if ((module.flags & FrostbiteModule_EngineBuildInfo) != 0)
                m_runtimeFlags |= FrostbiteRuntime_HasEngineBuildInfo;

            if ((module.flags & FrostbiteModule_RenderCore2) != 0)
                m_runtimeFlags |= FrostbiteRuntime_HasRenderCore2;

            const std::uint32_t frostbiteBits =
                FrostbiteModule_GameExecutable |
                FrostbiteModule_EngineBuildInfo |
                FrostbiteModule_RenderCore2 |
                FrostbiteModule_DirectStorage |
                FrostbiteModule_Oodle;

            if ((module.flags & frostbiteBits) != 0)
                ++m_frostbiteModuleCount;
        }

        const bool hasFrostbiteData =
            (m_runtimeFlags & FrostbiteRuntime_HasDataDirectory) != 0 &&
            ((m_runtimeFlags & FrostbiteRuntime_HasInitFs) != 0 ||
             (m_runtimeFlags & FrostbiteRuntime_HasLayoutToc) != 0 ||
             (m_runtimeFlags & FrostbiteRuntime_HasTocArchives) != 0 ||
             (m_runtimeFlags & FrostbiteRuntime_HasCasArchives) != 0) &&
            ((m_runtimeFlags & FrostbiteRuntime_HasTocArchives) != 0 ||
             (m_runtimeFlags & FrostbiteRuntime_HasCasArchives) != 0);

        const bool hasFrostbiteModules =
            (m_runtimeFlags & FrostbiteRuntime_HasEngineBuildInfo) != 0 ||
            (m_runtimeFlags & FrostbiteRuntime_HasRenderCore2) != 0;

        if (hasFrostbiteData || hasFrostbiteModules)
            m_runtimeFlags |= FrostbiteRuntime_IsFrostbiteProcess;

#if FROSTBITEUNIVERSAL_HAS_SHARED_IMGUI
        m_runtimeFlags |= FrostbiteRuntime_HasSharedImGui;
#endif
    }

    void Runtime::EnumerateExportsLocked()
    {
        m_exports.clear();

        for (std::uint32_t moduleIndex = 0; moduleIndex < m_modules.size(); ++moduleIndex)
        {
            const ModuleRecord& module = m_modules[moduleIndex];
            if (!ShouldCatalogExports(module))
                continue;

            const std::uintptr_t base = module.baseAddress;
            const std::uint32_t imageSize = module.imageSize;
            if (!IsValidImageRange(base, imageSize, 0, sizeof(IMAGE_DOS_HEADER)))
                continue;

            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
                continue;

            if (!IsValidImageRange(base, imageSize, static_cast<std::uint32_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS)))
                continue;

            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                continue;

            const IMAGE_DATA_DIRECTORY& exportDirectory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!IsValidImageRange(base, imageSize, exportDirectory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
                continue;

            const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exportDirectory.VirtualAddress);
            if (exports->NumberOfNames == 0)
                continue;

            if (!IsValidImageRange(base, imageSize, exports->AddressOfNames, sizeof(std::uint32_t) * exports->NumberOfNames) ||
                !IsValidImageRange(base, imageSize, exports->AddressOfNameOrdinals, sizeof(std::uint16_t) * exports->NumberOfNames) ||
                !IsValidImageRange(base, imageSize, exports->AddressOfFunctions, sizeof(std::uint32_t) * exports->NumberOfFunctions))
            {
                continue;
            }

            const auto* nameRvas = reinterpret_cast<const std::uint32_t*>(base + exports->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const std::uint16_t*>(base + exports->AddressOfNameOrdinals);
            const auto* functionRvas = reinterpret_cast<const std::uint32_t*>(base + exports->AddressOfFunctions);

            for (std::uint32_t nameIndex = 0; nameIndex < exports->NumberOfNames; ++nameIndex)
            {
                const char* exportName = ReadExportString(base, imageSize, nameRvas[nameIndex]);
                if (!exportName || exportName[0] == '\0')
                    continue;

                const std::uint16_t functionIndex = ordinals[nameIndex];
                if (functionIndex >= exports->NumberOfFunctions)
                    continue;

                const std::uint32_t functionRva = functionRvas[functionIndex];
                if (!IsValidImageRange(base, imageSize, functionRva, 1))
                    continue;

                ExportRecord record;
                record.moduleName = module.name;
                record.name = exportName;
                record.address = base + functionRva;
                record.moduleIndex = moduleIndex;
                record.ordinal = exports->Base + functionIndex;
                record.flags = (functionRva >= exportDirectory.VirtualAddress &&
                                functionRva < exportDirectory.VirtualAddress + exportDirectory.Size)
                    ? FrostbiteExport_Forwarded
                    : FrostbiteExport_None;
                m_exports.emplace_back(std::move(record));
            }
        }

        std::sort(m_exports.begin(), m_exports.end(), [](const ExportRecord& lhs, const ExportRecord& rhs) {
            if (lhs.moduleName != rhs.moduleName)
                return lhs.moduleName < rhs.moduleName;
            return lhs.name < rhs.name;
        });
    }

    void Runtime::BuildActorModelCatalogLocked()
    {
        constexpr std::size_t kMaxCatalogEntries = 2500;
        constexpr std::size_t kMaxAssetFilesVisited = 50000;

        m_catalog.clear();

        for (const ExportRecord& record : m_exports)
        {
            const std::wstring wideName = Utf8ToWide(record.name);
            const std::wstring lowerName = ToLower(wideName);
            const std::uint32_t flags = CatalogFlagsFromText(lowerName, true);
            if (!HasCatalogMeaning(flags))
                continue;

            CatalogRecord catalogRecord;
            catalogRecord.name = wideName.empty() ? L"<unnamed-export>" : wideName;
            catalogRecord.source = L"export";
            catalogRecord.path = record.moduleName;
            catalogRecord.address = record.address;
            catalogRecord.flags = flags;
            m_catalog.emplace_back(std::move(catalogRecord));

            if (m_catalog.size() >= kMaxCatalogEntries)
                break;
        }

        const fs::path root = m_gameRoot;
        if (!root.empty() && fs::exists(root) && m_catalog.size() < kMaxCatalogEntries)
        {
            std::error_code ec;
            std::size_t visited = 0;
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
            {
                if (ec || ++visited > kMaxAssetFilesVisited || m_catalog.size() >= kMaxCatalogEntries)
                    break;

                if (!entry.is_regular_file())
                    continue;

                const fs::path path = entry.path();
                const std::wstring lowerExtension = ToLower(path.extension().wstring());
                const std::wstring lowerName = ToLower(path.filename().wstring());
                if (!IsCatalogAssetExtension(lowerExtension) &&
                    lowerName.find(L"actor") == std::wstring::npos &&
                    lowerName.find(L"entity") == std::wstring::npos &&
                    lowerName.find(L"model") == std::wstring::npos &&
                    lowerName.find(L"mesh") == std::wstring::npos &&
                    lowerName.find(L"skeleton") == std::wstring::npos &&
                    lowerName.find(L"anim") == std::wstring::npos)
                {
                    continue;
                }

                std::uint32_t flags = CatalogFlagsFromText(ToLower(path.wstring()), false);
                if (IsCatalogAssetExtension(lowerExtension))
                {
                    if (lowerExtension == L".mesh" || lowerExtension == L".skinnedmesh")
                        flags |= FrostbiteCatalog_MeshAsset;
                    else if (lowerExtension == L".skeleton" || lowerExtension == L".animation" || lowerExtension == L".anim")
                        flags |= FrostbiteCatalog_AnimationAsset;
                    else
                        flags |= FrostbiteCatalog_ModelAsset;
                }

                if (!HasCatalogMeaning(flags))
                    continue;

                const std::wstring relativePath = RelativeOrFullPath(path, root);
                if (CatalogRecordExists(m_catalog, L"file", relativePath, path.filename().wstring()))
                    continue;

                CatalogRecord catalogRecord;
                catalogRecord.name = path.filename().wstring();
                catalogRecord.source = L"file";
                catalogRecord.path = relativePath;
                catalogRecord.address = 0;
                catalogRecord.flags = flags;
                m_catalog.emplace_back(std::move(catalogRecord));
            }
        }

        if (m_catalog.size() >= kMaxCatalogEntries)
        {
            CatalogRecord limitRecord;
            limitRecord.name = L"catalog limit reached";
            limitRecord.source = L"diagnostic";
            limitRecord.path = m_gameRoot;
            limitRecord.flags = FrostbiteCatalog_LimitReached;
            m_catalog.emplace_back(std::move(limitRecord));
        }

        std::sort(m_catalog.begin(), m_catalog.end(), [](const CatalogRecord& lhs, const CatalogRecord& rhs) {
            if (lhs.source != rhs.source)
                return lhs.source < rhs.source;
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.path < rhs.path;
        });
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_Initialize()
{
    return FrostbiteUniversal::GetRuntime().Initialize() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_Refresh()
{
    return FrostbiteUniversal::GetRuntime().Refresh() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_Shutdown()
{
    FrostbiteUniversal::GetRuntime().Shutdown();
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_IsInitialized()
{
    return FrostbiteUniversal::GetRuntime().IsInitialized() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_IsFrostbiteProcess()
{
    return FrostbiteUniversal::GetRuntime().IsFrostbiteProcess() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetRuntimeInfo(FrostbiteRuntimeInfo* outInfo)
{
    if (!outInfo)
        return 0;

    return FrostbiteUniversal::GetRuntime().GetInfo(*outInfo) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetModuleCount()
{
    return FrostbiteUniversal::GetRuntime().GetModuleCount();
}

FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetExportCount()
{
    return FrostbiteUniversal::GetRuntime().GetExportCount();
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetModuleInfo(std::uint32_t index, FrostbiteModuleInfo* outInfo)
{
    if (!outInfo)
        return 0;

    return FrostbiteUniversal::GetRuntime().GetModuleInfo(index, *outInfo) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetExportInfo(std::uint32_t index, FrostbiteExportInfo* outInfo)
{
    if (!outInfo)
        return 0;

    return FrostbiteUniversal::GetRuntime().GetExportInfo(index, *outInfo) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetCatalogCount()
{
    return FrostbiteUniversal::GetRuntime().GetCatalogCount();
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetCatalogInfo(std::uint32_t index, FrostbiteCatalogInfo* outInfo)
{
    if (!outInfo)
        return 0;

    return FrostbiteUniversal::GetRuntime().GetCatalogInfo(index, *outInfo) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_ClearActorModelList()
{
    std::lock_guard lock(g_projectBridgeMutex);
    g_actorModelList.clear();
    FrostbiteUniversal::Log::Write(L"Project actor/model list cleared");
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_AddActorModelInfo(const FrostbiteActorModelInfo* info)
{
    if (!info)
        return 0;

    std::lock_guard lock(g_projectBridgeMutex);
    if (g_actorModelList.size() >= 8192)
        return 0;

    FrostbiteActorModelInfo copy = *info;
    NormalizeActorModelInfo(copy);
    copy.flags |= FrostbiteActorModel_FromManualAdd;
    g_actorModelList.push_back(copy);
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetActorModelProvider(FrostbiteActorModelProviderCallback callback, void* userData)
{
    {
        std::lock_guard lock(g_projectBridgeMutex);
        g_actorModelProvider = callback;
        g_actorModelProviderUserData = userData;
    }

    FrostbiteUniversal::Log::Write(callback
        ? L"Project actor/model provider registered"
        : L"Project actor/model provider cleared");
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_RefreshActorModelList()
{
    constexpr std::uint32_t kMaxActorModels = 8192;

    FrostbiteActorModelProviderCallback provider = nullptr;
    void* providerUserData = nullptr;
    HostGetActorModelCountFn hostCount = nullptr;
    HostGetActorModelInfoFn hostInfo = nullptr;

    {
        std::lock_guard lock(g_projectBridgeMutex);
        ResolveHostBridgeExportsLocked();
        provider = g_actorModelProvider;
        providerUserData = g_actorModelProviderUserData;
        hostCount = g_hostGetActorModelCount;
        hostInfo = g_hostGetActorModelInfo;
    }

    std::vector<FrostbiteActorModelInfo> refreshed;
    refreshed.resize(kMaxActorModels);
    std::uint32_t count = 0;

    if (provider)
    {
        if (!SafeCallActorModelProvider(provider, refreshed.data(), kMaxActorModels, providerUserData, count))
            return static_cast<int>(FrostbiteUniversal_GetActorModelCount());

        if (count > kMaxActorModels)
            count = kMaxActorModels;

        refreshed.resize(count);
        for (FrostbiteActorModelInfo& item : refreshed)
        {
            NormalizeActorModelInfo(item);
            item.flags |= FrostbiteActorModel_FromProvider;
        }
    }
    else if (hostCount && hostInfo)
    {
        if (!SafeCallHostActorCount(hostCount, count))
            return static_cast<int>(FrostbiteUniversal_GetActorModelCount());

        if (count > kMaxActorModels)
            count = kMaxActorModels;

        refreshed.clear();
        refreshed.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            FrostbiteActorModelInfo item = {};
            if (!SafeCallHostActorInfo(hostInfo, index, item))
                continue;

            NormalizeActorModelInfo(item);
            item.flags |= FrostbiteActorModel_FromHostExport;
            refreshed.push_back(item);
        }
    }
    else
    {
        std::lock_guard lock(g_projectBridgeMutex);
        return static_cast<int>(g_actorModelList.size());
    }

    {
        std::lock_guard lock(g_projectBridgeMutex);
        g_actorModelList = std::move(refreshed);
        count = static_cast<std::uint32_t>(g_actorModelList.size());
    }

    std::wstringstream message;
    message << L"Project actor/model list refreshed: " << count << L" entries";
    FrostbiteUniversal::Log::Write(message.str());
    return static_cast<int>(count);
}

FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetActorModelCount()
{
    std::lock_guard lock(g_projectBridgeMutex);
    return static_cast<std::uint32_t>(g_actorModelList.size());
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetActorModelInfo(std::uint32_t index, FrostbiteActorModelInfo* outInfo)
{
    if (!outInfo)
        return 0;

    std::lock_guard lock(g_projectBridgeMutex);
    if (index >= g_actorModelList.size())
        return 0;

    *outInfo = g_actorModelList[index];
    NormalizeActorModelInfo(*outInfo);
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetTimescaleCallback(FrostbiteTimescaleCallback callback, void* userData)
{
    {
        std::lock_guard lock(g_projectBridgeMutex);
        g_timescaleCallback = callback;
        g_timescaleCallbackUserData = userData;
    }

    FrostbiteUniversal::Log::Write(callback
        ? L"Project timescale callback registered"
        : L"Project timescale callback cleared");
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetTimescale(float timescale)
{
    const float clamped = ClampTimescale(timescale);

    FrostbiteTimescaleCallback callback = nullptr;
    void* callbackUserData = nullptr;
    HostSetTimescaleFn hostSet = nullptr;

    {
        std::lock_guard lock(g_projectBridgeMutex);
        ResolveHostBridgeExportsLocked();
        g_localTimescale = clamped;
        callback = g_timescaleCallback;
        callbackUserData = g_timescaleCallbackUserData;
        hostSet = g_hostSetTimescale;
    }

    const bool hostSetOk = hostSet && SafeCallHostSetTimescale(hostSet, clamped);
    const bool callbackOk = callback && SafeCallTimescaleCallback(callback, clamped, callbackUserData);

    std::wstringstream message;
    message << L"Project timescale requested: " << clamped
            << L", bridge=" << ((hostSetOk || callbackOk) ? L"connected" : L"local-only");
    FrostbiteUniversal::Log::Write(message.str());
    return (hostSetOk || callbackOk) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API float FrostbiteUniversal_GetTimescale()
{
    std::lock_guard lock(g_projectBridgeMutex);
    return g_localTimescale;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SyncTimescaleFromHost()
{
    HostGetTimescaleFn hostGet = nullptr;

    {
        std::lock_guard lock(g_projectBridgeMutex);
        ResolveHostBridgeExportsLocked();
        hostGet = g_hostGetTimescale;
    }

    if (!hostGet)
        return 0;

    float hostTimescale = 1.0f;
    if (!SafeCallHostGetTimescale(hostGet, hostTimescale))
        return 0;

    {
        std::lock_guard lock(g_projectBridgeMutex);
        g_localTimescale = ClampTimescale(hostTimescale);
    }

    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasTimescaleBridge()
{
    std::lock_guard lock(g_projectBridgeMutex);
    return HasTimescaleBridgeLocked() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasActorModelBridge()
{
    std::lock_guard lock(g_projectBridgeMutex);
    return HasActorBridgeLocked() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetFeatureApplyCallback(FrostbiteFeatureApplyCallback callback, void* userData)
{
    {
        std::lock_guard lock(g_projectBridgeMutex);
        EnsureFeatureStateInitializedLocked();
        g_featureApplyCallback = callback;
        g_featureApplyCallbackUserData = userData;
    }

    FrostbiteUniversal::Log::Write(callback
        ? L"Project feature apply callback registered"
        : L"Project feature apply callback cleared");
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SetFeatureState(const FrostbiteUniversalFeatureState* state)
{
    if (!state)
        return 0;

    FrostbiteUniversalFeatureState copy = *state;
    NormalizeFeatureState(copy);

    {
        std::lock_guard lock(g_projectBridgeMutex);
        g_featureState = copy;
        g_localTimescale = copy.timescale;
    }

    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetFeatureState(FrostbiteUniversalFeatureState* outState)
{
    if (!outState)
        return 0;

    std::lock_guard lock(g_projectBridgeMutex);
    EnsureFeatureStateInitializedLocked();
    *outState = g_featureState;
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ApplyFeatureState()
{
    FrostbiteUniversalFeatureState state = {};
    FrostbiteFeatureApplyCallback callback = nullptr;
    void* callbackUserData = nullptr;
    HostApplyFeaturesFn hostApply = nullptr;
    HostSetSkyboxTintFn hostSetSkyboxTint = nullptr;
    HostSetChamsFn hostSetChams = nullptr;
    HostSetFogTintFn hostSetFogTint = nullptr;

    {
        std::lock_guard lock(g_projectBridgeMutex);
        EnsureFeatureStateInitializedLocked();
        ResolveHostBridgeExportsLocked();
        state = g_featureState;
        callback = g_featureApplyCallback;
        callbackUserData = g_featureApplyCallbackUserData;
        hostApply = g_hostApplyFeatures;
        hostSetSkyboxTint = g_hostSetSkyboxTint;
        hostSetChams = g_hostSetChams;
        hostSetFogTint = g_hostSetFogTint;
    }

    bool applied = false;
    if ((state.enabledFlags & FrostbiteFeature_Timescale) != 0)
        applied = FrostbiteUniversal_SetTimescale(state.timescale) != 0 || applied;

    if (callback)
        applied = SafeCallFeatureApplyCallback(callback, state, callbackUserData) || applied;

    if (hostApply)
        applied = SafeCallHostApplyFeatures(hostApply, state) || applied;

    if (hostSetSkyboxTint && (state.enabledFlags & (FrostbiteFeature_SkyboxTint | FrostbiteFeature_SkyboxRainbow)) != 0)
        applied = SafeCallHostSetSkyboxTint(hostSetSkyboxTint, state) || applied;

    if (hostSetChams && (state.enabledFlags & (FrostbiteFeature_Chams | FrostbiteFeature_ChamsRainbow | FrostbiteFeature_WireframeDebug)) != 0)
        applied = SafeCallHostSetChams(hostSetChams, state) || applied;

    if (hostSetFogTint && (state.enabledFlags & FrostbiteFeature_FogTint) != 0)
        applied = SafeCallHostSetFogTint(hostSetFogTint, state) || applied;

    std::wstringstream message;
    message << L"Project feature state applied: flags=0x" << std::hex << state.enabledFlags
            << L", bridge=" << (applied ? L"connected" : L"local-only");
    FrostbiteUniversal::Log::Write(message.str());
    return applied ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_HasFeatureBridge()
{
    std::lock_guard lock(g_projectBridgeMutex);
    return HasFeatureBridgeLocked() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_InstallOwnedProjectHooks()
{
    std::lock_guard lock(g_projectBridgeMutex);
    EnsureFeatureStateInitializedLocked();
    ResolveHostBridgeExportsLocked();

    if (!EnsureMinHookInitializedLocked())
        return 0;

    if (g_hostGetTimescale && !g_timescaleHookInstalled)
    {
        const std::wstring timescaleExportName = Utf8ToWide(g_bridgeExportNames.getTimescale);
        g_timescaleHookInstalled = InstallMinHookForExportLocked(
            reinterpret_cast<void*>(g_hostGetTimescale),
            reinterpret_cast<void*>(&HookedHostGetTimescale),
            reinterpret_cast<void**>(&g_originalHookedHostGetTimescale),
            timescaleExportName.c_str());
    }

    if (g_hostGetFeatureState && !g_featureStateHookInstalled)
    {
        const std::wstring featureStateExportName = Utf8ToWide(g_bridgeExportNames.getFeatureState);
        g_featureStateHookInstalled = InstallMinHookForExportLocked(
            reinterpret_cast<void*>(g_hostGetFeatureState),
            reinterpret_cast<void*>(&HookedHostGetFeatureState),
            reinterpret_cast<void**>(&g_originalHookedHostGetFeatureState),
            featureStateExportName.c_str());
    }

    std::wstringstream status;
    status << L"MinHook: timescale="
           << (g_timescaleHookInstalled ? L"hooked" : (g_hostGetTimescale ? L"available-not-hooked" : L"no target"))
           << L", feature-state="
           << (g_featureStateHookInstalled ? L"hooked" : (g_hostGetFeatureState ? L"available-not-hooked" : L"no target"))
           << L", direct-feature-bridge="
           << ((g_hostApplyFeatures || g_hostSetSkyboxTint || g_hostSetChams || g_hostSetFogTint || g_featureApplyCallback) ? L"available" : L"none");
    g_hookStatus = status.str();
    FrostbiteUniversal::Log::Write(g_hookStatus);
    return (g_timescaleHookInstalled || g_featureStateHookInstalled) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetOwnedProjectHookStatus(wchar_t* outStatus, std::uint32_t outStatusLength)
{
    if (!outStatus || outStatusLength == 0)
        return 0;

    std::lock_guard lock(g_projectBridgeMutex);
    wcsncpy_s(outStatus, outStatusLength, g_hookStatus.c_str(), _TRUNCATE);
    return 1;
}

FROSTBITEUNIVERSAL_API void* FrostbiteUniversal_GetExport(const wchar_t* moduleName, const char* exportName)
{
    return FrostbiteUniversal::GetRuntime().GetExport(moduleName, exportName);
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteRuntimeReport(const wchar_t* reportPath)
{
    return FrostbiteUniversal::GetRuntime().WriteReport(reportPath) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteExportReport(const wchar_t* reportPath)
{
    return FrostbiteUniversal::GetRuntime().WriteExportReport(reportPath) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteCatalogReport(const wchar_t* reportPath)
{
    return FrostbiteUniversal::GetRuntime().WriteCatalogReport(reportPath) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_RunUniversalValidation()
{
    FrostbiteUniversal_Refresh();

    FrostbiteRuntimeInfo info = {};
    const bool hasInfo = FrostbiteUniversal_GetRuntimeInfo(&info) != 0;
    const bool frostbite = FrostbiteUniversal_IsFrostbiteProcess() != 0;
    const bool hasModules = FrostbiteUniversal_GetModuleCount() > 0;
    const bool hasExports = FrostbiteUniversal_GetExportCount() > 0;
    const bool hasOverlay = FrostbiteUniversal_OverlayIsRunning() != 0;

    const bool wroteRuntimeReport = FrostbiteUniversal_WriteRuntimeReport(nullptr) != 0;
    const bool wroteExportReport = FrostbiteUniversal_WriteExportReport(nullptr) != 0;

    std::wstringstream message;
    message << L"Universal validation: "
            << L"info=" << (hasInfo ? L"true" : L"false")
            << L", frostbite=" << (frostbite ? L"true" : L"false")
            << L", modules=" << (hasModules ? L"true" : L"false")
            << L", exports=" << (hasExports ? L"true" : L"false")
            << L", overlay=" << (hasOverlay ? L"true" : L"false")
            << L", runtimeReport=" << (wroteRuntimeReport ? L"true" : L"false")
            << L", exportReport=" << (wroteExportReport ? L"true" : L"false");
    FrostbiteUniversal::Log::Write(message.str());

    return (hasInfo && frostbite && hasModules && hasOverlay && wroteRuntimeReport && wroteExportReport) ? 1 : 0;
}
