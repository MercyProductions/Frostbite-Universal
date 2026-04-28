#pragma once

#include "FrostbiteUniversal.h"

#include <Windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace FrostbiteUniversal
{
    struct ModuleRecord
    {
        std::wstring name;
        std::wstring path;
        HMODULE handle = nullptr;
        std::uintptr_t baseAddress = 0;
        std::uint32_t imageSize = 0;
        std::uint32_t flags = FrostbiteModule_None;
    };

    struct ExportRecord
    {
        std::wstring moduleName;
        std::string name;
        std::uintptr_t address = 0;
        std::uint32_t moduleIndex = 0;
        std::uint32_t ordinal = 0;
        std::uint32_t flags = FrostbiteExport_None;
    };

    struct CatalogRecord
    {
        std::wstring name;
        std::wstring source;
        std::wstring path;
        std::uintptr_t address = 0;
        std::uint32_t flags = FrostbiteCatalog_None;
    };

    class Runtime
    {
    public:
        bool Initialize();
        bool Refresh();
        void Shutdown();

        bool IsInitialized() const;
        bool IsFrostbiteProcess() const;
        bool GetInfo(FrostbiteRuntimeInfo& outInfo) const;
        bool GetModuleInfo(std::uint32_t index, FrostbiteModuleInfo& outInfo) const;
        std::uint32_t GetModuleCount() const;
        bool GetExportInfo(std::uint32_t index, FrostbiteExportInfo& outInfo) const;
        std::uint32_t GetExportCount() const;
        bool GetCatalogInfo(std::uint32_t index, FrostbiteCatalogInfo& outInfo) const;
        std::uint32_t GetCatalogCount() const;
        void* GetExport(const wchar_t* moduleName, const char* exportName) const;
        bool WriteReport(const wchar_t* reportPath) const;
        bool WriteExportReport(const wchar_t* reportPath) const;
        bool WriteCatalogReport(const wchar_t* reportPath) const;

    private:
        void RebuildLocked();
        void InspectGameRootLocked();
        void UpdateRuntimeFlagsLocked();
        void EnumerateExportsLocked();
        void BuildActorModelCatalogLocked();

        mutable std::mutex m_mutex;
        bool m_initialized = false;
        std::wstring m_processName;
        std::wstring m_processPath;
        std::wstring m_gameRoot;
        std::wstring m_detectedTitle;
        std::vector<ModuleRecord> m_modules;
        std::vector<ExportRecord> m_exports;
        std::vector<CatalogRecord> m_catalog;
        std::uint32_t m_frostbiteModuleCount = 0;
        std::uint32_t m_tocFileCount = 0;
        std::uint32_t m_casFileCount = 0;
        std::uint32_t m_runtimeFlags = FrostbiteRuntime_None;
    };

    Runtime& GetRuntime();
}
