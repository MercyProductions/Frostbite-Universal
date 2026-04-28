#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>

#include "FrostbiteSDKGenerator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")

namespace fs = std::filesystem;

namespace
{
    constexpr std::uintmax_t kMaxPeReadSize = 256ull * 1024ull * 1024ull;
    constexpr std::size_t kMaxSymbolsPerLiveModule = 200000;
    constexpr std::uint32_t kSymTagFunction = 5;
    bool g_verboseProgress = true;
    HMODULE g_generatorModule = nullptr;

    struct ImportRecord
    {
        std::string module;
        std::string name;
        std::uint16_t ordinal = 0;
        bool byOrdinal = false;
    };

    struct SymbolRecord
    {
        std::string name;
        std::string undecoratedName;
        std::string signature;
        std::uint64_t address = 0;
        std::uint32_t size = 0;
        std::uint32_t flags = 0;
        std::uint32_t tag = 0;
        bool isFunction = false;
        bool hasSignature = false;
    };

    struct VTableFunctionRecord
    {
        std::uint32_t slot = 0;
        std::uint64_t address = 0;
        std::string symbolName;
        std::string signature;
    };

    enum class ModuleRelevance
    {
        GameSpecific,
        FrostbiteLikely,
        ThirdParty,
        WindowsSystem,
        Unknown
    };

    struct SectionScanRecord
    {
        std::string name;
        std::uint64_t address = 0;
        std::uint32_t rva = 0;
        std::uint32_t size = 0;
        std::uint32_t characteristics = 0;
        bool readable = false;
        bool writable = false;
        bool executable = false;
        std::uint32_t stringCount = 0;
        std::uint32_t pointerCount = 0;
        std::uint32_t rttiCount = 0;
        double pointerDensity = 0.0;
        double rttiDensity = 0.0;
    };

    struct FunctionRangeRecord
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint32_t unwindRva = 0;
        std::string source;
    };

    struct CallEdgeRecord
    {
        std::string module;
        std::uint64_t caller = 0;
        std::uint64_t callee = 0;
        std::uint64_t instruction = 0;
    };

    struct RttiClassRecord
    {
        std::string name;
        std::string decoratedName;
        std::string relevance;
        std::uint32_t confidence = 0;
        std::uint64_t typeDescriptor = 0;
        std::uint64_t completeObjectLocator = 0;
        std::uint64_t classHierarchyDescriptor = 0;
        std::uint64_t vtable = 0;
        std::uint32_t objectOffset = 0;
        std::uint32_t hierarchyAttributes = 0;
        std::vector<std::string> baseClasses;
        std::vector<VTableFunctionRecord> virtualFunctions;
    };

    struct LiveModuleRecord
    {
        std::wstring name;
        fs::path path;
        std::uint64_t baseAddress = 0;
        std::uint32_t imageSize = 0;
        ModuleRelevance relevance = ModuleRelevance::Unknown;
        std::uint32_t relevanceScore = 0;
        std::string symbolType;
        std::string loadedPdb;
        std::vector<SectionScanRecord> sections;
        std::vector<FunctionRangeRecord> functionRanges;
        std::vector<std::string> exports;
        std::vector<ImportRecord> imports;
        std::vector<SymbolRecord> symbols;
        std::vector<RttiClassRecord> classes;
    };

    struct RuntimeCVarRecord
    {
        std::string providerModule;
        std::string name;
        std::string category;
        std::string typeName;
        std::string currentValue;
        std::string defaultValue;
        std::string description;
        std::uint32_t valueType = FrostbiteSDKValue_Unknown;
        std::uint64_t address = 0;
        std::uint32_t flags = 0;
    };

    struct RuntimeSystemRecord
    {
        std::string providerModule;
        std::string name;
        std::string kind;
        std::string module;
        std::string description;
        std::uint64_t address = 0;
        std::uint32_t flags = 0;
    };

    struct RuntimeEnvironmentRecord
    {
        std::string providerModule;
        std::string name;
        std::string system;
        std::string typeName;
        std::string currentValue;
        std::string description;
        std::uint32_t valueType = FrostbiteSDKValue_Unknown;
        std::uint64_t address = 0;
        std::uint32_t flags = 0;
    };

    struct RuntimeFieldRecord
    {
        std::string name;
        std::string typeName;
        std::uint32_t offset = 0;
        std::uint32_t sizeBytes = 0;
        std::uint32_t flags = 0;
    };

    struct RuntimeTypeRecord
    {
        std::string providerModule;
        std::string namespaceName;
        std::string name;
        std::string kind;
        std::uint32_t sizeBytes = 0;
        std::uint64_t address = 0;
        std::uint32_t flags = 0;
        std::vector<RuntimeFieldRecord> fields;
    };

    struct RuntimeIntrospectionDump
    {
        std::vector<RuntimeCVarRecord> cvars;
        std::vector<RuntimeSystemRecord> systems;
        std::vector<RuntimeEnvironmentRecord> environment;
        std::vector<RuntimeTypeRecord> reflectedTypes;
    };

    struct DiscoveredStringRecord
    {
        std::string value;
        std::string module;
        std::string section;
        std::string encoding;
        std::string category;
        std::uint64_t address = 0;
        std::uint32_t score = 0;
        std::uint32_t xrefCount = 0;
    };

    struct StringXrefRecord
    {
        std::string value;
        std::string category;
        std::string module;
        std::string section;
        std::string surroundingBytes;
        std::vector<std::uint64_t> nearbyCallTargets;
        std::vector<std::string> nearbyFloatCandidates;
        std::uint64_t stringAddress = 0;
        std::uint64_t referenceAddress = 0;
        std::uint64_t functionAddress = 0;
        std::uint32_t mathOperationCount = 0;
        std::uint32_t score = 0;
    };

    struct FunctionCandidateRecord
    {
        std::string module;
        std::string primaryCategory;
        std::vector<std::string> categories;
        std::vector<std::string> relatedStrings;
        std::vector<std::uint64_t> stringAddresses;
        std::vector<std::uint64_t> referenceAddresses;
        std::vector<std::uint64_t> nearbyCallTargets;
        std::vector<std::string> nearbyFloatCandidates;
        std::vector<std::string> floatClassifications;
        std::string reasoning;
        std::string tier;
        std::string cluster;
        std::string boundarySource;
        std::string phaseGuess;
        std::uint64_t functionAddress = 0;
        std::uint64_t functionEnd = 0;
        std::uint32_t boundaryConfidence = 0;
        std::uint32_t xrefCount = 0;
        std::uint32_t mathOperationCount = 0;
        std::uint32_t score = 0;
        bool xrefAtFunctionStart = false;
        bool fallbackOnly = false;
        std::vector<std::uint64_t> callers;
        std::vector<std::uint64_t> callees;
    };

    struct DecomposedNameRecord
    {
        std::string original;
        std::string prefixSystem;
        std::string kind;
        std::string value;
        std::string category;
        std::string module;
        std::uint64_t address = 0;
        std::uint32_t score = 0;
    };

    struct EnumTableRecord
    {
        std::string name;
        std::string suspectedSystem;
        std::vector<std::string> values;
        std::vector<std::uint64_t> valueAddresses;
        std::vector<std::uint64_t> functionXrefs;
        std::uint64_t minAddress = 0;
        std::uint64_t maxAddress = 0;
        std::uint32_t confidence = 0;
    };

    struct WatchValueRecord
    {
        std::string candidateLabel;
        std::string classification;
        std::string volatility;
        std::string runtimeClass;
        std::string notes;
        std::uint64_t address = 0;
        float staticValue = 0.0f;
        bool readable = false;
        std::vector<float> samples;
    };

    struct DiscoveryDump
    {
        std::vector<DiscoveredStringRecord> strings;
        std::vector<StringXrefRecord> xrefs;
        std::vector<FunctionCandidateRecord> functionCandidates;
        std::vector<CallEdgeRecord> callEdges;
        std::vector<DecomposedNameRecord> decomposedNames;
        std::vector<EnumTableRecord> enumTables;
    };

    struct AssetReferenceRecord
    {
        std::string value;
        std::string kind;
        std::string file;
        std::uint64_t fileOffset = 0;
        std::uint32_t score = 0;
    };

    struct AssetReferenceDump
    {
        std::vector<AssetReferenceRecord> references;
    };

    std::wstring FormatCount(std::uint64_t value)
    {
        std::wstringstream out;
        out << value;
        return out.str();
    }

    void Progress(const std::wstring& message)
    {
        if (!g_verboseProgress)
            return;

        std::wstring line = L"[FrostbiteSDKGenerator] " + message;
        std::wcout << line << std::endl;
        ::OutputDebugStringW((line + L"\n").c_str());
    }

    void ProgressError(const std::wstring& message)
    {
        std::wstring line = L"[FrostbiteSDKGenerator] ERROR: " + message;
        std::wcerr << line << std::endl;
        ::OutputDebugStringW((line + L"\n").c_str());
    }

    bool EnsureConsole()
    {
        const bool alreadyHadConsole = ::GetConsoleWindow() != nullptr;
        if (!alreadyHadConsole)
        {
            if (!::AttachConsole(ATTACH_PARENT_PROCESS) && !::AllocConsole())
                return false;
        }

        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
        ::SetConsoleTitleW(L"FrostbiteSDKGenerator");
        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleCP(CP_UTF8);

        Progress(alreadyHadConsole
            ? L"Console already attached."
            : L"Console opened for live SDK generation output.");
        return true;
    }

    enum FileKind : std::uint32_t
    {
        FileKind_Unknown = 0,
        FileKind_GameExecutable = 1u << 0,
        FileKind_FrostbiteEngineDll = 1u << 1,
        FileKind_RenderDll = 1u << 2,
        FileKind_Toc = 1u << 3,
        FileKind_Cas = 1u << 4,
        FileKind_InitFs = 1u << 5,
        FileKind_Layout = 1u << 6,
        FileKind_BuildSettings = 1u << 7,
        FileKind_ChunkManifest = 1u << 8,
        FileKind_CompressionDependency = 1u << 9,
        FileKind_DirectStorageDependency = 1u << 10,
        FileKind_ThirdPartyDependency = 1u << 11,
        FileKind_AntiCheat = 1u << 12
    };

    struct Options
    {
        std::vector<fs::path> gameRoots;
        fs::path outputDir;
        std::string queryText;
        std::vector<std::uint64_t> traceAddresses;
        std::vector<fs::path> diffInputs;
        fs::path watchListPath;
        bool includeThirdParty = false;
        bool includeAntiCheat = false;
        std::size_t maxExportsPerModule = 160;
    };

    struct FileRecord
    {
        fs::path absolutePath;
        fs::path relativePath;
        std::wstring name;
        std::wstring extension;
        std::uintmax_t size = 0;
        std::uint32_t kind = FileKind_Unknown;
        bool exportScanSkipped = false;
        std::vector<std::string> exports;
        std::vector<ImportRecord> imports;
    };

    struct GameRecord
    {
        std::wstring title;
        fs::path root;
        std::vector<FileRecord> files;
        std::uint32_t tocCount = 0;
        std::uint32_t casCount = 0;
        std::uint32_t moduleCount = 0;
        std::uint32_t antiCheatSkippedCount = 0;
        std::uintmax_t dataBytes = 0;
    };

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
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

    std::string PathToUtf8(const fs::path& path)
    {
        return WideToUtf8(path.wstring());
    }

    std::wstring AnsiToWide(const std::string& value)
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

    fs::path GetModulePath(HMODULE module)
    {
        std::array<wchar_t, MAX_PATH> stackBuffer{};
        DWORD length = ::GetModuleFileNameW(module, stackBuffer.data(), static_cast<DWORD>(stackBuffer.size()));
        if (length > 0 && length < stackBuffer.size())
            return fs::path(stackBuffer.data());

        std::vector<wchar_t> buffer(32768);
        length = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return {};

        return fs::path(buffer.data());
    }

    std::wstring SanitizeFileName(std::wstring value)
    {
        if (value.empty())
            return L"unknown";

        for (wchar_t& ch : value)
        {
            switch (ch)
            {
            case L'<':
            case L'>':
            case L':':
            case L'"':
            case L'/':
            case L'\\':
            case L'|':
            case L'?':
            case L'*':
                ch = L'_';
                break;
            default:
                break;
            }
        }
        return value;
    }

    fs::path GetDefaultInjectedOutputDir()
    {
        fs::path base = fs::current_path();
        const fs::path generatorPath = GetModulePath(g_generatorModule);
        if (!generatorPath.empty())
        {
            const fs::path parent = generatorPath.parent_path();
            if (ToLower(parent.filename().wstring()) == L"tools" && parent.has_parent_path())
                base = parent.parent_path();
            else
                base = parent;
        }

        const fs::path processPath = GetModulePath(nullptr);
        const std::wstring processName = SanitizeFileName(processPath.empty()
            ? L"Process"
            : processPath.stem().wstring());

        std::wstringstream folderName;
        folderName << L"Injected_" << processName << L"_" << ::GetCurrentProcessId();
        return base / L"GeneratedSDK" / folderName.str();
    }

    std::string JsonEscape(const std::string& value)
    {
        std::ostringstream out;
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20)
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                else
                    out << static_cast<char>(ch);
                break;
            }
        }
        return out.str();
    }

    std::string HeaderEscape(const std::string& value)
    {
        std::string out;
        out.reserve(value.size() + 8);
        for (const char ch : value)
        {
            if (ch == '\\' || ch == '"')
                out.push_back('\\');
            out.push_back(ch);
        }
        return out;
    }

    std::string JoinStrings(const std::vector<std::string>& values, const char* separator)
    {
        std::string out;
        for (const std::string& value : values)
        {
            if (!out.empty())
                out += separator;
            out += value;
        }
        return out;
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string TrimAscii(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    bool ContainsAscii(const std::string& haystack, const char* needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    std::uint32_t ClampScore(int value)
    {
        if (value < 0)
            return 0;
        if (value > 100)
            return 100;
        return static_cast<std::uint32_t>(value);
    }

    std::string RelevanceToString(ModuleRelevance relevance)
    {
        switch (relevance)
        {
        case ModuleRelevance::GameSpecific: return "GameSpecific";
        case ModuleRelevance::FrostbiteLikely: return "FrostbiteLikely";
        case ModuleRelevance::ThirdParty: return "ThirdParty";
        case ModuleRelevance::WindowsSystem: return "WindowsSystem";
        default: return "Unknown";
        }
    }

    std::string UndecorateSymbol(const std::string& name)
    {
        if (name.empty())
            return {};

        std::array<char, 8192> buffer{};
        const DWORD result = ::UnDecorateSymbolName(
            name.c_str(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            UNDNAME_COMPLETE);

        if (result == 0)
            return name;

        return std::string(buffer.data());
    }

    bool LooksLikeFunctionSignature(const std::string& value)
    {
        return value.find('(') != std::string::npos &&
               value.find(')') != std::string::npos;
    }

    std::string SymTypeToString(SYM_TYPE type)
    {
        switch (type)
        {
        case SymNone: return "none";
        case SymCoff: return "coff";
        case SymCv: return "codeview";
        case SymPdb: return "pdb";
        case SymExport: return "export";
        case SymDeferred: return "deferred";
        case SymSym: return "sym";
        case SymDia: return "dia";
        case SymVirtual: return "virtual";
        default: return "unknown";
        }
    }

    template <std::size_t N>
    std::string FixedWideToUtf8(const wchar_t (&value)[N])
    {
        return WideToUtf8(std::wstring(value, wcsnlen_s(value, N)));
    }

    struct AddressRange
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };

    struct ModuleAddressRanges
    {
        std::vector<AddressRange> readableData;
        std::vector<AddressRange> executableCode;
        std::vector<SectionScanRecord> sections;
    };

    bool AddressInRange(std::uint64_t address, std::size_t size, const AddressRange& range)
    {
        if (address < range.begin)
            return false;

        const std::uint64_t end = address + static_cast<std::uint64_t>(size);
        if (end < address)
            return false;

        return end <= range.end;
    }

    bool AddressInRanges(std::uint64_t address, std::size_t size, const std::vector<AddressRange>& ranges)
    {
        for (const AddressRange& range : ranges)
        {
            if (AddressInRange(address, size, range))
                return true;
        }

        return false;
    }

    bool AddressInModule(const LiveModuleRecord& module, std::uint64_t address, std::size_t size = 1)
    {
        if (address < module.baseAddress)
            return false;

        const std::uint64_t end = address + static_cast<std::uint64_t>(size);
        if (end < address)
            return false;

        return end <= module.baseAddress + module.imageSize;
    }

    bool SafeReadBytes(std::uint64_t address, void* out, std::size_t size)
    {
        if (!address || !out || size == 0)
            return false;

        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T>
    bool SafeReadValue(std::uint64_t address, T& out)
    {
        return SafeReadBytes(address, &out, sizeof(T));
    }

    std::string SafeReadCString(std::uint64_t address, std::size_t maxLength)
    {
        std::string out;
        out.reserve(std::min<std::size_t>(maxLength, 256));

        for (std::size_t index = 0; index < maxLength; ++index)
        {
            char ch = 0;
            if (!SafeReadValue(address + index, ch))
                return {};
            if (ch == '\0')
                return out;
            if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t')
                return {};
            out.push_back(ch);
        }

        return {};
    }

    bool GetLoadedModuleRanges(const LiveModuleRecord& module, ModuleAddressRanges& outRanges)
    {
        outRanges = {};
        if (!module.baseAddress || module.imageSize < sizeof(IMAGE_DOS_HEADER))
            return false;

        IMAGE_DOS_HEADER dosCopy{};
        if (!SafeReadValue(module.baseAddress, dosCopy) || dosCopy.e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        IMAGE_NT_HEADERS64 nt{};
        const std::uint64_t ntAddress = module.baseAddress + static_cast<std::uint32_t>(dosCopy.e_lfanew);
        if (!SafeReadValue(ntAddress, nt) || nt.Signature != IMAGE_NT_SIGNATURE)
            return false;

        const std::uint64_t sectionAddress = ntAddress + FIELD_OFFSET(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        for (std::uint16_t index = 0; index < nt.FileHeader.NumberOfSections; ++index)
        {
            IMAGE_SECTION_HEADER section{};
            if (!SafeReadValue(sectionAddress + sizeof(IMAGE_SECTION_HEADER) * index, section))
                continue;

            const std::uint32_t virtualSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
            if (virtualSize == 0)
                continue;

            AddressRange range;
            range.begin = module.baseAddress + section.VirtualAddress;
            range.end = range.begin + virtualSize;
            const bool readable = (section.Characteristics & IMAGE_SCN_MEM_READ) != 0;
            const bool executable = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            const bool writable = (section.Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            const bool discardable = (section.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0;

            SectionScanRecord sectionRecord;
            char sectionName[9] = {};
            std::memcpy(sectionName, section.Name, 8);
            sectionRecord.name = sectionName;
            sectionRecord.address = range.begin;
            sectionRecord.rva = section.VirtualAddress;
            sectionRecord.size = virtualSize;
            sectionRecord.characteristics = section.Characteristics;
            sectionRecord.readable = readable;
            sectionRecord.writable = writable;
            sectionRecord.executable = executable;

            if (readable && virtualSize >= sizeof(std::uint64_t) && virtualSize <= 128u * 1024u * 1024u)
            {
                std::vector<std::uint8_t> bytes(virtualSize);
                if (SafeReadBytes(range.begin, bytes.data(), bytes.size()))
                {
                    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= bytes.size(); offset += sizeof(std::uint64_t))
                    {
                        std::uint64_t value = 0;
                        std::memcpy(&value, bytes.data() + offset, sizeof(value));
                        if (AddressInModule(module, value))
                            ++sectionRecord.pointerCount;
                    }

                    const double kb = std::max(1.0, static_cast<double>(virtualSize) / 1024.0);
                    sectionRecord.pointerDensity = static_cast<double>(sectionRecord.pointerCount) / kb;
                }
            }

            outRanges.sections.push_back(sectionRecord);

            if (readable && executable)
                outRanges.executableCode.push_back(range);
            else if (readable && !discardable)
                outRanges.readableData.push_back(range);
        }

        return !outRanges.readableData.empty() || !outRanges.executableCode.empty();
    }

    std::vector<FunctionRangeRecord> RecoverFunctionRangesFromPdata(const LiveModuleRecord& module)
    {
        std::vector<FunctionRangeRecord> ranges;
        if (!module.baseAddress || module.imageSize < sizeof(IMAGE_DOS_HEADER))
            return ranges;

        IMAGE_DOS_HEADER dos{};
        if (!SafeReadValue(module.baseAddress, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
            return ranges;

        IMAGE_NT_HEADERS64 nt{};
        const std::uint64_t ntAddress = module.baseAddress + static_cast<std::uint32_t>(dos.e_lfanew);
        if (!SafeReadValue(ntAddress, nt) || nt.Signature != IMAGE_NT_SIGNATURE)
            return ranges;

        const IMAGE_DATA_DIRECTORY& exceptionDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exceptionDir.VirtualAddress == 0 || exceptionDir.Size < sizeof(RUNTIME_FUNCTION))
            return ranges;

        const std::uint32_t count = exceptionDir.Size / sizeof(RUNTIME_FUNCTION);
        const std::uint64_t tableAddress = module.baseAddress + exceptionDir.VirtualAddress;
        ranges.reserve(std::min<std::uint32_t>(count, 200000));

        for (std::uint32_t index = 0; index < count && index < 200000; ++index)
        {
            RUNTIME_FUNCTION runtimeFunction{};
            if (!SafeReadValue(tableAddress + sizeof(RUNTIME_FUNCTION) * index, runtimeFunction))
                continue;
            if (runtimeFunction.BeginAddress >= runtimeFunction.EndAddress ||
                runtimeFunction.EndAddress > module.imageSize)
            {
                continue;
            }

            FunctionRangeRecord record;
            record.begin = module.baseAddress + runtimeFunction.BeginAddress;
            record.end = module.baseAddress + runtimeFunction.EndAddress;
            record.unwindRva = runtimeFunction.UnwindData;
            record.source = ".pdata";
            ranges.push_back(record);
        }

        std::sort(ranges.begin(), ranges.end(), [](const FunctionRangeRecord& lhs, const FunctionRangeRecord& rhs) {
            if (lhs.begin != rhs.begin)
                return lhs.begin < rhs.begin;
            return lhs.end < rhs.end;
        });

        ranges.erase(std::unique(ranges.begin(), ranges.end(), [](const FunctionRangeRecord& lhs, const FunctionRangeRecord& rhs) {
            return lhs.begin == rhs.begin && lhs.end == rhs.end;
        }), ranges.end());

        return ranges;
    }

    const FunctionRangeRecord* FindFunctionRange(const LiveModuleRecord& module, std::uint64_t address)
    {
        const auto it = std::upper_bound(
            module.functionRanges.begin(),
            module.functionRanges.end(),
            address,
            [](std::uint64_t value, const FunctionRangeRecord& range) {
                return value < range.begin;
            });

        if (it == module.functionRanges.begin())
            return nullptr;

        const auto candidate = std::prev(it);
        return address >= candidate->begin && address < candidate->end ? &(*candidate) : nullptr;
    }

    bool RvaToAddress(const LiveModuleRecord& module, std::int32_t rva, std::uint64_t& outAddress)
    {
        if (rva <= 0 || static_cast<std::uint32_t>(rva) >= module.imageSize)
            return false;

        outAddress = module.baseAddress + static_cast<std::uint32_t>(rva);
        return true;
    }

    std::string StripTypeDecorations(std::string name)
    {
        name = TrimAscii(name);
        const std::string classPrefix = "class ";
        const std::string structPrefix = "struct ";
        if (name.rfind(classPrefix, 0) == 0)
            name.erase(0, classPrefix.size());
        if (name.rfind(structPrefix, 0) == 0)
            name.erase(0, structPrefix.size());
        name = TrimAscii(name);

        while (name.rfind("??", 0) == 0)
            name.erase(0, 2);
        name = TrimAscii(name);

        if (name.rfind(".?AV", 0) == 0 || name.rfind(".?AU", 0) == 0)
            name.erase(0, 4);
        else if (name.rfind("?AV", 0) == 0 || name.rfind("?AU", 0) == 0)
            name.erase(0, 3);

        if (name.size() >= 2 && name.compare(name.size() - 2, 2, "@@") == 0)
            name.resize(name.size() - 2);

        if (name.find('@') != std::string::npos && name.find("::") == std::string::npos)
        {
            std::vector<std::string> parts;
            std::stringstream stream(name);
            std::string part;
            while (std::getline(stream, part, '@'))
            {
                if (!part.empty())
                    parts.push_back(part);
            }

            if (!parts.empty())
            {
                std::reverse(parts.begin(), parts.end());
                name = JoinStrings(parts, "::");
            }
        }

        return name;
    }

    std::string DecodeMsvcTypeDescriptorName(const std::string& decorated)
    {
        if (decorated.empty())
            return {};

        const std::string noLeadingDot = decorated[0] == '.' ? decorated.substr(1) : decorated;
        const std::string undecorated = StripTypeDecorations(UndecorateSymbol(noLeadingDot));
        if (!undecorated.empty() && undecorated != noLeadingDot && undecorated.find("@@") == std::string::npos)
            return StripTypeDecorations(undecorated);

        std::string body;
        if (decorated.rfind(".?AV", 0) == 0 || decorated.rfind(".?AU", 0) == 0)
            body = decorated.substr(4);
        else if (decorated.rfind("?AV", 0) == 0 || decorated.rfind("?AU", 0) == 0)
            body = decorated.substr(3);
        else
            return decorated;

        if (body.size() >= 2 && body.compare(body.size() - 2, 2, "@@") == 0)
            body.resize(body.size() - 2);

        std::vector<std::string> parts;
        std::stringstream stream(body);
        std::string part;
        while (std::getline(stream, part, '@'))
        {
            if (!part.empty())
                parts.push_back(part);
        }

        std::reverse(parts.begin(), parts.end());
        std::string decoded;
        for (const std::string& namespacePart : parts)
        {
            if (!decoded.empty())
                decoded += "::";
            decoded += namespacePart;
        }

        return StripTypeDecorations(decoded.empty() ? decorated : decoded);
    }

    bool LooksLikeTypeDescriptorName(const std::string& name)
    {
        return name.rfind(".?AV", 0) == 0 ||
               name.rfind(".?AU", 0) == 0 ||
               name.rfind("?AV", 0) == 0 ||
               name.rfind("?AU", 0) == 0;
    }

#pragma pack(push, 1)
    struct MsvcCompleteObjectLocator64
    {
        std::uint32_t signature;
        std::uint32_t offset;
        std::uint32_t cdOffset;
        std::int32_t typeDescriptorRva;
        std::int32_t classDescriptorRva;
        std::int32_t selfRva;
    };

    struct MsvcClassHierarchyDescriptor64
    {
        std::uint32_t signature;
        std::uint32_t attributes;
        std::uint32_t numBaseClasses;
        std::int32_t baseClassArrayRva;
    };

    struct MsvcBaseClassDescriptor64
    {
        std::int32_t typeDescriptorRva;
        std::uint32_t numContainedBases;
        std::int32_t mdisp;
        std::int32_t pdisp;
        std::int32_t vdisp;
        std::uint32_t attributes;
        std::int32_t classHierarchyDescriptorRva;
    };
#pragma pack(pop)

    bool ReadMsvcTypeDescriptorName(const ModuleAddressRanges& ranges, std::uint64_t typeDescriptor, std::string& decorated, std::string& decoded)
    {
        if (!AddressInRanges(typeDescriptor, sizeof(void*) * 2 + 4, ranges.readableData))
            return false;

        decorated = SafeReadCString(typeDescriptor + sizeof(void*) * 2, 512);
        if (!LooksLikeTypeDescriptorName(decorated))
            return false;

        decoded = DecodeMsvcTypeDescriptorName(decorated);
        return !decoded.empty();
    }

    bool ReadMsvcClassFromCompleteObjectLocator(
        const LiveModuleRecord& module,
        const ModuleAddressRanges& ranges,
        std::uint64_t colAddress,
        std::uint64_t vtableAddress,
        RttiClassRecord& outClass)
    {
        if (!AddressInRanges(colAddress, sizeof(MsvcCompleteObjectLocator64), ranges.readableData))
            return false;

        MsvcCompleteObjectLocator64 col{};
        if (!SafeReadValue(colAddress, col))
            return false;
        if (col.signature != 1 || col.offset > 0x100000)
            return false;

        std::uint64_t selfAddress = 0;
        if (!RvaToAddress(module, col.selfRva, selfAddress) || selfAddress != colAddress)
            return false;

        std::uint64_t typeDescriptor = 0;
        std::uint64_t classHierarchy = 0;
        if (!RvaToAddress(module, col.typeDescriptorRva, typeDescriptor) ||
            !RvaToAddress(module, col.classDescriptorRva, classHierarchy))
        {
            return false;
        }

        std::string decorated;
        std::string decoded;
        if (!ReadMsvcTypeDescriptorName(ranges, typeDescriptor, decorated, decoded))
            return false;

        outClass = {};
        outClass.name = decoded;
        outClass.decoratedName = decorated;
        outClass.typeDescriptor = typeDescriptor;
        outClass.completeObjectLocator = colAddress;
        outClass.classHierarchyDescriptor = classHierarchy;
        outClass.vtable = vtableAddress;
        outClass.objectOffset = col.offset;

        MsvcClassHierarchyDescriptor64 hierarchy{};
        if (AddressInRanges(classHierarchy, sizeof(hierarchy), ranges.readableData) && SafeReadValue(classHierarchy, hierarchy))
        {
            outClass.hierarchyAttributes = hierarchy.attributes;
            if (hierarchy.numBaseClasses > 0 && hierarchy.numBaseClasses <= 128)
            {
                std::uint64_t baseClassArray = 0;
                if (RvaToAddress(module, hierarchy.baseClassArrayRva, baseClassArray) &&
                    AddressInRanges(baseClassArray, sizeof(std::int32_t) * hierarchy.numBaseClasses, ranges.readableData))
                {
                    for (std::uint32_t index = 0; index < hierarchy.numBaseClasses; ++index)
                    {
                        std::int32_t baseClassRva = 0;
                        if (!SafeReadValue(baseClassArray + sizeof(std::int32_t) * index, baseClassRva))
                            continue;

                        std::uint64_t baseClassAddress = 0;
                        if (!RvaToAddress(module, baseClassRva, baseClassAddress))
                            continue;

                        MsvcBaseClassDescriptor64 baseClass{};
                        if (!AddressInRanges(baseClassAddress, sizeof(baseClass), ranges.readableData) ||
                            !SafeReadValue(baseClassAddress, baseClass))
                        {
                            continue;
                        }

                        std::uint64_t baseTypeDescriptor = 0;
                        if (!RvaToAddress(module, baseClass.typeDescriptorRva, baseTypeDescriptor))
                            continue;

                        std::string baseDecorated;
                        std::string baseDecoded;
                        if (!ReadMsvcTypeDescriptorName(ranges, baseTypeDescriptor, baseDecorated, baseDecoded))
                            continue;

                        if (baseDecoded != outClass.name &&
                            std::find(outClass.baseClasses.begin(), outClass.baseClasses.end(), baseDecoded) == outClass.baseClasses.end())
                        {
                            outClass.baseClasses.push_back(baseDecoded);
                        }
                    }
                }
            }
        }

        return true;
    }

    const SymbolRecord* FindSymbolForAddress(const std::vector<SymbolRecord>& symbols, std::uint64_t address)
    {
        const SymbolRecord* nearest = nullptr;
        for (const SymbolRecord& symbol : symbols)
        {
            if (symbol.address == address)
                return &symbol;

            if (symbol.address < address && symbol.size > 0 && address < symbol.address + symbol.size)
                nearest = &symbol;
        }

        return nearest;
    }

    std::uint32_t ScoreRttiClass(const LiveModuleRecord& module, const RttiClassRecord& record)
    {
        int score = static_cast<int>(module.relevanceScore);
        const std::string lowerName = ToLowerAscii(record.name);
        const std::string lowerBases = ToLowerAscii(JoinStrings(record.baseClasses, ";"));

        if (lowerName.rfind("fb::", 0) == 0 || lowerName.find("::fb::") != std::string::npos)
            score += 25;
        if (ContainsAscii(lowerName, "game") ||
            ContainsAscii(lowerName, "world") ||
            ContainsAscii(lowerName, "level") ||
            ContainsAscii(lowerName, "scene") ||
            ContainsAscii(lowerName, "entity") ||
            ContainsAscii(lowerName, "component") ||
            ContainsAscii(lowerName, "transform") ||
            ContainsAscii(lowerName, "render") ||
            ContainsAscii(lowerName, "camera") ||
            ContainsAscii(lowerName, "sky") ||
            ContainsAscii(lowerName, "fog") ||
            ContainsAscii(lowerName, "time") ||
            ContainsAscii(lowerName, "tick") ||
            ContainsAscii(lowerName, "physics"))
        {
            score += 20;
        }

        if (ContainsAscii(lowerBases, "entity") ||
            ContainsAscii(lowerBases, "component") ||
            ContainsAscii(lowerBases, "system"))
        {
            score += 10;
        }

        if (record.virtualFunctions.size() >= 4)
            score += 5;
        if (module.relevance == ModuleRelevance::ThirdParty)
            score -= 20;
        if (module.relevance == ModuleRelevance::WindowsSystem)
            score -= 40;

        return ClampScore(score);
    }

    std::string ClassRelevanceLabel(const LiveModuleRecord& module, const RttiClassRecord& record)
    {
        const std::string lowerName = ToLowerAscii(record.name);
        if (module.relevance == ModuleRelevance::GameSpecific)
            return "GameSpecific";
        if (module.relevance == ModuleRelevance::FrostbiteLikely ||
            lowerName.rfind("fb::", 0) == 0 ||
            ContainsAscii(lowerName, "frostbite") ||
            ContainsAscii(lowerName, "entity") ||
            ContainsAscii(lowerName, "component") ||
            ContainsAscii(lowerName, "world") ||
            ContainsAscii(lowerName, "render"))
        {
            return "FrostbiteLikely";
        }
        if (module.relevance == ModuleRelevance::ThirdParty)
            return "ThirdParty";
        if (module.relevance == ModuleRelevance::WindowsSystem)
            return "WindowsSystem";
        return "Unknown";
    }

    void PopulateVTableFunctions(const LiveModuleRecord& module, const ModuleAddressRanges& ranges, RttiClassRecord& record)
    {
        constexpr std::uint32_t kMaxVirtualFunctionSlots = 256;
        for (std::uint32_t slot = 0; slot < kMaxVirtualFunctionSlots; ++slot)
        {
            std::uint64_t functionAddress = 0;
            const std::uint64_t slotAddress = record.vtable + sizeof(void*) * slot;
            if (!AddressInRanges(slotAddress, sizeof(void*), ranges.readableData) ||
                !SafeReadValue(slotAddress, functionAddress) ||
                !AddressInRanges(functionAddress, 1, ranges.executableCode))
            {
                break;
            }

            VTableFunctionRecord function;
            function.slot = slot;
            function.address = functionAddress;

            if (const SymbolRecord* symbol = FindSymbolForAddress(module.symbols, functionAddress))
            {
                function.symbolName = symbol->undecoratedName.empty() ? symbol->name : symbol->undecoratedName;
                function.signature = symbol->signature;
            }

            record.virtualFunctions.push_back(std::move(function));
        }
    }

    void CaptureRttiForModule(LiveModuleRecord& module, bool verbose)
    {
        ModuleAddressRanges ranges;
        if (!GetLoadedModuleRanges(module, ranges))
            return;
        module.sections = ranges.sections;
        module.functionRanges = RecoverFunctionRangesFromPdata(module);

        std::map<std::pair<std::uint64_t, std::uint64_t>, RttiClassRecord> discovered;
        for (const AddressRange& range : ranges.readableData)
        {
            for (std::uint64_t address = range.begin; address + sizeof(void*) * 2 <= range.end; address += sizeof(void*))
            {
                std::uint64_t colAddress = 0;
                if (!SafeReadValue(address, colAddress) ||
                    !AddressInRanges(colAddress, sizeof(MsvcCompleteObjectLocator64), ranges.readableData))
                {
                    continue;
                }

                RttiClassRecord candidate;
                const std::uint64_t vtableAddress = address + sizeof(void*);
                if (!ReadMsvcClassFromCompleteObjectLocator(module, ranges, colAddress, vtableAddress, candidate))
                    continue;

                std::uint64_t firstVirtual = 0;
                if (!SafeReadValue(vtableAddress, firstVirtual) ||
                    !AddressInRanges(firstVirtual, 1, ranges.executableCode))
                {
                    continue;
                }

                PopulateVTableFunctions(module, ranges, candidate);
                if (candidate.virtualFunctions.empty())
                    continue;

                discovered.emplace(std::make_pair(candidate.typeDescriptor, candidate.vtable), std::move(candidate));
            }
        }

        module.classes.clear();
        module.classes.reserve(discovered.size());
        for (auto& [_, record] : discovered)
        {
            record.name = StripTypeDecorations(record.name);
            for (std::string& baseClass : record.baseClasses)
                baseClass = StripTypeDecorations(baseClass);
            record.relevance = ClassRelevanceLabel(module, record);
            record.confidence = ScoreRttiClass(module, record);
            module.classes.push_back(std::move(record));
        }

        std::sort(module.classes.begin(), module.classes.end(), [](const RttiClassRecord& lhs, const RttiClassRecord& rhs) {
            if (lhs.confidence != rhs.confidence)
                return lhs.confidence > rhs.confidence;
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.vtable < rhs.vtable;
        });

        for (RttiClassRecord& record : module.classes)
        {
            for (SectionScanRecord& section : module.sections)
            {
                if (AddressInRange(record.completeObjectLocator, sizeof(MsvcCompleteObjectLocator64), { section.address, section.address + section.size }) ||
                    AddressInRange(record.vtable, sizeof(void*), { section.address, section.address + section.size }))
                {
                    ++section.rttiCount;
                    const double kb = std::max(1.0, static_cast<double>(section.size) / 1024.0);
                    section.rttiDensity = static_cast<double>(section.rttiCount) / kb;
                }
            }
        }

        if (verbose && !module.classes.empty())
            Progress(L"MSVC RTTI scan: " + module.name + L" -> classes=" + FormatCount(module.classes.size()));
        if (verbose && !module.functionRanges.empty())
            Progress(L"PE .pdata unwind scan: " + module.name + L" -> functions=" + FormatCount(module.functionRanges.size()));
    }

    std::wstring GuessTitle(const fs::path& root)
    {
        std::wstring title = root.filename().wstring();
        return title.empty() ? L"Unknown Frostbite Game" : title;
    }

    bool IsAntiCheatName(const std::wstring& lowerName)
    {
        return lowerName.find(L"eaanticheat") != std::wstring::npos ||
               lowerName.find(L"javelin") != std::wstring::npos ||
               lowerName == L"preloader_l.dll" ||
               lowerName == L"preloader_l.dll_b";
    }

    std::uint32_t ClassifyFile(const fs::path& path, const fs::path& root)
    {
        const std::wstring lowerName = ToLower(path.filename().wstring());
        const std::wstring lowerExt = ToLower(path.extension().wstring());
        const std::wstring relative = ToLower(fs::relative(path, root).wstring());

        std::uint32_t kind = FileKind_Unknown;

        if (IsAntiCheatName(lowerName))
            kind |= FileKind_AntiCheat;

        if (lowerExt == L".exe" &&
            (lowerName != L"eaanticheat.gameservicelauncher.exe") &&
            lowerName.find(L"installer") == std::wstring::npos)
        {
            kind |= FileKind_GameExecutable;
        }

        if (lowerName == L"engine.buildinfo.dll" ||
            StartsWith(lowerName, L"engine."))
        {
            kind |= FileKind_FrostbiteEngineDll;
        }

        if (lowerName == L"engine.render.core2.platformpcdx12.dll" ||
            lowerName == L"engine.render.core2.platformpcdx11.dll")
        {
            kind |= FileKind_RenderDll;
        }

        if (lowerExt == L".toc")
            kind |= FileKind_Toc;

        if (lowerExt == L".cas")
            kind |= FileKind_Cas;

        if (lowerName == L"initfs_win32")
            kind |= FileKind_InitFs;

        if (lowerName == L"layout.toc")
            kind |= FileKind_Layout | FileKind_Toc;

        if (lowerExt == L".buildsettings")
            kind |= FileKind_BuildSettings;

        if (lowerName == L"chunkmanifest")
            kind |= FileKind_ChunkManifest;

        if (StartsWith(lowerName, L"oo2"))
            kind |= FileKind_CompressionDependency;

        if (lowerName == L"dstorage.dll" || lowerName == L"dstoragecore.dll")
            kind |= FileKind_DirectStorageDependency;

        if (StartsWith(lowerName, L"steam_api") ||
            StartsWith(lowerName, L"sl.") ||
            StartsWith(lowerName, L"nvngx_") ||
            StartsWith(lowerName, L"libxess") ||
            StartsWith(lowerName, L"amd_ags") ||
            lowerName == L"dxcompiler.dll" ||
            lowerName == L"dxil.dll" ||
            lowerName == L"d3d12core.dll" ||
            lowerName == L"d3d10warp.dll" ||
            lowerName == L"gfsdk_aftermath_lib.x64.dll" ||
            relative.find(L"__installer") != std::wstring::npos)
        {
            kind |= FileKind_ThirdPartyDependency;
        }

        return kind;
    }

    bool ShouldIncludeFile(std::uint32_t kind, const Options& options)
    {
        if ((kind & FileKind_AntiCheat) != 0)
            return options.includeAntiCheat;

        const std::uint32_t defaultKinds =
            FileKind_GameExecutable |
            FileKind_FrostbiteEngineDll |
            FileKind_RenderDll |
            FileKind_Toc |
            FileKind_Cas |
            FileKind_InitFs |
            FileKind_Layout |
            FileKind_BuildSettings |
            FileKind_ChunkManifest |
            FileKind_CompressionDependency |
            FileKind_DirectStorageDependency;

        if ((kind & defaultKinds) != 0)
            return true;

        if ((kind & FileKind_ThirdPartyDependency) != 0)
            return options.includeThirdParty;

        return false;
    }

    bool IsThirdPartyModuleName(const std::wstring& lowerName)
    {
        return StartsWith(lowerName, L"steam_api") ||
               StartsWith(lowerName, L"sl.") ||
               StartsWith(lowerName, L"nvngx_") ||
               StartsWith(lowerName, L"libxess") ||
               StartsWith(lowerName, L"amd_ags") ||
               StartsWith(lowerName, L"oo2") ||
               StartsWith(lowerName, L"vcruntime") ||
               StartsWith(lowerName, L"msvcp") ||
               StartsWith(lowerName, L"ucrtbase") ||
               lowerName == L"dxcompiler.dll" ||
               lowerName == L"dxil.dll" ||
               lowerName == L"d3d12core.dll" ||
               lowerName == L"d3d10warp.dll" ||
               lowerName == L"gfsdk_aftermath_lib.x64.dll";
    }

    bool IsScannerSelfModuleName(const std::wstring& lowerName)
    {
        return lowerName.find(L"frostbitesdkgenerator") != std::wstring::npos ||
               lowerName.find(L"frostbiteuniversal") != std::wstring::npos ||
               lowerName.find(L"imgui") != std::wstring::npos ||
               lowerName == L"minhook.x64.dll" ||
               lowerName == L"minhook.dll";
    }

    bool IsWindowsSystemPath(const std::wstring& lowerPath)
    {
        return lowerPath.find(L"\\windows\\system32\\") != std::wstring::npos ||
               lowerPath.find(L"\\windows\\syswow64\\") != std::wstring::npos ||
               lowerPath.find(L"\\windows\\winsxs\\") != std::wstring::npos;
    }

    ModuleRelevance ClassifyLiveModuleRelevance(const LiveModuleRecord& module)
    {
        const std::wstring lowerName = ToLower(module.name);
        const std::wstring lowerPath = ToLower(module.path.wstring());
        const std::wstring processPath = ToLower(GetModulePath(nullptr).wstring());
        const fs::path processDir = GetModulePath(nullptr).parent_path();
        const std::wstring lowerProcessDir = ToLower(processDir.wstring());

        if (IsWindowsSystemPath(lowerPath))
            return ModuleRelevance::WindowsSystem;

        if (IsScannerSelfModuleName(lowerName))
            return ModuleRelevance::ThirdParty;

        if (IsThirdPartyModuleName(lowerName))
            return ModuleRelevance::ThirdParty;

        if (StartsWith(lowerName, L"engine.") ||
            lowerName.find(L"frostbite") != std::wstring::npos ||
            lowerName.find(L"render.core2") != std::wstring::npos)
        {
            return ModuleRelevance::FrostbiteLikely;
        }

        if (!processPath.empty() && lowerPath == processPath)
            return ModuleRelevance::GameSpecific;

        if (!lowerProcessDir.empty() && lowerPath.rfind(lowerProcessDir, 0) == 0)
        {
            if (IsScannerSelfModuleName(lowerName))
            {
                return ModuleRelevance::ThirdParty;
            }

            return ModuleRelevance::GameSpecific;
        }

        return ModuleRelevance::Unknown;
    }

    std::uint32_t ScoreLiveModule(const LiveModuleRecord& module)
    {
        int score = 10;
        switch (module.relevance)
        {
        case ModuleRelevance::GameSpecific: score = 70; break;
        case ModuleRelevance::FrostbiteLikely: score = 65; break;
        case ModuleRelevance::Unknown: score = 25; break;
        case ModuleRelevance::ThirdParty: score = 10; break;
        case ModuleRelevance::WindowsSystem: score = 0; break;
        }

        const std::wstring lowerName = ToLower(module.name);
        if (EndsWith(lowerName, L".exe"))
            score += 20;
        if (StartsWith(lowerName, L"engine.render"))
            score += 20;
        if (lowerName == L"engine.buildinfo.dll")
            score += 15;
        if (lowerName.find(L"render") != std::wstring::npos)
            score += 10;
        if (lowerName.find(L"game") != std::wstring::npos)
            score += 10;

        return ClampScore(score);
    }

    bool IsPrimaryDiscoveryModule(const LiveModuleRecord& module)
    {
        if (IsScannerSelfModuleName(ToLower(module.name)))
            return false;

        return module.relevance == ModuleRelevance::GameSpecific ||
               module.relevance == ModuleRelevance::FrostbiteLikely;
    }

    std::string KindToString(std::uint32_t kind)
    {
        std::vector<const char*> names;

        if ((kind & FileKind_GameExecutable) != 0) names.push_back("game_executable");
        if ((kind & FileKind_FrostbiteEngineDll) != 0) names.push_back("frostbite_engine_dll");
        if ((kind & FileKind_RenderDll) != 0) names.push_back("render_dll");
        if ((kind & FileKind_Toc) != 0) names.push_back("toc");
        if ((kind & FileKind_Cas) != 0) names.push_back("cas");
        if ((kind & FileKind_InitFs) != 0) names.push_back("initfs");
        if ((kind & FileKind_Layout) != 0) names.push_back("layout");
        if ((kind & FileKind_BuildSettings) != 0) names.push_back("build_settings");
        if ((kind & FileKind_ChunkManifest) != 0) names.push_back("chunk_manifest");
        if ((kind & FileKind_CompressionDependency) != 0) names.push_back("compression_dependency");
        if ((kind & FileKind_DirectStorageDependency) != 0) names.push_back("direct_storage_dependency");
        if ((kind & FileKind_ThirdPartyDependency) != 0) names.push_back("third_party_dependency");
        if ((kind & FileKind_AntiCheat) != 0) names.push_back("anti_cheat");

        if (names.empty())
            return "unknown";

        std::ostringstream out;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (i != 0)
                out << ",";
            out << names[i];
        }
        return out.str();
    }

    bool ReadFileBytes(const fs::path& path, std::vector<std::uint8_t>& outBytes)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        outBytes.assign(std::istreambuf_iterator<char>(file), {});
        return !outBytes.empty();
    }

    bool BoundsCheck(std::size_t offset, std::size_t size, std::size_t total)
    {
        return offset <= total && size <= total - offset;
    }

    std::optional<std::uint32_t> RvaToOffset(std::uint32_t rva, const IMAGE_SECTION_HEADER* sections, WORD sectionCount)
    {
        for (WORD i = 0; i < sectionCount; ++i)
        {
            const IMAGE_SECTION_HEADER& section = sections[i];
            const std::uint32_t virtualAddress = section.VirtualAddress;
            const std::uint32_t virtualSize = std::max(section.Misc.VirtualSize, section.SizeOfRawData);

            if (rva >= virtualAddress && rva < virtualAddress + virtualSize)
                return section.PointerToRawData + (rva - virtualAddress);
        }

        return std::nullopt;
    }

    const char* ReadNullTerminatedString(const std::vector<std::uint8_t>& bytes, std::uint32_t offset)
    {
        if (offset >= bytes.size())
            return nullptr;

        const char* value = reinterpret_cast<const char*>(bytes.data() + offset);
        const std::size_t maxLength = bytes.size() - offset;
        for (std::size_t i = 0; i < maxLength; ++i)
        {
            if (value[i] == '\0')
                return value;
        }

        return nullptr;
    }

    std::vector<std::string> ParsePeExports(const fs::path& path, bool& skipped, bool verbose)
    {
        skipped = false;

        std::error_code ec;
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size > kMaxPeReadSize)
        {
            skipped = size > kMaxPeReadSize;
            if (verbose)
            {
                Progress(skipped
                    ? L"Export scan skipped, file too large: " + path.wstring()
                    : L"Export scan skipped, file size unavailable: " + path.wstring());
            }
            return {};
        }

        std::vector<std::uint8_t> bytes;
        if (!ReadFileBytes(path, bytes) || bytes.size() < sizeof(IMAGE_DOS_HEADER))
        {
            if (verbose)
                Progress(L"Export scan found no readable PE data: " + path.wstring());
            return {};
        }

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return {};

        const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
        if (!BoundsCheck(ntOffset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), bytes.size()))
            return {};

        const DWORD signature = *reinterpret_cast<const DWORD*>(bytes.data() + ntOffset);
        if (signature != IMAGE_NT_SIGNATURE)
            return {};

        const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(bytes.data() + ntOffset + sizeof(DWORD));
        const std::size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!BoundsCheck(optionalOffset, fileHeader->SizeOfOptionalHeader, bytes.size()))
            return {};

        IMAGE_DATA_DIRECTORY exportDirectory = {};
        const IMAGE_SECTION_HEADER* sections = nullptr;
        const WORD magic = *reinterpret_cast<const WORD*>(bytes.data() + optionalOffset);

        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
                return {};

            const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(bytes.data() + optionalOffset);
            exportDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            sections = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + ntOffset));
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
                return {};

            const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(bytes.data() + optionalOffset);
            exportDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            sections = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + ntOffset));
        }
        else
        {
            return {};
        }

        if (exportDirectory.VirtualAddress == 0 || exportDirectory.Size == 0)
            return {};

        const auto exportOffset = RvaToOffset(exportDirectory.VirtualAddress, sections, fileHeader->NumberOfSections);
        if (!exportOffset || !BoundsCheck(*exportOffset, sizeof(IMAGE_EXPORT_DIRECTORY), bytes.size()))
            return {};

        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(bytes.data() + *exportOffset);
        const auto namesOffset = RvaToOffset(exports->AddressOfNames, sections, fileHeader->NumberOfSections);
        if (!namesOffset || !BoundsCheck(*namesOffset, sizeof(std::uint32_t) * exports->NumberOfNames, bytes.size()))
            return {};

        std::vector<std::string> names;
        const auto* nameRvas = reinterpret_cast<const std::uint32_t*>(bytes.data() + *namesOffset);

        for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        {
            const auto nameOffset = RvaToOffset(nameRvas[i], sections, fileHeader->NumberOfSections);
            if (!nameOffset)
                continue;

            if (const char* name = ReadNullTerminatedString(bytes, *nameOffset))
                names.emplace_back(name);
        }

        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());

        if (verbose)
            Progress(L"Export scan complete: " + path.wstring() + L" -> " + FormatCount(names.size()) + L" named exports");

        return names;
    }

    std::vector<ImportRecord> ParsePeImports(const fs::path& path, bool verbose)
    {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size > kMaxPeReadSize)
            return {};

        std::vector<std::uint8_t> bytes;
        if (!ReadFileBytes(path, bytes) || bytes.size() < sizeof(IMAGE_DOS_HEADER))
            return {};

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return {};

        const std::size_t ntOffset = static_cast<std::size_t>(dos->e_lfanew);
        if (!BoundsCheck(ntOffset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), bytes.size()))
            return {};

        const DWORD signature = *reinterpret_cast<const DWORD*>(bytes.data() + ntOffset);
        if (signature != IMAGE_NT_SIGNATURE)
            return {};

        const auto* fileHeader = reinterpret_cast<const IMAGE_FILE_HEADER*>(bytes.data() + ntOffset + sizeof(DWORD));
        const std::size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!BoundsCheck(optionalOffset, fileHeader->SizeOfOptionalHeader, bytes.size()))
            return {};

        IMAGE_DATA_DIRECTORY importDirectory = {};
        const IMAGE_SECTION_HEADER* sections = nullptr;
        bool is64 = false;
        const WORD magic = *reinterpret_cast<const WORD*>(bytes.data() + optionalOffset);

        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
                return {};

            const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(bytes.data() + optionalOffset);
            importDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            sections = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes.data() + ntOffset));
            is64 = true;
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            if (fileHeader->SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
                return {};

            const auto* optional = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(bytes.data() + optionalOffset);
            importDirectory = optional->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            sections = IMAGE_FIRST_SECTION(reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + ntOffset));
        }
        else
        {
            return {};
        }

        if (importDirectory.VirtualAddress == 0 || importDirectory.Size == 0)
            return {};

        const auto importOffset = RvaToOffset(importDirectory.VirtualAddress, sections, fileHeader->NumberOfSections);
        if (!importOffset || !BoundsCheck(*importOffset, sizeof(IMAGE_IMPORT_DESCRIPTOR), bytes.size()))
            return {};

        std::vector<ImportRecord> imports;
        for (std::size_t descriptorIndex = 0; descriptorIndex < 4096; ++descriptorIndex)
        {
            const std::size_t descriptorOffset = *importOffset + descriptorIndex * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            if (!BoundsCheck(descriptorOffset, sizeof(IMAGE_IMPORT_DESCRIPTOR), bytes.size()))
                break;

            const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(bytes.data() + descriptorOffset);
            if (descriptor->OriginalFirstThunk == 0 && descriptor->FirstThunk == 0 && descriptor->Name == 0)
                break;

            const auto moduleNameOffset = RvaToOffset(descriptor->Name, sections, fileHeader->NumberOfSections);
            const char* moduleName = moduleNameOffset ? ReadNullTerminatedString(bytes, *moduleNameOffset) : nullptr;
            const std::string module = moduleName ? moduleName : "";

            const std::uint32_t thunkRva = descriptor->OriginalFirstThunk != 0
                ? descriptor->OriginalFirstThunk
                : descriptor->FirstThunk;
            const auto thunkOffset = RvaToOffset(thunkRva, sections, fileHeader->NumberOfSections);
            if (!thunkOffset)
                continue;

            for (std::size_t thunkIndex = 0; thunkIndex < 65536; ++thunkIndex)
            {
                ImportRecord record;
                record.module = module;

                if (is64)
                {
                    const std::size_t currentThunkOffset = *thunkOffset + thunkIndex * sizeof(IMAGE_THUNK_DATA64);
                    if (!BoundsCheck(currentThunkOffset, sizeof(IMAGE_THUNK_DATA64), bytes.size()))
                        break;

                    const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(bytes.data() + currentThunkOffset);
                    if (thunk->u1.AddressOfData == 0)
                        break;

                    if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal))
                    {
                        record.byOrdinal = true;
                        record.ordinal = static_cast<std::uint16_t>(IMAGE_ORDINAL64(thunk->u1.Ordinal));
                    }
                    else
                    {
                        const auto importByNameOffset = RvaToOffset(static_cast<std::uint32_t>(thunk->u1.AddressOfData), sections, fileHeader->NumberOfSections);
                        if (!importByNameOffset || !BoundsCheck(*importByNameOffset, sizeof(WORD), bytes.size()))
                            continue;

                        const char* name = ReadNullTerminatedString(bytes, *importByNameOffset + sizeof(WORD));
                        if (name)
                            record.name = name;
                    }
                }
                else
                {
                    const std::size_t currentThunkOffset = *thunkOffset + thunkIndex * sizeof(IMAGE_THUNK_DATA32);
                    if (!BoundsCheck(currentThunkOffset, sizeof(IMAGE_THUNK_DATA32), bytes.size()))
                        break;

                    const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA32*>(bytes.data() + currentThunkOffset);
                    if (thunk->u1.AddressOfData == 0)
                        break;

                    if (IMAGE_SNAP_BY_ORDINAL32(thunk->u1.Ordinal))
                    {
                        record.byOrdinal = true;
                        record.ordinal = static_cast<std::uint16_t>(IMAGE_ORDINAL32(thunk->u1.Ordinal));
                    }
                    else
                    {
                        const auto importByNameOffset = RvaToOffset(thunk->u1.AddressOfData, sections, fileHeader->NumberOfSections);
                        if (!importByNameOffset || !BoundsCheck(*importByNameOffset, sizeof(WORD), bytes.size()))
                            continue;

                        const char* name = ReadNullTerminatedString(bytes, *importByNameOffset + sizeof(WORD));
                        if (name)
                            record.name = name;
                    }
                }

                if (!record.module.empty() && (record.byOrdinal || !record.name.empty()))
                    imports.push_back(std::move(record));
            }
        }

        if (verbose && !imports.empty())
            Progress(L"Import scan complete: " + path.wstring() + L" -> " + FormatCount(imports.size()) + L" imports");

        return imports;
    }

    GameRecord ScanGameRoot(const fs::path& root, const Options& options, bool verbose)
    {
        GameRecord game;
        game.root = root;
        game.title = GuessTitle(root);

        if (!fs::exists(root))
            return game;

        if (verbose)
        {
            Progress(L"------------------------------------------------------------");
            Progress(L"Scanning game root: " + root.wstring());
        }

        std::error_code ec;
        std::uint64_t visitedCount = 0;
        std::uint64_t includedCount = 0;
        std::uint64_t skippedCount = 0;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
        {
            if (ec || !entry.is_regular_file())
                continue;

            ++visitedCount;
            if (verbose && (visitedCount % 1000) == 0)
                Progress(L"Visited " + FormatCount(visitedCount) + L" files so far under " + root.wstring());

            const std::uint32_t kind = ClassifyFile(entry.path(), root);
            const fs::path relativePath = fs::relative(entry.path(), root);
            if ((kind & FileKind_AntiCheat) != 0 && !options.includeAntiCheat)
            {
                ++game.antiCheatSkippedCount;
                ++skippedCount;
                if (verbose)
                    Progress(L"Skipping anti-cheat file: " + relativePath.wstring());
                continue;
            }

            if (!ShouldIncludeFile(kind, options))
            {
                ++skippedCount;
                if (verbose && (kind & FileKind_ThirdPartyDependency) != 0)
                    Progress(L"Skipping third-party dependency: " + relativePath.wstring());
                continue;
            }

            FileRecord record;
            record.absolutePath = entry.path();
            record.relativePath = relativePath;
            record.name = entry.path().filename().wstring();
            record.extension = ToLower(entry.path().extension().wstring());
            record.size = entry.file_size();
            record.kind = kind;
            ++includedCount;

            if (verbose)
            {
                const std::string kindText = KindToString(kind);
                Progress(L"Including: " + record.relativePath.wstring() + L" [" + std::wstring(kindText.begin(), kindText.end()) + L"]");
            }

            if ((kind & FileKind_Toc) != 0)
                ++game.tocCount;

            if ((kind & FileKind_Cas) != 0)
                ++game.casCount;

            if ((kind & (FileKind_Toc | FileKind_Cas | FileKind_InitFs | FileKind_Layout | FileKind_ChunkManifest)) != 0)
                game.dataBytes += record.size;

            const bool isModule = record.extension == L".dll" || record.extension == L".exe";
            if (isModule)
            {
                ++game.moduleCount;
                if (verbose)
                    Progress(L"Scanning PE exports: " + record.relativePath.wstring());
                record.exports = ParsePeExports(entry.path(), record.exportScanSkipped, verbose);
                if (verbose)
                    Progress(L"Scanning PE imports: " + record.relativePath.wstring());
                record.imports = ParsePeImports(entry.path(), verbose);
            }

            game.files.push_back(std::move(record));
        }

        std::sort(game.files.begin(), game.files.end(), [](const FileRecord& lhs, const FileRecord& rhs) {
            return lhs.relativePath.native() < rhs.relativePath.native();
        });

        if (verbose)
        {
            Progress(L"Finished game root: " + root.wstring());
            Progress(L"Visited files: " + FormatCount(visitedCount));
            Progress(L"Included files: " + FormatCount(includedCount));
            Progress(L"Skipped files: " + FormatCount(skippedCount));
            Progress(L"TOC files: " + FormatCount(game.tocCount));
            Progress(L"CAS files: " + FormatCount(game.casCount));
            Progress(L"Modules: " + FormatCount(game.moduleCount));
            Progress(L"Anti-cheat skipped: " + FormatCount(game.antiCheatSkippedCount));
        }

        return game;
    }

    struct SymbolEnumContext
    {
        std::vector<SymbolRecord>* symbols = nullptr;
    };

    BOOL CALLBACK EnumSymbolCallback(PSYMBOL_INFO symbol, ULONG, PVOID userContext)
    {
        auto* context = reinterpret_cast<SymbolEnumContext*>(userContext);
        if (!context || !context->symbols || !symbol || context->symbols->size() >= kMaxSymbolsPerLiveModule)
            return TRUE;

        SymbolRecord record;
        record.name.assign(symbol->Name, symbol->NameLen);
        record.undecoratedName = UndecorateSymbol(record.name);
        record.signature = LooksLikeFunctionSignature(record.undecoratedName) ? record.undecoratedName : "";
        record.hasSignature = !record.signature.empty();
        record.address = static_cast<std::uint64_t>(symbol->Address);
        record.size = symbol->Size;
        record.flags = symbol->Flags;
        record.tag = symbol->Tag;
        record.isFunction =
            symbol->Tag == kSymTagFunction ||
            (symbol->Flags & SYMFLAG_FUNCTION) != 0 ||
            record.hasSignature;

        context->symbols->push_back(std::move(record));
        return TRUE;
    }

    void CaptureSymbolsForModule(HANDLE process, LiveModuleRecord& module, bool verbose)
    {
        const std::wstring modulePath = module.path.wstring();
        const DWORD64 loadedBase = ::SymLoadModuleExW(
            process,
            nullptr,
            modulePath.empty() ? nullptr : modulePath.c_str(),
            nullptr,
            module.baseAddress,
            module.imageSize,
            nullptr,
            0);

        if (loadedBase == 0)
        {
            if (verbose)
                Progress(L"DbgHelp symbols unavailable for: " + module.path.wstring());
            module.symbolType = "unavailable";
            return;
        }

        IMAGEHLP_MODULE64 moduleInfo{};
        moduleInfo.SizeOfStruct = sizeof(moduleInfo);
        if (::SymGetModuleInfo64(process, loadedBase, &moduleInfo))
        {
            module.symbolType = SymTypeToString(moduleInfo.SymType);
            if (moduleInfo.LoadedPdbName[0] != '\0')
                module.loadedPdb = moduleInfo.LoadedPdbName;
        }
        else
        {
            module.symbolType = "loaded";
        }

        SymbolEnumContext context;
        context.symbols = &module.symbols;
        ::SymEnumSymbols(process, loadedBase, nullptr, EnumSymbolCallback, &context);

        std::sort(module.symbols.begin(), module.symbols.end(), [](const SymbolRecord& lhs, const SymbolRecord& rhs) {
            if (lhs.address != rhs.address)
                return lhs.address < rhs.address;
            return lhs.name < rhs.name;
        });

        if (verbose)
        {
            std::uint64_t functionCount = 0;
            std::uint64_t signatureCount = 0;
            for (const SymbolRecord& symbol : module.symbols)
            {
                if (symbol.isFunction)
                    ++functionCount;
                if (symbol.hasSignature)
                    ++signatureCount;
            }

            Progress(L"DbgHelp scan: " + module.name +
                L" -> symbols=" + FormatCount(module.symbols.size()) +
                L", functions=" + FormatCount(functionCount) +
                L", signatures=" + FormatCount(signatureCount) +
                L", source=" + AnsiToWide(module.symbolType));
        }
    }

    std::vector<LiveModuleRecord> CaptureLiveProcessModules(bool verbose)
    {
        std::vector<LiveModuleRecord> modules;
        const DWORD processId = ::GetCurrentProcessId();

        if (verbose)
        {
            Progress(L"============================================================");
            Progress(L"Capturing live process modules.");
            Progress(L"Process ID: " + FormatCount(processId));
            Progress(L"Process path: " + GetModulePath(nullptr).wstring());
        }

        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            if (verbose)
                ProgressError(L"CreateToolhelp32Snapshot failed.");
            return modules;
        }

        HANDLE process = ::GetCurrentProcess();
        const DWORD previousOptions = ::SymGetOptions();
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        const BOOL symbolsReady = ::SymInitialize(process, nullptr, FALSE);

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = ::Module32FirstW(snapshot, &entry); ok; ok = ::Module32NextW(snapshot, &entry))
        {
            const std::wstring lowerName = ToLower(entry.szModule);
            if (IsAntiCheatName(lowerName))
            {
                if (verbose)
                    Progress(L"Skipping anti-cheat module in live snapshot: " + std::wstring(entry.szModule));
                continue;
            }

            LiveModuleRecord module;
            module.name = entry.szModule;
            module.path = entry.szExePath;
            module.baseAddress = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            module.imageSize = entry.modBaseSize;
            module.relevance = ClassifyLiveModuleRelevance(module);
            module.relevanceScore = ScoreLiveModule(module);

            if (verbose)
                Progress(L"Live module: " + module.name + L" @ 0x" + [&]() {
                    std::wstringstream out;
                    out << std::hex << module.baseAddress;
                    return out.str();
                }() + L" [" + AnsiToWide(RelevanceToString(module.relevance)) + L"]");

            bool exportSkipped = false;
            module.exports = ParsePeExports(module.path, exportSkipped, false);
            module.imports = ParsePeImports(module.path, false);

            if (symbolsReady)
                CaptureSymbolsForModule(process, module, verbose);
            else
                module.symbolType = "dbghelp_init_failed";

            CaptureRttiForModule(module, verbose);

            modules.push_back(std::move(module));
        }

        if (symbolsReady)
            ::SymCleanup(process);
        ::SymSetOptions(previousOptions);
        ::CloseHandle(snapshot);

        std::sort(modules.begin(), modules.end(), [](const LiveModuleRecord& lhs, const LiveModuleRecord& rhs) {
            return lhs.baseAddress < rhs.baseAddress;
        });

        if (verbose)
            Progress(L"Live process capture complete. Modules captured: " + FormatCount(modules.size()));

        return modules;
    }

    using SdkProviderGetCountFn = std::uint32_t(__stdcall*)();
    using SdkProviderGetCVarInfoFn = int(__stdcall*)(std::uint32_t index, FrostbiteSDKCVarInfo* outInfo);
    using SdkProviderGetSystemInfoFn = int(__stdcall*)(std::uint32_t index, FrostbiteSDKSystemInfo* outInfo);
    using SdkProviderGetEnvironmentInfoFn = int(__stdcall*)(std::uint32_t index, FrostbiteSDKEnvironmentInfo* outInfo);
    using SdkProviderGetTypeInfoFn = int(__stdcall*)(std::uint32_t index, FrostbiteSDKTypeInfo* outInfo);
    using SdkProviderGetFieldInfoFn = int(__stdcall*)(std::uint32_t typeIndex, std::uint32_t fieldIndex, FrostbiteSDKFieldInfo* outInfo);

    bool SafeProviderGetCount(SdkProviderGetCountFn function, std::uint32_t& outCount)
    {
        if (!function)
            return false;

        __try
        {
            outCount = function();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outCount = 0;
            return false;
        }
    }

    bool SafeProviderGetCVarInfo(SdkProviderGetCVarInfoFn function, std::uint32_t index, FrostbiteSDKCVarInfo& outInfo)
    {
        if (!function)
            return false;

        __try
        {
            return function(index, &outInfo) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeProviderGetSystemInfo(SdkProviderGetSystemInfoFn function, std::uint32_t index, FrostbiteSDKSystemInfo& outInfo)
    {
        if (!function)
            return false;

        __try
        {
            return function(index, &outInfo) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeProviderGetEnvironmentInfo(SdkProviderGetEnvironmentInfoFn function, std::uint32_t index, FrostbiteSDKEnvironmentInfo& outInfo)
    {
        if (!function)
            return false;

        __try
        {
            return function(index, &outInfo) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeProviderGetTypeInfo(SdkProviderGetTypeInfoFn function, std::uint32_t index, FrostbiteSDKTypeInfo& outInfo)
    {
        if (!function)
            return false;

        __try
        {
            return function(index, &outInfo) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SafeProviderGetFieldInfo(SdkProviderGetFieldInfoFn function, std::uint32_t typeIndex, std::uint32_t fieldIndex, FrostbiteSDKFieldInfo& outInfo)
    {
        if (!function)
            return false;

        __try
        {
            return function(typeIndex, fieldIndex, &outInfo) != 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    RuntimeIntrospectionDump CaptureRuntimeIntrospection(const std::vector<LiveModuleRecord>& modules, bool verbose)
    {
        constexpr std::uint32_t kMaxProviderItems = 20000;
        RuntimeIntrospectionDump dump;

        for (const LiveModuleRecord& module : modules)
        {
            HMODULE providerModule = reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(module.baseAddress));
            if (!providerModule)
                continue;

            const std::string providerName = WideToUtf8(module.name);

            auto* getCVarCount = reinterpret_cast<SdkProviderGetCountFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetCVarCount"));
            auto* getCVarInfo = reinterpret_cast<SdkProviderGetCVarInfoFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetCVarInfo"));
            if (getCVarCount && getCVarInfo)
            {
                std::uint32_t count = 0;
                if (SafeProviderGetCount(getCVarCount, count))
                {
                    count = std::min(count, kMaxProviderItems);
                    for (std::uint32_t index = 0; index < count; ++index)
                    {
                        FrostbiteSDKCVarInfo info{};
                        info.size = sizeof(info);
                        if (!SafeProviderGetCVarInfo(getCVarInfo, index, info))
                            continue;

                        RuntimeCVarRecord record;
                        record.providerModule = providerName;
                        record.name = FixedWideToUtf8(info.name);
                        record.category = FixedWideToUtf8(info.category);
                        record.typeName = FixedWideToUtf8(info.typeName);
                        record.valueType = info.valueType;
                        record.currentValue = FixedWideToUtf8(info.currentValue);
                        record.defaultValue = FixedWideToUtf8(info.defaultValue);
                        record.description = FixedWideToUtf8(info.description);
                        record.address = info.address;
                        record.flags = info.flags;
                        dump.cvars.push_back(std::move(record));
                    }

                    if (verbose)
                        Progress(L"Owned SDK CVar provider: " + module.name + L" -> cvars=" + FormatCount(count));
                }
            }

            auto* getSystemCount = reinterpret_cast<SdkProviderGetCountFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetSystemCount"));
            auto* getSystemInfo = reinterpret_cast<SdkProviderGetSystemInfoFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetSystemInfo"));
            if (getSystemCount && getSystemInfo)
            {
                std::uint32_t count = 0;
                if (SafeProviderGetCount(getSystemCount, count))
                {
                    count = std::min(count, kMaxProviderItems);
                    for (std::uint32_t index = 0; index < count; ++index)
                    {
                        FrostbiteSDKSystemInfo info{};
                        info.size = sizeof(info);
                        if (!SafeProviderGetSystemInfo(getSystemInfo, index, info))
                            continue;

                        RuntimeSystemRecord record;
                        record.providerModule = providerName;
                        record.name = FixedWideToUtf8(info.name);
                        record.kind = FixedWideToUtf8(info.kind);
                        record.module = FixedWideToUtf8(info.module);
                        record.description = FixedWideToUtf8(info.description);
                        record.address = info.address;
                        record.flags = info.flags;
                        dump.systems.push_back(std::move(record));
                    }

                    if (verbose)
                        Progress(L"Owned SDK system provider: " + module.name + L" -> systems=" + FormatCount(count));
                }
            }

            auto* getEnvironmentCount = reinterpret_cast<SdkProviderGetCountFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetEnvironmentCount"));
            auto* getEnvironmentInfo = reinterpret_cast<SdkProviderGetEnvironmentInfoFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetEnvironmentInfo"));
            if (getEnvironmentCount && getEnvironmentInfo)
            {
                std::uint32_t count = 0;
                if (SafeProviderGetCount(getEnvironmentCount, count))
                {
                    count = std::min(count, kMaxProviderItems);
                    for (std::uint32_t index = 0; index < count; ++index)
                    {
                        FrostbiteSDKEnvironmentInfo info{};
                        info.size = sizeof(info);
                        if (!SafeProviderGetEnvironmentInfo(getEnvironmentInfo, index, info))
                            continue;

                        RuntimeEnvironmentRecord record;
                        record.providerModule = providerName;
                        record.name = FixedWideToUtf8(info.name);
                        record.system = FixedWideToUtf8(info.system);
                        record.typeName = FixedWideToUtf8(info.typeName);
                        record.valueType = info.valueType;
                        record.currentValue = FixedWideToUtf8(info.currentValue);
                        record.description = FixedWideToUtf8(info.description);
                        record.address = info.address;
                        record.flags = info.flags;
                        dump.environment.push_back(std::move(record));
                    }

                    if (verbose)
                        Progress(L"Owned SDK environment provider: " + module.name + L" -> values=" + FormatCount(count));
                }
            }

            auto* getTypeCount = reinterpret_cast<SdkProviderGetCountFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetTypeCount"));
            auto* getTypeInfo = reinterpret_cast<SdkProviderGetTypeInfoFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetTypeInfo"));
            auto* getFieldInfo = reinterpret_cast<SdkProviderGetFieldInfoFn>(::GetProcAddress(providerModule, "FrostbiteSDK_GetFieldInfo"));
            if (getTypeCount && getTypeInfo)
            {
                std::uint32_t count = 0;
                if (SafeProviderGetCount(getTypeCount, count))
                {
                    count = std::min(count, kMaxProviderItems);
                    for (std::uint32_t typeIndex = 0; typeIndex < count; ++typeIndex)
                    {
                        FrostbiteSDKTypeInfo info{};
                        info.size = sizeof(info);
                        if (!SafeProviderGetTypeInfo(getTypeInfo, typeIndex, info))
                            continue;

                        RuntimeTypeRecord record;
                        record.providerModule = providerName;
                        record.namespaceName = FixedWideToUtf8(info.namespaceName);
                        record.name = FixedWideToUtf8(info.name);
                        record.kind = FixedWideToUtf8(info.kind);
                        record.sizeBytes = info.sizeBytes;
                        record.address = info.address;
                        record.flags = info.flags;

                        const std::uint32_t fieldCount = std::min(info.fieldCount, kMaxProviderItems);
                        if (getFieldInfo)
                        {
                            for (std::uint32_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex)
                            {
                                FrostbiteSDKFieldInfo fieldInfo{};
                                fieldInfo.size = sizeof(fieldInfo);
                                if (!SafeProviderGetFieldInfo(getFieldInfo, typeIndex, fieldIndex, fieldInfo))
                                    continue;

                                RuntimeFieldRecord field;
                                field.name = FixedWideToUtf8(fieldInfo.name);
                                field.typeName = FixedWideToUtf8(fieldInfo.typeName);
                                field.offset = fieldInfo.offset;
                                field.sizeBytes = fieldInfo.sizeBytes;
                                field.flags = fieldInfo.flags;
                                record.fields.push_back(std::move(field));
                            }
                        }

                        dump.reflectedTypes.push_back(std::move(record));
                    }

                    if (verbose)
                        Progress(L"Owned SDK reflection provider: " + module.name + L" -> types=" + FormatCount(count));
                }
            }
        }

        std::sort(dump.cvars.begin(), dump.cvars.end(), [](const RuntimeCVarRecord& lhs, const RuntimeCVarRecord& rhs) {
            if (lhs.category != rhs.category)
                return lhs.category < rhs.category;
            return lhs.name < rhs.name;
        });
        std::sort(dump.systems.begin(), dump.systems.end(), [](const RuntimeSystemRecord& lhs, const RuntimeSystemRecord& rhs) {
            if (lhs.kind != rhs.kind)
                return lhs.kind < rhs.kind;
            return lhs.name < rhs.name;
        });
        std::sort(dump.environment.begin(), dump.environment.end(), [](const RuntimeEnvironmentRecord& lhs, const RuntimeEnvironmentRecord& rhs) {
            if (lhs.system != rhs.system)
                return lhs.system < rhs.system;
            return lhs.name < rhs.name;
        });
        std::sort(dump.reflectedTypes.begin(), dump.reflectedTypes.end(), [](const RuntimeTypeRecord& lhs, const RuntimeTypeRecord& rhs) {
            if (lhs.namespaceName != rhs.namespaceName)
                return lhs.namespaceName < rhs.namespaceName;
            return lhs.name < rhs.name;
        });

        if (verbose)
        {
            Progress(L"Runtime introspection capture complete: cvars=" + FormatCount(dump.cvars.size()) +
                L", systems=" + FormatCount(dump.systems.size()) +
                L", environment=" + FormatCount(dump.environment.size()) +
                L", reflectedTypes=" + FormatCount(dump.reflectedTypes.size()));
        }

        return dump;
    }

    const SectionScanRecord* FindSectionForAddress(const LiveModuleRecord& module, std::uint64_t address, std::size_t size = 1)
    {
        for (const SectionScanRecord& section : module.sections)
        {
            if (AddressInRange(address, size, { section.address, section.address + section.size }))
                return &section;
        }

        return nullptr;
    }

    SectionScanRecord* FindMutableSectionForAddress(LiveModuleRecord& module, std::uint64_t address, std::size_t size = 1)
    {
        for (SectionScanRecord& section : module.sections)
        {
            if (AddressInRange(address, size, { section.address, section.address + section.size }))
                return &section;
        }

        return nullptr;
    }

    bool IsReadableDataAddress(const LiveModuleRecord& module, std::uint64_t address, std::size_t size)
    {
        const SectionScanRecord* section = FindSectionForAddress(module, address, size);
        return section && section->readable && !section->executable;
    }

    bool IsPrintableAscii(unsigned char ch)
    {
        return ch >= 0x20 && ch <= 0x7e;
    }

    bool ContainsAnyKeyword(const std::string& lower, const std::vector<const char*>& keywords)
    {
        for (const char* keyword : keywords)
        {
            if (lower.find(keyword) != std::string::npos)
                return true;
        }
        return false;
    }

    std::string GuessStringCategory(const std::string& value)
    {
        const std::string lower = ToLowerAscii(value);
        static const std::vector<const char*> timeKeywords = {
            "timescale", "timescale", "deltatime", "gametime", "worldtime", "tick", "tickrate", "fixeddelta", "pause", "simulation"
        };
        static const std::vector<const char*> environmentKeywords = {
            "skybox", "sky", "skylight", "sun", "moon", "fog", "atmosphere", "exposure", "cubemap", "reflection", "probe", "environment"
        };
        static const std::vector<const char*> renderKeywords = {
            "render", "renderer", "view", "camera", "dx12", "shader", "material", "texture", "lighting", "postprocess", "post-process"
        };
        static const std::vector<const char*> entityKeywords = {
            "entity", "component", "transform", "position", "rotation", "world", "level", "scene", "blueprint"
        };
        static const std::vector<const char*> physicsKeywords = {
            "physics", "rigidbody", "collision", "gravity", "mass", "velocity", "acceleration", "force"
        };
        static const std::vector<const char*> audioKeywords = {
            "audio", "sound", "music", "voice", "volume", "mixer", "reverb"
        };
        static const std::vector<const char*> uiKeywords = {
            "ui", "hud", "menu", "widget", "font", "screen", "reticle"
        };
        static const std::vector<const char*> debugKeywords = {
            "debug", "assert", "profile", "telemetry", "trace", "warning", "error"
        };
        static const std::vector<const char*> consoleKeywords = {
            "cvar", "console", "command", "variable", "config", "cfg", "ini"
        };
        static const std::vector<const char*> assetKeywords = {
            ".dds", ".png", ".tga", ".jpg", ".mesh", ".texture", ".asset", ".toc", ".cas", ".bundle", "/data/", "\\data\\"
        };

        if (ContainsAnyKeyword(lower, timeKeywords)) return "Time";
        if (ContainsAnyKeyword(lower, environmentKeywords)) return "Environment";
        if (ContainsAnyKeyword(lower, renderKeywords)) return "Rendering";
        if (ContainsAnyKeyword(lower, entityKeywords)) return "Entity";
        if (ContainsAnyKeyword(lower, physicsKeywords)) return "Physics";
        if (ContainsAnyKeyword(lower, audioKeywords)) return "Audio";
        if (ContainsAnyKeyword(lower, uiKeywords)) return "UI";
        if (ContainsAnyKeyword(lower, debugKeywords)) return "Debug";
        if (ContainsAnyKeyword(lower, consoleKeywords)) return "Console/CVar";
        if (ContainsAnyKeyword(lower, assetKeywords)) return "Asset/Resource";
        return {};
    }

    bool HasExactHighValueKeyword(const std::string& lower)
    {
        static const std::vector<const char*> exactish = {
            "timescale", "deltatime", "tickrate", "skybox", "skylight", "environment", "exposure",
            "cubemap", "render", "renderer", "entity", "component", "transform", "world", "level",
            "scene", "fog", "sun", "atmosphere"
        };
        return ContainsAnyKeyword(lower, exactish);
    }

    std::uint32_t ScoreDiscoveredString(const LiveModuleRecord& module, const std::string& value, const std::string& category)
    {
        int score = static_cast<int>(module.relevanceScore);
        const std::string lower = ToLowerAscii(value);
        if (HasExactHighValueKeyword(lower))
            score += 20;
        if (category == "Time" || category == "Environment" || category == "Rendering" || category == "Console/CVar")
            score += 10;
        if (module.relevance == ModuleRelevance::ThirdParty)
            score -= 20;
        if (module.relevance == ModuleRelevance::WindowsSystem)
            score -= 30;
        if (value.size() > 160)
            score -= 10;
        return ClampScore(score);
    }

    int KeywordStrengthScore(const std::string& value, const std::string& category)
    {
        const std::string lower = ToLowerAscii(value);
        int score = 0;
        if (category == "Time")
        {
            if (ContainsAscii(lower, "timescale") || ContainsAscii(lower, "timescale"))
                score += 30;
            if (ContainsAscii(lower, "deltatime") || ContainsAscii(lower, "tickrate") || ContainsAscii(lower, "fixeddelta"))
                score += 25;
            if (ContainsAscii(lower, "tick") || ContainsAscii(lower, "simulation") || ContainsAscii(lower, "pause"))
                score += 15;
        }
        else if (category == "Environment" || category == "Rendering")
        {
            if (ContainsAscii(lower, "skybox") || ContainsAscii(lower, "environment") || ContainsAscii(lower, "cubemap"))
                score += 30;
            if (ContainsAscii(lower, "fog") || ContainsAscii(lower, "sun") || ContainsAscii(lower, "atmosphere") || ContainsAscii(lower, "exposure"))
                score += 25;
            if (ContainsAscii(lower, "render") || ContainsAscii(lower, "lighting") || ContainsAscii(lower, "shader") || ContainsAscii(lower, "material"))
                score += 15;
        }
        else if (category == "Entity")
        {
            if (ContainsAscii(lower, "entity") || ContainsAscii(lower, "component") || ContainsAscii(lower, "transform"))
                score += 20;
            if (ContainsAscii(lower, "world") || ContainsAscii(lower, "level") || ContainsAscii(lower, "scene"))
                score += 15;
        }
        else if (category == "Physics")
        {
            if (ContainsAscii(lower, "physics") || ContainsAscii(lower, "gravity") || ContainsAscii(lower, "collision"))
                score += 20;
        }
        else if (category == "Audio")
        {
            if (ContainsAscii(lower, "audio") || ContainsAscii(lower, "sound") || ContainsAscii(lower, "music"))
                score += 15;
        }

        if (HasExactHighValueKeyword(lower))
            score += 10;
        return score;
    }

    void AddDiscoveredString(
        LiveModuleRecord& module,
        DiscoveryDump& dump,
        std::unordered_map<std::uint64_t, std::size_t>& stringAddressToIndex,
        const std::string& value,
        std::uint64_t address,
        const std::string& sectionName,
        const char* encoding)
    {
        if (value.size() < 3 || value.size() > 512)
            return;

        const std::string category = GuessStringCategory(value);
        if (category.empty())
            return;

        DiscoveredStringRecord record;
        record.value = value;
        record.module = WideToUtf8(module.name);
        record.section = sectionName;
        record.encoding = encoding;
        record.category = category;
        record.address = address;
        record.score = ScoreDiscoveredString(module, value, category);
        if (record.score < 25)
            return;

        if (stringAddressToIndex.find(address) != stringAddressToIndex.end())
            return;

        if (SectionScanRecord* section = FindMutableSectionForAddress(module, address, 1))
            ++section->stringCount;

        stringAddressToIndex[address] = dump.strings.size();
        dump.strings.push_back(std::move(record));
    }

    void ScanAsciiStringsInSection(
        LiveModuleRecord& module,
        DiscoveryDump& dump,
        std::unordered_map<std::uint64_t, std::size_t>& stringAddressToIndex,
        const SectionScanRecord& section,
        const std::vector<std::uint8_t>& bytes)
    {
        std::size_t index = 0;
        while (index < bytes.size())
        {
            if (!IsPrintableAscii(bytes[index]))
            {
                ++index;
                continue;
            }

            const std::size_t start = index;
            std::string value;
            while (index < bytes.size() && IsPrintableAscii(bytes[index]))
            {
                value.push_back(static_cast<char>(bytes[index]));
                ++index;
            }

            AddDiscoveredString(module, dump, stringAddressToIndex, value, section.address + start, section.name, "ascii");
        }
    }

    void ScanUtf16StringsInSection(
        LiveModuleRecord& module,
        DiscoveryDump& dump,
        std::unordered_map<std::uint64_t, std::size_t>& stringAddressToIndex,
        const SectionScanRecord& section,
        const std::vector<std::uint8_t>& bytes)
    {
        for (std::size_t index = 0; index + 1 < bytes.size(); ++index)
        {
            if (bytes[index + 1] != 0 || !IsPrintableAscii(bytes[index]))
                continue;

            const std::size_t start = index;
            std::string value;
            while (index + 1 < bytes.size() && bytes[index + 1] == 0 && IsPrintableAscii(bytes[index]))
            {
                value.push_back(static_cast<char>(bytes[index]));
                index += 2;
            }

            AddDiscoveredString(module, dump, stringAddressToIndex, value, section.address + start, section.name, "utf16");
        }
    }

    bool TryDecodeRipRelativeReference(
        const std::vector<std::uint8_t>& bytes,
        std::size_t offset,
        std::uint64_t instructionAddress,
        std::uint64_t& outTarget,
        std::size_t& outLength)
    {
        auto readDisp = [&](std::size_t dispOffset, std::size_t instructionLength) -> bool {
            if (offset + dispOffset + sizeof(std::int32_t) > bytes.size())
                return false;
            std::int32_t disp = 0;
            std::memcpy(&disp, bytes.data() + offset + dispOffset, sizeof(disp));
            outTarget = instructionAddress + instructionLength + disp;
            outLength = instructionLength;
            return true;
        };

        if (offset + 7 <= bytes.size() &&
            (bytes[offset] == 0x48 || bytes[offset] == 0x4C) &&
            (bytes[offset + 1] == 0x8D || bytes[offset + 1] == 0x8B || bytes[offset + 1] == 0x39 || bytes[offset + 1] == 0x3B) &&
            ((bytes[offset + 2] & 0xC7) == 0x05))
        {
            return readDisp(3, 7);
        }

        if (offset + 6 <= bytes.size() &&
            (bytes[offset] == 0x8D || bytes[offset] == 0x8B || bytes[offset] == 0x39 || bytes[offset] == 0x3B) &&
            ((bytes[offset + 1] & 0xC7) == 0x05))
        {
            return readDisp(2, 6);
        }

        return false;
    }

    std::string BytesToHex(const std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t count)
    {
        std::ostringstream out;
        const std::size_t end = std::min(bytes.size(), start + count);
        for (std::size_t index = start; index < end; ++index)
        {
            if (index != start)
                out << ' ';
            out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[index]);
        }
        return out.str();
    }

    std::uint64_t GuessFunctionStart(const LiveModuleRecord& module, const std::vector<std::uint8_t>& bytes, const SectionScanRecord& section, std::size_t referenceOffset)
    {
        const std::uint64_t referenceAddress = section.address + referenceOffset;
        if (const FunctionRangeRecord* range = FindFunctionRange(module, referenceAddress))
            return range->begin;

        const SymbolRecord* best = nullptr;
        for (const SymbolRecord& symbol : module.symbols)
        {
            if (!symbol.isFunction || symbol.address > referenceAddress)
                continue;
            if (referenceAddress - symbol.address > 0x4000)
                continue;
            if (!best || symbol.address > best->address)
                best = &symbol;
        }
        if (best)
            return best->address;

        const std::size_t searchStart = referenceOffset > 160 ? referenceOffset - 160 : 0;
        for (std::size_t index = referenceOffset; index > searchStart; --index)
        {
            if (index + 4 >= bytes.size())
                continue;
            if ((bytes[index] == 0x40 && (bytes[index + 1] & 0xF0) == 0x50) ||
                (bytes[index] == 0x48 && bytes[index + 1] == 0x83 && bytes[index + 2] == 0xEC) ||
                (bytes[index] == 0x48 && bytes[index + 1] == 0x89 && bytes[index + 2] == 0x5C))
            {
                return section.address + index;
            }
        }

        return referenceAddress;
    }

    std::vector<std::uint64_t> FindNearbyCallTargets(const LiveModuleRecord& module, const std::vector<std::uint8_t>& bytes, const SectionScanRecord& section, std::size_t referenceOffset)
    {
        std::vector<std::uint64_t> targets;
        const std::size_t start = referenceOffset > 96 ? referenceOffset - 96 : 0;
        const std::size_t end = std::min(bytes.size(), referenceOffset + 96);
        for (std::size_t index = start; index + 5 <= end; ++index)
        {
            if (bytes[index] != 0xE8)
                continue;

            std::int32_t disp = 0;
            std::memcpy(&disp, bytes.data() + index + 1, sizeof(disp));
            const std::uint64_t target = section.address + index + 5 + disp;
            if (AddressInModule(module, target) &&
                std::find(targets.begin(), targets.end(), target) == targets.end())
            {
                targets.push_back(target);
                if (targets.size() >= 8)
                    break;
            }
        }

        return targets;
    }

    bool IsPriorityFloatValue(float value)
    {
        const float candidates[] = { 0.0f, 0.0083333f, 0.016f, 0.0166667f, 0.0333333f, 0.0174533f, 0.5f, 1.0f, 2.0f, 30.0f, 57.2958f, 60.0f, 120.0f };
        for (float candidate : candidates)
        {
            if (std::fabs(value - candidate) <= 0.0025f)
                return true;
        }
        return false;
    }

    std::string ClassifyFloatValue(float value)
    {
        struct KnownFloat
        {
            float value;
            float tolerance;
            const char* label;
        };

        static const KnownFloat known[] = {
            { 0.0f, 0.0001f, "ZeroDefaultOrVectorComponent" },
            { 0.0083333f, 0.0010f, "FrameDelta120" },
            { 0.0166667f, 0.0015f, "FrameDelta60" },
            { 0.016f, 0.0015f, "FrameDeltaApprox60" },
            { 0.0333333f, 0.0015f, "FrameDelta30" },
            { 0.0174533f, 0.0008f, "DegToRad" },
            { 57.2958f, 0.05f, "RadToDeg" },
            { 1.0f, 0.0025f, "IdentityMultiplier" },
            { 30.0f, 0.05f, "Fps30OrTickRate" },
            { 60.0f, 0.05f, "Fps60OrTickRate" },
            { 0.5f, 0.0025f, "HalfMultiplier" },
            { 2.0f, 0.0025f, "DoubleMultiplier" },
            { 120.0f, 0.1f, "Fps120OrTickRate" }
        };

        for (const KnownFloat& candidate : known)
        {
            if (std::fabs(value - candidate.value) <= candidate.tolerance)
                return candidate.label;
        }

        if (value > 0.0f && value < 10.0f)
            return "PositiveMultiplier";
        if (value < 0.0f && value > -10.0f)
            return "NegativeMultiplier";
        if (std::fabs(value) >= 10000.0f)
            return "HighValueCounterIdPackedOrNoise";
        return "UnclassifiedFloat";
    }

    void AddFloatCandidate(std::vector<std::string>& candidates, std::uint64_t address, float value, const char* source)
    {
        if (!std::isfinite(value) || std::fabs(value) > 1000000000.0f)
            return;
        if (value == 0.0f && candidates.size() >= 4)
            return;

        const std::string classification = ClassifyFloatValue(value);
        std::ostringstream out;
        out << (IsPriorityFloatValue(value) ? "*" : "")
            << "0x" << std::hex << address << std::dec
            << "=" << std::fixed << std::setprecision(6) << value
            << " [" << classification << "]"
            << " [" << source << "]";

        const std::string text = out.str();
        if (std::find(candidates.begin(), candidates.end(), text) == candidates.end())
            candidates.push_back(text);
    }

    std::uint32_t CountNearbyMathOperations(const std::vector<std::uint8_t>& bytes, std::size_t referenceOffset)
    {
        std::uint32_t count = 0;
        const std::size_t start = referenceOffset > 128 ? referenceOffset - 128 : 0;
        const std::size_t end = std::min(bytes.size(), referenceOffset + 128);
        for (std::size_t index = start; index + 3 < end; ++index)
        {
            const bool sseScalar =
                (bytes[index] == 0xF3 || bytes[index] == 0xF2) &&
                bytes[index + 1] == 0x0F &&
                (bytes[index + 2] == 0x10 || bytes[index + 2] == 0x11 ||
                 bytes[index + 2] == 0x58 || bytes[index + 2] == 0x59 ||
                 bytes[index + 2] == 0x5C || bytes[index + 2] == 0x5D ||
                 bytes[index + 2] == 0x5E || bytes[index + 2] == 0x2A ||
                 bytes[index + 2] == 0x2C);

            const bool ssePacked =
                bytes[index] == 0x0F &&
                (bytes[index + 1] == 0x28 || bytes[index + 1] == 0x29 ||
                 bytes[index + 1] == 0x58 || bytes[index + 1] == 0x59 ||
                 bytes[index + 1] == 0x5C || bytes[index + 1] == 0x5E);

            if (sseScalar || ssePacked)
                ++count;
        }

        return count;
    }

    std::vector<std::string> FindNearbyFloatCandidates(const LiveModuleRecord& module, const std::vector<std::uint8_t>& bytes, const SectionScanRecord& section, std::size_t referenceOffset)
    {
        std::vector<std::string> candidates;
        const std::size_t start = referenceOffset > 128 ? referenceOffset - 128 : 0;
        const std::size_t end = std::min(bytes.size(), referenceOffset + 128);

        for (std::size_t index = start; index + sizeof(float) <= end; ++index)
        {
            float immediate = 0.0f;
            std::memcpy(&immediate, bytes.data() + index, sizeof(immediate));
            if (IsPriorityFloatValue(immediate))
                AddFloatCandidate(candidates, section.address + index, immediate, "nearby-code");
            if (candidates.size() >= 12)
                return candidates;
        }

        for (std::size_t index = start; index < end; ++index)
        {
            std::uint64_t target = 0;
            std::size_t instructionLength = 0;
            if (!TryDecodeRipRelativeReference(bytes, index, section.address + index, target, instructionLength))
                continue;
            if (!IsReadableDataAddress(module, target, sizeof(float)))
                continue;

            for (int relative = -4; relative <= 4; ++relative)
            {
                const std::uint64_t floatAddress = target + static_cast<std::int64_t>(relative * 4);
                if (!IsReadableDataAddress(module, floatAddress, sizeof(float)))
                    continue;
                float value = 0.0f;
                if (!SafeReadValue(floatAddress, value))
                    continue;
                AddFloatCandidate(candidates, floatAddress, value, relative == 0 ? "rip-data" : "near-rip-data");
            }

            if (candidates.size() >= 12)
                break;
        }

        return candidates;
    }

    template <typename T>
    void AddUniqueValue(std::vector<T>& values, const T& value)
    {
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }

    const LiveModuleRecord* FindModuleByUtf8Name(const std::vector<LiveModuleRecord>& modules, const std::string& name)
    {
        for (const LiveModuleRecord& module : modules)
        {
            if (WideToUtf8(module.name) == name)
                return &module;
        }

        return nullptr;
    }

    std::uint64_t NormalizeFunctionAddress(const LiveModuleRecord& module, std::uint64_t address)
    {
        if (const FunctionRangeRecord* range = FindFunctionRange(module, address))
            return range->begin;
        return address;
    }

    std::vector<CallEdgeRecord> BuildCallGraph(const std::vector<LiveModuleRecord>& modules)
    {
        std::vector<CallEdgeRecord> edges;
        std::set<std::string> seen;

        for (const LiveModuleRecord& module : modules)
        {
            if (!IsPrimaryDiscoveryModule(module))
                continue;

            const std::string moduleName = WideToUtf8(module.name);
            for (const SectionScanRecord& section : module.sections)
            {
                if (!section.readable || !section.executable || section.size == 0 || section.size > 128u * 1024u * 1024u)
                    continue;

                std::vector<std::uint8_t> bytes(section.size);
                if (!SafeReadBytes(section.address, bytes.data(), bytes.size()))
                    continue;

                for (std::size_t offset = 0; offset + 5 <= bytes.size(); ++offset)
                {
                    if (bytes[offset] != 0xE8)
                        continue;

                    std::int32_t disp = 0;
                    std::memcpy(&disp, bytes.data() + offset + 1, sizeof(disp));
                    const std::uint64_t instruction = section.address + offset;
                    const std::uint64_t rawTarget = instruction + 5 + disp;
                    if (!AddressInModule(module, rawTarget))
                        continue;

                    const std::uint64_t caller = NormalizeFunctionAddress(module, instruction);
                    const std::uint64_t callee = NormalizeFunctionAddress(module, rawTarget);
                    if (!caller || !callee || caller == callee)
                        continue;

                    std::ostringstream key;
                    key << moduleName << "|" << std::hex << caller << "|" << callee << "|" << instruction;
                    if (!seen.insert(key.str()).second)
                        continue;

                    CallEdgeRecord edge;
                    edge.module = moduleName;
                    edge.caller = caller;
                    edge.callee = callee;
                    edge.instruction = instruction;
                    edges.push_back(edge);

                    if (edges.size() >= 500000)
                        return edges;
                }
            }
        }

        return edges;
    }

    std::vector<std::string> ExtractFloatClassifications(const std::vector<std::string>& candidates)
    {
        std::vector<std::string> labels;
        for (const std::string& value : candidates)
        {
            std::size_t first = value.find('[');
            if (first == std::string::npos)
                continue;
            std::size_t last = value.find(']', first + 1);
            if (last == std::string::npos || last <= first + 1)
                continue;
            AddUniqueValue(labels, value.substr(first + 1, last - first - 1));
        }
        return labels;
    }

    bool IsNameDecompositionCandidate(const std::string& value)
    {
        static const std::vector<const char*> prefixes = {
            "Ecs", "VisualEnvironment", "Shader", "Physics", "Entity", "Audio",
            "SceneOp", "Component", "Asset", "Data", "ContextVariable"
        };

        for (const char* prefix : prefixes)
        {
            if (value.rfind(prefix, 0) == 0)
                return true;
        }
        return false;
    }

    std::string CategoryFromDecomposedSystem(const std::string& system)
    {
        const std::string lower = ToLowerAscii(system);
        if (ContainsAscii(lower, "visualenvironment") || ContainsAscii(lower, "sky") || ContainsAscii(lower, "lighting") || ContainsAscii(lower, "fog"))
            return "Environment";
        if (ContainsAscii(lower, "shader") || ContainsAscii(lower, "render") || ContainsAscii(lower, "material"))
            return "Rendering";
        if (ContainsAscii(lower, "physics"))
            return "Physics";
        if (ContainsAscii(lower, "entity") || ContainsAscii(lower, "component") || ContainsAscii(lower, "ecs"))
            return "Entity";
        if (ContainsAscii(lower, "audio"))
            return "Audio";
        if (ContainsAscii(lower, "asset") || ContainsAscii(lower, "data"))
            return "Asset/Resource";
        return "Unknown";
    }

    std::string GuessKindFromTableName(const std::string& tableName)
    {
        static const std::vector<const char*> kinds = {
            "BlendMode", "SupportedTypes", "DataType", "Type", "Mode", "State",
            "Flags", "Quality", "Format", "Pass", "Stage", "Op", "Policy"
        };
        for (const char* kind : kinds)
        {
            const std::string kindString = kind;
            if (tableName.size() >= kindString.size() &&
                tableName.compare(tableName.size() - kindString.size(), kindString.size(), kindString) == 0)
            {
                return kindString;
            }
        }
        return "Name";
    }

    DecomposedNameRecord DecomposeFrostbiteName(const DiscoveredStringRecord& record)
    {
        DecomposedNameRecord out;
        out.original = record.value;
        out.module = record.module;
        out.address = record.address;
        out.score = record.score;
        out.category = record.category;

        std::string tableName = record.value;
        std::string value;
        const std::size_t underscore = record.value.rfind('_');
        if (underscore != std::string::npos && underscore + 1 < record.value.size())
        {
            tableName = record.value.substr(0, underscore);
            value = record.value.substr(underscore + 1);
        }

        const std::string kind = GuessKindFromTableName(tableName);
        out.kind = kind;
        out.value = value.empty() ? tableName : value;
        if (kind != "Name" && tableName.size() > kind.size())
            out.prefixSystem = tableName.substr(0, tableName.size() - kind.size());
        else
            out.prefixSystem = tableName;

        if (out.category.empty() || out.category == "Unknown")
            out.category = CategoryFromDecomposedSystem(out.prefixSystem);
        return out;
    }

    void BuildNameAndEnumDiscovery(DiscoveryDump& dump)
    {
        std::map<std::string, EnumTableRecord> tables;

        for (const DiscoveredStringRecord& record : dump.strings)
        {
            if (!IsNameDecompositionCandidate(record.value))
                continue;

            DecomposedNameRecord decomposed = DecomposeFrostbiteName(record);
            dump.decomposedNames.push_back(decomposed);

            const std::size_t underscore = record.value.rfind('_');
            if (underscore == std::string::npos || underscore + 1 >= record.value.size())
                continue;

            const std::string tableName = record.value.substr(0, underscore);
            const std::string valueName = record.value.substr(underscore + 1);
            EnumTableRecord& table = tables[tableName];
            table.name = tableName;
            table.suspectedSystem = decomposed.category;
            AddUniqueValue(table.values, valueName);
            AddUniqueValue(table.valueAddresses, record.address);
            table.minAddress = table.minAddress == 0 ? record.address : std::min(table.minAddress, record.address);
            table.maxAddress = std::max(table.maxAddress, record.address + static_cast<std::uint64_t>(record.value.size()));

            for (const StringXrefRecord& xref : dump.xrefs)
            {
                if (xref.stringAddress == record.address)
                    AddUniqueValue(table.functionXrefs, xref.functionAddress);
            }
        }

        for (auto& [_, table] : tables)
        {
            if (table.values.size() < 2)
                continue;
            int confidence = 30 + static_cast<int>(table.values.size()) * 8 + static_cast<int>(table.functionXrefs.size()) * 5;
            if (table.name.find("Mode") != std::string::npos || table.name.find("Type") != std::string::npos || table.name.find("State") != std::string::npos)
                confidence += 15;
            table.confidence = ClampScore(confidence);
            dump.enumTables.push_back(std::move(table));
        }

        std::sort(dump.decomposedNames.begin(), dump.decomposedNames.end(), [](const DecomposedNameRecord& lhs, const DecomposedNameRecord& rhs) {
            if (lhs.score != rhs.score)
                return lhs.score > rhs.score;
            return lhs.original < rhs.original;
        });

        std::sort(dump.enumTables.begin(), dump.enumTables.end(), [](const EnumTableRecord& lhs, const EnumTableRecord& rhs) {
            if (lhs.confidence != rhs.confidence)
                return lhs.confidence > rhs.confidence;
            return lhs.name < rhs.name;
        });
    }

    bool CandidateMatchesCategory(const FunctionCandidateRecord& candidate, const std::string& category)
    {
        return std::find(candidate.categories.begin(), candidate.categories.end(), category) != candidate.categories.end();
    }

    bool CandidateMatchesAnyCategory(const FunctionCandidateRecord& candidate, const std::vector<const char*>& categories)
    {
        for (const char* category : categories)
        {
            if (CandidateMatchesCategory(candidate, category))
                return true;
        }
        return false;
    }

    bool HasPriorityFloatCandidate(const FunctionCandidateRecord& candidate);

    std::string DetermineSystemCluster(const FunctionCandidateRecord& candidate)
    {
        std::string combined;
        for (const std::string& value : candidate.relatedStrings)
        {
            combined += value;
            combined.push_back(' ');
        }
        combined = ToLowerAscii(combined);

        if (ContainsAscii(combined, "tick") || ContainsAscii(combined, "delta") || ContainsAscii(combined, "timescale"))
            return "Tick";
        if (ContainsAscii(combined, "physics") || ContainsAscii(combined, "rigidbody") || ContainsAscii(combined, "collision") || ContainsAscii(combined, "gravity"))
            return "Physics Simulation";
        if (ContainsAscii(combined, "visualenvironment") || ContainsAscii(combined, "environment") || ContainsAscii(combined, "exposure"))
            return "Visual Environment";
        if (ContainsAscii(combined, "sky") || ContainsAscii(combined, "sun") || ContainsAscii(combined, "lighting") || ContainsAscii(combined, "fog") || ContainsAscii(combined, "atmosphere"))
            return "Sky / Lighting";
        if (ContainsAscii(combined, "shader") || ContainsAscii(combined, "material") || ContainsAscii(combined, "postprocess") || ContainsAscii(combined, "renderpass"))
            return "Shader Pipeline";
        if (ContainsAscii(combined, "entity") || ContainsAscii(combined, "component") || ContainsAscii(combined, "transform") || ContainsAscii(combined, "ecs"))
            return "Entity / Component";
        if (ContainsAscii(combined, "asset") || ContainsAscii(combined, "bundle") || ContainsAscii(combined, ".ebx") || ContainsAscii(combined, ".toc") || ContainsAscii(combined, ".cas"))
            return "Asset Loading";
        if (ContainsAscii(combined, "audio") || ContainsAscii(combined, "sound") || ContainsAscii(combined, "music"))
            return "Audio";

        const char* clusters[] = { "Time", "Environment", "Rendering", "Physics", "Entity", "Audio" };
        for (const char* cluster : clusters)
        {
            if (CandidateMatchesCategory(candidate, cluster))
                return cluster;
        }
        return candidate.primaryCategory.empty() ? "Unknown" : candidate.primaryCategory;
    }

    std::string GuessExecutionPhase(const FunctionCandidateRecord& candidate)
    {
        std::string combined;
        for (const std::string& value : candidate.relatedStrings)
        {
            combined += value;
            combined.push_back(' ');
        }
        combined = ToLowerAscii(combined);

        if (ContainsAscii(combined, "init") || ContainsAscii(combined, "startup") || ContainsAscii(combined, "create") || ContainsAscii(combined, "register"))
            return "init-only";
        if (ContainsAscii(combined, "tick") || ContainsAscii(combined, "update") || ContainsAscii(combined, "delta") || ContainsAscii(combined, "frame"))
            return "per-frame";
        if (ContainsAscii(combined, "render") || ContainsAscii(combined, "shader") || ContainsAscii(combined, "postprocess") || ContainsAscii(combined, "lighting") || ContainsAscii(combined, "view"))
            return "render-pass";
        if (ContainsAscii(combined, "physics") || ContainsAscii(combined, "simulate") || ContainsAscii(combined, "collision") || ContainsAscii(combined, "rigidbody"))
            return "physics-step";
        if (ContainsAscii(combined, "asset") || ContainsAscii(combined, "bundle") || ContainsAscii(combined, "stream") || ContainsAscii(combined, "load") || ContainsAscii(combined, ".ebx"))
            return "asset-load";
        if (ContainsAscii(combined, "debug") || ContainsAscii(combined, "log") || ContainsAscii(combined, "assert") || ContainsAscii(combined, "warning"))
            return "debug/logging";
        if (candidate.cluster == "Shader Pipeline" || candidate.cluster == "Sky / Lighting" || candidate.cluster == "Visual Environment")
            return "render-pass";
        if (candidate.cluster == "Tick" || candidate.cluster == "Time")
            return "per-frame";
        if (candidate.cluster == "Physics Simulation")
            return "physics-step";
        return "unknown";
    }

    std::string DetermineCandidateTier(const FunctionCandidateRecord& candidate)
    {
        if (candidate.tier == "Self-DLL Noise" || candidate.tier == "Third-Party Noise")
            return candidate.tier;

        if (candidate.fallbackOnly || candidate.primaryCategory == "Unknown")
        {
            if (candidate.score >= 50)
                return "Weak Candidate";
            return "Noise";
        }

        const bool hasPdataBounds = candidate.boundarySource == ".pdata";
        const bool hasGraphEvidence = !candidate.callers.empty() || !candidate.callees.empty();
        const bool hasDataEvidence = HasPriorityFloatCandidate(candidate) || candidate.mathOperationCount > 0;

        if (candidate.score >= 90 && hasPdataBounds && candidate.xrefCount >= 2 && (hasGraphEvidence || hasDataEvidence))
            return "Confirmed Lead";
        if (candidate.score >= 70 && hasPdataBounds && candidate.xrefCount >= 1)
            return "Strong Candidate";
        if (candidate.score >= 40)
            return "Weak Candidate";
        return "Noise";
    }

    bool HasPriorityFloatCandidate(const FunctionCandidateRecord& candidate)
    {
        for (const std::string& value : candidate.nearbyFloatCandidates)
        {
            if (!value.empty() && value[0] == '*')
                return true;
        }
        return false;
    }

    std::uint32_t ScoreFunctionCandidate(FunctionCandidateRecord& candidate, const std::unordered_map<std::string, std::uint32_t>& moduleScores)
    {
        const auto moduleIt = moduleScores.find(candidate.module);
        int score = moduleIt == moduleScores.end() ? 15 : static_cast<int>(moduleIt->second / 2);

        int keywordScore = 0;
        for (const std::string& value : candidate.relatedStrings)
        {
            for (const std::string& category : candidate.categories)
                keywordScore = std::max(keywordScore, KeywordStrengthScore(value, category));
        }

        score += keywordScore;
        score += std::min<int>(20, static_cast<int>(candidate.xrefCount) * 5);
        score += candidate.nearbyFloatCandidates.empty() ? 0 : 10;
        score += HasPriorityFloatCandidate(candidate) ? 10 : 0;
        score += std::min<int>(15, static_cast<int>(candidate.mathOperationCount) * 3);
        score += std::min<int>(10, static_cast<int>(candidate.nearbyCallTargets.size()) * 2);
        score += candidate.boundarySource == ".pdata" ? 8 : 0;
        score += std::min<int>(10, static_cast<int>(candidate.callers.size() + candidate.callees.size()));
        if (candidate.fallbackOnly)
            score -= 25;

        std::vector<std::string> reasons;
        reasons.push_back("module relevance " + std::to_string(moduleIt == moduleScores.end() ? 0 : moduleIt->second));
        reasons.push_back("keyword strength " + std::to_string(keywordScore));
        reasons.push_back(std::to_string(candidate.xrefCount) + " string xref(s)");
        if (!candidate.nearbyFloatCandidates.empty())
            reasons.push_back(std::to_string(candidate.nearbyFloatCandidates.size()) + " nearby float/data value(s)");
        if (HasPriorityFloatCandidate(candidate))
            reasons.push_back("priority float value near 0.016/1.0/60.0/multiplier");
        if (candidate.mathOperationCount > 0)
            reasons.push_back(std::to_string(candidate.mathOperationCount) + " nearby SSE/math opcode(s)");
        if (!candidate.nearbyCallTargets.empty())
            reasons.push_back(std::to_string(candidate.nearbyCallTargets.size()) + " nearby call target(s)");
        if (candidate.boundarySource == ".pdata")
            reasons.push_back("function boundary recovered from PE .pdata unwind info");
        if (candidate.functionEnd > candidate.functionAddress)
            reasons.push_back("function size " + std::to_string(candidate.functionEnd - candidate.functionAddress) + " bytes, boundary confidence " + std::to_string(candidate.boundaryConfidence));
        if (candidate.xrefAtFunctionStart)
            reasons.push_back("xref is at or near function start");
        if (!candidate.phaseGuess.empty() && candidate.phaseGuess != "unknown")
            reasons.push_back("phase guess " + candidate.phaseGuess);
        if (!candidate.callers.empty() || !candidate.callees.empty())
            reasons.push_back(std::to_string(candidate.callers.size()) + " caller(s), " + std::to_string(candidate.callees.size()) + " callee(s) in local call graph");
        if (!candidate.floatClassifications.empty())
            reasons.push_back("float classes: " + JoinStrings(candidate.floatClassifications, ", "));
        if (candidate.fallbackOnly)
            reasons.push_back("string-only fallback; no code xref decoded");
        candidate.reasoning = JoinStrings(reasons, "; ");

        return ClampScore(score);
    }

    std::string PickPrimaryCategory(const FunctionCandidateRecord& candidate)
    {
        const char* priority[] = { "Time", "Environment", "Rendering", "Entity", "Physics", "Audio" };
        for (const char* category : priority)
        {
            if (CandidateMatchesCategory(candidate, category))
                return category;
        }
        return candidate.categories.empty() ? "Unknown" : candidate.categories.front();
    }

    void EnrichFunctionCandidatesWithValidation(DiscoveryDump& dump, const std::vector<LiveModuleRecord>& modules)
    {
        for (FunctionCandidateRecord& candidate : dump.functionCandidates)
        {
            const LiveModuleRecord* module = FindModuleByUtf8Name(modules, candidate.module);
            if (module)
            {
                if (const FunctionRangeRecord* range = FindFunctionRange(*module, candidate.functionAddress))
                {
                    candidate.functionAddress = range->begin;
                    candidate.functionEnd = range->end;
                    candidate.boundarySource = range->source;
                    candidate.boundaryConfidence = 95;
                }
                else if (!candidate.fallbackOnly)
                {
                    candidate.boundarySource = "heuristic";
                    candidate.boundaryConfidence = 45;
                }

                if (module->relevance == ModuleRelevance::ThirdParty || module->relevance == ModuleRelevance::WindowsSystem)
                    candidate.tier = IsScannerSelfModuleName(ToLower(module->name)) ? "Self-DLL Noise" : "Third-Party Noise";
            }

            for (std::uint64_t& callTarget : candidate.nearbyCallTargets)
            {
                if (module)
                    callTarget = NormalizeFunctionAddress(*module, callTarget);
            }
            std::sort(candidate.nearbyCallTargets.begin(), candidate.nearbyCallTargets.end());
            candidate.nearbyCallTargets.erase(std::unique(candidate.nearbyCallTargets.begin(), candidate.nearbyCallTargets.end()), candidate.nearbyCallTargets.end());

            for (const CallEdgeRecord& edge : dump.callEdges)
            {
                if (edge.module != candidate.module)
                    continue;
                if (edge.caller == candidate.functionAddress)
                    AddUniqueValue(candidate.callees, edge.callee);
                if (edge.callee == candidate.functionAddress)
                    AddUniqueValue(candidate.callers, edge.caller);
            }

            for (std::uint64_t callTarget : candidate.nearbyCallTargets)
                AddUniqueValue(candidate.callees, callTarget);

            candidate.floatClassifications = ExtractFloatClassifications(candidate.nearbyFloatCandidates);
            candidate.cluster = DetermineSystemCluster(candidate);
            candidate.phaseGuess = GuessExecutionPhase(candidate);

            for (std::uint64_t referenceAddress : candidate.referenceAddresses)
            {
                if (referenceAddress == candidate.functionAddress || (referenceAddress > candidate.functionAddress && referenceAddress - candidate.functionAddress <= 8))
                {
                    candidate.xrefAtFunctionStart = true;
                    break;
                }
            }
        }
    }

    void BuildFunctionCandidates(DiscoveryDump& dump, const std::vector<LiveModuleRecord>& modules)
    {
        std::unordered_map<std::string, std::uint32_t> moduleScores;
        for (const LiveModuleRecord& module : modules)
            moduleScores[WideToUtf8(module.name)] = module.relevanceScore;

        std::map<std::string, std::size_t> byFunction;
        for (const StringXrefRecord& xref : dump.xrefs)
        {
            const std::string key = xref.module + "|" + std::to_string(xref.functionAddress);
            auto [it, inserted] = byFunction.emplace(key, dump.functionCandidates.size());
            if (inserted)
            {
                FunctionCandidateRecord candidate;
                candidate.module = xref.module;
                candidate.functionAddress = xref.functionAddress;
                dump.functionCandidates.push_back(std::move(candidate));
            }

            FunctionCandidateRecord& candidate = dump.functionCandidates[it->second];
            AddUniqueValue(candidate.categories, xref.category);
            AddUniqueValue(candidate.relatedStrings, xref.value);
            AddUniqueValue(candidate.stringAddresses, xref.stringAddress);
            AddUniqueValue(candidate.referenceAddresses, xref.referenceAddress);
            for (std::uint64_t callTarget : xref.nearbyCallTargets)
                AddUniqueValue(candidate.nearbyCallTargets, callTarget);
            for (const std::string& value : xref.nearbyFloatCandidates)
                AddUniqueValue(candidate.nearbyFloatCandidates, value);
            candidate.xrefCount += 1;
            candidate.mathOperationCount += xref.mathOperationCount;
        }

        auto ensureFallback = [&](const char* category) {
            for (const FunctionCandidateRecord& candidate : dump.functionCandidates)
            {
                if (CandidateMatchesCategory(candidate, category))
                    return;
            }

            for (const DiscoveredStringRecord& stringRecord : dump.strings)
            {
                if (stringRecord.category != category)
                    continue;

                FunctionCandidateRecord candidate;
                candidate.module = stringRecord.module;
                candidate.functionAddress = stringRecord.address;
                candidate.primaryCategory = category;
                candidate.categories.push_back(category);
                candidate.relatedStrings.push_back(stringRecord.value);
                candidate.stringAddresses.push_back(stringRecord.address);
                candidate.fallbackOnly = true;
                candidate.xrefCount = 0;
                candidate.score = ScoreFunctionCandidate(candidate, moduleScores);
                dump.functionCandidates.push_back(std::move(candidate));
                return;
            }
        };

        ensureFallback("Time");
        ensureFallback("Environment");
        ensureFallback("Rendering");
        ensureFallback("Entity");
        ensureFallback("Physics");
        ensureFallback("Audio");

        for (FunctionCandidateRecord& candidate : dump.functionCandidates)
        {
            if (candidate.primaryCategory.empty())
                candidate.primaryCategory = PickPrimaryCategory(candidate);
        }

        EnrichFunctionCandidatesWithValidation(dump, modules);

        for (FunctionCandidateRecord& candidate : dump.functionCandidates)
        {
            candidate.score = ScoreFunctionCandidate(candidate, moduleScores);
            candidate.tier = DetermineCandidateTier(candidate);
        }

        std::sort(dump.functionCandidates.begin(), dump.functionCandidates.end(), [](const FunctionCandidateRecord& lhs, const FunctionCandidateRecord& rhs) {
            if (lhs.score != rhs.score)
                return lhs.score > rhs.score;
            if (lhs.xrefCount != rhs.xrefCount)
                return lhs.xrefCount > rhs.xrefCount;
            return lhs.functionAddress < rhs.functionAddress;
        });
    }

    DiscoveryDump CaptureReadOnlyDiscovery(std::vector<LiveModuleRecord>& modules, bool verbose)
    {
        DiscoveryDump dump;
        std::unordered_map<std::uint64_t, std::size_t> stringAddressToIndex;

        if (verbose)
            Progress(L"Starting read-only string discovery and xref scan.");

        for (LiveModuleRecord& module : modules)
        {
            if (!IsPrimaryDiscoveryModule(module))
                continue;

            for (const SectionScanRecord& section : module.sections)
            {
                if (!section.readable || section.size == 0 || section.size > 128u * 1024u * 1024u)
                    continue;

                std::vector<std::uint8_t> bytes(section.size);
                if (!SafeReadBytes(section.address, bytes.data(), bytes.size()))
                    continue;

                ScanAsciiStringsInSection(module, dump, stringAddressToIndex, section, bytes);
                ScanUtf16StringsInSection(module, dump, stringAddressToIndex, section, bytes);
            }
        }

        for (const LiveModuleRecord& module : modules)
        {
            if (!IsPrimaryDiscoveryModule(module))
                continue;

            for (const SectionScanRecord& section : module.sections)
            {
                if (!section.readable || !section.executable || section.size == 0 || section.size > 128u * 1024u * 1024u)
                    continue;

                std::vector<std::uint8_t> bytes(section.size);
                if (!SafeReadBytes(section.address, bytes.data(), bytes.size()))
                    continue;

                for (std::size_t offset = 0; offset < bytes.size(); ++offset)
                {
                    std::uint64_t target = 0;
                    std::size_t instructionLength = 0;
                    if (!TryDecodeRipRelativeReference(bytes, offset, section.address + offset, target, instructionLength))
                        continue;

                    const auto stringIt = stringAddressToIndex.find(target);
                    if (stringIt == stringAddressToIndex.end())
                        continue;

                    DiscoveredStringRecord& stringRecord = dump.strings[stringIt->second];
                    if (stringRecord.xrefCount >= 64)
                        continue;

                    StringXrefRecord xref;
                    xref.value = stringRecord.value;
                    xref.category = stringRecord.category;
                    xref.module = WideToUtf8(module.name);
                    xref.section = section.name;
                    xref.stringAddress = target;
                    xref.referenceAddress = section.address + offset;
                    xref.functionAddress = GuessFunctionStart(module, bytes, section, offset);
                    const std::size_t surroundingStart = offset > 12 ? offset - 12 : 0;
                    xref.surroundingBytes = BytesToHex(bytes, surroundingStart, 32);
                    xref.nearbyCallTargets = FindNearbyCallTargets(module, bytes, section, offset);
                    xref.nearbyFloatCandidates = FindNearbyFloatCandidates(module, bytes, section, offset);
                    xref.mathOperationCount = CountNearbyMathOperations(bytes, offset);
                    xref.score = ClampScore(static_cast<int>(stringRecord.score) + 15 +
                        (xref.nearbyFloatCandidates.empty() ? 0 : 10) +
                        (xref.mathOperationCount > 0 ? 10 : 0));
                    dump.xrefs.push_back(std::move(xref));

                    ++stringRecord.xrefCount;
                    stringRecord.score = ClampScore(static_cast<int>(stringRecord.score) + 15);
                }
            }
        }

        std::sort(dump.strings.begin(), dump.strings.end(), [](const DiscoveredStringRecord& lhs, const DiscoveredStringRecord& rhs) {
            if (lhs.score != rhs.score)
                return lhs.score > rhs.score;
            if (lhs.category != rhs.category)
                return lhs.category < rhs.category;
            return lhs.value < rhs.value;
        });

        std::sort(dump.xrefs.begin(), dump.xrefs.end(), [](const StringXrefRecord& lhs, const StringXrefRecord& rhs) {
            if (lhs.score != rhs.score)
                return lhs.score > rhs.score;
            if (lhs.category != rhs.category)
                return lhs.category < rhs.category;
            return lhs.referenceAddress < rhs.referenceAddress;
        });

        dump.callEdges = BuildCallGraph(modules);
        BuildNameAndEnumDiscovery(dump);
        BuildFunctionCandidates(dump, modules);

        if (verbose)
        {
            Progress(L"Read-only discovery complete: strings=" + FormatCount(dump.strings.size()) +
                L", xrefs=" + FormatCount(dump.xrefs.size()) +
                L", call edges=" + FormatCount(dump.callEdges.size()) +
                L", enum/table candidates=" + FormatCount(dump.enumTables.size()) +
                L", function candidates=" + FormatCount(dump.functionCandidates.size()));
        }

        return dump;
    }

    std::string HexAddress(std::uint64_t address);

    bool WriteLiveProcessJson(const fs::path& path, const std::vector<LiveModuleRecord>& modules)
    {
        Progress(L"Writing live process JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator injected snapshot\",\n";
        file << "  \"processId\": " << ::GetCurrentProcessId() << ",\n";
        file << "  \"processPath\": \"" << JsonEscape(PathToUtf8(GetModulePath(nullptr))) << "\",\n";
        file << "  \"note\": \"Function signatures require decorated symbols or PDB data. Stripped native binaries cannot expose reliable parameter/type data.\",\n";
        file << "  \"moduleCount\": " << modules.size() << ",\n";
        file << "  \"modules\": [\n";

        for (std::size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex)
        {
            const LiveModuleRecord& module = modules[moduleIndex];
            file << "    {\n";
            file << "      \"name\": \"" << JsonEscape(WideToUtf8(module.name)) << "\",\n";
            file << "      \"path\": \"" << JsonEscape(PathToUtf8(module.path)) << "\",\n";
            file << "      \"baseAddress\": " << module.baseAddress << ",\n";
            file << "      \"baseAddressHex\": \"0x" << std::hex << module.baseAddress << std::dec << "\",\n";
            file << "      \"imageSize\": " << module.imageSize << ",\n";
            file << "      \"relevance\": \"" << RelevanceToString(module.relevance) << "\",\n";
            file << "      \"relevanceScore\": " << module.relevanceScore << ",\n";
            file << "      \"symbolType\": \"" << JsonEscape(module.symbolType) << "\",\n";
            file << "      \"loadedPdb\": \"" << JsonEscape(module.loadedPdb) << "\",\n";
            file << "      \"exportCount\": " << module.exports.size() << ",\n";
            file << "      \"importCount\": " << module.imports.size() << ",\n";
            file << "      \"symbolCount\": " << module.symbols.size() << ",\n";
            file << "      \"classCount\": " << module.classes.size() << ",\n";
            file << "      \"functionRangeCount\": " << module.functionRanges.size() << ",\n";
            file << "      \"sections\": [\n";
            for (std::size_t sectionIndex = 0; sectionIndex < module.sections.size(); ++sectionIndex)
            {
                const SectionScanRecord& section = module.sections[sectionIndex];
                file << "        { \"name\": \"" << JsonEscape(section.name)
                     << "\", \"address\": " << section.address
                     << ", \"addressHex\": \"" << HexAddress(section.address)
                     << "\", \"rva\": " << section.rva
                     << ", \"size\": " << section.size
                     << ", \"readable\": " << (section.readable ? "true" : "false")
                     << ", \"writable\": " << (section.writable ? "true" : "false")
                     << ", \"executable\": " << (section.executable ? "true" : "false")
                     << ", \"stringCount\": " << section.stringCount
                     << ", \"pointerCount\": " << section.pointerCount
                     << ", \"rttiCount\": " << section.rttiCount
                     << ", \"pointerDensity\": " << section.pointerDensity
                     << ", \"rttiDensity\": " << section.rttiDensity
                     << " }" << (sectionIndex + 1 < module.sections.size() ? "," : "") << "\n";
            }
            file << "      ],\n";
            file << "      \"exports\": [\n";
            for (std::size_t exportIndex = 0; exportIndex < module.exports.size(); ++exportIndex)
            {
                const std::string undecorated = UndecorateSymbol(module.exports[exportIndex]);
                file << "        { \"name\": \"" << JsonEscape(module.exports[exportIndex]) << "\", "
                     << "\"undecorated\": \"" << JsonEscape(undecorated) << "\", "
                     << "\"signature\": \"" << JsonEscape(LooksLikeFunctionSignature(undecorated) ? undecorated : "") << "\" }"
                     << (exportIndex + 1 < module.exports.size() ? "," : "") << "\n";
            }
            file << "      ],\n";
            file << "      \"imports\": [\n";
            for (std::size_t importIndex = 0; importIndex < module.imports.size(); ++importIndex)
            {
                const ImportRecord& importRecord = module.imports[importIndex];
                file << "        { \"module\": \"" << JsonEscape(importRecord.module) << "\", ";
                if (importRecord.byOrdinal)
                    file << "\"ordinal\": " << importRecord.ordinal << ", \"byOrdinal\": true";
                else
                    file << "\"name\": \"" << JsonEscape(importRecord.name) << "\", \"byOrdinal\": false";
                file << " }" << (importIndex + 1 < module.imports.size() ? "," : "") << "\n";
            }
            file << "      ],\n";
            file << "      \"symbols\": [\n";
            for (std::size_t symbolIndex = 0; symbolIndex < module.symbols.size(); ++symbolIndex)
            {
                const SymbolRecord& symbol = module.symbols[symbolIndex];
                file << "        { "
                     << "\"name\": \"" << JsonEscape(symbol.name) << "\", "
                     << "\"undecorated\": \"" << JsonEscape(symbol.undecoratedName) << "\", "
                     << "\"signature\": \"" << JsonEscape(symbol.signature) << "\", "
                     << "\"hasSignature\": " << (symbol.hasSignature ? "true" : "false") << ", "
                     << "\"isFunction\": " << (symbol.isFunction ? "true" : "false") << ", "
                     << "\"address\": " << symbol.address << ", "
                     << "\"addressHex\": \"0x" << std::hex << symbol.address << std::dec << "\", "
                     << "\"size\": " << symbol.size << ", "
                     << "\"flags\": " << symbol.flags << ", "
                     << "\"tag\": " << symbol.tag
                     << " }" << (symbolIndex + 1 < module.symbols.size() ? "," : "") << "\n";
            }
            file << "      ],\n";
            file << "      \"classes\": [\n";
            for (std::size_t classIndex = 0; classIndex < module.classes.size(); ++classIndex)
            {
                const RttiClassRecord& record = module.classes[classIndex];
                file << "        {\n";
                file << "          \"name\": \"" << JsonEscape(record.name) << "\",\n";
                file << "          \"decoratedName\": \"" << JsonEscape(record.decoratedName) << "\",\n";
                file << "          \"relevance\": \"" << JsonEscape(record.relevance) << "\",\n";
                file << "          \"confidence\": " << record.confidence << ",\n";
                file << "          \"typeDescriptor\": " << record.typeDescriptor << ",\n";
                file << "          \"typeDescriptorHex\": \"0x" << std::hex << record.typeDescriptor << std::dec << "\",\n";
                file << "          \"completeObjectLocator\": " << record.completeObjectLocator << ",\n";
                file << "          \"completeObjectLocatorHex\": \"0x" << std::hex << record.completeObjectLocator << std::dec << "\",\n";
                file << "          \"classHierarchyDescriptor\": " << record.classHierarchyDescriptor << ",\n";
                file << "          \"classHierarchyDescriptorHex\": \"0x" << std::hex << record.classHierarchyDescriptor << std::dec << "\",\n";
                file << "          \"vtable\": " << record.vtable << ",\n";
                file << "          \"vtableHex\": \"0x" << std::hex << record.vtable << std::dec << "\",\n";
                file << "          \"objectOffset\": " << record.objectOffset << ",\n";
                file << "          \"hierarchyAttributes\": " << record.hierarchyAttributes << ",\n";
                file << "          \"baseClasses\": [";
                for (std::size_t baseIndex = 0; baseIndex < record.baseClasses.size(); ++baseIndex)
                {
                    file << "\"" << JsonEscape(record.baseClasses[baseIndex]) << "\""
                         << (baseIndex + 1 < record.baseClasses.size() ? ", " : "");
                }
                file << "],\n";
                file << "          \"virtualFunctions\": [\n";
                for (std::size_t functionIndex = 0; functionIndex < record.virtualFunctions.size(); ++functionIndex)
                {
                    const VTableFunctionRecord& function = record.virtualFunctions[functionIndex];
                    file << "            { \"slot\": " << function.slot
                         << ", \"address\": " << function.address
                         << ", \"addressHex\": \"0x" << std::hex << function.address << std::dec << "\""
                         << ", \"symbol\": \"" << JsonEscape(function.symbolName) << "\""
                         << ", \"signature\": \"" << JsonEscape(function.signature) << "\" }"
                         << (functionIndex + 1 < record.virtualFunctions.size() ? "," : "") << "\n";
                }
                file << "          ]\n";
                file << "        }" << (classIndex + 1 < module.classes.size() ? "," : "") << "\n";
            }
            file << "      ]\n";
            file << "    }" << (moduleIndex + 1 < modules.size() ? "," : "") << "\n";
        }

        file << "  ]\n";
        file << "}\n";
        Progress(L"Wrote live process JSON: " + path.wstring());
        return true;
    }

    bool WriteRuntimeIntrospectionJson(const fs::path& path, const RuntimeIntrospectionDump& dump)
    {
        Progress(L"Writing runtime introspection JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator runtime introspection\",\n";
        file << "  \"scope\": \"owned-project provider exports only\",\n";
        file << "  \"cvarCount\": " << dump.cvars.size() << ",\n";
        file << "  \"systemCount\": " << dump.systems.size() << ",\n";
        file << "  \"environmentCount\": " << dump.environment.size() << ",\n";
        file << "  \"reflectedTypeCount\": " << dump.reflectedTypes.size() << ",\n";
        file << "  \"cvars\": [\n";
        for (std::size_t index = 0; index < dump.cvars.size(); ++index)
        {
            const RuntimeCVarRecord& record = dump.cvars[index];
            file << "    { \"providerModule\": \"" << JsonEscape(record.providerModule)
                 << "\", \"name\": \"" << JsonEscape(record.name)
                 << "\", \"category\": \"" << JsonEscape(record.category)
                 << "\", \"typeName\": \"" << JsonEscape(record.typeName)
                 << "\", \"valueType\": " << record.valueType
                 << ", \"currentValue\": \"" << JsonEscape(record.currentValue)
                 << "\", \"defaultValue\": \"" << JsonEscape(record.defaultValue)
                 << "\", \"description\": \"" << JsonEscape(record.description)
                 << "\", \"address\": " << record.address
                 << ", \"addressHex\": \"0x" << std::hex << record.address << std::dec
                 << "\", \"flags\": " << record.flags << " }"
                 << (index + 1 < dump.cvars.size() ? "," : "") << "\n";
        }
        file << "  ],\n";

        file << "  \"systems\": [\n";
        for (std::size_t index = 0; index < dump.systems.size(); ++index)
        {
            const RuntimeSystemRecord& record = dump.systems[index];
            file << "    { \"providerModule\": \"" << JsonEscape(record.providerModule)
                 << "\", \"name\": \"" << JsonEscape(record.name)
                 << "\", \"kind\": \"" << JsonEscape(record.kind)
                 << "\", \"module\": \"" << JsonEscape(record.module)
                 << "\", \"description\": \"" << JsonEscape(record.description)
                 << "\", \"address\": " << record.address
                 << ", \"addressHex\": \"0x" << std::hex << record.address << std::dec
                 << "\", \"flags\": " << record.flags << " }"
                 << (index + 1 < dump.systems.size() ? "," : "") << "\n";
        }
        file << "  ],\n";

        file << "  \"environment\": [\n";
        for (std::size_t index = 0; index < dump.environment.size(); ++index)
        {
            const RuntimeEnvironmentRecord& record = dump.environment[index];
            file << "    { \"providerModule\": \"" << JsonEscape(record.providerModule)
                 << "\", \"name\": \"" << JsonEscape(record.name)
                 << "\", \"system\": \"" << JsonEscape(record.system)
                 << "\", \"typeName\": \"" << JsonEscape(record.typeName)
                 << "\", \"valueType\": " << record.valueType
                 << ", \"currentValue\": \"" << JsonEscape(record.currentValue)
                 << "\", \"description\": \"" << JsonEscape(record.description)
                 << "\", \"address\": " << record.address
                 << ", \"addressHex\": \"0x" << std::hex << record.address << std::dec
                 << "\", \"flags\": " << record.flags << " }"
                 << (index + 1 < dump.environment.size() ? "," : "") << "\n";
        }
        file << "  ],\n";

        file << "  \"reflectedTypes\": [\n";
        for (std::size_t typeIndex = 0; typeIndex < dump.reflectedTypes.size(); ++typeIndex)
        {
            const RuntimeTypeRecord& record = dump.reflectedTypes[typeIndex];
            file << "    {\n";
            file << "      \"providerModule\": \"" << JsonEscape(record.providerModule) << "\",\n";
            file << "      \"namespace\": \"" << JsonEscape(record.namespaceName) << "\",\n";
            file << "      \"name\": \"" << JsonEscape(record.name) << "\",\n";
            file << "      \"kind\": \"" << JsonEscape(record.kind) << "\",\n";
            file << "      \"sizeBytes\": " << record.sizeBytes << ",\n";
            file << "      \"address\": " << record.address << ",\n";
            file << "      \"addressHex\": \"0x" << std::hex << record.address << std::dec << "\",\n";
            file << "      \"flags\": " << record.flags << ",\n";
            file << "      \"fields\": [\n";
            for (std::size_t fieldIndex = 0; fieldIndex < record.fields.size(); ++fieldIndex)
            {
                const RuntimeFieldRecord& field = record.fields[fieldIndex];
                file << "        { \"name\": \"" << JsonEscape(field.name)
                     << "\", \"typeName\": \"" << JsonEscape(field.typeName)
                     << "\", \"offset\": " << field.offset
                     << ", \"sizeBytes\": " << field.sizeBytes
                     << ", \"flags\": " << field.flags << " }"
                     << (fieldIndex + 1 < record.fields.size() ? "," : "") << "\n";
            }
            file << "      ]\n";
            file << "    }" << (typeIndex + 1 < dump.reflectedTypes.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    bool WriteRuntimeIntrospectionMarkdown(const fs::path& path, const RuntimeIntrospectionDump& dump)
    {
        Progress(L"Writing runtime introspection report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Frostbite Runtime Introspection\n\n";
        file << "This report is built from optional owned-project provider exports. It does not scan hidden engine globals or write process memory.\n\n";
        file << "- CVars: `" << dump.cvars.size() << "`\n";
        file << "- Systems: `" << dump.systems.size() << "`\n";
        file << "- Environment values: `" << dump.environment.size() << "`\n";
        file << "- Reflected types: `" << dump.reflectedTypes.size() << "`\n\n";

        file << "## CVars\n\n";
        file << "| Name | Category | Type | Current | Default | Flags | Provider |\n";
        file << "| --- | --- | --- | --- | --- | ---: | --- |\n";
        for (const RuntimeCVarRecord& record : dump.cvars)
        {
            file << "| `" << JsonEscape(record.name) << "` | `" << JsonEscape(record.category)
                 << "` | `" << JsonEscape(record.typeName)
                 << "` | `" << JsonEscape(record.currentValue)
                 << "` | `" << JsonEscape(record.defaultValue)
                 << "` | `" << record.flags
                 << "` | `" << JsonEscape(record.providerModule) << "` |\n";
        }

        file << "\n## Systems\n\n";
        file << "| Name | Kind | Module | Address | Provider |\n";
        file << "| --- | --- | --- | ---: | --- |\n";
        for (const RuntimeSystemRecord& record : dump.systems)
        {
            file << "| `" << JsonEscape(record.name) << "` | `" << JsonEscape(record.kind)
                 << "` | `" << JsonEscape(record.module)
                 << "` | `0x" << std::hex << record.address << std::dec
                 << "` | `" << JsonEscape(record.providerModule) << "` |\n";
        }

        file << "\n## Render And Environment\n\n";
        file << "| Name | System | Type | Current | Flags | Provider |\n";
        file << "| --- | --- | --- | --- | ---: | --- |\n";
        for (const RuntimeEnvironmentRecord& record : dump.environment)
        {
            file << "| `" << JsonEscape(record.name) << "` | `" << JsonEscape(record.system)
                 << "` | `" << JsonEscape(record.typeName)
                 << "` | `" << JsonEscape(record.currentValue)
                 << "` | `" << record.flags
                 << "` | `" << JsonEscape(record.providerModule) << "` |\n";
        }

        file << "\n## Reflected Types\n\n";
        for (const RuntimeTypeRecord& record : dump.reflectedTypes)
        {
            const std::string fullName = record.namespaceName.empty()
                ? record.name
                : record.namespaceName + "::" + record.name;
            file << "### `" << JsonEscape(fullName) << "`\n\n";
            file << "- Kind: `" << JsonEscape(record.kind) << "`\n";
            file << "- Size: `" << record.sizeBytes << "`\n";
            file << "- Fields: `" << record.fields.size() << "`\n\n";
            for (const RuntimeFieldRecord& field : record.fields)
            {
                file << "- `0x" << std::hex << field.offset << std::dec << "` `"
                     << JsonEscape(field.typeName) << " " << JsonEscape(field.name)
                     << "` size `" << field.sizeBytes << "`\n";
            }
            file << "\n";
        }

        return true;
    }

    std::string HexAddress(std::uint64_t address)
    {
        std::ostringstream out;
        out << "0x" << std::hex << address;
        return out.str();
    }

    std::string JoinHexAddresses(const std::vector<std::uint64_t>& addresses)
    {
        std::vector<std::string> values;
        for (std::uint64_t address : addresses)
            values.push_back(HexAddress(address));
        return JoinStrings(values, ", ");
    }

    std::string MakeSafeLabel(std::string value)
    {
        if (value.empty())
            value = "Candidate";

        std::string out;
        out.reserve(value.size());
        for (char ch : value)
        {
            if (std::isalnum(static_cast<unsigned char>(ch)))
                out.push_back(ch);
            else if (ch == ':' || ch == '_' || ch == '-')
                out.push_back('_');
        }

        while (!out.empty() && out.front() == '_')
            out.erase(out.begin());
        if (out.empty())
            out = "Candidate";
        if (std::isdigit(static_cast<unsigned char>(out.front())))
            out.insert(out.begin(), '_');
        if (out.size() > 48)
            out.resize(48);
        return out;
    }

    std::string CandidateLabel(const FunctionCandidateRecord& candidate)
    {
        std::string seed = candidate.relatedStrings.empty()
            ? candidate.primaryCategory
            : candidate.relatedStrings.front();
        std::ostringstream out;
        out << "FB_" << MakeSafeLabel(candidate.primaryCategory)
            << "_" << std::setw(3) << std::setfill('0') << candidate.score
            << "_" << MakeSafeLabel(seed);
        return out.str();
    }

    bool ParseHexAddressFromText(const std::string& text, std::uint64_t& outAddress)
    {
        const std::size_t start = text.find("0x");
        if (start == std::string::npos)
            return false;

        std::size_t end = start + 2;
        while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
            ++end;
        if (end == start + 2)
            return false;

        std::istringstream in(text.substr(start + 2, end - start - 2));
        in >> std::hex >> outAddress;
        return !in.fail();
    }

    bool ParseFloatCandidateValue(const std::string& text, float& outValue)
    {
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos)
            return false;

        std::size_t end = equals + 1;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-' || text[end] == '+' || text[end] == '.'))
            ++end;

        if (end == equals + 1)
            return false;

        outValue = std::strtof(text.c_str() + equals + 1, nullptr);
        return std::isfinite(outValue);
    }

    bool WriteStringsJson(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing discovered strings JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator read-only string discovery\",\n";
        file << "  \"stringCount\": " << discovery.strings.size() << ",\n";
        file << "  \"strings\": [\n";
        for (std::size_t index = 0; index < discovery.strings.size(); ++index)
        {
            const DiscoveredStringRecord& record = discovery.strings[index];
            file << "    {\n";
            file << "      \"value\": \"" << JsonEscape(record.value) << "\",\n";
            file << "      \"address\": " << record.address << ",\n";
            file << "      \"addressHex\": \"" << HexAddress(record.address) << "\",\n";
            file << "      \"module\": \"" << JsonEscape(record.module) << "\",\n";
            file << "      \"section\": \"" << JsonEscape(record.section) << "\",\n";
            file << "      \"encoding\": \"" << JsonEscape(record.encoding) << "\",\n";
            file << "      \"category\": \"" << JsonEscape(record.category) << "\",\n";
            file << "      \"score\": " << record.score << ",\n";
            file << "      \"xrefCount\": " << record.xrefCount << ",\n";
            file << "      \"nearbyXrefs\": [";
            std::size_t writtenXrefs = 0;
            for (const StringXrefRecord& xref : discovery.xrefs)
            {
                if (xref.stringAddress != record.address)
                    continue;
                if (writtenXrefs > 0)
                    file << ", ";
                file << "{ \"reference\": \"" << HexAddress(xref.referenceAddress)
                     << "\", \"function\": \"" << HexAddress(xref.functionAddress)
                     << "\", \"module\": \"" << JsonEscape(xref.module) << "\" }";
                if (++writtenXrefs >= 8)
                    break;
            }
            file << "]\n";
            file << "    }" << (index + 1 < discovery.strings.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    bool WriteStringXrefsJson(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing string xrefs JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator read-only xref discovery\",\n";
        file << "  \"xrefCount\": " << discovery.xrefs.size() << ",\n";
        file << "  \"xrefs\": [\n";
        for (std::size_t index = 0; index < discovery.xrefs.size(); ++index)
        {
            const StringXrefRecord& record = discovery.xrefs[index];
            file << "    {\n";
            file << "      \"value\": \"" << JsonEscape(record.value) << "\",\n";
            file << "      \"category\": \"" << JsonEscape(record.category) << "\",\n";
            file << "      \"module\": \"" << JsonEscape(record.module) << "\",\n";
            file << "      \"section\": \"" << JsonEscape(record.section) << "\",\n";
            file << "      \"stringAddress\": " << record.stringAddress << ",\n";
            file << "      \"stringAddressHex\": \"" << HexAddress(record.stringAddress) << "\",\n";
            file << "      \"referenceAddress\": " << record.referenceAddress << ",\n";
            file << "      \"referenceAddressHex\": \"" << HexAddress(record.referenceAddress) << "\",\n";
            file << "      \"functionAddress\": " << record.functionAddress << ",\n";
            file << "      \"functionAddressHex\": \"" << HexAddress(record.functionAddress) << "\",\n";
            file << "      \"surroundingBytes\": \"" << JsonEscape(record.surroundingBytes) << "\",\n";
            file << "      \"mathOperationCount\": " << record.mathOperationCount << ",\n";
            file << "      \"score\": " << record.score << ",\n";
            file << "      \"nearbyCallTargets\": [";
            for (std::size_t callIndex = 0; callIndex < record.nearbyCallTargets.size(); ++callIndex)
            {
                file << "\"" << HexAddress(record.nearbyCallTargets[callIndex]) << "\""
                     << (callIndex + 1 < record.nearbyCallTargets.size() ? ", " : "");
            }
            file << "],\n";
            file << "      \"nearbyFloatCandidates\": [";
            for (std::size_t valueIndex = 0; valueIndex < record.nearbyFloatCandidates.size(); ++valueIndex)
            {
                file << "\"" << JsonEscape(record.nearbyFloatCandidates[valueIndex]) << "\""
                     << (valueIndex + 1 < record.nearbyFloatCandidates.size() ? ", " : "");
            }
            file << "]\n";
            file << "    }" << (index + 1 < discovery.xrefs.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    bool WriteInterestingModulesMarkdown(const fs::path& path, const std::vector<LiveModuleRecord>& modules)
    {
        Progress(L"Writing interesting modules report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::vector<const LiveModuleRecord*> ranked;
        for (const LiveModuleRecord& module : modules)
            ranked.push_back(&module);
        std::sort(ranked.begin(), ranked.end(), [](const LiveModuleRecord* lhs, const LiveModuleRecord* rhs) {
            if (lhs->relevanceScore != rhs->relevanceScore)
                return lhs->relevanceScore > rhs->relevanceScore;
            return ToLower(lhs->name) < ToLower(rhs->name);
        });

        file << "# Interesting Modules\n\n";
        file << "Ranked by game/Frostbite relevance. Windows, Steam, NVIDIA, and CRT runtime modules are intentionally de-prioritized.\n\n";
        file << "| Score | Bucket | Module | Sections | PDATA Functions | RTTI Classes | Symbols | Path |\n";
        file << "| ---: | --- | --- | ---: | ---: | ---: | ---: | --- |\n";
        for (const LiveModuleRecord* module : ranked)
        {
            file << "| " << module->relevanceScore
                 << " | `" << RelevanceToString(module->relevance)
                 << "` | `" << JsonEscape(WideToUtf8(module->name))
                 << "` | " << module->sections.size()
                 << " | " << module->functionRanges.size()
                 << " | " << module->classes.size()
                 << " | " << module->symbols.size()
                 << " | `" << JsonEscape(PathToUtf8(module->path)) << "` |\n";
        }

        return true;
    }

    bool IsRelevantClassForMainReport(const RttiClassRecord& record)
    {
        return record.relevance == "GameSpecific" ||
               record.relevance == "FrostbiteLikely" ||
               record.confidence >= 55;
    }

    bool WriteFrostbiteTypesMarkdown(const fs::path& path, const std::vector<LiveModuleRecord>& modules)
    {
        Progress(L"Writing Frostbite types report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Frostbite/Game RTTI Types\n\n";
        file << "This report filters out Windows, Steam, NVIDIA, and CRT noise. It is based on MSVC RTTI and vtable discovery, so field offsets require PDB data or the owned reflection provider.\n\n";
        file << "| Confidence | Bucket | Module | Class | Bases | VTable | Virtuals |\n";
        file << "| ---: | --- | --- | --- | --- | ---: | ---: |\n";
        for (const LiveModuleRecord& module : modules)
        {
            for (const RttiClassRecord& record : module.classes)
            {
                if (!IsRelevantClassForMainReport(record))
                    continue;

                file << "| " << record.confidence
                     << " | `" << JsonEscape(record.relevance)
                     << "` | `" << JsonEscape(WideToUtf8(module.name))
                     << "` | `" << JsonEscape(record.name)
                     << "` | `" << JsonEscape(JoinStrings(record.baseClasses, "; "))
                     << "` | `" << HexAddress(record.vtable)
                     << "` | " << record.virtualFunctions.size() << " |\n";
            }
        }

        return true;
    }

    bool WriteFrostbiteTypesHeader(const fs::path& path, const std::vector<LiveModuleRecord>& modules)
    {
        Progress(L"Writing Frostbite types generated header: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteResearchSDK::Types\n";
        file << "{\n";
        file << "    struct TypeInfo\n";
        file << "    {\n";
        file << "        const char* module;\n";
        file << "        const char* name;\n";
        file << "        const char* bases;\n";
        file << "        const char* bucket;\n";
        file << "        std::uint64_t vtable;\n";
        file << "        std::uint32_t virtualFunctionCount;\n";
        file << "        std::uint32_t confidence;\n";
        file << "    };\n\n";
        file << "    inline constexpr TypeInfo Types[] =\n";
        file << "    {\n";
        std::size_t count = 0;
        for (const LiveModuleRecord& module : modules)
        {
            for (const RttiClassRecord& record : module.classes)
            {
                if (!IsRelevantClassForMainReport(record))
                    continue;
                ++count;
                file << "        { \"" << HeaderEscape(WideToUtf8(module.name)) << "\", \""
                     << HeaderEscape(record.name) << "\", \""
                     << HeaderEscape(JoinStrings(record.baseClasses, ";")) << "\", \""
                     << HeaderEscape(record.relevance) << "\", "
                     << "0x" << std::hex << record.vtable << std::dec << "ull, "
                     << record.virtualFunctions.size() << "u, "
                     << record.confidence << "u },\n";
            }
        }
        if (count == 0)
            file << "        { \"\", \"\", \"\", \"\", 0ull, 0u, 0u },\n";
        file << "    };\n\n";
        file << "    inline constexpr std::size_t TypeCount = " << count << ";\n";
        file << "}\n";
        return true;
    }

    bool CategoryMatchesCandidateReport(const std::string& category, bool environmentReport)
    {
        if (environmentReport)
            return category == "Environment" || category == "Rendering";
        return category == "Time";
    }

    bool FunctionCandidateMatchesReport(const FunctionCandidateRecord& candidate, bool environmentReport)
    {
        if (environmentReport)
            return CandidateMatchesAnyCategory(candidate, { "Environment", "Rendering" });
        return CandidateMatchesCategory(candidate, "Time");
    }

    bool WriteCandidateReport(
        const fs::path& path,
        const char* title,
        bool environmentReport,
        const DiscoveryDump& discovery)
    {
        Progress(L"Writing candidate report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# " << title << "\n\n";
        if (environmentReport)
            file << "**No environment manager found. Ranked candidates only, derived from strings, xrefs, nearby calls, nearby floats, and math-op proximity.**\n\n";
        else
            file << "**No CVar registry found. Ranked candidates only, derived from strings, xrefs, nearby calls, nearby floats, and math-op proximity.**\n\n";

        file << "## Function Candidates\n\n";
        file << "| Score | Tier | Cluster | Phase | Function Range | Size | Boundary | Module | Xrefs | Strings | Floats/Data | Graph | Reasoning |\n";
        file << "| ---: | --- | --- | --- | ---: | ---: | --- | --- | ---: | --- | --- | --- | --- |\n";
        std::size_t written = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (!FunctionCandidateMatchesReport(candidate, environmentReport))
                continue;
            ++written;
            file << "| " << candidate.score
                 << " | `" << JsonEscape(candidate.tier)
                 << "` | `" << JsonEscape(candidate.cluster)
                 << "` | `" << JsonEscape(candidate.phaseGuess)
                 << "` | `" << HexAddress(candidate.functionAddress)
                 << "-" << HexAddress(candidate.functionEnd)
                 << (candidate.fallbackOnly ? " (data-only fallback)" : "")
                 << "` | " << (candidate.functionEnd > candidate.functionAddress ? candidate.functionEnd - candidate.functionAddress : 0)
                 << " | `" << JsonEscape(candidate.boundarySource) << " / " << candidate.boundaryConfidence
                 << "% / xrefStart=" << (candidate.xrefAtFunctionStart ? "yes" : "no")
                 << "` | `" << JsonEscape(candidate.module)
                 << "` | " << candidate.xrefCount
                 << " | `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; "))
                 << "` | `" << JsonEscape(JoinStrings(candidate.nearbyFloatCandidates, "; "))
                 << "` | `callers=" << JsonEscape(JoinHexAddresses(candidate.callers))
                 << "; callees=" << JsonEscape(JoinHexAddresses(candidate.callees))
                 << "` | " << JsonEscape(candidate.reasoning)
                 << "` |\n";
        }

        if (written == 0)
            file << "| 0 | `Noise` | `none` | `unknown` | `0x0-0x0` | 0 | `none` | `none` | 0 | `No matching strings survived filtering` | `` | `` | `No ranked candidate could be produced for this category.` |\n";

        file << "\n## Candidate Strings\n\n";
        file << "| Score | Category | Module | Address | Xrefs | Value |\n";
        file << "| ---: | --- | --- | ---: | ---: | --- |\n";
        for (const DiscoveredStringRecord& record : discovery.strings)
        {
            if (!CategoryMatchesCandidateReport(record.category, environmentReport))
                continue;
            file << "| " << record.score
                 << " | `" << JsonEscape(record.category)
                 << "` | `" << JsonEscape(record.module)
                 << "` | `" << HexAddress(record.address)
                 << "` | " << record.xrefCount
                 << " | `" << JsonEscape(record.value) << "` |\n";
        }

        file << "\n## Notes\n\n";
        if (environmentReport)
            file << "Likely render/environment systems are the functions with sky/fog/sun/exposure/cubemap strings, nearby call targets, math op proximity, and nearby read-only float/data references. Treat these as leads, not verified managers.\n";
        else
            file << "Likely time/simulation systems are the functions with timescale/delta/tick strings, nearby SSE math, and nearby read-only float/data values such as 0.016, 1.0, or 60.0. Treat these as leads, not verified CVars.\n";
        return true;
    }

    bool WriteHighConfidenceFunctionsMarkdown(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing high confidence functions report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# High Confidence Functions\n\n";
        file << "Ranked read-only function candidates grouped by string xrefs, `.pdata` function bounds, and local caller/callee evidence. These are validation leads, not write targets.\n\n";
        file << "| Score | Tier | Cluster | Phase | Function Range | Size | Boundary | Module | Xrefs | Strings | Floats/Data | Calls | Reasoning |\n";
        file << "| ---: | --- | --- | --- | ---: | ---: | --- | --- | ---: | --- | --- | --- | --- |\n";
        std::size_t written = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score < 60 && written > 0)
                continue;

            file << "| " << candidate.score
                 << " | `" << JsonEscape(candidate.tier)
                 << "` | `" << JsonEscape(candidate.cluster)
                 << "` | `" << JsonEscape(candidate.phaseGuess)
                 << "` | `" << HexAddress(candidate.functionAddress)
                 << "-" << HexAddress(candidate.functionEnd)
                 << (candidate.fallbackOnly ? " (data-only fallback)" : "")
                 << "` | " << (candidate.functionEnd > candidate.functionAddress ? candidate.functionEnd - candidate.functionAddress : 0)
                 << " | `" << JsonEscape(candidate.boundarySource) << " / " << candidate.boundaryConfidence
                 << "% / xrefStart=" << (candidate.xrefAtFunctionStart ? "yes" : "no")
                 << "` | `" << JsonEscape(candidate.module)
                 << "` | " << candidate.xrefCount
                 << " | `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; "))
                 << "` | `" << JsonEscape(JoinStrings(candidate.nearbyFloatCandidates, "; "))
                 << "` | `callers=" << JsonEscape(JoinHexAddresses(candidate.callers))
                 << "; callees=" << JsonEscape(JoinHexAddresses(candidate.callees))
                 << "`"
                 << " | `" << JsonEscape(candidate.reasoning) << "` |\n";

            if (++written >= 200)
                break;
        }

        if (written == 0)
            file << "| 0 | `Noise` | `none` | `unknown` | `0x0-0x0` | 0 | `none` | `none` | 0 | `No matching strings survived filtering` | `` | `` | `No ranked candidates were produced.` |\n";

        return true;
    }

    bool WriteRuntimeDiscoveryJson(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing discovery runtime introspection JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator discovery-based runtime introspection\",\n";
        file << "  \"note\": \"No provider exports, reflection registry, or CVar registry are required. Candidates are derived from read-only strings, xrefs, nearby calls, nearby floats, and math-op proximity.\",\n";
        file << "  \"functionCandidateCount\": " << discovery.functionCandidates.size() << ",\n";
        file << "  \"callEdgeCount\": " << discovery.callEdges.size() << ",\n";
        file << "  \"enumTableCount\": " << discovery.enumTables.size() << ",\n";
        file << "  \"decomposedNameCount\": " << discovery.decomposedNames.size() << ",\n";
        file << "  \"functionCandidates\": [\n";
        for (std::size_t index = 0; index < discovery.functionCandidates.size(); ++index)
        {
            const FunctionCandidateRecord& candidate = discovery.functionCandidates[index];
            file << "    {\n";
            file << "      \"module\": \"" << JsonEscape(candidate.module) << "\",\n";
            file << "      \"primaryCategory\": \"" << JsonEscape(candidate.primaryCategory) << "\",\n";
            file << "      \"cluster\": \"" << JsonEscape(candidate.cluster) << "\",\n";
            file << "      \"tier\": \"" << JsonEscape(candidate.tier) << "\",\n";
            file << "      \"phaseGuess\": \"" << JsonEscape(candidate.phaseGuess) << "\",\n";
            file << "      \"functionAddress\": " << candidate.functionAddress << ",\n";
            file << "      \"functionAddressHex\": \"" << HexAddress(candidate.functionAddress) << "\",\n";
            file << "      \"functionEnd\": " << candidate.functionEnd << ",\n";
            file << "      \"functionEndHex\": \"" << HexAddress(candidate.functionEnd) << "\",\n";
            file << "      \"functionSize\": " << (candidate.functionEnd > candidate.functionAddress ? candidate.functionEnd - candidate.functionAddress : 0) << ",\n";
            file << "      \"boundarySource\": \"" << JsonEscape(candidate.boundarySource) << "\",\n";
            file << "      \"boundaryConfidence\": " << candidate.boundaryConfidence << ",\n";
            file << "      \"xrefAtFunctionStart\": " << (candidate.xrefAtFunctionStart ? "true" : "false") << ",\n";
            file << "      \"score\": " << candidate.score << ",\n";
            file << "      \"xrefCount\": " << candidate.xrefCount << ",\n";
            file << "      \"mathOperationCount\": " << candidate.mathOperationCount << ",\n";
            file << "      \"fallbackOnly\": " << (candidate.fallbackOnly ? "true" : "false") << ",\n";
            file << "      \"reasoning\": \"" << JsonEscape(candidate.reasoning) << "\",\n";
            file << "      \"categories\": [";
            for (std::size_t categoryIndex = 0; categoryIndex < candidate.categories.size(); ++categoryIndex)
                file << "\"" << JsonEscape(candidate.categories[categoryIndex]) << "\"" << (categoryIndex + 1 < candidate.categories.size() ? ", " : "");
            file << "],\n";
            file << "      \"relatedStrings\": [";
            for (std::size_t stringIndex = 0; stringIndex < candidate.relatedStrings.size(); ++stringIndex)
                file << "\"" << JsonEscape(candidate.relatedStrings[stringIndex]) << "\"" << (stringIndex + 1 < candidate.relatedStrings.size() ? ", " : "");
            file << "],\n";
            file << "      \"referenceAddresses\": [";
            for (std::size_t refIndex = 0; refIndex < candidate.referenceAddresses.size(); ++refIndex)
                file << "\"" << HexAddress(candidate.referenceAddresses[refIndex]) << "\"" << (refIndex + 1 < candidate.referenceAddresses.size() ? ", " : "");
            file << "],\n";
            file << "      \"nearbyCallTargets\": [";
            for (std::size_t callIndex = 0; callIndex < candidate.nearbyCallTargets.size(); ++callIndex)
                file << "\"" << HexAddress(candidate.nearbyCallTargets[callIndex]) << "\"" << (callIndex + 1 < candidate.nearbyCallTargets.size() ? ", " : "");
            file << "],\n";
            file << "      \"nearbyFloatCandidates\": [";
            for (std::size_t valueIndex = 0; valueIndex < candidate.nearbyFloatCandidates.size(); ++valueIndex)
                file << "\"" << JsonEscape(candidate.nearbyFloatCandidates[valueIndex]) << "\"" << (valueIndex + 1 < candidate.nearbyFloatCandidates.size() ? ", " : "");
            file << "],\n";
            file << "      \"floatClassifications\": [";
            for (std::size_t valueIndex = 0; valueIndex < candidate.floatClassifications.size(); ++valueIndex)
                file << "\"" << JsonEscape(candidate.floatClassifications[valueIndex]) << "\"" << (valueIndex + 1 < candidate.floatClassifications.size() ? ", " : "");
            file << "],\n";
            file << "      \"callers\": [";
            for (std::size_t callerIndex = 0; callerIndex < candidate.callers.size(); ++callerIndex)
                file << "\"" << HexAddress(candidate.callers[callerIndex]) << "\"" << (callerIndex + 1 < candidate.callers.size() ? ", " : "");
            file << "],\n";
            file << "      \"callees\": [";
            for (std::size_t calleeIndex = 0; calleeIndex < candidate.callees.size(); ++calleeIndex)
                file << "\"" << HexAddress(candidate.callees[calleeIndex]) << "\"" << (calleeIndex + 1 < candidate.callees.size() ? ", " : "");
            file << "]\n";
            file << "    }" << (index + 1 < discovery.functionCandidates.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    bool WriteRuntimeDiscoveryMarkdown(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing discovery runtime introspection report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Discovery-Based Runtime Introspection\n\n";
        file << "**No provider exports are used. No CVar registry found. No environment manager found.**\n\n";
        file << "This file ranks candidate functions from read-only string scanning and xref analysis.\n\n";
        file << "- Function candidates: `" << discovery.functionCandidates.size() << "`\n";
        file << "- Relevant strings: `" << discovery.strings.size() << "`\n";
        file << "- String xrefs: `" << discovery.xrefs.size() << "`\n";
        file << "- Local call graph edges: `" << discovery.callEdges.size() << "`\n\n";
        file << "See `ResearchDashboard.md`, `TimeCandidates.md`, `EnvironmentCandidates.md`, `HighConfidenceFunctions.md`, and `FunctionTraces` for focused validation reports.\n";
        return true;
    }

    bool WriteCandidateCallGraphJson(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing candidate call graph JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator read-only candidate call graph\",\n";
        file << "  \"edgeCount\": " << discovery.callEdges.size() << ",\n";
        std::vector<const FunctionCandidateRecord*> graphCandidates;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score < 50)
                continue;
            graphCandidates.push_back(&candidate);
            if (graphCandidates.size() >= 300)
                break;
        }

        file << "  \"candidateGraphs\": [\n";
        for (std::size_t graphIndex = 0; graphIndex < graphCandidates.size(); ++graphIndex)
        {
            const FunctionCandidateRecord& candidate = *graphCandidates[graphIndex];

            file << "    {\n";
            file << "      \"label\": \"" << JsonEscape(CandidateLabel(candidate)) << "\",\n";
            file << "      \"module\": \"" << JsonEscape(candidate.module) << "\",\n";
            file << "      \"tier\": \"" << JsonEscape(candidate.tier) << "\",\n";
            file << "      \"cluster\": \"" << JsonEscape(candidate.cluster) << "\",\n";
            file << "      \"function\": \"" << HexAddress(candidate.functionAddress) << "\",\n";
            file << "      \"functionEnd\": \"" << HexAddress(candidate.functionEnd) << "\",\n";
            file << "      \"callers\": [";
            for (std::size_t index = 0; index < candidate.callers.size(); ++index)
                file << "\"" << HexAddress(candidate.callers[index]) << "\"" << (index + 1 < candidate.callers.size() ? ", " : "");
            file << "],\n";
            file << "      \"callees\": [";
            for (std::size_t index = 0; index < candidate.callees.size(); ++index)
                file << "\"" << HexAddress(candidate.callees[index]) << "\"" << (index + 1 < candidate.callees.size() ? ", " : "");
            file << "]\n";
            file << "    }" << (graphIndex + 1 < graphCandidates.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    std::string ClassifyInstructionBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t& outLength)
    {
        outLength = 1;
        if (offset >= bytes.size())
            return "db";

        const std::uint8_t op = bytes[offset];
        if (op == 0xE8 && offset + 5 <= bytes.size())
        {
            outLength = 5;
            return "call rel32";
        }
        if (op == 0xE9 && offset + 5 <= bytes.size())
        {
            outLength = 5;
            return "jmp rel32";
        }
        if (op == 0xEB && offset + 2 <= bytes.size())
        {
            outLength = 2;
            return "jmp rel8";
        }
        if (op >= 0x70 && op <= 0x7F && offset + 2 <= bytes.size())
        {
            outLength = 2;
            return "jcc rel8";
        }
        if (op == 0x0F && offset + 6 <= bytes.size() && bytes[offset + 1] >= 0x80 && bytes[offset + 1] <= 0x8F)
        {
            outLength = 6;
            return "jcc rel32";
        }
        if (op == 0xFF && offset + 2 <= bytes.size())
        {
            if ((bytes[offset + 1] & 0x38) == 0x10)
                return "call indirect";
            if ((bytes[offset + 1] & 0x38) == 0x20)
                return "jmp indirect";
        }
        if ((op == 0xF3 || op == 0xF2) && offset + 3 <= bytes.size() && bytes[offset + 1] == 0x0F)
        {
            outLength = 4;
            return "scalar SSE/math";
        }
        if (op == 0x0F && offset + 2 <= bytes.size())
        {
            if (bytes[offset + 1] == 0x28 || bytes[offset + 1] == 0x29 ||
                bytes[offset + 1] == 0x58 || bytes[offset + 1] == 0x59 ||
                bytes[offset + 1] == 0x5C || bytes[offset + 1] == 0x5E)
            {
                outLength = 3;
                return "packed SSE/math";
            }
        }
        if ((op == 0x48 || op == 0x4C) && offset + 7 <= bytes.size() &&
            (bytes[offset + 1] == 0x8D || bytes[offset + 1] == 0x8B || bytes[offset + 1] == 0x39 || bytes[offset + 1] == 0x3B) &&
            ((bytes[offset + 2] & 0xC7) == 0x05))
        {
            outLength = 7;
            return "rip-relative data";
        }
        if ((op == 0x8D || op == 0x8B || op == 0x39 || op == 0x3B) && offset + 6 <= bytes.size() &&
            ((bytes[offset + 1] & 0xC7) == 0x05))
        {
            outLength = 6;
            return "rip-relative data";
        }
        if (op == 0xC3)
            return "ret";
        return "byte";
    }

    std::string AnnotationForInstruction(
        const LiveModuleRecord& module,
        const FunctionCandidateRecord& candidate,
        const std::vector<std::uint8_t>& bytes,
        std::size_t offset,
        std::uint64_t address)
    {
        std::vector<std::string> annotations;

        if (std::find(candidate.referenceAddresses.begin(), candidate.referenceAddresses.end(), address) != candidate.referenceAddresses.end())
            annotations.push_back("string xref site");

        if (offset + 5 <= bytes.size() && bytes[offset] == 0xE8)
        {
            std::int32_t disp = 0;
            std::memcpy(&disp, bytes.data() + offset + 1, sizeof(disp));
            const std::uint64_t target = address + 5 + disp;
            annotations.push_back("call target " + HexAddress(NormalizeFunctionAddress(module, target)));
        }

        if (offset + 5 <= bytes.size() && bytes[offset] == 0xE9)
        {
            std::int32_t disp = 0;
            std::memcpy(&disp, bytes.data() + offset + 1, sizeof(disp));
            annotations.push_back("branch target " + HexAddress(address + 5 + disp));
        }
        else if (offset + 2 <= bytes.size() && (bytes[offset] == 0xEB || (bytes[offset] >= 0x70 && bytes[offset] <= 0x7F)))
        {
            const std::int8_t disp = static_cast<std::int8_t>(bytes[offset + 1]);
            annotations.push_back(std::string(bytes[offset] == 0xEB ? "branch target " : "conditional branch target ") + HexAddress(address + 2 + disp));
        }
        else if (offset + 6 <= bytes.size() && bytes[offset] == 0x0F && bytes[offset + 1] >= 0x80 && bytes[offset + 1] <= 0x8F)
        {
            std::int32_t disp = 0;
            std::memcpy(&disp, bytes.data() + offset + 2, sizeof(disp));
            annotations.push_back("conditional branch target " + HexAddress(address + 6 + disp));
        }

        std::uint64_t ripTarget = 0;
        std::size_t instructionLength = 0;
        if (TryDecodeRipRelativeReference(bytes, offset, address, ripTarget, instructionLength))
        {
            if (std::find(candidate.stringAddresses.begin(), candidate.stringAddresses.end(), ripTarget) != candidate.stringAddresses.end())
                annotations.push_back("RIP string " + HexAddress(ripTarget));
            else
                annotations.push_back("RIP data " + HexAddress(ripTarget));

            float value = 0.0f;
            if (IsReadableDataAddress(module, ripTarget, sizeof(float)) && SafeReadValue(ripTarget, value))
                annotations.push_back("float " + ClassifyFloatValue(value) + " = " + std::to_string(value));
        }

        if (offset + 2 <= bytes.size() && bytes[offset] == 0xFF)
        {
            if ((bytes[offset + 1] & 0x38) == 0x10)
                annotations.push_back("vtable-like or function-pointer indirect call");
            if ((bytes[offset + 1] & 0x38) == 0x20)
                annotations.push_back("possible switch/jump-table indirect branch");
        }

        if ((offset + 3 <= bytes.size() && (bytes[offset] == 0xF3 || bytes[offset] == 0xF2) && bytes[offset + 1] == 0x0F) ||
            (offset + 2 <= bytes.size() && bytes[offset] == 0x0F))
        {
            annotations.push_back("math/SSE proximity");
        }

        return JoinStrings(annotations, "; ");
    }

    const FunctionCandidateRecord* FindCandidateByAddress(const DiscoveryDump& discovery, std::uint64_t address)
    {
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.functionAddress == address)
                return &candidate;
        }
        return nullptr;
    }

    bool WriteAnnotatedDisassembly(
        std::ofstream& trace,
        const LiveModuleRecord& module,
        const FunctionCandidateRecord& candidate)
    {
        if (candidate.functionEnd <= candidate.functionAddress)
        {
            trace << "Function range unavailable, so no annotated bytes were emitted.\n";
            return true;
        }

        const std::uint64_t size64 = candidate.functionEnd - candidate.functionAddress;
        const std::size_t readSize = static_cast<std::size_t>(std::min<std::uint64_t>(size64, 4096));
        std::vector<std::uint8_t> bytes(readSize);
        if (!SafeReadBytes(candidate.functionAddress, bytes.data(), bytes.size()))
        {
            trace << "Could not read function bytes safely.\n";
            return true;
        }

        trace << "| Address | Bytes | Decoded Hint | Annotation |\n";
        trace << "| ---: | --- | --- | --- |\n";
        for (std::size_t offset = 0; offset < bytes.size();)
        {
            std::size_t length = 1;
            const std::string hint = ClassifyInstructionBytes(bytes, offset, length);
            length = std::max<std::size_t>(1, std::min<std::size_t>(length, bytes.size() - offset));
            const std::uint64_t address = candidate.functionAddress + offset;
            const std::string annotation = AnnotationForInstruction(module, candidate, bytes, offset, address);

            if (!annotation.empty() || hint != "byte")
            {
                trace << "| `" << HexAddress(address)
                      << "` | `" << BytesToHex(bytes, offset, length)
                      << "` | `" << JsonEscape(hint)
                      << "` | `" << JsonEscape(annotation) << "` |\n";
            }

            offset += length;
        }

        if (readSize < size64)
            trace << "\nTrace clipped to first `" << readSize << "` bytes of `" << size64 << "` bytes.\n";
        return true;
    }

    bool WriteFunctionTraceReports(const fs::path& directory, const DiscoveryDump& discovery, const std::vector<LiveModuleRecord>& modules)
    {
        Progress(L"Writing focused function trace reports: " + directory.wstring());
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec)
            return false;

        const fs::path indexPath = directory / L"Index.md";
        std::ofstream index(indexPath, std::ios::binary | std::ios::trunc);
        if (!index)
            return false;

        index << "# Focused Function Traces\n\n";
        index << "Each trace is read-only evidence for one ranked candidate: string xrefs, `.pdata` range, float constants, and local caller/callee graph.\n\n";
        index << "| Rank | Score | Tier | Cluster | Function | Trace |\n";
        index << "| ---: | ---: | --- | --- | ---: | --- |\n";

        std::size_t rank = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score < 60 && rank > 0)
                continue;
            ++rank;

            std::ostringstream fileName;
            fileName << "Trace_" << MakeSafeLabel(candidate.relatedStrings.empty() ? candidate.cluster : candidate.relatedStrings.front())
                     << "_" << std::hex << candidate.functionAddress << ".md";
            const fs::path tracePath = directory / fileName.str();
            std::ofstream trace(tracePath, std::ios::binary | std::ios::trunc);
            if (!trace)
                return false;

            trace << "# Function Trace: `" << HexAddress(candidate.functionAddress) << "`\n\n";
            trace << "- Label: `" << JsonEscape(CandidateLabel(candidate)) << "`\n";
            trace << "- Tier: `" << JsonEscape(candidate.tier) << "`\n";
            trace << "- Cluster: `" << JsonEscape(candidate.cluster) << "`\n";
            trace << "- Phase guess: `" << JsonEscape(candidate.phaseGuess) << "`\n";
            trace << "- Module: `" << JsonEscape(candidate.module) << "`\n";
            trace << "- Range: `" << HexAddress(candidate.functionAddress) << "-" << HexAddress(candidate.functionEnd) << "` via `" << JsonEscape(candidate.boundarySource) << "`\n";
            trace << "- Size: `" << (candidate.functionEnd > candidate.functionAddress ? candidate.functionEnd - candidate.functionAddress : 0) << "` bytes\n";
            trace << "- Boundary confidence: `" << candidate.boundaryConfidence << "`\n";
            trace << "- Xref at function start: `" << (candidate.xrefAtFunctionStart ? "true" : "false") << "`\n";
            trace << "- Score: `" << candidate.score << "`\n";
            trace << "- Reasoning: `" << JsonEscape(candidate.reasoning) << "`\n\n";

            trace << "## Related Strings\n\n";
            for (std::size_t indexString = 0; indexString < candidate.relatedStrings.size(); ++indexString)
            {
                trace << "- `" << JsonEscape(candidate.relatedStrings[indexString]) << "`";
                if (indexString < candidate.stringAddresses.size())
                    trace << " at `" << HexAddress(candidate.stringAddresses[indexString]) << "`";
                trace << "\n";
            }
            if (candidate.relatedStrings.empty())
                trace << "No related strings recorded.\n";

            trace << "\n## String References\n\n";
            for (std::uint64_t reference : candidate.referenceAddresses)
                trace << "- `" << HexAddress(reference) << "`\n";
            if (candidate.referenceAddresses.empty())
                trace << "No decoded code references recorded.\n";

            trace << "\n## Float/Data Evidence\n\n";
            for (const std::string& value : candidate.nearbyFloatCandidates)
                trace << "- `" << JsonEscape(value) << "`\n";
            if (candidate.nearbyFloatCandidates.empty())
                trace << "No nearby read-only float/data values recorded.\n";

            trace << "\n## Local Call Graph\n\n";
            trace << "### Callers\n\n";
            for (std::uint64_t caller : candidate.callers)
            {
                const FunctionCandidateRecord* callerCandidate = FindCandidateByAddress(discovery, caller);
                trace << "- `" << HexAddress(caller) << "` relevance `"
                      << (callerCandidate ? callerCandidate->score : 0)
                      << "` phase `" << JsonEscape(callerCandidate ? callerCandidate->phaseGuess : "unknown") << "`\n";
            }
            if (candidate.callers.empty())
                trace << "No callers found in the local direct-call graph.\n";

            trace << "\n### Callees\n\n";
            for (std::uint64_t callee : candidate.callees)
            {
                const FunctionCandidateRecord* calleeCandidate = FindCandidateByAddress(discovery, callee);
                trace << "- `" << HexAddress(callee) << "` relevance `"
                      << (calleeCandidate ? calleeCandidate->score : 0)
                      << "` phase `" << JsonEscape(calleeCandidate ? calleeCandidate->phaseGuess : "unknown") << "`\n";
            }
            if (candidate.callees.empty())
                trace << "No callees found in the local direct-call graph.\n";

            trace << "\n## Annotated Disassembly\n\n";
            if (const LiveModuleRecord* module = FindModuleByUtf8Name(modules, candidate.module))
                WriteAnnotatedDisassembly(trace, *module, candidate);
            else
                trace << "Module record unavailable, so annotated bytes were skipped.\n";

            index << "| " << rank
                  << " | " << candidate.score
                  << " | `" << JsonEscape(candidate.tier)
                  << "` | `" << JsonEscape(candidate.cluster)
                  << "` | `" << HexAddress(candidate.functionAddress)
                  << "` | `" << JsonEscape(fileName.str()) << "` |\n";

            if (rank >= 24)
                break;
        }

        if (rank == 0)
            index << "| 0 | 0 | `Noise` | `none` | `0x0` | `No trace candidates survived filtering.` |\n";

        return true;
    }

    std::string ClassifyWatchSamples(const WatchValueRecord& record)
    {
        if (!record.readable || record.samples.empty())
            return "unknown";

        float minValue = record.samples.front();
        float maxValue = record.samples.front();
        bool monotonicIncreasing = true;
        for (std::size_t index = 0; index < record.samples.size(); ++index)
        {
            minValue = std::min(minValue, record.samples[index]);
            maxValue = std::max(maxValue, record.samples[index]);
            if (index > 0 && record.samples[index] + 0.0001f < record.samples[index - 1])
                monotonicIncreasing = false;
        }

        const float span = maxValue - minValue;
        if (span <= 0.0001f)
        {
            if (record.classification.find("FrameDelta") != std::string::npos ||
                record.classification.find("Fps") != std::string::npos ||
                record.classification.find("Multiplier") != std::string::npos)
            {
                return "config constant";
            }
            return "static";
        }
        if (span > 0.0001f && span < 0.1f && monotonicIncreasing)
            return "frame timer";
        if (span >= 1.0f && monotonicIncreasing)
            return "counter";
        if (span > 0.0f && span <= 4.0f)
            return "state value";
        return "unknown";
    }

    std::vector<WatchValueRecord> CaptureRuntimeWatchValues(const DiscoveryDump& discovery, std::size_t limit)
    {
        std::vector<WatchValueRecord> records;
        std::set<std::uint64_t> seen;

        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score < 50)
                continue;

            for (const std::string& text : candidate.nearbyFloatCandidates)
            {
                std::uint64_t address = 0;
                if (!ParseHexAddressFromText(text, address) || !seen.insert(address).second)
                    continue;

                WatchValueRecord record;
                record.address = address;
                record.candidateLabel = CandidateLabel(candidate);
                record.classification = "Unknown";
                record.notes = "pause/menu/level sensitivity requires comparing WatchReport outputs from separate runs";

                const std::size_t open = text.find('[');
                const std::size_t close = open == std::string::npos ? std::string::npos : text.find(']', open + 1);
                if (open != std::string::npos && close != std::string::npos && close > open + 1)
                    record.classification = text.substr(open + 1, close - open - 1);

                ParseFloatCandidateValue(text, record.staticValue);
                for (int sample = 0; sample < 8; ++sample)
                {
                    float value = 0.0f;
                    const bool ok = SafeReadValue(address, value);
                    record.readable = record.readable || ok;
                    if (ok)
                        record.samples.push_back(value);
                    ::Sleep(33);
                }
                if (record.samples.empty())
                    record.volatility = "unreadable";
                else
                {
                    const auto [minIt, maxIt] = std::minmax_element(record.samples.begin(), record.samples.end());
                    const float span = *maxIt - *minIt;
                    if (span <= 0.0001f)
                        record.volatility = "static";
                    else if (span < 0.1f)
                        record.volatility = "low";
                    else if (span < 10.0f)
                        record.volatility = "medium";
                    else
                        record.volatility = "high";
                }
                record.runtimeClass = ClassifyWatchSamples(record);
                records.push_back(std::move(record));

                if (records.size() >= limit)
                    return records;
            }
        }

        return records;
    }

    bool WriteRuntimeValueWatchersMarkdown(const fs::path& path, const DiscoveryDump& discovery, const std::vector<WatchValueRecord>& records)
    {
        (void)discovery;
        Progress(L"Writing read-only runtime value watcher: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Runtime Value Watchers\n\n";
        file << "Read-only snapshots for suspected float/global addresses found near candidate functions. This report never writes or patches process memory.\n\n";
        file << "| Address | Candidate | Classification | Static Value | Samples | Volatility | Runtime Class | Notes |\n";
        file << "| ---: | --- | --- | ---: | --- | --- | --- | --- |\n";

        for (const WatchValueRecord& record : records)
        {
            std::vector<std::string> sampleText;
            for (float sample : record.samples)
            {
                std::ostringstream out;
                out << std::fixed << std::setprecision(6) << sample;
                sampleText.push_back(out.str());
            }
            file << "| `" << HexAddress(record.address)
                 << "` | `" << JsonEscape(record.candidateLabel)
                 << "` | `" << JsonEscape(record.classification)
                 << "` | " << std::fixed << std::setprecision(6) << record.staticValue
                 << " | `" << JsonEscape(JoinStrings(sampleText, ", "))
                 << "` | `" << JsonEscape(record.volatility)
                 << "` | `" << JsonEscape(record.runtimeClass)
                 << "` | `" << JsonEscape(record.notes) << "` |\n";
        }

        if (records.empty())
            file << "| `0x0` | `none` | `none` | 0 | `` | `unreadable` | `unknown` | `No candidate float addresses were discovered.` |\n";

        return true;
    }

    bool WriteRuntimeValueWatchersJson(const fs::path& path, const std::vector<WatchValueRecord>& records)
    {
        Progress(L"Writing read-only runtime value watcher JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator read-only watch report\",\n";
        file << "  \"note\": \"Values are sampled only with guarded reads. No memory writes or patching are performed.\",\n";
        file << "  \"watchCount\": " << records.size() << ",\n";
        file << "  \"watches\": [\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const WatchValueRecord& record = records[index];
            file << "    {\n";
            file << "      \"address\": \"" << HexAddress(record.address) << "\",\n";
            file << "      \"candidate\": \"" << JsonEscape(record.candidateLabel) << "\",\n";
            file << "      \"classification\": \"" << JsonEscape(record.classification) << "\",\n";
            file << "      \"staticValue\": " << std::fixed << std::setprecision(6) << record.staticValue << ",\n";
            file << "      \"readable\": " << (record.readable ? "true" : "false") << ",\n";
            file << "      \"volatility\": \"" << JsonEscape(record.volatility) << "\",\n";
            file << "      \"runtimeClass\": \"" << JsonEscape(record.runtimeClass) << "\",\n";
            file << "      \"samples\": [";
            for (std::size_t sampleIndex = 0; sampleIndex < record.samples.size(); ++sampleIndex)
                file << std::fixed << std::setprecision(6) << record.samples[sampleIndex] << (sampleIndex + 1 < record.samples.size() ? ", " : "");
            file << "]\n";
            file << "    }" << (index + 1 < records.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    bool WriteSystemClustersMarkdown(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing system clusters report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# System Clusters\n\n";
        file << "Candidates grouped by inferred engine system. Clusters are derived from string categories and evidence scores.\n\n";
        const char* clusters[] = {
            "Time", "Tick", "Physics Simulation", "Visual Environment", "Sky / Lighting",
            "Shader Pipeline", "Entity / Component", "Audio", "Asset Loading",
            "Environment", "Rendering", "Physics", "Entity"
        };
        for (const char* cluster : clusters)
        {
            file << "## " << cluster << "\n\n";
            std::vector<std::uint64_t> sharedCalls;
            std::vector<std::string> sharedFloats;
            std::uint32_t bestScore = 0;
            std::uint64_t recommended = 0;
            for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
            {
                if (candidate.cluster != cluster && candidate.primaryCategory != cluster)
                    continue;
                if (candidate.score > bestScore)
                {
                    bestScore = candidate.score;
                    recommended = candidate.functionAddress;
                }
                for (std::uint64_t callee : candidate.callees)
                    AddUniqueValue(sharedCalls, callee);
                for (const std::string& value : candidate.nearbyFloatCandidates)
                    AddUniqueValue(sharedFloats, value);
            }

            file << "- Cluster confidence: `" << bestScore << "`\n";
            file << "- Recommended next trace target: `" << HexAddress(recommended) << "`\n";
            file << "- Shared call/data targets: `" << JsonEscape(JoinHexAddresses(sharedCalls)) << "`\n";
            file << "- Shared floats/data refs: `" << JsonEscape(JoinStrings(sharedFloats, "; ")) << "`\n\n";

            file << "| Score | Tier | Phase | Function | Module | Strings | Evidence |\n";
            file << "| ---: | --- | --- | ---: | --- | --- | --- |\n";
            std::size_t written = 0;
            for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
            {
                if (candidate.cluster != cluster && candidate.primaryCategory != cluster)
                    continue;
                file << "| " << candidate.score
                     << " | `" << JsonEscape(candidate.tier)
                     << "` | `" << JsonEscape(candidate.phaseGuess)
                     << "` | `" << HexAddress(candidate.functionAddress)
                     << "` | `" << JsonEscape(candidate.module)
                     << "` | `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; "))
                     << "` | `" << JsonEscape(candidate.reasoning) << "` |\n";
                if (++written >= 40)
                    break;
            }
            if (written == 0)
                file << "| 0 | `Noise` | `0x0` | `none` | `No candidates` | `` |\n";
            file << "\n";
        }

        return true;
    }

    bool WriteCandidateLabelExports(const fs::path& directory, const DiscoveryDump& discovery)
    {
        Progress(L"Writing IDA/Ghidra/Binary Ninja label exports: " + directory.wstring());
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec)
            return false;

        std::ofstream ida(directory / L"CandidateLabels_IDA.py", std::ios::binary | std::ios::trunc);
        std::ofstream ghidra(directory / L"CandidateLabels_Ghidra.py", std::ios::binary | std::ios::trunc);
        std::ofstream bn(directory / L"CandidateLabels_BinaryNinja.py", std::ios::binary | std::ios::trunc);
        std::ofstream csv(directory / L"CandidateLabels.csv", std::ios::binary | std::ios::trunc);
        if (!ida || !ghidra || !bn || !csv)
            return false;

        ida << "import ida_name\n\n";
        ghidra << "# Ghidra script: run from Script Manager after loading the same module base.\n\n";
        bn << "from binaryninja import Symbol, SymbolType\n\n";
        csv << "address,label,module,tier,cluster,score\n";

        std::size_t written = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score < 45 || candidate.functionAddress == 0)
                continue;

            const std::string label = CandidateLabel(candidate);
            ida << "ida_name.set_name(" << HexAddress(candidate.functionAddress) << ", \"" << HeaderEscape(label) << "\", ida_name.SN_CHECK)\n";
            ghidra << "createLabel(toAddr(" << HexAddress(candidate.functionAddress) << "), \"" << HeaderEscape(label) << "\", True)\n";
            bn << "current_view.define_user_symbol(Symbol(SymbolType.FunctionSymbol, " << HexAddress(candidate.functionAddress) << ", \"" << HeaderEscape(label) << "\"))\n";
            csv << HexAddress(candidate.functionAddress) << ","
                << label << ","
                << candidate.module << ","
                << candidate.tier << ","
                << candidate.cluster << ","
                << candidate.score << "\n";

            if (++written >= 300)
                break;
        }

        return true;
    }

    std::set<std::uint64_t> LoadCandidateAddressesFromJson(const fs::path& path)
    {
        std::set<std::uint64_t> addresses;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return addresses;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.find("functionAddressHex") == std::string::npos)
                continue;
            std::uint64_t address = 0;
            if (ParseHexAddressFromText(line, address))
                addresses.insert(address);
        }
        return addresses;
    }

    std::set<std::string> LoadInterestingJsonLines(const fs::path& path, const std::vector<const char*>& tokens)
    {
        std::set<std::string> lines;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return lines;

        std::string line;
        while (std::getline(file, line))
        {
            for (const char* token : tokens)
            {
                if (line.find(token) != std::string::npos)
                {
                    lines.insert(TrimAscii(line));
                    break;
                }
            }
        }
        return lines;
    }

    void WriteSetDifferencePreview(std::ofstream& file, const char* title, const std::set<std::string>& previous, const std::set<std::string>& current, std::size_t limit)
    {
        file << "\n## " << title << "\n\n";
        std::size_t written = 0;
        for (const std::string& value : current)
        {
            if (previous.find(value) != previous.end())
                continue;
            file << "- `" << JsonEscape(value) << "`\n";
            if (++written >= limit)
                break;
        }
        if (written == 0)
            file << "No changed/new rows for this category.\n";
    }

    bool WriteRunDiffMarkdown(const fs::path& path, const fs::path& currentOutputDir, const DiscoveryDump& discovery)
    {
        Progress(L"Writing run diff report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::vector<std::pair<fs::file_time_type, fs::path>> previousRuns;
        const fs::path parent = currentOutputDir.parent_path();
        std::error_code ec;
        if (!parent.empty() && fs::exists(parent, ec))
        {
            for (const fs::directory_entry& entry : fs::directory_iterator(parent, ec))
            {
                if (!entry.is_directory(ec))
                    continue;
                const fs::path candidateDir = entry.path();
                if (ToLower(candidateDir.wstring()) == ToLower(currentOutputDir.wstring()))
                    continue;
                const fs::path jsonPath = candidateDir / L"FrostbiteRuntimeIntrospection.json";
                if (!fs::exists(jsonPath, ec))
                    continue;
                previousRuns.emplace_back(fs::last_write_time(jsonPath, ec), jsonPath);
            }
        }

        std::sort(previousRuns.begin(), previousRuns.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first > rhs.first;
        });

        std::set<std::uint64_t> current;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score >= 40)
                current.insert(candidate.functionAddress);
        }

        file << "# Run Diff\n\n";
        if (previousRuns.empty())
        {
            file << "No previous injected runtime introspection run was found beside this output directory.\n";
            return true;
        }

        const fs::path previousPath = previousRuns.front().second;
        const std::set<std::uint64_t> previous = LoadCandidateAddressesFromJson(previousPath);
        file << "- Previous run: `" << JsonEscape(PathToUtf8(previousPath)) << "`\n";
        file << "- Previous candidate addresses: `" << previous.size() << "`\n";
        file << "- Current candidate addresses: `" << current.size() << "`\n\n";

        file << "## New Candidates\n\n";
        std::size_t written = 0;
        for (std::uint64_t address : current)
        {
            if (previous.find(address) != previous.end())
                continue;
            file << "- `" << HexAddress(address) << "`\n";
            if (++written >= 80)
                break;
        }
        if (written == 0)
            file << "No new candidate function addresses.\n";

        file << "\n## Missing Since Previous Run\n\n";
        written = 0;
        for (std::uint64_t address : previous)
        {
            if (current.find(address) != current.end())
                continue;
            file << "- `" << HexAddress(address) << "`\n";
            if (++written >= 80)
                break;
        }
        if (written == 0)
            file << "No previous candidate addresses disappeared.\n";

        const fs::path currentJsonPath = currentOutputDir / L"FrostbiteRuntimeIntrospection.json";
        const std::set<std::string> previousStrings = LoadInterestingJsonLines(previousPath, { "\"relatedStrings\"", "\"primaryCategory\"", "\"cluster\"" });
        const std::set<std::string> currentStrings = LoadInterestingJsonLines(currentJsonPath, { "\"relatedStrings\"", "\"primaryCategory\"", "\"cluster\"" });
        const std::set<std::string> previousModules = LoadInterestingJsonLines(previousPath, { "\"module\"" });
        const std::set<std::string> currentModules = LoadInterestingJsonLines(currentJsonPath, { "\"module\"" });
        const std::set<std::string> previousScores = LoadInterestingJsonLines(previousPath, { "\"score\"", "\"tier\"", "\"phaseGuess\"" });
        const std::set<std::string> currentScores = LoadInterestingJsonLines(currentJsonPath, { "\"score\"", "\"tier\"", "\"phaseGuess\"" });
        WriteSetDifferencePreview(file, "Changed Or New Strings/Clusters", previousStrings, currentStrings, 80);
        WriteSetDifferencePreview(file, "Changed Or New Modules", previousModules, currentModules, 80);
        WriteSetDifferencePreview(file, "Changed Candidate Scores/Tiers/Phases", previousScores, currentScores, 80);

        const fs::path previousWatchPath = previousPath.parent_path() / L"WatchReport.json";
        const fs::path currentWatchPath = currentOutputDir / L"WatchReport.json";
        const std::set<std::string> previousWatch = LoadInterestingJsonLines(previousWatchPath, { "\"address\"", "\"volatility\"", "\"runtimeClass\"", "\"samples\"" });
        const std::set<std::string> currentWatch = LoadInterestingJsonLines(currentWatchPath, { "\"address\"", "\"volatility\"", "\"runtimeClass\"", "\"samples\"" });
        WriteSetDifferencePreview(file, "Changed Watched Values", previousWatch, currentWatch, 80);

        file << "\n## Likely Runtime Differences\n\n";
        std::string joined;
        for (const std::string& line : currentStrings)
            joined += ToLowerAscii(line) + " ";
        if (ContainsAscii(joined, "timescale") || ContainsAscii(joined, "tick") || ContainsAscii(joined, "delta"))
            file << "- Time/tick candidates changed or became visible between runs.\n";
        if (ContainsAscii(joined, "sky") || ContainsAscii(joined, "fog") || ContainsAscii(joined, "visualenvironment") || ContainsAscii(joined, "exposure"))
            file << "- Environment/rendering candidates changed or became visible between runs.\n";
        if (ContainsAscii(joined, "physics") || ContainsAscii(joined, "collision"))
            file << "- Physics candidates changed or became visible between runs.\n";
        file << "- Use separate captures for menu, in-game, paused, unpaused, cutscene, interior, exterior, and different zones to make this section more meaningful.\n";

        return true;
    }

    bool IsAssetReferenceExtension(const fs::path& path)
    {
        const std::wstring ext = ToLower(path.extension().wstring());
        return ext == L".ebx" ||
               ext == L".toc" ||
               ext == L".cas" ||
               ext == L".sb" ||
               ext == L".chunk" ||
               ext == L".txt" ||
               ext == L".json" ||
               ext == L".xml";
    }

    bool LooksLikeGuidText(const std::string& value)
    {
        if (value.size() != 36)
            return false;
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
            if (dash)
            {
                if (value[index] != '-')
                    return false;
            }
            else if (!std::isxdigit(static_cast<unsigned char>(value[index])))
            {
                return false;
            }
        }
        return true;
    }

    std::string ClassifyAssetReference(const std::string& value)
    {
        const std::string lower = ToLowerAscii(value);
        if (LooksLikeGuidText(value))
            return "GUID";
        if (lower.find("typedescriptor") != std::string::npos || lower.find("typeinfo") != std::string::npos || lower.find("fb::") != std::string::npos)
            return "TypeDescriptor";
        if (lower.find("component") != std::string::npos || lower.find("entity") != std::string::npos || lower.find("blueprint") != std::string::npos)
            return "ComponentName";
        if (lower.find("bundle") != std::string::npos || lower.find(".toc") != std::string::npos || lower.find(".cas") != std::string::npos || lower.find(".sb") != std::string::npos)
            return "BundleName";
        if (lower.find("/") != std::string::npos || lower.find("\\") != std::string::npos || lower.find(".ebx") != std::string::npos || lower.find(".mesh") != std::string::npos || lower.find(".texture") != std::string::npos)
            return "AssetReference";
        return {};
    }

    void AddAssetReference(AssetReferenceDump& dump, std::set<std::string>& seen, const fs::path& filePath, std::uint64_t offset, const std::string& value)
    {
        if (value.size() < 4 || value.size() > 220)
            return;

        const std::string kind = ClassifyAssetReference(value);
        if (kind.empty())
            return;

        std::ostringstream key;
        key << PathToUtf8(filePath) << "|" << offset << "|" << value;
        if (!seen.insert(key.str()).second)
            return;

        AssetReferenceRecord record;
        record.value = value;
        record.kind = kind;
        record.file = PathToUtf8(filePath);
        record.fileOffset = offset;
        record.score = 40;
        if (kind == "TypeDescriptor" || kind == "ComponentName")
            record.score += 25;
        if (kind == "GUID")
            record.score += 15;
        if (value.find("fb::") != std::string::npos)
            record.score += 20;
        dump.references.push_back(std::move(record));
    }

    void ScanAssetBufferStrings(AssetReferenceDump& dump, std::set<std::string>& seen, const fs::path& filePath, const std::vector<std::uint8_t>& bytes)
    {
        for (std::size_t index = 0; index < bytes.size();)
        {
            if (!IsPrintableAscii(bytes[index]))
            {
                ++index;
                continue;
            }
            const std::size_t start = index;
            std::string value;
            while (index < bytes.size() && IsPrintableAscii(bytes[index]) && value.size() <= 220)
                value.push_back(static_cast<char>(bytes[index++]));
            AddAssetReference(dump, seen, filePath, start, value);
        }

        for (std::size_t index = 0; index + 1 < bytes.size();)
        {
            const unsigned char low = bytes[index];
            const unsigned char high = bytes[index + 1];
            if (high != 0 || !IsPrintableAscii(low))
            {
                index += 2;
                continue;
            }
            const std::size_t start = index;
            std::string value;
            while (index + 1 < bytes.size() && bytes[index + 1] == 0 && IsPrintableAscii(bytes[index]) && value.size() <= 220)
            {
                value.push_back(static_cast<char>(bytes[index]));
                index += 2;
            }
            AddAssetReference(dump, seen, filePath, start, value);
        }

        for (std::size_t index = 0; index + 36 <= bytes.size(); ++index)
        {
            std::string value(reinterpret_cast<const char*>(bytes.data() + index), 36);
            if (LooksLikeGuidText(value))
                AddAssetReference(dump, seen, filePath, index, value);
        }
    }

    AssetReferenceDump CaptureAssetReferences(const Options& options, bool verbose)
    {
        AssetReferenceDump dump;
        std::set<std::string> seen;
        const std::uintmax_t maxBytesPerFile = 32ull * 1024ull * 1024ull;

        for (const fs::path& root : options.gameRoots)
        {
            std::error_code ec;
            if (!fs::exists(root, ec))
                continue;

            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file(ec))
                    continue;
                const fs::path filePath = entry.path();
                if (!IsAssetReferenceExtension(filePath))
                    continue;

                const std::uintmax_t fileSize = fs::file_size(filePath, ec);
                if (ec || fileSize == 0)
                    continue;

                const std::size_t readSize = static_cast<std::size_t>(std::min<std::uintmax_t>(fileSize, maxBytesPerFile));
                std::ifstream file(filePath, std::ios::binary);
                if (!file)
                    continue;

                std::vector<std::uint8_t> bytes(readSize);
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<std::size_t>(file.gcount()));
                ScanAssetBufferStrings(dump, seen, filePath, bytes);
            }
        }

        std::sort(dump.references.begin(), dump.references.end(), [](const AssetReferenceRecord& lhs, const AssetReferenceRecord& rhs) {
            if (lhs.score != rhs.score)
                return lhs.score > rhs.score;
            if (lhs.kind != rhs.kind)
                return lhs.kind < rhs.kind;
            return lhs.value < rhs.value;
        });

        if (dump.references.size() > 10000)
            dump.references.resize(10000);

        if (verbose)
            Progress(L"Asset reference scan complete: references=" + FormatCount(dump.references.size()));
        return dump;
    }

    bool WriteAssetReferencesJson(const fs::path& path, const AssetReferenceDump& dump)
    {
        Progress(L"Writing asset references JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator read-only EBX/TOC/CAS reference scan\",\n";
        file << "  \"note\": \"This is string/GUID discovery from game-owned asset files, not a full CAS decompressor.\",\n";
        file << "  \"referenceCount\": " << dump.references.size() << ",\n";
        file << "  \"references\": [\n";
        for (std::size_t index = 0; index < dump.references.size(); ++index)
        {
            const AssetReferenceRecord& record = dump.references[index];
            file << "    { \"kind\": \"" << JsonEscape(record.kind)
                 << "\", \"score\": " << record.score
                 << ", \"file\": \"" << JsonEscape(record.file)
                 << "\", \"fileOffset\": " << record.fileOffset
                 << ", \"value\": \"" << JsonEscape(record.value) << "\" }"
                 << (index + 1 < dump.references.size() ? "," : "") << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return true;
    }

    bool WriteAssetReferencesMarkdown(const fs::path& path, const AssetReferenceDump& dump)
    {
        Progress(L"Writing asset references report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Asset References\n\n";
        file << "Read-only EBX/TOC/CAS-adjacent string and GUID scan. This does not decompress or modify assets.\n\n";
        file << "| Score | Kind | Offset | Value | File |\n";
        file << "| ---: | --- | ---: | --- | --- |\n";
        const std::size_t limit = std::min<std::size_t>(dump.references.size(), 500);
        for (std::size_t index = 0; index < limit; ++index)
        {
            const AssetReferenceRecord& record = dump.references[index];
            file << "| " << record.score
                 << " | `" << JsonEscape(record.kind)
                 << "` | `" << HexAddress(record.fileOffset)
                 << "` | `" << JsonEscape(record.value)
                 << "` | `" << JsonEscape(record.file) << "` |\n";
        }
        if (limit == 0)
            file << "| 0 | `none` | `0x0` | `No EBX/TOC/CAS references found in sampled asset files.` | `` |\n";
        return true;
    }

    bool WriteTypeDescriptorReport(const fs::path& path, const AssetReferenceDump& assets, const DiscoveryDump& discovery)
    {
        Progress(L"Writing type descriptor report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Type Descriptor Report\n\n";
        file << "Read-only string/type descriptor candidates from loaded modules and local EBX/TOC/CAS-adjacent metadata.\n\n";
        file << "## Asset Metadata Type-Like References\n\n";
        file << "| Score | Kind | Value | File | Offset |\n";
        file << "| ---: | --- | --- | --- | ---: |\n";
        std::size_t written = 0;
        for (const AssetReferenceRecord& record : assets.references)
        {
            if (record.kind != "TypeDescriptor" && record.kind != "ComponentName")
                continue;
            file << "| " << record.score
                 << " | `" << JsonEscape(record.kind)
                 << "` | `" << JsonEscape(record.value)
                 << "` | `" << JsonEscape(record.file)
                 << "` | `" << HexAddress(record.fileOffset) << "` |\n";
            if (++written >= 300)
                break;
        }
        if (written == 0)
            file << "| 0 | `none` | `No type descriptor strings found.` | `` | `0x0` |\n";

        file << "\n## Loaded-Module Decomposed Names\n\n";
        file << "| Score | Category | System | Kind | Value | Original | Module | Address |\n";
        file << "| ---: | --- | --- | --- | --- | --- | --- | ---: |\n";
        written = 0;
        for (const DecomposedNameRecord& record : discovery.decomposedNames)
        {
            file << "| " << record.score
                 << " | `" << JsonEscape(record.category)
                 << "` | `" << JsonEscape(record.prefixSystem)
                 << "` | `" << JsonEscape(record.kind)
                 << "` | `" << JsonEscape(record.value)
                 << "` | `" << JsonEscape(record.original)
                 << "` | `" << JsonEscape(record.module)
                 << "` | `" << HexAddress(record.address) << "` |\n";
            if (++written >= 300)
                break;
        }
        if (written == 0)
            file << "| 0 | `none` | `none` | `none` | `none` | `No Frostbite-like names found.` | `` | `0x0` |\n";

        return true;
    }

    bool WriteEnumTablesMarkdown(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing enum/table detection report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Enum And Lookup Table Candidates\n\n";
        file << "Detected from grouped Frostbite-like strings that share a common prefix before the final underscore.\n\n";
        file << "| Confidence | Table | System | Address Range | Values | Function Xrefs |\n";
        file << "| ---: | --- | --- | ---: | --- | --- |\n";
        for (const EnumTableRecord& table : discovery.enumTables)
        {
            file << "| " << table.confidence
                 << " | `" << JsonEscape(table.name)
                 << "` | `" << JsonEscape(table.suspectedSystem)
                 << "` | `" << HexAddress(table.minAddress) << "-" << HexAddress(table.maxAddress)
                 << "` | `" << JsonEscape(JoinStrings(table.values, ", "))
                 << "` | `" << JsonEscape(JoinHexAddresses(table.functionXrefs)) << "` |\n";
        }
        if (discovery.enumTables.empty())
            file << "| 0 | `none` | `none` | `0x0-0x0` | `No grouped enum/table strings found.` | `` |\n";
        return true;
    }

    bool WriteNameDecompositionMarkdown(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing name decomposition report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Frostbite Name Decomposition\n\n";
        file << "Frostbite-style names split into prefix/system, kind, value, and category.\n\n";
        file << "| Score | Category | Prefix/System | Kind | Value | Original | Module | Address |\n";
        file << "| ---: | --- | --- | --- | --- | --- | --- | ---: |\n";
        const std::size_t limit = std::min<std::size_t>(discovery.decomposedNames.size(), 1000);
        for (std::size_t index = 0; index < limit; ++index)
        {
            const DecomposedNameRecord& record = discovery.decomposedNames[index];
            file << "| " << record.score
                 << " | `" << JsonEscape(record.category)
                 << "` | `" << JsonEscape(record.prefixSystem)
                 << "` | `" << JsonEscape(record.kind)
                 << "` | `" << JsonEscape(record.value)
                 << "` | `" << JsonEscape(record.original)
                 << "` | `" << JsonEscape(record.module)
                 << "` | `" << HexAddress(record.address) << "` |\n";
        }
        if (limit == 0)
            file << "| 0 | `none` | `none` | `none` | `none` | `No decomposable names found.` | `` | `0x0` |\n";
        return true;
    }

    std::string GuessShaderPipelineGroup(const std::string& value)
    {
        const std::string lower = ToLowerAscii(value);
        if (ContainsAscii(lower, "surfel") || ContainsAscii(lower, "gi") || ContainsAscii(lower, "globalillum"))
            return "GI / surfel";
        if (ContainsAscii(lower, "reflection") || ContainsAscii(lower, "probe") || ContainsAscii(lower, "cubemap"))
            return "reflections";
        if (ContainsAscii(lower, "irradiance"))
            return "irradiance";
        if (ContainsAscii(lower, "shadow"))
            return "shadows";
        if (ContainsAscii(lower, "fog"))
            return "fog";
        if (ContainsAscii(lower, "atmosphere") || ContainsAscii(lower, "sky"))
            return "atmosphere";
        if (ContainsAscii(lower, "exposure") || ContainsAscii(lower, "hdr"))
            return "exposure";
        if (ContainsAscii(lower, "material"))
            return "material";
        if (ContainsAscii(lower, "transparent") || ContainsAscii(lower, "transparency"))
            return "transparency";
        if (ContainsAscii(lower, "emissive"))
            return "emissive";
        if (ContainsAscii(lower, "postprocess") || ContainsAscii(lower, "post-process"))
            return "postprocess";
        if (ContainsAscii(lower, "shader") || ContainsAscii(lower, "render"))
            return "general render/shader";
        return {};
    }

    bool WriteShaderPipelineMapMarkdown(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing shader pipeline mapper: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        struct ShaderGroup
        {
            std::vector<std::string> strings;
            std::vector<std::uint64_t> functions;
            std::vector<std::string> floats;
        };
        std::map<std::string, ShaderGroup> groups;

        for (const DiscoveredStringRecord& record : discovery.strings)
        {
            const std::string group = GuessShaderPipelineGroup(record.value);
            if (group.empty())
                continue;
            AddUniqueValue(groups[group].strings, record.value);
            for (const StringXrefRecord& xref : discovery.xrefs)
            {
                if (xref.stringAddress != record.address)
                    continue;
                AddUniqueValue(groups[group].functions, xref.functionAddress);
                for (const std::string& value : xref.nearbyFloatCandidates)
                    AddUniqueValue(groups[group].floats, value);
            }
        }

        file << "# Shader Pipeline Map\n\n";
        file << "Render/shader-related strings grouped by likely Frostbite render pass. These are trace targets, not verified engine APIs.\n\n";
        for (const auto& [groupName, group] : groups)
        {
            file << "## " << JsonEscape(groupName) << "\n\n";
            file << "- Likely render pass: `" << JsonEscape(groupName) << "`\n";
            file << "- Candidate functions: `" << JsonEscape(JoinHexAddresses(group.functions)) << "`\n";
            file << "- Related floats: `" << JsonEscape(JoinStrings(group.floats, "; ")) << "`\n\n";
            file << "### Strings\n\n";
            for (std::size_t index = 0; index < std::min<std::size_t>(group.strings.size(), 80); ++index)
                file << "- `" << JsonEscape(group.strings[index]) << "`\n";
            file << "\n";
        }
        if (groups.empty())
            file << "No shader/render pipeline groups survived filtering.\n";
        return true;
    }

    bool WriteAssetReferenceGraphMarkdown(const fs::path& path, const AssetReferenceDump& assets)
    {
        Progress(L"Writing asset reference graph: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::map<std::string, std::vector<const AssetReferenceRecord*>> byFile;
        for (const AssetReferenceRecord& record : assets.references)
            byFile[record.file].push_back(&record);

        file << "# Asset Reference Graph\n\n";
        file << "File-to-reference map from readable local metadata strings. This does not decrypt, unpack, or modify files.\n\n";
        for (const auto& [filePath, records] : byFile)
        {
            file << "## `" << JsonEscape(filePath) << "`\n\n";
            std::size_t count = 0;
            for (const AssetReferenceRecord* record : records)
            {
                file << "- `" << JsonEscape(record->kind) << "` `" << JsonEscape(record->value) << "` at `" << HexAddress(record->fileOffset) << "`\n";
                if (++count >= 80)
                    break;
            }
            file << "\n";
            if (file.tellp() > static_cast<std::streampos>(2 * 1024 * 1024))
            {
                file << "\nReport clipped after 2MB.\n";
                break;
            }
        }
        if (byFile.empty())
            file << "No asset references found.\n";
        return true;
    }

    bool WriteBundleMapMarkdown(const fs::path& path, const AssetReferenceDump& assets)
    {
        Progress(L"Writing bundle map: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Bundle Map\n\n";
        file << "Bundle, level, TOC, CAS, and SB references discovered from readable local metadata strings.\n\n";
        file << "| Kind | Value | File | Offset |\n";
        file << "| --- | --- | --- | ---: |\n";
        std::size_t written = 0;
        for (const AssetReferenceRecord& record : assets.references)
        {
            const std::string lower = ToLowerAscii(record.value);
            if (record.kind != "BundleName" &&
                !ContainsAscii(lower, "level") &&
                !ContainsAscii(lower, "world") &&
                !ContainsAscii(lower, ".toc") &&
                !ContainsAscii(lower, ".cas") &&
                !ContainsAscii(lower, ".sb"))
            {
                continue;
            }
            file << "| `" << JsonEscape(record.kind)
                 << "` | `" << JsonEscape(record.value)
                 << "` | `" << JsonEscape(record.file)
                 << "` | `" << HexAddress(record.fileOffset) << "` |\n";
            if (++written >= 600)
                break;
        }
        if (written == 0)
            file << "| `none` | `No bundle/level/archive references found.` | `` | `0x0` |\n";
        return true;
    }

    bool WriteResearchDashboardMarkdown(const fs::path& path, const DiscoveryDump& discovery, const std::vector<LiveModuleRecord>& modules, const AssetReferenceDump& assets)
    {
        Progress(L"Writing research dashboard: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::map<std::string, std::size_t> tierCounts;
        std::map<std::string, std::size_t> clusterCounts;
        std::map<std::string, std::size_t> moduleCandidateCounts;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            ++tierCounts[candidate.tier.empty() ? "Unclassified" : candidate.tier];
            ++clusterCounts[candidate.cluster.empty() ? "Unknown" : candidate.cluster];
            ++moduleCandidateCounts[candidate.module];
        }

        file << "# Research Dashboard\n\n";
        file << "Read-only candidate validation summary. Nothing in this run patches process memory, bypasses protections, or modifies assets.\n\n";
        file << "## Snapshot\n\n";
        file << "- Candidate functions: `" << discovery.functionCandidates.size() << "`\n";
        file << "- String xrefs: `" << discovery.xrefs.size() << "`\n";
        file << "- Call graph edges: `" << discovery.callEdges.size() << "`\n";
        file << "- Asset references: `" << assets.references.size() << "`\n";
        file << "- Modules captured: `" << modules.size() << "`\n\n";

        file << "## Tier Counts\n\n";
        for (const auto& [tier, count] : tierCounts)
            file << "- `" << JsonEscape(tier) << "`: `" << count << "`\n";
        if (tierCounts.empty())
            file << "- `Noise`: `0`\n";

        file << "\n## Cluster Counts\n\n";
        for (const auto& [cluster, count] : clusterCounts)
            file << "- `" << JsonEscape(cluster) << "`: `" << count << "`\n";
        if (clusterCounts.empty())
            file << "- `Unknown`: `0`\n";

        file << "\n## Best Targets\n\n";
        file << "| Score | Tier | Cluster | Function | Module | Key Strings | Evidence |\n";
        file << "| ---: | --- | --- | ---: | --- | --- | --- |\n";
        std::size_t written = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.score < 50 && written > 0)
                continue;
            file << "| " << candidate.score
                 << " | `" << JsonEscape(candidate.tier)
                 << "` | `" << JsonEscape(candidate.cluster)
                 << "` | `" << HexAddress(candidate.functionAddress)
                 << "` | `" << JsonEscape(candidate.module)
                 << "` | `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; "))
                 << "` | `" << JsonEscape(candidate.reasoning) << "` |\n";
            if (++written >= 20)
                break;
        }
        if (written == 0)
            file << "| 0 | `Noise` | `none` | `0x0` | `none` | `none` | `No candidate evidence produced.` |\n";

        file << "\n## Best Time Leads\n\n";
        std::size_t timeWritten = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.cluster != "Time")
                continue;
            file << "- `" << HexAddress(candidate.functionAddress) << "` score `" << candidate.score << "` tier `" << JsonEscape(candidate.tier) << "` strings `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; ")) << "`\n";
            if (++timeWritten >= 10)
                break;
        }
        if (timeWritten == 0)
            file << "- No time candidate survived filtering.\n";

        file << "\n## Best Environment/Rendering Leads\n\n";
        std::size_t environmentWritten = 0;
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            if (candidate.cluster != "Environment" && candidate.cluster != "Rendering")
                continue;
            file << "- `" << HexAddress(candidate.functionAddress) << "` score `" << candidate.score << "` tier `" << JsonEscape(candidate.tier) << "` strings `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; ")) << "`\n";
            if (++environmentWritten >= 10)
                break;
        }
        if (environmentWritten == 0)
            file << "- No environment/rendering candidate survived filtering.\n";

        auto writeTopCluster = [&](const char* title, const std::vector<const char*>& clusters) {
            file << "\n## Top 20 " << title << "\n\n";
            file << "| Score | Tier | Phase | Function | Module | Strings | Confidence |\n";
            file << "| ---: | --- | --- | ---: | --- | --- | --- |\n";
            std::size_t count = 0;
            for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
            {
                bool matched = false;
                for (const char* cluster : clusters)
                {
                    if (candidate.cluster == cluster || candidate.primaryCategory == cluster)
                        matched = true;
                }
                if (!matched)
                    continue;
                file << "| " << candidate.score
                     << " | `" << JsonEscape(candidate.tier)
                     << "` | `" << JsonEscape(candidate.phaseGuess)
                     << "` | `" << HexAddress(candidate.functionAddress)
                     << "` | `" << JsonEscape(candidate.module)
                     << "` | `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; "))
                     << "` | `" << JsonEscape(candidate.reasoning) << "` |\n";
                if (++count >= 20)
                    break;
            }
            if (count == 0)
                file << "| 0 | `Noise` | `unknown` | `0x0` | `none` | `none` | `No candidates.` |\n";
        };

        writeTopCluster("Time Candidates", { "Time", "Tick" });
        writeTopCluster("Environment Candidates", { "Environment", "Visual Environment", "Sky / Lighting" });
        writeTopCluster("Render Candidates", { "Rendering", "Shader Pipeline" });
        writeTopCluster("Physics Candidates", { "Physics", "Physics Simulation" });
        writeTopCluster("Entity Candidates", { "Entity", "Entity / Component" });

        file << "\n## Best Modules To Analyze\n\n";
        file << "| Score | Bucket | Module | PDATA Functions | RTTI Classes | Path |\n";
        file << "| ---: | --- | --- | ---: | ---: | --- |\n";
        std::vector<const LiveModuleRecord*> rankedModules;
        for (const LiveModuleRecord& module : modules)
            rankedModules.push_back(&module);
        std::sort(rankedModules.begin(), rankedModules.end(), [](const LiveModuleRecord* lhs, const LiveModuleRecord* rhs) {
            if (lhs->relevanceScore != rhs->relevanceScore)
                return lhs->relevanceScore > rhs->relevanceScore;
            return ToLower(lhs->name) < ToLower(rhs->name);
        });
        for (std::size_t index = 0; index < std::min<std::size_t>(rankedModules.size(), 20); ++index)
        {
            const LiveModuleRecord& module = *rankedModules[index];
            file << "| " << module.relevanceScore
                 << " | `" << RelevanceToString(module.relevance)
                 << "` | `" << JsonEscape(WideToUtf8(module.name))
                 << "` | " << module.functionRanges.size()
                 << " | " << module.classes.size()
                 << " | `" << JsonEscape(PathToUtf8(module.path)) << "` |\n";
        }

        file << "\n## Known Noise Modules\n\n";
        for (const LiveModuleRecord& module : modules)
        {
            if (module.relevance == ModuleRelevance::WindowsSystem || module.relevance == ModuleRelevance::ThirdParty)
                file << "- `" << JsonEscape(WideToUtf8(module.name)) << "` (`" << RelevanceToString(module.relevance) << "`)\n";
        }

        file << "\n## Strongest Strings\n\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(discovery.strings.size(), 30); ++index)
        {
            const DiscoveredStringRecord& record = discovery.strings[index];
            file << "- `" << JsonEscape(record.value) << "` score `" << record.score << "` category `" << JsonEscape(record.category) << "` module `" << JsonEscape(record.module) << "`\n";
        }

        file << "\n## Recommended Next Actions\n\n";
        file << "1. Open `HighConfidenceFunctions.md` and inspect `Confirmed` or `Strong Candidate` rows first.\n";
        file << "2. Open matching files under `FunctionTraces` to see string xrefs, caller/callee context, and float evidence.\n";
        file << "3. Import labels from `Labels` into IDA, Ghidra, or Binary Ninja using the script for your tool.\n";
        file << "4. Use `RuntimeValueWatchers.md` to see whether suspected float/global addresses are readable and changing over time.\n";
        file << "5. Use `RunDiff.md` after repeated runs to separate stable targets from one-off noise.\n";

        return true;
    }

    bool WriteResearchDashboardHtml(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing HTML research dashboard: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Frostbite Research Dashboard</title>";
        file << "<style>body{font-family:Segoe UI,Arial,sans-serif;margin:24px;background:#111;color:#eee}table{border-collapse:collapse;width:100%;font-size:13px}td,th{border:1px solid #333;padding:6px;vertical-align:top}th{background:#222}code{color:#9cdcfe}.score{font-weight:700}</style>";
        file << "</head><body><h1>Frostbite Research Dashboard</h1>";
        file << "<p>Read-only candidate validation summary. No process memory writes or patching.</p>";
        file << "<table><tr><th>Score</th><th>Tier</th><th>Cluster</th><th>Phase</th><th>Function</th><th>Module</th><th>Strings</th><th>Reasoning</th></tr>";
        for (std::size_t index = 0; index < std::min<std::size_t>(discovery.functionCandidates.size(), 100); ++index)
        {
            const FunctionCandidateRecord& candidate = discovery.functionCandidates[index];
            file << "<tr><td class=\"score\">" << candidate.score << "</td><td><code>" << JsonEscape(candidate.tier)
                 << "</code></td><td><code>" << JsonEscape(candidate.cluster)
                 << "</code></td><td><code>" << JsonEscape(candidate.phaseGuess)
                 << "</code></td><td><code>" << HexAddress(candidate.functionAddress)
                 << "</code></td><td><code>" << JsonEscape(candidate.module)
                 << "</code></td><td><code>" << JsonEscape(JoinStrings(candidate.relatedStrings, "; "))
                 << "</code></td><td>" << JsonEscape(candidate.reasoning) << "</td></tr>";
        }
        file << "</table></body></html>";
        return true;
    }

    bool WriteSQLiteImportSql(const fs::path& path, const DiscoveryDump& discovery, const AssetReferenceDump& assets)
    {
        Progress(L"Writing SQLite import SQL: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        auto sqlEscape = [](std::string value) {
            std::string out;
            for (char ch : value)
            {
                if (ch == '\'')
                    out += "''";
                else
                    out.push_back(ch);
            }
            return out;
        };

        file << "BEGIN TRANSACTION;\n";
        file << "CREATE TABLE IF NOT EXISTS candidates(address TEXT PRIMARY KEY, address_end TEXT, module TEXT, tier TEXT, cluster TEXT, phase TEXT, score INTEGER, xrefs INTEGER, strings TEXT, reasoning TEXT);\n";
        file << "CREATE TABLE IF NOT EXISTS strings(address TEXT PRIMARY KEY, module TEXT, category TEXT, score INTEGER, xrefs INTEGER, value TEXT);\n";
        file << "CREATE TABLE IF NOT EXISTS assets(kind TEXT, score INTEGER, file TEXT, offset_hex TEXT, value TEXT);\n";
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            file << "INSERT OR REPLACE INTO candidates VALUES('"
                 << HexAddress(candidate.functionAddress) << "','"
                 << HexAddress(candidate.functionEnd) << "','"
                 << sqlEscape(candidate.module) << "','"
                 << sqlEscape(candidate.tier) << "','"
                 << sqlEscape(candidate.cluster) << "','"
                 << sqlEscape(candidate.phaseGuess) << "',"
                 << candidate.score << ","
                 << candidate.xrefCount << ",'"
                 << sqlEscape(JoinStrings(candidate.relatedStrings, "; ")) << "','"
                 << sqlEscape(candidate.reasoning) << "');\n";
        }
        for (const DiscoveredStringRecord& record : discovery.strings)
        {
            file << "INSERT OR REPLACE INTO strings VALUES('"
                 << HexAddress(record.address) << "','"
                 << sqlEscape(record.module) << "','"
                 << sqlEscape(record.category) << "',"
                 << record.score << ","
                 << record.xrefCount << ",'"
                 << sqlEscape(record.value) << "');\n";
        }
        for (const AssetReferenceRecord& record : assets.references)
        {
            file << "INSERT INTO assets VALUES('"
                 << sqlEscape(record.kind) << "',"
                 << record.score << ",'"
                 << sqlEscape(record.file) << "','"
                 << HexAddress(record.fileOffset) << "','"
                 << sqlEscape(record.value) << "');\n";
        }
        file << "COMMIT;\n";
        return true;
    }

    bool WriteReClassNotes(const fs::path& path, const DiscoveryDump& discovery)
    {
        Progress(L"Writing ReClass.NET notes: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# ReClass.NET Notes\n\n";
        file << "Paste these notes beside manually created ReClass nodes. Addresses are read-only candidate leads.\n\n";
        for (std::size_t index = 0; index < std::min<std::size_t>(discovery.functionCandidates.size(), 120); ++index)
        {
            const FunctionCandidateRecord& candidate = discovery.functionCandidates[index];
            file << "## `" << CandidateLabel(candidate) << "`\n\n";
            file << "- Function: `" << HexAddress(candidate.functionAddress) << "-" << HexAddress(candidate.functionEnd) << "`\n";
            file << "- Tier: `" << JsonEscape(candidate.tier) << "`\n";
            file << "- Cluster: `" << JsonEscape(candidate.cluster) << "`\n";
            file << "- Phase: `" << JsonEscape(candidate.phaseGuess) << "`\n";
            file << "- Float/data leads: `" << JsonEscape(JoinStrings(candidate.nearbyFloatCandidates, "; ")) << "`\n";
            file << "- Strings: `" << JsonEscape(JoinStrings(candidate.relatedStrings, "; ")) << "`\n\n";
        }
        return true;
    }

    bool WriteTieredCandidateHeaders(const fs::path& directory, const DiscoveryDump& discovery)
    {
        Progress(L"Writing tiered candidate headers: " + directory.wstring());
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec)
            return false;

        auto writeHeader = [&](const fs::path& path, const char* namespaceName, auto predicate) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file << "#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\n";
            file << "namespace FrostbiteResearchSDK::" << namespaceName << "\n{\n";
            file << "    struct Candidate { const char* module; const char* tier; const char* cluster; const char* phase; const char* strings; std::uint64_t start; std::uint64_t end; std::uint32_t score; };\n";
            file << "    inline constexpr Candidate Items[] =\n    {\n";
            std::size_t count = 0;
            for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
            {
                if (!predicate(candidate))
                    continue;
                ++count;
                file << "        { \"" << HeaderEscape(candidate.module) << "\", \""
                     << HeaderEscape(candidate.tier) << "\", \""
                     << HeaderEscape(candidate.cluster) << "\", \""
                     << HeaderEscape(candidate.phaseGuess) << "\", \""
                     << HeaderEscape(JoinStrings(candidate.relatedStrings, ";")) << "\", "
                     << "0x" << std::hex << candidate.functionAddress << "ull, 0x" << candidate.functionEnd << std::dec << "ull, "
                     << candidate.score << "u },\n";
            }
            if (count == 0)
                file << "        { \"\", \"\", \"\", \"\", \"\", 0ull, 0ull, 0u },\n";
            file << "    };\n    inline constexpr std::size_t Count = " << count << ";\n}\n";
            return true;
        };

        return writeHeader(directory / L"Confirmed.generated.h", "Confirmed", [](const FunctionCandidateRecord& candidate) {
            return candidate.tier == "Confirmed Lead" || candidate.tier == "Strong Candidate";
        }) &&
        writeHeader(directory / L"Candidates.generated.h", "Candidates", [](const FunctionCandidateRecord& candidate) {
            return candidate.tier == "Weak Candidate";
        }) &&
        writeHeader(directory / L"Noise.generated.h", "Noise", [](const FunctionCandidateRecord& candidate) {
            return candidate.tier == "Noise" || candidate.tier == "Self-DLL Noise" || candidate.tier == "Third-Party Noise";
        });
    }


    bool WriteScanSummaryMarkdown(const fs::path& path, const std::vector<LiveModuleRecord>& modules, const DiscoveryDump& discovery)
    {
        Progress(L"Writing scan summary: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Read-Only Scan Summary\n\n";
        file << "- Modules captured: `" << modules.size() << "`\n";
        file << "- Relevant strings: `" << discovery.strings.size() << "`\n";
        file << "- String xrefs: `" << discovery.xrefs.size() << "`\n";
        file << "- Function candidates: `" << discovery.functionCandidates.size() << "`\n";
        file << "- Local call graph edges: `" << discovery.callEdges.size() << "`\n\n";

        file << "**No CVar registry found.** The time report is candidate-only.\n\n";
        file << "**No environment manager found.** The environment report is candidate-only.\n\n";

        file << "## Sections\n\n";
        file << "| Module | Bucket | PDATA Functions | Section | RVA | Size | Perms | Strings | Pointer Density | RTTI Density |\n";
        file << "| --- | --- | ---: | --- | ---: | ---: | --- | ---: | ---: | ---: |\n";
        for (const LiveModuleRecord& module : modules)
        {
            if (!IsPrimaryDiscoveryModule(module))
                continue;
            for (const SectionScanRecord& section : module.sections)
            {
                std::string perms;
                perms += section.readable ? "R" : "-";
                perms += section.writable ? "W" : "-";
                perms += section.executable ? "X" : "-";
                file << "| `" << JsonEscape(WideToUtf8(module.name))
                     << "` | `" << RelevanceToString(module.relevance)
                     << "` | " << module.functionRanges.size()
                     << " | `" << JsonEscape(section.name)
                     << "` | `0x" << std::hex << section.rva << std::dec
                     << "` | " << section.size
                     << " | `" << perms
                     << "` | " << section.stringCount
                     << " | " << std::fixed << std::setprecision(2) << section.pointerDensity
                     << " | " << std::fixed << std::setprecision(2) << section.rttiDensity << " |\n";
            }
        }

        return true;
    }

    bool WriteCleanSdkGeneratedHeader(const fs::path& path, const std::vector<LiveModuleRecord>& modules, const DiscoveryDump& discovery)
    {
        Progress(L"Writing clean SDK generated header: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteResearchSDK\n";
        file << "{\n";
        file << "    struct ModuleInfo { const char* name; const char* bucket; std::uint32_t score; std::uint32_t classCount; std::uint32_t stringCount; };\n";
        file << "    struct StringInfo { const char* module; const char* category; const char* value; std::uint64_t address; std::uint32_t score; std::uint32_t xrefCount; };\n\n";
        file << "    inline constexpr ModuleInfo Modules[] =\n";
        file << "    {\n";
        std::size_t moduleCount = 0;
        for (const LiveModuleRecord& module : modules)
        {
            if (!IsPrimaryDiscoveryModule(module))
                continue;
            std::uint32_t stringCount = 0;
            for (const SectionScanRecord& section : module.sections)
                stringCount += section.stringCount;
            ++moduleCount;
            file << "        { \"" << HeaderEscape(WideToUtf8(module.name)) << "\", \""
                 << RelevanceToString(module.relevance) << "\", "
                 << module.relevanceScore << "u, "
                 << module.classes.size() << "u, "
                 << stringCount << "u },\n";
        }
        if (moduleCount == 0)
            file << "        { \"\", \"\", 0u, 0u, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr StringInfo InterestingStrings[] =\n";
        file << "    {\n";
        const std::size_t stringLimit = std::min<std::size_t>(discovery.strings.size(), 1024);
        for (std::size_t index = 0; index < stringLimit; ++index)
        {
            const DiscoveredStringRecord& record = discovery.strings[index];
            file << "        { \"" << HeaderEscape(record.module) << "\", \""
                 << HeaderEscape(record.category) << "\", \""
                 << HeaderEscape(record.value) << "\", "
                 << "0x" << std::hex << record.address << std::dec << "ull, "
                 << record.score << "u, "
                 << record.xrefCount << "u },\n";
        }
        if (stringLimit == 0)
            file << "        { \"\", \"\", \"\", 0ull, 0u, 0u },\n";
        file << "    };\n\n";
        file << "    inline constexpr std::size_t ModuleCount = " << moduleCount << ";\n";
        file << "    inline constexpr std::size_t InterestingStringCount = " << stringLimit << ";\n";
        file << "}\n";
        return true;
    }

    bool WriteLiveSymbolsMarkdown(const fs::path& path, const std::vector<LiveModuleRecord>& modules, std::size_t previewLimit)
    {
        Progress(L"Writing live symbols report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Frostbite Injected SDK Snapshot\n\n";
        file << "Generated from the process that loaded `FrostbiteSDKGenerator.dll`.\n\n";
        file << "This is a symbol-backed SDK catalog. Parameter and type data only appears when it is available from decorated symbols or PDB/debug data. Stripped native game binaries usually do not contain enough information to recover full prototypes reliably.\n\n";
        file << "## Process\n\n";
        file << "- PID: `" << ::GetCurrentProcessId() << "`\n";
        file << "- Path: `" << JsonEscape(PathToUtf8(GetModulePath(nullptr))) << "`\n";
        file << "- Modules captured: `" << modules.size() << "`\n\n";

        file << "## Module Summary\n\n";
        file << "| Module | Base | Size | Symbols | RTTI Classes | Exports | Imports | Symbol Source | PDB |\n";
        file << "| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |\n";
        for (const LiveModuleRecord& module : modules)
        {
            file << "| `" << JsonEscape(WideToUtf8(module.name)) << "` | `0x"
                 << std::hex << module.baseAddress << std::dec << "` | "
                 << module.imageSize << " | "
                 << module.symbols.size() << " | "
                 << module.classes.size() << " | "
                 << module.exports.size() << " | "
                 << module.imports.size() << " | `"
                 << JsonEscape(module.symbolType) << "` | `"
                 << JsonEscape(module.loadedPdb) << "` |\n";
        }

        for (const LiveModuleRecord& module : modules)
        {
            file << "\n## " << JsonEscape(WideToUtf8(module.name)) << "\n\n";
            file << "- Path: `" << JsonEscape(PathToUtf8(module.path)) << "`\n";
            file << "- Base: `0x" << std::hex << module.baseAddress << std::dec << "`\n";
            file << "- Full symbol/export/import data is in `FrostbiteInjectedProcess.json`.\n\n";

            file << "### RTTI Class Preview\n\n";
            const std::size_t classLimit = std::min(previewLimit, module.classes.size());
            for (std::size_t classIndex = 0; classIndex < classLimit; ++classIndex)
            {
                const RttiClassRecord& record = module.classes[classIndex];
                file << "- `" << JsonEscape(record.name) << "`";
                if (!record.baseClasses.empty())
                {
                    file << " : ";
                    for (std::size_t baseIndex = 0; baseIndex < record.baseClasses.size(); ++baseIndex)
                    {
                        file << "`" << JsonEscape(record.baseClasses[baseIndex]) << "`"
                             << (baseIndex + 1 < record.baseClasses.size() ? ", " : "");
                    }
                }
                file << " - vtable `0x" << std::hex << record.vtable << std::dec
                     << "`, virtual slots `" << record.virtualFunctions.size() << "`\n";
            }
            if (classLimit == 0)
                file << "No MSVC RTTI classes discovered for this module.\n";
            else if (classLimit < module.classes.size())
                file << "- ... preview stopped at `" << classLimit << "` RTTI classes for this module\n";
            file << "\n";

            file << "### Function Signature Preview\n\n";
            std::size_t written = 0;
            for (const SymbolRecord& symbol : module.symbols)
            {
                if (!symbol.isFunction)
                    continue;

                file << "- `0x" << std::hex << symbol.address << std::dec << "` ";
                if (symbol.hasSignature)
                    file << "`" << JsonEscape(symbol.signature) << "`";
                else
                    file << "`" << JsonEscape(symbol.undecoratedName.empty() ? symbol.name : symbol.undecoratedName) << "` `(signature unavailable)`";
                file << "\n";

                if (++written >= previewLimit)
                    break;
            }

            if (written == 0)
                file << "No function symbols were available for this module.\n";
            else if (written < module.symbols.size())
                file << "- ... preview stopped at `" << written << "` entries for this module\n";

            file << "\n### Export Preview\n\n";
            const std::size_t exportLimit = std::min(previewLimit, module.exports.size());
            for (std::size_t i = 0; i < exportLimit; ++i)
            {
                const std::string undecorated = UndecorateSymbol(module.exports[i]);
                file << "- `" << JsonEscape(undecorated) << "`\n";
            }
            if (exportLimit == 0)
                file << "No named exports.\n";
            else if (exportLimit < module.exports.size())
                file << "- ... `" << (module.exports.size() - exportLimit) << "` more exports in JSON\n";
            file << "\n";
        }

        Progress(L"Wrote live symbols report: " + path.wstring());
        return true;
    }

    bool WriteLiveGeneratedHeader(const fs::path& path, const std::vector<LiveModuleRecord>& modules)
    {
        Progress(L"Writing live generated C++ header: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "// Generated by FrostbiteSDKGenerator.dll from a live process snapshot.\n";
        file << "// Full symbol/import/export data is stored in FrostbiteInjectedProcess.json.\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteInjectedSDK\n";
        file << "{\n";
        file << "    struct ModuleRecord\n";
        file << "    {\n";
        file << "        const char* name;\n";
        file << "        const char* path;\n";
        file << "        std::uintptr_t baseAddress;\n";
        file << "        std::uint32_t imageSize;\n";
        file << "        std::uint32_t symbolCount;\n";
        file << "        std::uint32_t classCount;\n";
        file << "        std::uint32_t exportCount;\n";
        file << "        std::uint32_t importCount;\n";
        file << "    };\n\n";
        file << "    inline constexpr ModuleRecord Modules[] =\n";
        file << "    {\n";
        for (const LiveModuleRecord& module : modules)
        {
            file << "        { \""
                 << HeaderEscape(WideToUtf8(module.name)) << "\", \""
                 << HeaderEscape(PathToUtf8(module.path)) << "\", "
                 << "static_cast<std::uintptr_t>(0x" << std::hex << module.baseAddress << std::dec << "), "
                 << module.imageSize << "u, "
                 << module.symbols.size() << "u, "
                 << module.classes.size() << "u, "
                 << module.exports.size() << "u, "
                 << module.imports.size() << "u },\n";
        }
        file << "    };\n\n";
        file << "    inline constexpr std::size_t ModuleCount = sizeof(Modules) / sizeof(Modules[0]);\n";
        file << "}\n";
        Progress(L"Wrote live generated C++ header: " + path.wstring());
        return true;
    }

    bool WriteReusableSdkIndex(const fs::path& sdkDir)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"SDK.hpp";
        Progress(L"Writing reusable SDK index: " + path.wstring());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "// Single-include entry point generated by FrostbiteSDKGenerator.\n";
        file << "// Include this file from another C++17 project to consume the dumped metadata.\n\n";
        file << "#include \"LiveProcess.hpp\"\n";
        file << "#include \"RuntimeIntrospection.hpp\"\n";
        file << "#include \"StaticScan.hpp\"\n";
        return true;
    }

    bool WriteReusableSdkReadme(const fs::path& sdkDir)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"README.md";
        Progress(L"Writing reusable SDK README: " + path.wstring());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Reusable Frostbite SDK Dump\n\n";
        file << "Drop this `SDK` folder into another C++17 project and include:\n\n";
        file << "```cpp\n";
        file << "#include \"SDK/SDK.hpp\"\n";
        file << "```\n\n";
        file << "Files:\n\n";
        file << "- `SDK.hpp`: single include entry point.\n";
        file << "- `LiveProcess.hpp`: modules, imports, exports, and symbols captured from the injected process.\n";
        file << "- `RuntimeIntrospection.hpp`: discovery-based function candidates from strings, xrefs, nearby calls, nearby floats, and math-op proximity.\n";
        file << "- `StaticScan.hpp`: install-folder file/module manifest from the process folder or explicit `--game-root`.\n\n";
        file << "Function signatures are only available when decorated symbols or PDB/debug data are present. Stripped native binaries expose addresses/names/imports/exports, but not reliable parameter types.\n";
        return true;
    }

    bool EnsureEmptyRuntimeIntrospectionHeader(const fs::path& sdkDir)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"RuntimeIntrospection.hpp";
        if (fs::exists(path))
            return true;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::Runtime\n";
        file << "{\n";
        file << "    struct CVarInfo { const char* providerModule; const char* name; const char* category; const char* typeName; const char* currentValue; const char* defaultValue; const char* description; std::uint32_t valueType; std::uint64_t address; std::uint32_t flags; };\n";
        file << "    struct SystemInfo { const char* providerModule; const char* name; const char* kind; const char* module; const char* description; std::uint64_t address; std::uint32_t flags; };\n";
        file << "    struct EnvironmentInfo { const char* providerModule; const char* name; const char* system; const char* typeName; const char* currentValue; const char* description; std::uint32_t valueType; std::uint64_t address; std::uint32_t flags; };\n";
        file << "    struct TypeInfo { const char* providerModule; const char* namespaceName; const char* name; const char* kind; std::uint32_t sizeBytes; std::uint64_t address; std::uint32_t flags; std::uint32_t fieldCount; };\n";
        file << "    struct FieldInfo { const char* ownerType; const char* name; const char* typeName; std::uint32_t offset; std::uint32_t sizeBytes; std::uint32_t flags; };\n";
        file << "    inline constexpr CVarInfo CVars[1] = { { \"\", \"\", \"\", \"\", \"\", \"\", \"\", 0u, 0ull, 0u } };\n";
        file << "    inline constexpr SystemInfo Systems[1] = { { \"\", \"\", \"\", \"\", \"\", 0ull, 0u } };\n";
        file << "    inline constexpr EnvironmentInfo Environment[1] = { { \"\", \"\", \"\", \"\", \"\", \"\", 0u, 0ull, 0u } };\n";
        file << "    inline constexpr TypeInfo Types[1] = { { \"\", \"\", \"\", \"\", 0u, 0ull, 0u, 0u } };\n";
        file << "    inline constexpr FieldInfo Fields[1] = { { \"\", \"\", \"\", 0u, 0u, 0u } };\n";
        file << "    inline constexpr std::size_t CVarCount = 0;\n";
        file << "    inline constexpr std::size_t SystemCount = 0;\n";
        file << "    inline constexpr std::size_t EnvironmentCount = 0;\n";
        file << "    inline constexpr std::size_t TypeCount = 0;\n";
        file << "    inline constexpr std::size_t FieldCount = 0;\n";
        file << "}\n";
        return true;
    }

    bool WriteReusableRuntimeIntrospectionSdk(const fs::path& sdkDir, const RuntimeIntrospectionDump& dump)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"RuntimeIntrospection.hpp";
        Progress(L"Writing reusable runtime introspection header: " + path.wstring());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::size_t fieldCount = 0;
        for (const RuntimeTypeRecord& type : dump.reflectedTypes)
            fieldCount += type.fields.size();

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::Runtime\n";
        file << "{\n";
        file << "    struct CVarInfo { const char* providerModule; const char* name; const char* category; const char* typeName; const char* currentValue; const char* defaultValue; const char* description; std::uint32_t valueType; std::uint64_t address; std::uint32_t flags; };\n";
        file << "    struct SystemInfo { const char* providerModule; const char* name; const char* kind; const char* module; const char* description; std::uint64_t address; std::uint32_t flags; };\n";
        file << "    struct EnvironmentInfo { const char* providerModule; const char* name; const char* system; const char* typeName; const char* currentValue; const char* description; std::uint32_t valueType; std::uint64_t address; std::uint32_t flags; };\n";
        file << "    struct TypeInfo { const char* providerModule; const char* namespaceName; const char* name; const char* kind; std::uint32_t sizeBytes; std::uint64_t address; std::uint32_t flags; std::uint32_t fieldCount; };\n";
        file << "    struct FieldInfo { const char* ownerType; const char* name; const char* typeName; std::uint32_t offset; std::uint32_t sizeBytes; std::uint32_t flags; };\n\n";

        file << "    inline constexpr CVarInfo CVars[] =\n";
        file << "    {\n";
        for (const RuntimeCVarRecord& record : dump.cvars)
        {
            file << "        { \"" << HeaderEscape(record.providerModule) << "\", \""
                 << HeaderEscape(record.name) << "\", \""
                 << HeaderEscape(record.category) << "\", \""
                 << HeaderEscape(record.typeName) << "\", \""
                 << HeaderEscape(record.currentValue) << "\", \""
                 << HeaderEscape(record.defaultValue) << "\", \""
                 << HeaderEscape(record.description) << "\", "
                 << record.valueType << "u, 0x" << std::hex << record.address << std::dec << "ull, "
                 << record.flags << "u },\n";
        }
        if (dump.cvars.empty())
            file << "        { \"\", \"\", \"\", \"\", \"\", \"\", \"\", 0u, 0ull, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr SystemInfo Systems[] =\n";
        file << "    {\n";
        for (const RuntimeSystemRecord& record : dump.systems)
        {
            file << "        { \"" << HeaderEscape(record.providerModule) << "\", \""
                 << HeaderEscape(record.name) << "\", \""
                 << HeaderEscape(record.kind) << "\", \""
                 << HeaderEscape(record.module) << "\", \""
                 << HeaderEscape(record.description) << "\", "
                 << "0x" << std::hex << record.address << std::dec << "ull, "
                 << record.flags << "u },\n";
        }
        if (dump.systems.empty())
            file << "        { \"\", \"\", \"\", \"\", \"\", 0ull, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr EnvironmentInfo Environment[] =\n";
        file << "    {\n";
        for (const RuntimeEnvironmentRecord& record : dump.environment)
        {
            file << "        { \"" << HeaderEscape(record.providerModule) << "\", \""
                 << HeaderEscape(record.name) << "\", \""
                 << HeaderEscape(record.system) << "\", \""
                 << HeaderEscape(record.typeName) << "\", \""
                 << HeaderEscape(record.currentValue) << "\", \""
                 << HeaderEscape(record.description) << "\", "
                 << record.valueType << "u, 0x" << std::hex << record.address << std::dec << "ull, "
                 << record.flags << "u },\n";
        }
        if (dump.environment.empty())
            file << "        { \"\", \"\", \"\", \"\", \"\", \"\", 0u, 0ull, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr TypeInfo Types[] =\n";
        file << "    {\n";
        for (const RuntimeTypeRecord& record : dump.reflectedTypes)
        {
            file << "        { \"" << HeaderEscape(record.providerModule) << "\", \""
                 << HeaderEscape(record.namespaceName) << "\", \""
                 << HeaderEscape(record.name) << "\", \""
                 << HeaderEscape(record.kind) << "\", "
                 << record.sizeBytes << "u, 0x" << std::hex << record.address << std::dec << "ull, "
                 << record.flags << "u, "
                 << record.fields.size() << "u },\n";
        }
        if (dump.reflectedTypes.empty())
            file << "        { \"\", \"\", \"\", \"\", 0u, 0ull, 0u, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr FieldInfo Fields[] =\n";
        file << "    {\n";
        for (const RuntimeTypeRecord& type : dump.reflectedTypes)
        {
            const std::string ownerType = type.namespaceName.empty() ? type.name : type.namespaceName + "::" + type.name;
            for (const RuntimeFieldRecord& field : type.fields)
            {
                file << "        { \"" << HeaderEscape(ownerType) << "\", \""
                     << HeaderEscape(field.name) << "\", \""
                     << HeaderEscape(field.typeName) << "\", "
                     << field.offset << "u, "
                     << field.sizeBytes << "u, "
                     << field.flags << "u },\n";
            }
        }
        if (fieldCount == 0)
            file << "        { \"\", \"\", \"\", 0u, 0u, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr std::size_t CVarCount = " << dump.cvars.size() << ";\n";
        file << "    inline constexpr std::size_t SystemCount = " << dump.systems.size() << ";\n";
        file << "    inline constexpr std::size_t EnvironmentCount = " << dump.environment.size() << ";\n";
        file << "    inline constexpr std::size_t TypeCount = " << dump.reflectedTypes.size() << ";\n";
        file << "    inline constexpr std::size_t FieldCount = " << fieldCount << ";\n";
        file << "}\n";
        return true;
    }

    bool WriteReusableDiscoveryRuntimeSdk(const fs::path& sdkDir, const DiscoveryDump& discovery)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"RuntimeIntrospection.hpp";
        Progress(L"Writing discovery runtime introspection header: " + path.wstring());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::Runtime\n";
        file << "{\n";
        file << "    struct FunctionCandidateInfo\n";
        file << "    {\n";
        file << "        const char* module;\n";
        file << "        const char* primaryCategory;\n";
        file << "        const char* tier;\n";
        file << "        const char* cluster;\n";
        file << "        const char* categories;\n";
        file << "        const char* relatedStrings;\n";
        file << "        const char* nearbyFloatCandidates;\n";
        file << "        const char* floatClassifications;\n";
        file << "        const char* nearbyCallTargets;\n";
        file << "        const char* callers;\n";
        file << "        const char* callees;\n";
        file << "        const char* reasoning;\n";
        file << "        std::uint64_t functionAddress;\n";
        file << "        std::uint64_t functionEnd;\n";
        file << "        std::uint32_t score;\n";
        file << "        std::uint32_t xrefCount;\n";
        file << "        std::uint32_t mathOperationCount;\n";
        file << "        bool fallbackOnly;\n";
        file << "    };\n\n";
        file << "    inline constexpr FunctionCandidateInfo FunctionCandidates[] =\n";
        file << "    {\n";
        for (const FunctionCandidateRecord& candidate : discovery.functionCandidates)
        {
            file << "        { \""
                 << HeaderEscape(candidate.module) << "\", \""
                 << HeaderEscape(candidate.primaryCategory) << "\", \""
                 << HeaderEscape(candidate.tier) << "\", \""
                 << HeaderEscape(candidate.cluster) << "\", \""
                 << HeaderEscape(JoinStrings(candidate.categories, ";")) << "\", \""
                 << HeaderEscape(JoinStrings(candidate.relatedStrings, ";")) << "\", \""
                 << HeaderEscape(JoinStrings(candidate.nearbyFloatCandidates, ";")) << "\", \""
                 << HeaderEscape(JoinStrings(candidate.floatClassifications, ";")) << "\", \""
                 << HeaderEscape(JoinHexAddresses(candidate.nearbyCallTargets)) << "\", \""
                 << HeaderEscape(JoinHexAddresses(candidate.callers)) << "\", \""
                 << HeaderEscape(JoinHexAddresses(candidate.callees)) << "\", \""
                 << HeaderEscape(candidate.reasoning) << "\", "
                 << "0x" << std::hex << candidate.functionAddress << std::dec << "ull, "
                 << "0x" << std::hex << candidate.functionEnd << std::dec << "ull, "
                 << candidate.score << "u, "
                 << candidate.xrefCount << "u, "
                 << candidate.mathOperationCount << "u, "
                 << (candidate.fallbackOnly ? "true" : "false") << " },\n";
        }
        if (discovery.functionCandidates.empty())
            file << "        { \"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\", \"\", 0ull, 0ull, 0u, 0u, 0u, true },\n";
        file << "    };\n\n";
        file << "    inline constexpr std::size_t FunctionCandidateCount = " << discovery.functionCandidates.size() << ";\n";
        file << "    inline constexpr std::size_t CVarCount = 0;\n";
        file << "    inline constexpr std::size_t SystemCount = 0;\n";
        file << "    inline constexpr std::size_t EnvironmentCount = 0;\n";
        file << "    inline constexpr std::size_t TypeCount = 0;\n";
        file << "}\n";
        return true;
    }

    bool EnsureEmptyLiveProcessHeader(const fs::path& sdkDir)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"LiveProcess.hpp";
        if (fs::exists(path))
            return true;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::Live\n";
        file << "{\n";
        file << "    struct ModuleInfo { const char* name; const char* path; std::uint64_t baseAddress; std::uint32_t imageSize; };\n";
        file << "    struct ExportInfo { const char* ownerModule; const char* name; const char* undecorated; const char* signature; };\n";
        file << "    struct ImportInfo { const char* ownerModule; const char* importModule; const char* name; std::uint16_t ordinal; bool byOrdinal; };\n";
        file << "    struct SymbolInfo { const char* ownerModule; const char* name; const char* undecorated; const char* signature; std::uint64_t address; std::uint32_t size; std::uint32_t flags; std::uint32_t tag; bool isFunction; bool hasSignature; };\n";
        file << "    struct ClassInfo { const char* ownerModule; const char* name; const char* decoratedName; const char* bases; std::uint64_t typeDescriptor; std::uint64_t completeObjectLocator; std::uint64_t classHierarchyDescriptor; std::uint64_t vtable; std::uint32_t objectOffset; std::uint32_t hierarchyAttributes; std::uint32_t virtualFunctionCount; };\n";
        file << "    struct VTableFunctionInfo { const char* ownerModule; const char* className; std::uint32_t slot; std::uint64_t address; const char* symbol; const char* signature; };\n";
        file << "    inline constexpr ModuleInfo Modules[1] = { { \"\", \"\", 0ull, 0u } };\n";
        file << "    inline constexpr ExportInfo Exports[1] = { { \"\", \"\", \"\", \"\" } };\n";
        file << "    inline constexpr ImportInfo Imports[1] = { { \"\", \"\", \"\", 0u, false } };\n";
        file << "    inline constexpr SymbolInfo Symbols[1] = { { \"\", \"\", \"\", \"\", 0ull, 0u, 0u, 0u, false, false } };\n";
        file << "    inline constexpr ClassInfo Classes[1] = { { \"\", \"\", \"\", \"\", 0ull, 0ull, 0ull, 0ull, 0u, 0u, 0u } };\n";
        file << "    inline constexpr VTableFunctionInfo VTableFunctions[1] = { { \"\", \"\", 0u, 0ull, \"\", \"\" } };\n";
        file << "    inline constexpr std::size_t ModuleCount = 0;\n";
        file << "    inline constexpr std::size_t ExportCount = 0;\n";
        file << "    inline constexpr std::size_t ImportCount = 0;\n";
        file << "    inline constexpr std::size_t SymbolCount = 0;\n";
        file << "    inline constexpr std::size_t ClassCount = 0;\n";
        file << "    inline constexpr std::size_t VTableFunctionCount = 0;\n";
        file << "}\n";
        return true;
    }

    bool EnsureEmptyStaticScanHeader(const fs::path& sdkDir)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"StaticScan.hpp";
        if (fs::exists(path))
            return true;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::StaticScan\n";
        file << "{\n";
        file << "    struct GameInfo { const char* title; const char* root; std::uint32_t tocCount; std::uint32_t casCount; std::uint32_t moduleCount; std::uint64_t dataBytes; };\n";
        file << "    struct FileInfo { const char* game; const char* path; const char* kind; std::uint64_t size; std::uint32_t exportCount; std::uint32_t importCount; };\n";
        file << "    inline constexpr GameInfo Games[1] = { { \"\", \"\", 0u, 0u, 0u, 0ull } };\n";
        file << "    inline constexpr FileInfo Files[1] = { { \"\", \"\", \"\", 0ull, 0u, 0u } };\n";
        file << "    inline constexpr std::size_t GameCount = 0;\n";
        file << "    inline constexpr std::size_t FileCount = 0;\n";
        file << "}\n";
        return true;
    }

    bool WriteReusableLiveSdk(const fs::path& sdkDir, const std::vector<LiveModuleRecord>& modules)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"LiveProcess.hpp";
        Progress(L"Writing reusable live SDK header: " + path.wstring());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::size_t exportCount = 0;
        std::size_t importCount = 0;
        std::size_t symbolCount = 0;
        std::size_t classCount = 0;
        std::size_t vtableFunctionCount = 0;
        for (const LiveModuleRecord& module : modules)
        {
            exportCount += module.exports.size();
            importCount += module.imports.size();
            symbolCount += module.symbols.size();
            classCount += module.classes.size();
            for (const RttiClassRecord& record : module.classes)
                vtableFunctionCount += record.virtualFunctions.size();
        }

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::Live\n";
        file << "{\n";
        file << "    struct ModuleInfo\n";
        file << "    {\n";
        file << "        const char* name;\n";
        file << "        const char* path;\n";
        file << "        const char* symbolType;\n";
        file << "        const char* loadedPdb;\n";
        file << "        std::uint64_t baseAddress;\n";
        file << "        std::uint32_t imageSize;\n";
        file << "        std::uint32_t exportCount;\n";
        file << "        std::uint32_t importCount;\n";
        file << "        std::uint32_t symbolCount;\n";
        file << "        std::uint32_t classCount;\n";
        file << "    };\n\n";
        file << "    struct ExportInfo\n";
        file << "    {\n";
        file << "        const char* ownerModule;\n";
        file << "        const char* name;\n";
        file << "        const char* undecorated;\n";
        file << "        const char* signature;\n";
        file << "    };\n\n";
        file << "    struct ImportInfo\n";
        file << "    {\n";
        file << "        const char* ownerModule;\n";
        file << "        const char* importModule;\n";
        file << "        const char* name;\n";
        file << "        std::uint16_t ordinal;\n";
        file << "        bool byOrdinal;\n";
        file << "    };\n\n";
        file << "    struct SymbolInfo\n";
        file << "    {\n";
        file << "        const char* ownerModule;\n";
        file << "        const char* name;\n";
        file << "        const char* undecorated;\n";
        file << "        const char* signature;\n";
        file << "        std::uint64_t address;\n";
        file << "        std::uint32_t size;\n";
        file << "        std::uint32_t flags;\n";
        file << "        std::uint32_t tag;\n";
        file << "        bool isFunction;\n";
        file << "        bool hasSignature;\n";
        file << "    };\n\n";

        file << "    struct ClassInfo\n";
        file << "    {\n";
        file << "        const char* ownerModule;\n";
        file << "        const char* name;\n";
        file << "        const char* decoratedName;\n";
        file << "        const char* bases;\n";
        file << "        std::uint64_t typeDescriptor;\n";
        file << "        std::uint64_t completeObjectLocator;\n";
        file << "        std::uint64_t classHierarchyDescriptor;\n";
        file << "        std::uint64_t vtable;\n";
        file << "        std::uint32_t objectOffset;\n";
        file << "        std::uint32_t hierarchyAttributes;\n";
        file << "        std::uint32_t virtualFunctionCount;\n";
        file << "    };\n\n";

        file << "    struct VTableFunctionInfo\n";
        file << "    {\n";
        file << "        const char* ownerModule;\n";
        file << "        const char* className;\n";
        file << "        std::uint32_t slot;\n";
        file << "        std::uint64_t address;\n";
        file << "        const char* symbol;\n";
        file << "        const char* signature;\n";
        file << "    };\n\n";

        file << "    inline constexpr ModuleInfo Modules[] =\n";
        file << "    {\n";
        if (modules.empty())
        {
            file << "        { \"\", \"\", \"\", \"\", 0ull, 0u, 0u, 0u, 0u, 0u },\n";
        }
        else
        {
            for (const LiveModuleRecord& module : modules)
            {
                file << "        { \""
                     << HeaderEscape(WideToUtf8(module.name)) << "\", \""
                     << HeaderEscape(PathToUtf8(module.path)) << "\", \""
                     << HeaderEscape(module.symbolType) << "\", \""
                     << HeaderEscape(module.loadedPdb) << "\", "
                     << "0x" << std::hex << module.baseAddress << std::dec << "ull, "
                     << module.imageSize << "u, "
                     << module.exports.size() << "u, "
                     << module.imports.size() << "u, "
                     << module.symbols.size() << "u, "
                     << module.classes.size() << "u },\n";
            }
        }
        file << "    };\n\n";

        file << "    inline constexpr ExportInfo Exports[] =\n";
        file << "    {\n";
        for (const LiveModuleRecord& module : modules)
        {
            const std::string owner = WideToUtf8(module.name);
            for (const std::string& exportName : module.exports)
            {
                const std::string undecorated = UndecorateSymbol(exportName);
                const std::string signature = LooksLikeFunctionSignature(undecorated) ? undecorated : "";
                file << "        { \""
                     << HeaderEscape(owner) << "\", \""
                     << HeaderEscape(exportName) << "\", \""
                     << HeaderEscape(undecorated) << "\", \""
                     << HeaderEscape(signature) << "\" },\n";
            }
        }
        if (exportCount == 0)
            file << "        { \"\", \"\", \"\", \"\" },\n";
        file << "    };\n\n";

        file << "    inline constexpr ImportInfo Imports[] =\n";
        file << "    {\n";
        for (const LiveModuleRecord& module : modules)
        {
            const std::string owner = WideToUtf8(module.name);
            for (const ImportRecord& importRecord : module.imports)
            {
                file << "        { \""
                     << HeaderEscape(owner) << "\", \""
                     << HeaderEscape(importRecord.module) << "\", \""
                     << HeaderEscape(importRecord.name) << "\", "
                     << importRecord.ordinal << "u, "
                     << (importRecord.byOrdinal ? "true" : "false")
                     << " },\n";
            }
        }
        if (importCount == 0)
            file << "        { \"\", \"\", \"\", 0u, false },\n";
        file << "    };\n\n";

        file << "    inline constexpr SymbolInfo Symbols[] =\n";
        file << "    {\n";
        for (const LiveModuleRecord& module : modules)
        {
            const std::string owner = WideToUtf8(module.name);
            for (const SymbolRecord& symbol : module.symbols)
            {
                file << "        { \""
                     << HeaderEscape(owner) << "\", \""
                     << HeaderEscape(symbol.name) << "\", \""
                     << HeaderEscape(symbol.undecoratedName) << "\", \""
                     << HeaderEscape(symbol.signature) << "\", "
                     << "0x" << std::hex << symbol.address << std::dec << "ull, "
                     << symbol.size << "u, "
                     << symbol.flags << "u, "
                     << symbol.tag << "u, "
                     << (symbol.isFunction ? "true" : "false") << ", "
                     << (symbol.hasSignature ? "true" : "false")
                     << " },\n";
            }
        }
        if (symbolCount == 0)
            file << "        { \"\", \"\", \"\", \"\", 0ull, 0u, 0u, 0u, false, false },\n";
        file << "    };\n\n";

        file << "    inline constexpr ClassInfo Classes[] =\n";
        file << "    {\n";
        for (const LiveModuleRecord& module : modules)
        {
            const std::string owner = WideToUtf8(module.name);
            for (const RttiClassRecord& record : module.classes)
            {
                file << "        { \""
                     << HeaderEscape(owner) << "\", \""
                     << HeaderEscape(record.name) << "\", \""
                     << HeaderEscape(record.decoratedName) << "\", \""
                     << HeaderEscape(JoinStrings(record.baseClasses, ";")) << "\", "
                     << "0x" << std::hex << record.typeDescriptor << "ull, "
                     << "0x" << record.completeObjectLocator << "ull, "
                     << "0x" << record.classHierarchyDescriptor << "ull, "
                     << "0x" << record.vtable << std::dec << "ull, "
                     << record.objectOffset << "u, "
                     << record.hierarchyAttributes << "u, "
                     << record.virtualFunctions.size() << "u },\n";
            }
        }
        if (classCount == 0)
            file << "        { \"\", \"\", \"\", \"\", 0ull, 0ull, 0ull, 0ull, 0u, 0u, 0u },\n";
        file << "    };\n\n";

        file << "    inline constexpr VTableFunctionInfo VTableFunctions[] =\n";
        file << "    {\n";
        for (const LiveModuleRecord& module : modules)
        {
            const std::string owner = WideToUtf8(module.name);
            for (const RttiClassRecord& record : module.classes)
            {
                for (const VTableFunctionRecord& function : record.virtualFunctions)
                {
                    file << "        { \""
                         << HeaderEscape(owner) << "\", \""
                         << HeaderEscape(record.name) << "\", "
                         << function.slot << "u, "
                         << "0x" << std::hex << function.address << std::dec << "ull, \""
                         << HeaderEscape(function.symbolName) << "\", \""
                         << HeaderEscape(function.signature) << "\" },\n";
                }
            }
        }
        if (vtableFunctionCount == 0)
            file << "        { \"\", \"\", 0u, 0ull, \"\", \"\" },\n";
        file << "    };\n\n";

        file << "    inline constexpr std::size_t ModuleCount = " << modules.size() << ";\n";
        file << "    inline constexpr std::size_t ExportCount = " << exportCount << ";\n";
        file << "    inline constexpr std::size_t ImportCount = " << importCount << ";\n";
        file << "    inline constexpr std::size_t SymbolCount = " << symbolCount << ";\n";
        file << "    inline constexpr std::size_t ClassCount = " << classCount << ";\n";
        file << "    inline constexpr std::size_t VTableFunctionCount = " << vtableFunctionCount << ";\n";
        file << "}\n";

        return EnsureEmptyStaticScanHeader(sdkDir) &&
               EnsureEmptyRuntimeIntrospectionHeader(sdkDir) &&
               WriteReusableSdkIndex(sdkDir) &&
               WriteReusableSdkReadme(sdkDir);
    }

    bool WriteReusableStaticSdk(const fs::path& sdkDir, const std::vector<GameRecord>& games)
    {
        fs::create_directories(sdkDir);
        const fs::path path = sdkDir / L"StaticScan.hpp";
        Progress(L"Writing reusable static SDK header: " + path.wstring());

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        std::size_t fileCount = 0;
        for (const GameRecord& game : games)
            fileCount += game.files.size();

        file << "#pragma once\n\n";
        file << "#include <cstddef>\n";
        file << "#include <cstdint>\n\n";
        file << "namespace FrostbiteSDK::StaticScan\n";
        file << "{\n";
        file << "    struct GameInfo\n";
        file << "    {\n";
        file << "        const char* title;\n";
        file << "        const char* root;\n";
        file << "        std::uint32_t tocCount;\n";
        file << "        std::uint32_t casCount;\n";
        file << "        std::uint32_t moduleCount;\n";
        file << "        std::uint64_t dataBytes;\n";
        file << "    };\n\n";
        file << "    struct FileInfo\n";
        file << "    {\n";
        file << "        const char* game;\n";
        file << "        const char* path;\n";
        file << "        const char* kind;\n";
        file << "        std::uint64_t size;\n";
        file << "        std::uint32_t exportCount;\n";
        file << "        std::uint32_t importCount;\n";
        file << "    };\n\n";

        file << "    inline constexpr GameInfo Games[] =\n";
        file << "    {\n";
        if (games.empty())
        {
            file << "        { \"\", \"\", 0u, 0u, 0u, 0ull },\n";
        }
        else
        {
            for (const GameRecord& game : games)
            {
                file << "        { \""
                     << HeaderEscape(WideToUtf8(game.title)) << "\", \""
                     << HeaderEscape(PathToUtf8(game.root)) << "\", "
                     << game.tocCount << "u, "
                     << game.casCount << "u, "
                     << game.moduleCount << "u, "
                     << game.dataBytes << "ull },\n";
            }
        }
        file << "    };\n\n";

        file << "    inline constexpr FileInfo Files[] =\n";
        file << "    {\n";
        if (fileCount == 0)
        {
            file << "        { \"\", \"\", \"\", 0ull, 0u, 0u },\n";
        }
        else
        {
            for (const GameRecord& game : games)
            {
                const std::string title = WideToUtf8(game.title);
                for (const FileRecord& record : game.files)
                {
                    file << "        { \""
                         << HeaderEscape(title) << "\", \""
                         << HeaderEscape(PathToUtf8(record.relativePath)) << "\", \""
                         << HeaderEscape(KindToString(record.kind)) << "\", "
                         << record.size << "ull, "
                         << record.exports.size() << "u, "
                         << record.imports.size() << "u },\n";
                }
            }
        }
        file << "    };\n\n";
        file << "    inline constexpr std::size_t GameCount = " << games.size() << ";\n";
        file << "    inline constexpr std::size_t FileCount = " << fileCount << ";\n";
        file << "}\n";

        return EnsureEmptyLiveProcessHeader(sdkDir) &&
               EnsureEmptyRuntimeIntrospectionHeader(sdkDir) &&
               WriteReusableSdkIndex(sdkDir) &&
               WriteReusableSdkReadme(sdkDir);
    }

    bool WriteManifestJson(const fs::path& path, const std::vector<GameRecord>& games, const Options& options)
    {
        Progress(L"Writing manifest JSON: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "{\n";
        file << "  \"generator\": \"FrostbiteSDKGenerator\",\n";
        file << "  \"includeThirdParty\": " << (options.includeThirdParty ? "true" : "false") << ",\n";
        file << "  \"includeAntiCheat\": " << (options.includeAntiCheat ? "true" : "false") << ",\n";
        file << "  \"games\": [\n";

        for (std::size_t gameIndex = 0; gameIndex < games.size(); ++gameIndex)
        {
            const GameRecord& game = games[gameIndex];
            file << "    {\n";
            file << "      \"title\": \"" << JsonEscape(WideToUtf8(game.title)) << "\",\n";
            file << "      \"root\": \"" << JsonEscape(PathToUtf8(game.root)) << "\",\n";
            file << "      \"tocCount\": " << game.tocCount << ",\n";
            file << "      \"casCount\": " << game.casCount << ",\n";
            file << "      \"moduleCount\": " << game.moduleCount << ",\n";
            file << "      \"antiCheatSkippedCount\": " << game.antiCheatSkippedCount << ",\n";
            file << "      \"dataBytes\": " << game.dataBytes << ",\n";
            file << "      \"files\": [\n";

            for (std::size_t fileIndex = 0; fileIndex < game.files.size(); ++fileIndex)
            {
                const FileRecord& record = game.files[fileIndex];
                file << "        {\n";
                file << "          \"path\": \"" << JsonEscape(PathToUtf8(record.relativePath)) << "\",\n";
                file << "          \"absolutePath\": \"" << JsonEscape(PathToUtf8(record.absolutePath)) << "\",\n";
                file << "          \"kind\": \"" << KindToString(record.kind) << "\",\n";
                file << "          \"size\": " << record.size << ",\n";
                file << "          \"exportScanSkipped\": " << (record.exportScanSkipped ? "true" : "false") << ",\n";
                file << "          \"exportCount\": " << record.exports.size() << ",\n";
                file << "          \"importCount\": " << record.imports.size() << ",\n";
                file << "          \"exports\": [";

                for (std::size_t exportIndex = 0; exportIndex < record.exports.size(); ++exportIndex)
                {
                    file << "\"" << JsonEscape(record.exports[exportIndex]) << "\"";
                    if (exportIndex + 1 < record.exports.size())
                        file << ", ";
                }

                file << "],\n";
                file << "          \"imports\": [\n";
                for (std::size_t importIndex = 0; importIndex < record.imports.size(); ++importIndex)
                {
                    const ImportRecord& importRecord = record.imports[importIndex];
                    file << "            { \"module\": \"" << JsonEscape(importRecord.module) << "\", ";
                    if (importRecord.byOrdinal)
                        file << "\"ordinal\": " << importRecord.ordinal << ", \"byOrdinal\": true";
                    else
                        file << "\"name\": \"" << JsonEscape(importRecord.name) << "\", \"byOrdinal\": false";
                    file << " }" << (importIndex + 1 < record.imports.size() ? "," : "") << "\n";
                }
                file << "          ]\n";
                file << "        }" << (fileIndex + 1 < game.files.size() ? "," : "") << "\n";
            }

            file << "      ]\n";
            file << "    }" << (gameIndex + 1 < games.size() ? "," : "") << "\n";
        }

        file << "  ]\n";
        file << "}\n";
        Progress(L"Wrote manifest JSON: " + path.wstring());
        return true;
    }

    bool WriteReportMarkdown(const fs::path& path, const std::vector<GameRecord>& games, const Options& options)
    {
        Progress(L"Writing Markdown report: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Frostbite SDK Report\n\n";
        file << "Generated from local Frostbite game installs.\n\n";
        file << "This report catalogs local files and PE export tables. It does not decrypt, patch, unpack, or bypass anything.\n\n";
        file << "## Options\n\n";
        file << "- Include third-party dependencies: `" << (options.includeThirdParty ? "true" : "false") << "`\n";
        file << "- Include anti-cheat files: `" << (options.includeAntiCheat ? "true" : "false") << "`\n";
        file << "- Markdown export preview limit: `" << options.maxExportsPerModule << "`\n\n";

        file << "## Games\n\n";
        file << "| Game | TOC | CAS | Modules | Data Bytes | Anti-Cheat Skipped |\n";
        file << "| --- | ---: | ---: | ---: | ---: | ---: |\n";
        for (const GameRecord& game : games)
        {
            file << "| `" << JsonEscape(WideToUtf8(game.title)) << "` | "
                 << game.tocCount << " | "
                 << game.casCount << " | "
                 << game.moduleCount << " | "
                 << game.dataBytes << " | "
                 << game.antiCheatSkippedCount << " |\n";
        }

        for (const GameRecord& game : games)
        {
            file << "\n## " << JsonEscape(WideToUtf8(game.title)) << "\n\n";
            file << "- Root: `" << JsonEscape(PathToUtf8(game.root)) << "`\n";
            file << "- Files included: `" << game.files.size() << "`\n\n";

            file << "### Included Files\n\n";
            file << "| Path | Kind | Size | Exports | Imports |\n";
            file << "| --- | --- | ---: | ---: | ---: |\n";
            for (const FileRecord& record : game.files)
            {
                file << "| `" << JsonEscape(PathToUtf8(record.relativePath)) << "` | `"
                     << KindToString(record.kind) << "` | "
                     << record.size << " | "
                     << record.exports.size();

                if (record.exportScanSkipped)
                    file << " skipped";

                file << " | "
                     << record.imports.size()
                     << " |\n";
            }

            file << "\n### Export Preview\n\n";
            for (const FileRecord& record : game.files)
            {
                if (record.exports.empty() && !record.exportScanSkipped)
                    continue;

                file << "#### " << JsonEscape(PathToUtf8(record.relativePath)) << "\n\n";

                if (record.exportScanSkipped)
                {
                    file << "Export scan skipped because the file is larger than the safe preview limit.\n\n";
                    continue;
                }

                const std::size_t limit = std::min(options.maxExportsPerModule, record.exports.size());
                for (std::size_t i = 0; i < limit; ++i)
                    file << "- `" << JsonEscape(record.exports[i]) << "`\n";

                if (limit < record.exports.size())
                    file << "- ... `" << (record.exports.size() - limit) << "` more exports hidden by the preview limit\n";

                file << "\n";
            }

            file << "\n### Import Preview\n\n";
            for (const FileRecord& record : game.files)
            {
                if (record.imports.empty())
                    continue;

                file << "#### " << JsonEscape(PathToUtf8(record.relativePath)) << "\n\n";
                const std::size_t limit = std::min(options.maxExportsPerModule, record.imports.size());
                for (std::size_t i = 0; i < limit; ++i)
                {
                    const ImportRecord& importRecord = record.imports[i];
                    file << "- `" << JsonEscape(importRecord.module) << "!";
                    if (importRecord.byOrdinal)
                        file << "#" << importRecord.ordinal;
                    else
                        file << JsonEscape(importRecord.name);
                    file << "`\n";
                }

                if (limit < record.imports.size())
                    file << "- ... `" << (record.imports.size() - limit) << "` more imports hidden by the preview limit\n";

                file << "\n";
            }
        }

        Progress(L"Wrote Markdown report: " + path.wstring());
        return true;
    }

    bool WriteGeneratedHeader(const fs::path& path, const std::vector<GameRecord>& games)
    {
        Progress(L"Writing generated C++ header: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "#pragma once\n\n";
        file << "// This file was generated by FrostbiteSDKGenerator.\n";
        file << "// It is a lightweight manifest, not an official Frostbite SDK.\n\n";
        file << "#include <cstddef>\n\n";
        file << "namespace FrostbiteGeneratedSDK\n";
        file << "{\n";
        file << "    struct GameRecord\n";
        file << "    {\n";
        file << "        const char* title;\n";
        file << "        const char* root;\n";
        file << "        unsigned int tocCount;\n";
        file << "        unsigned int casCount;\n";
        file << "        unsigned int moduleCount;\n";
        file << "        unsigned long long dataBytes;\n";
        file << "    };\n\n";
        file << "    inline constexpr GameRecord Games[] =\n";
        file << "    {\n";
        for (const GameRecord& game : games)
        {
            file << "        { \""
                 << HeaderEscape(WideToUtf8(game.title)) << "\", \""
                 << HeaderEscape(PathToUtf8(game.root)) << "\", "
                 << game.tocCount << "u, "
                 << game.casCount << "u, "
                 << game.moduleCount << "u, "
                 << game.dataBytes << "ull },\n";
        }
        file << "    };\n\n";
        file << "    inline constexpr std::size_t GameCount = sizeof(Games) / sizeof(Games[0]);\n";
        file << "}\n";
        Progress(L"Wrote generated C++ header: " + path.wstring());
        return true;
    }

    bool WriteGeneratedReadme(const fs::path& path)
    {
        Progress(L"Writing generated README: " + path.wstring());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;

        file << "# Generated Frostbite SDK Folder\n\n";
        file << "This folder was produced by `FrostbiteSDKGenerator.exe`.\n\n";
        file << "Files:\n\n";
        file << "- `FrostbiteSDKManifest.json`: machine-readable manifest.\n";
        file << "- `FrostbiteModules.md`: human-readable report.\n";
        file << "- `FrostbiteSDK.generated.h`: tiny C++ manifest header.\n\n";
        file << "This is not an official Frostbite SDK. It is a local catalog of files, archive counts, modules, and named PE exports.\n";
        Progress(L"Wrote generated README: " + path.wstring());
        return true;
    }

    std::vector<std::uint64_t> ExtractHexAddressesFromFile(const fs::path& path)
    {
        std::vector<std::uint64_t> addresses;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return addresses;

        std::string line;
        while (std::getline(file, line))
        {
            std::size_t search = 0;
            while (true)
            {
                const std::size_t found = line.find("0x", search);
                if (found == std::string::npos)
                    break;
                std::size_t end = found + 2;
                while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end])))
                    ++end;
                if (end > found + 2)
                {
                    std::uint64_t address = 0;
                    std::istringstream in(line.substr(found + 2, end - found - 2));
                    in >> std::hex >> address;
                    if (!in.fail())
                        AddUniqueValue(addresses, address);
                }
                search = end;
            }
        }

        return addresses;
    }

    int RunQueryMode(const Options& options)
    {
        if (!options.queryText.empty())
        {
            const std::string query = ToLowerAscii(options.queryText);
            std::wcout << L"Query: " << AnsiToWide(options.queryText) << L"\n";
            std::size_t hits = 0;
            std::error_code ec;
            if (fs::exists(options.outputDir, ec))
            {
                for (const fs::directory_entry& entry : fs::recursive_directory_iterator(options.outputDir, fs::directory_options::skip_permission_denied, ec))
                {
                    if (ec)
                        break;
                    if (!entry.is_regular_file(ec))
                        continue;
                    const fs::path ext = entry.path().extension();
                    const std::wstring lowerExt = ToLower(ext.wstring());
                    if (lowerExt != L".md" && lowerExt != L".json" && lowerExt != L".csv" && lowerExt != L".sql")
                        continue;
                    std::ifstream file(entry.path(), std::ios::binary);
                    if (!file)
                        continue;
                    std::string line;
                    std::size_t lineNumber = 0;
                    while (std::getline(file, line))
                    {
                        ++lineNumber;
                        if (ToLowerAscii(line).find(query) == std::string::npos)
                            continue;
                        std::wcout << entry.path().wstring() << L":" << lineNumber << L": " << AnsiToWide(line.substr(0, 220)) << L"\n";
                        if (++hits >= 200)
                            break;
                    }
                    if (hits >= 200)
                        break;
                }
            }
            std::wcout << L"Hits: " << hits << L"\n";
        }

        for (std::uint64_t traceAddress : options.traceAddresses)
        {
            std::wcout << L"Trace lookup: " << AnsiToWide(HexAddress(traceAddress)) << L"\n";
            const fs::path traceDir = options.outputDir / L"FunctionTraces";
            bool foundTrace = false;
            std::error_code ec;
            if (fs::exists(traceDir, ec))
            {
                const std::string needle = ToLowerAscii(HexAddress(traceAddress));
                for (const fs::directory_entry& entry : fs::directory_iterator(traceDir, ec))
                {
                    if (!entry.is_regular_file(ec))
                        continue;
                    const std::string name = ToLowerAscii(PathToUtf8(entry.path().filename()));
                    if (name.find(needle.substr(2)) != std::string::npos)
                    {
                        std::wcout << L"Trace file: " << entry.path().wstring() << L"\n";
                        foundTrace = true;
                    }
                }
            }
            if (!foundTrace)
                std::wcout << L"No existing trace file found for that address. Run the injected dumper to generate fresh traces.\n";
        }

        if (options.diffInputs.size() >= 2)
        {
            const std::vector<std::uint64_t> left = ExtractHexAddressesFromFile(options.diffInputs[0]);
            const std::vector<std::uint64_t> right = ExtractHexAddressesFromFile(options.diffInputs[1]);
            std::set<std::uint64_t> leftSet(left.begin(), left.end());
            std::set<std::uint64_t> rightSet(right.begin(), right.end());
            std::wcout << L"Diff left addresses: " << leftSet.size() << L"\n";
            std::wcout << L"Diff right addresses: " << rightSet.size() << L"\n";
            std::wcout << L"New in right:\n";
            std::size_t printed = 0;
            for (std::uint64_t address : rightSet)
            {
                if (leftSet.find(address) != leftSet.end())
                    continue;
                std::wcout << L"  " << AnsiToWide(HexAddress(address)) << L"\n";
                if (++printed >= 80)
                    break;
            }
        }

        if (!options.watchListPath.empty())
        {
            std::wcout << L"Watchlist read-only sample from current process: " << options.watchListPath.wstring() << L"\n";
            const std::vector<std::uint64_t> addresses = ExtractHexAddressesFromFile(options.watchListPath);
            for (std::size_t index = 0; index < std::min<std::size_t>(addresses.size(), 80); ++index)
            {
                float value = 0.0f;
                const bool ok = SafeReadValue(addresses[index], value);
                std::wcout << L"  " << AnsiToWide(HexAddress(addresses[index])) << L" -> "
                           << (ok ? AnsiToWide(std::to_string(value)) : L"unreadable") << L"\n";
            }
        }

        return 0;
    }

    void PrintUsage()
    {
        std::wcout << L"FrostbiteSDKGenerator\n\n";
        std::wcout << L"Usage:\n";
        std::wcout << L"  FrostbiteSDKGenerator.exe [options]\n\n";
        std::wcout << L"Options:\n";
        std::wcout << L"  --game-root <path>       Add a Frostbite game root. Can be repeated.\n";
        std::wcout << L"  --out <path>             Output folder. Defaults to .\\GeneratedSDK.\n";
        std::wcout << L"  --include-third-party    Include third-party DLLs like DLSS, Streamline, Steam, etc.\n";
        std::wcout << L"  --include-anticheat      Include anti-cheat files in reports. Default is skip.\n";
        std::wcout << L"  --max-exports <n>        Export preview limit in Markdown. Default: 160.\n";
        std::wcout << L"  --query <text>           Search generated reports under --out for a term, e.g. TimeScale.\n";
        std::wcout << L"  --trace <address>        Find an existing trace report for an address.\n";
        std::wcout << L"  --watch <path>           Read-only sample hex addresses listed in a watchlist file from this process.\n";
        std::wcout << L"  --diff <a> <b>           Compare hex addresses in two generated JSON/Markdown reports.\n";
        std::wcout << L"  --help                   Show this help text.\n";
        std::wcout << L"\nIf no --game-root is supplied, the generator scans the current process folder only.\n";
    }

    bool IsHelpRequested(int argc, wchar_t* argv[])
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i];
            if (arg == L"--help" || arg == L"-h" || arg == L"/?")
                return true;
        }
        return false;
    }

    std::optional<Options> ParseOptions(int argc, wchar_t* argv[])
    {
        Options options;
        options.outputDir = fs::current_path() / L"GeneratedSDK";

        for (int i = 1; i < argc; ++i)
        {
            const std::wstring arg = argv[i];

            if (arg == L"--game-root" || arg == L"-g")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"Missing value after " << arg << L"\n";
                    return std::nullopt;
                }
                options.gameRoots.emplace_back(argv[++i]);
                continue;
            }

            if (arg == L"--out" || arg == L"-o")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"Missing value after " << arg << L"\n";
                    return std::nullopt;
                }
                options.outputDir = argv[++i];
                continue;
            }

            if (arg == L"--include-third-party")
            {
                options.includeThirdParty = true;
                continue;
            }

            if (arg == L"--include-anticheat")
            {
                options.includeAntiCheat = true;
                continue;
            }

            if (arg == L"--max-exports")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"Missing value after " << arg << L"\n";
                    return std::nullopt;
                }
                options.maxExportsPerModule = std::max<std::size_t>(1, std::wcstoull(argv[++i], nullptr, 10));
                continue;
            }

            if (arg == L"--query")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"Missing value after " << arg << L"\n";
                    return std::nullopt;
                }
                options.queryText = WideToUtf8(argv[++i]);
                continue;
            }

            if (arg == L"--trace")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"Missing value after " << arg << L"\n";
                    return std::nullopt;
                }
                std::wstring value = argv[++i];
                if (value.rfind(L"0x", 0) == 0 || value.rfind(L"0X", 0) == 0)
                    value = value.substr(2);
                options.traceAddresses.push_back(std::wcstoull(value.c_str(), nullptr, 16));
                continue;
            }

            if (arg == L"--watch")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"Missing value after " << arg << L"\n";
                    return std::nullopt;
                }
                options.watchListPath = argv[++i];
                continue;
            }

            if (arg == L"--diff")
            {
                if (i + 2 >= argc)
                {
                    std::wcerr << L"Missing two values after " << arg << L"\n";
                    return std::nullopt;
                }
                options.diffInputs.emplace_back(argv[++i]);
                options.diffInputs.emplace_back(argv[++i]);
                continue;
            }

            std::wcerr << L"Unknown option: " << arg << L"\n";
            return std::nullopt;
        }

        if (options.gameRoots.empty())
        {
            const fs::path processPath = GetModulePath(nullptr);
            if (!processPath.empty() && processPath.has_parent_path())
                options.gameRoots.emplace_back(processPath.parent_path());
        }

        return options;
    }

    int GenerateFromOptions(const Options& options, bool verbose)
    {
        const bool previousVerbose = g_verboseProgress;
        g_verboseProgress = verbose;

        if (verbose)
        {
            Progress(L"============================================================");
            Progress(L"Frostbite SDK generation started.");
            Progress(L"Output folder: " + options.outputDir.wstring());
            Progress(L"Include third-party dependencies: " + std::wstring(options.includeThirdParty ? L"true" : L"false"));
            Progress(L"Include anti-cheat files: " + std::wstring(options.includeAntiCheat ? L"true" : L"false"));
            Progress(L"Markdown export preview limit: " + FormatCount(options.maxExportsPerModule));
        }

        std::vector<GameRecord> games;
        for (const fs::path& root : options.gameRoots)
        {
            if (!fs::exists(root))
            {
                if (verbose)
                    ProgressError(L"Game root does not exist, skipping: " + root.wstring());
                continue;
            }

            if (verbose)
                Progress(L"Scanning: " + root.wstring());

            games.push_back(ScanGameRoot(root, options, verbose));
        }

        if (games.empty())
        {
            if (verbose)
                ProgressError(L"No valid game roots were scanned.");
            g_verboseProgress = previousVerbose;
            return 2;
        }

        std::error_code ec;
        if (verbose)
            Progress(L"Creating output folder: " + options.outputDir.wstring());
        fs::create_directories(options.outputDir, ec);
        if (ec)
        {
            if (verbose)
                ProgressError(L"Could not create output folder: " + options.outputDir.wstring());
            g_verboseProgress = previousVerbose;
            return 3;
        }

        const fs::path manifestPath = options.outputDir / L"FrostbiteSDKManifest.json";
        const fs::path reportPath = options.outputDir / L"FrostbiteModules.md";
        const fs::path headerPath = options.outputDir / L"FrostbiteSDK.generated.h";
        const fs::path readmePath = options.outputDir / L"README.generated.md";
        const fs::path sdkDir = options.outputDir / L"SDK";

        if (!WriteManifestJson(manifestPath, games, options) ||
            !WriteReportMarkdown(reportPath, games, options) ||
            !WriteGeneratedHeader(headerPath, games) ||
            !WriteGeneratedReadme(readmePath) ||
            !WriteReusableStaticSdk(sdkDir, games))
        {
            if (verbose)
                ProgressError(L"Failed to write one or more output files.");
            g_verboseProgress = previousVerbose;
            return 4;
        }

        if (verbose)
        {
            std::uint32_t totalToc = 0;
            std::uint32_t totalCas = 0;
            std::uint32_t totalModules = 0;
            for (const GameRecord& game : games)
            {
                totalToc += game.tocCount;
                totalCas += game.casCount;
                totalModules += game.moduleCount;
            }

            Progress(L"FrostbiteSDKGenerator completed.");
            Progress(L"Games scanned: " + FormatCount(games.size()));
            Progress(L"TOC files: " + FormatCount(totalToc));
            Progress(L"CAS files: " + FormatCount(totalCas));
            Progress(L"Modules: " + FormatCount(totalModules));
            Progress(L"Output folder: " + options.outputDir.wstring());
            Progress(L"============================================================");
        }

        g_verboseProgress = previousVerbose;
        return 0;
    }

    void AddUniqueGameRoot(Options& options, const fs::path& root)
    {
        if (root.empty())
            return;

        std::error_code ec;
        const fs::path normalized = fs::weakly_canonical(root, ec);
        const fs::path candidate = ec ? root : normalized;
        for (const fs::path& existing : options.gameRoots)
        {
            const fs::path existingNormalized = fs::weakly_canonical(existing, ec);
            if (ToLower((ec ? existing : existingNormalized).wstring()) == ToLower(candidate.wstring()))
                return;
        }

        if (fs::exists(candidate))
            options.gameRoots.push_back(candidate);
    }

    int GenerateInjectedSnapshotInternal(const wchar_t* outputDir, bool verbose)
    {
        const bool previousVerbose = g_verboseProgress;
        g_verboseProgress = verbose;

        if (verbose)
            EnsureConsole();

        Options options;
        options.outputDir = (outputDir && outputDir[0] != L'\0')
            ? fs::path(outputDir)
            : GetDefaultInjectedOutputDir();
        options.includeThirdParty = true;
        options.includeAntiCheat = false;
        options.maxExportsPerModule = 1000;

        const fs::path processPath = GetModulePath(nullptr);
        if (!processPath.empty())
            AddUniqueGameRoot(options, processPath.parent_path());

        try
        {
            Progress(L"============================================================");
            Progress(L"Injected SDK snapshot started.");
            Progress(L"Output folder: " + options.outputDir.wstring());
            Progress(L"Target process: " + processPath.wstring());
            Progress(L"This snapshot is scoped to the process that loaded the DLL and its install folder.");
            Progress(L"It uses loaded modules, PE metadata, and DbgHelp/PDB symbols when available.");

            std::error_code ec;
            fs::create_directories(options.outputDir, ec);
            if (ec)
            {
                ProgressError(L"Could not create injected snapshot output folder: " + options.outputDir.wstring());
                g_verboseProgress = previousVerbose;
                return 3;
            }

            std::vector<LiveModuleRecord> modules = CaptureLiveProcessModules(verbose);
            const DiscoveryDump discovery = CaptureReadOnlyDiscovery(modules, verbose);
            const AssetReferenceDump assetReferences = CaptureAssetReferences(options, verbose);
            const std::vector<WatchValueRecord> watchValues = CaptureRuntimeWatchValues(discovery, 80);
            const fs::path liveJsonPath = options.outputDir / L"FrostbiteInjectedProcess.json";
            const fs::path liveReportPath = options.outputDir / L"FrostbiteInjectedSymbols.md";
            const fs::path liveHeaderPath = options.outputDir / L"FrostbiteInjectedSDK.generated.h";
            const fs::path introspectionJsonPath = options.outputDir / L"FrostbiteRuntimeIntrospection.json";
            const fs::path introspectionReportPath = options.outputDir / L"FrostbiteRuntimeIntrospection.md";
            const fs::path stringsJsonPath = options.outputDir / L"Strings.json";
            const fs::path stringXrefsJsonPath = options.outputDir / L"StringXrefs.json";
            const fs::path timeCandidatesPath = options.outputDir / L"TimeCandidates.md";
            const fs::path environmentCandidatesPath = options.outputDir / L"EnvironmentCandidates.md";
            const fs::path highConfidenceFunctionsPath = options.outputDir / L"HighConfidenceFunctions.md";
            const fs::path frostbiteTypesPath = options.outputDir / L"FrostbiteTypes.md";
            const fs::path interestingModulesPath = options.outputDir / L"InterestingModules.md";
            const fs::path scanSummaryPath = options.outputDir / L"ScanSummary.md";
            const fs::path callGraphPath = options.outputDir / L"CandidateCallGraph.json";
            const fs::path traceDir = options.outputDir / L"FunctionTraces";
            const fs::path valueWatchersPath = options.outputDir / L"RuntimeValueWatchers.md";
            const fs::path watchReportPath = options.outputDir / L"WatchReport.md";
            const fs::path watchReportJsonPath = options.outputDir / L"WatchReport.json";
            const fs::path systemClustersPath = options.outputDir / L"SystemClusters.md";
            const fs::path labelsDir = options.outputDir / L"Labels";
            const fs::path runDiffPath = options.outputDir / L"RunDiff.md";
            const fs::path assetReferencesJsonPath = options.outputDir / L"AssetReferences.json";
            const fs::path assetReferencesPath = options.outputDir / L"AssetReferences.md";
            const fs::path typeDescriptorPath = options.outputDir / L"TypeDescriptorReport.md";
            const fs::path assetReferenceGraphPath = options.outputDir / L"AssetReferenceGraph.md";
            const fs::path bundleMapPath = options.outputDir / L"BundleMap.md";
            const fs::path enumTablesPath = options.outputDir / L"EnumTables.md";
            const fs::path nameDecompositionPath = options.outputDir / L"NameDecomposition.md";
            const fs::path shaderPipelinePath = options.outputDir / L"ShaderPipelineMap.md";
            const fs::path researchDashboardPath = options.outputDir / L"ResearchDashboard.md";
            const fs::path researchDashboardHtmlPath = options.outputDir / L"ResearchDashboard.html";
            const fs::path sqliteImportPath = options.outputDir / L"Research.sqlite.sql";
            const fs::path reclassNotesPath = options.outputDir / L"ReClassNotes.md";
            const fs::path frostbiteTypesHeaderPath = options.outputDir / L"FrostbiteTypes.generated.h";
            const fs::path cleanSdkHeaderPath = options.outputDir / L"SDK.generated.h";
            const fs::path tierHeadersDir = options.outputDir / L"SDK";
            const fs::path sdkDir = options.outputDir / L"SDK";

            if (!WriteLiveProcessJson(liveJsonPath, modules) ||
                !WriteLiveSymbolsMarkdown(liveReportPath, modules, options.maxExportsPerModule) ||
                !WriteLiveGeneratedHeader(liveHeaderPath, modules) ||
                !WriteRuntimeDiscoveryJson(introspectionJsonPath, discovery) ||
                !WriteRuntimeDiscoveryMarkdown(introspectionReportPath, discovery) ||
                !WriteStringsJson(stringsJsonPath, discovery) ||
                !WriteStringXrefsJson(stringXrefsJsonPath, discovery) ||
                !WriteCandidateReport(timeCandidatesPath, "Time Candidates", false, discovery) ||
                !WriteCandidateReport(environmentCandidatesPath, "Environment Candidates", true, discovery) ||
                !WriteHighConfidenceFunctionsMarkdown(highConfidenceFunctionsPath, discovery) ||
                !WriteFrostbiteTypesMarkdown(frostbiteTypesPath, modules) ||
                !WriteInterestingModulesMarkdown(interestingModulesPath, modules) ||
                !WriteScanSummaryMarkdown(scanSummaryPath, modules, discovery) ||
                !WriteCandidateCallGraphJson(callGraphPath, discovery) ||
                !WriteFunctionTraceReports(traceDir, discovery, modules) ||
                !WriteRuntimeValueWatchersMarkdown(valueWatchersPath, discovery, watchValues) ||
                !WriteRuntimeValueWatchersMarkdown(watchReportPath, discovery, watchValues) ||
                !WriteRuntimeValueWatchersJson(watchReportJsonPath, watchValues) ||
                !WriteSystemClustersMarkdown(systemClustersPath, discovery) ||
                !WriteCandidateLabelExports(labelsDir, discovery) ||
                !WriteRunDiffMarkdown(runDiffPath, options.outputDir, discovery) ||
                !WriteAssetReferencesJson(assetReferencesJsonPath, assetReferences) ||
                !WriteAssetReferencesMarkdown(assetReferencesPath, assetReferences) ||
                !WriteTypeDescriptorReport(typeDescriptorPath, assetReferences, discovery) ||
                !WriteAssetReferenceGraphMarkdown(assetReferenceGraphPath, assetReferences) ||
                !WriteBundleMapMarkdown(bundleMapPath, assetReferences) ||
                !WriteEnumTablesMarkdown(enumTablesPath, discovery) ||
                !WriteNameDecompositionMarkdown(nameDecompositionPath, discovery) ||
                !WriteShaderPipelineMapMarkdown(shaderPipelinePath, discovery) ||
                !WriteResearchDashboardMarkdown(researchDashboardPath, discovery, modules, assetReferences) ||
                !WriteResearchDashboardHtml(researchDashboardHtmlPath, discovery) ||
                !WriteSQLiteImportSql(sqliteImportPath, discovery, assetReferences) ||
                !WriteReClassNotes(reclassNotesPath, discovery) ||
                !WriteFrostbiteTypesHeader(frostbiteTypesHeaderPath, modules) ||
                !WriteCleanSdkGeneratedHeader(cleanSdkHeaderPath, modules, discovery) ||
                !WriteTieredCandidateHeaders(tierHeadersDir, discovery) ||
                !WriteReusableLiveSdk(sdkDir, modules) ||
                !WriteReusableDiscoveryRuntimeSdk(sdkDir, discovery))
            {
                ProgressError(L"Failed to write injected live-process SDK files.");
                g_verboseProgress = previousVerbose;
                return 4;
            }

            const int staticResult = GenerateFromOptions(options, verbose);
            if (staticResult != 0)
                ProgressError(L"Static game-root scan returned code: " + FormatCount(staticResult));

            Progress(L"Injected SDK snapshot complete.");
            Progress(L"Live JSON: " + liveJsonPath.wstring());
            Progress(L"Live report: " + liveReportPath.wstring());
            Progress(L"Runtime introspection JSON: " + introspectionJsonPath.wstring());
            Progress(L"Runtime introspection report: " + introspectionReportPath.wstring());
            Progress(L"Strings JSON: " + stringsJsonPath.wstring());
            Progress(L"String xrefs JSON: " + stringXrefsJsonPath.wstring());
            Progress(L"Time candidates: " + timeCandidatesPath.wstring());
            Progress(L"Environment candidates: " + environmentCandidatesPath.wstring());
            Progress(L"High confidence functions: " + highConfidenceFunctionsPath.wstring());
            Progress(L"Research dashboard: " + researchDashboardPath.wstring());
            Progress(L"Function traces: " + traceDir.wstring());
            Progress(L"Runtime value watchers: " + valueWatchersPath.wstring());
            Progress(L"Candidate labels: " + labelsDir.wstring());
            Progress(L"Run diff: " + runDiffPath.wstring());
            Progress(L"Asset references: " + assetReferencesPath.wstring());
            Progress(L"Frostbite types: " + frostbiteTypesPath.wstring());
            Progress(L"Interesting modules: " + interestingModulesPath.wstring());
            Progress(L"Scan summary: " + scanSummaryPath.wstring());
            Progress(L"Generated header: " + liveHeaderPath.wstring());
            Progress(L"============================================================");

            g_verboseProgress = previousVerbose;
            return staticResult == 0 ? 0 : staticResult;
        }
        catch (...)
        {
            ProgressError(L"Unexpected exception during injected SDK snapshot.");
            g_verboseProgress = previousVerbose;
            return 10;
        }
    }

#if defined(FROSTBITE_SDK_GENERATOR_DLL)
    DWORD WINAPI InjectedGeneratorThread(LPVOID)
    {
        ::Sleep(250);

        wchar_t disableAutoRun[16]{};
        const DWORD disableLength = ::GetEnvironmentVariableW(
            L"FROSTBITE_SDKGEN_NO_AUTORUN",
            disableAutoRun,
            static_cast<DWORD>(std::size(disableAutoRun)));
        if (disableLength > 0 && disableAutoRun[0] == L'1')
        {
            EnsureConsole();
            Progress(L"DLL auto-run disabled by FROSTBITE_SDKGEN_NO_AUTORUN=1.");
            return 0;
        }

        return static_cast<DWORD>(GenerateInjectedSnapshotInternal(nullptr, true));
    }
#endif
}

int wmain(int argc, wchar_t* argv[])
{
    if (IsHelpRequested(argc, argv))
    {
        PrintUsage();
        return 0;
    }

    const auto parsed = ParseOptions(argc, argv);
    if (!parsed)
        return 1;

    const Options& options = *parsed;
    if (!options.queryText.empty() || !options.traceAddresses.empty() || !options.watchListPath.empty() || options.diffInputs.size() >= 2)
        return RunQueryMode(options);

    return GenerateFromOptions(options, true);
}

FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_OpenConsole()
{
    return EnsureConsole() ? 1 : 0;
}

FROSTBITE_SDK_GENERATOR_API void FrostbiteSDKGenerator_SetVerbose(int verbose)
{
    g_verboseProgress = verbose != 0;
}

FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_GenerateDefault(const wchar_t* outputDir)
{
    try
    {
        if (g_verboseProgress)
            EnsureConsole();

        Progress(L"DLL request: GenerateDefault");

        Options options;
        const fs::path processPath = GetModulePath(nullptr);
        if (!processPath.empty() && processPath.has_parent_path())
            options.gameRoots.emplace_back(processPath.parent_path());
        options.outputDir = (outputDir && outputDir[0] != L'\0')
            ? fs::path(outputDir)
            : fs::current_path() / L"GeneratedSDK";
        return GenerateFromOptions(options, g_verboseProgress);
    }
    catch (...)
    {
        ProgressError(L"Unexpected exception inside GenerateDefault.");
        return 10;
    }
}

FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_GenerateInjectedSnapshot(const wchar_t* outputDir)
{
    return GenerateInjectedSnapshotInternal(outputDir, g_verboseProgress);
}

FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_GenerateFromRoots(
    const wchar_t** gameRoots,
    std::uint32_t gameRootCount,
    const wchar_t* outputDir,
    int includeThirdParty,
    int includeAntiCheat,
    std::uint32_t maxExportsPerModule)
{
    try
    {
        if (g_verboseProgress)
            EnsureConsole();

        Progress(L"DLL request: GenerateFromRoots");

        Options options;
        options.outputDir = (outputDir && outputDir[0] != L'\0')
            ? fs::path(outputDir)
            : fs::current_path() / L"GeneratedSDK";
        options.includeThirdParty = includeThirdParty != 0;
        options.includeAntiCheat = includeAntiCheat != 0;
        options.maxExportsPerModule = maxExportsPerModule == 0 ? 160 : maxExportsPerModule;

        if (gameRoots && gameRootCount != 0)
        {
            for (std::uint32_t i = 0; i < gameRootCount; ++i)
            {
                if (gameRoots[i] && gameRoots[i][0] != L'\0')
                    options.gameRoots.emplace_back(gameRoots[i]);
            }
        }

        if (options.gameRoots.empty())
        {
            const fs::path processPath = GetModulePath(nullptr);
            if (!processPath.empty() && processPath.has_parent_path())
                options.gameRoots.emplace_back(processPath.parent_path());
        }

        return GenerateFromOptions(options, g_verboseProgress);
    }
    catch (...)
    {
        ProgressError(L"Unexpected exception inside GenerateFromRoots.");
        return 10;
    }
}

#if defined(FROSTBITE_SDK_GENERATOR_DLL)
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_generatorModule = module;
        ::DisableThreadLibraryCalls(module);

        HANDLE thread = ::CreateThread(nullptr, 0, InjectedGeneratorThread, nullptr, 0, nullptr);
        if (thread)
            ::CloseHandle(thread);
    }

    return TRUE;
}
#endif
