#include "FrostbiteUniversal.h"
#include "FrostbiteLog.h"

#include <FrostbiteSDKGenerator.h>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    std::mutex g_sdkDumpMutex;
    bool g_sdkDumpRunning = false;
    int g_lastSdkDumpResult = -1;
    std::wstring g_sdkDumpStatus = L"SDK dumper idle.";
    std::wstring g_lastSdkDumpOutputDir;
    FrostbiteGeneratedSdkInfo g_generatedSdkInfo = {};
    std::vector<FrostbiteGeneratedSdkSymbolInfo> g_generatedSdkSymbols;
    std::uint32_t g_generatedSdkReloadGeneration = 0;
    constexpr std::size_t kMaxGeneratedSdkSymbols = 2048;
    constexpr std::size_t kMaxGeneratedSdkFunctionSymbols = 1024;
    constexpr std::size_t kMaxGeneratedSdkStringSymbols = 1024;

    std::wstring WidenAscii(const char* text)
    {
        std::wstring result;
        if (!text)
            return result;

        while (*text)
            result.push_back(static_cast<unsigned char>(*text++));
        return result;
    }

    void LogSdkBackendException(const wchar_t* context, const std::exception& ex)
    {
        std::wstring message = context ? context : L"SDK backend";
        message += L" caught C++ exception: ";
        message += WidenAscii(ex.what());
        FrostbiteUniversal::Log::Write(message);
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::wstring SanitizeFileName(std::wstring value)
    {
        if (value.empty())
            return L"Process";

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

    fs::path GetModulePathFromAddress(const void* address)
    {
        HMODULE module = nullptr;
        if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address),
                &module))
        {
            return {};
        }

        wchar_t path[MAX_PATH] = {};
        const DWORD length = ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path))
            return {};

        return fs::path(path);
    }

    fs::path GetProcessPath()
    {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = ::GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path))
            return {};

        return fs::path(path);
    }

    std::wstring BuildDefaultOutputDir()
    {
        fs::path base = fs::current_path();
        const fs::path universalPath = GetModulePathFromAddress(reinterpret_cast<const void*>(&FrostbiteUniversal_StartSdkDump));
        if (!universalPath.empty())
        {
            const fs::path parent = universalPath.parent_path();
            base = ToLower(parent.filename().wstring()) == L"tools" && parent.has_parent_path()
                ? parent.parent_path()
                : parent;
        }

        const fs::path processPath = GetProcessPath();
        const std::wstring processName = SanitizeFileName(processPath.empty()
            ? std::wstring(L"Process")
            : processPath.stem().wstring());

        std::wstringstream folderName;
        folderName << L"Injected_" << processName << L"_" << ::GetCurrentProcessId();
        return (base / L"GeneratedSDK" / folderName.str()).wstring();
    }

    std::wstring ResolveOutputDir(const wchar_t* outputDir)
    {
        if (outputDir && outputDir[0] != L'\0')
            return outputDir;

        return BuildDefaultOutputDir();
    }

    void CopyWideString(const std::wstring& value, wchar_t* outValue, std::uint32_t outValueLength)
    {
        if (!outValue || outValueLength == 0)
            return;

        wcsncpy_s(outValue, outValueLength, value.c_str(), _TRUNCATE);
    }

    template <std::size_t Size>
    void CopyPath(const fs::path& value, wchar_t (&outValue)[Size])
    {
        CopyWideString(value.wstring(), outValue, static_cast<std::uint32_t>(Size));
    }

    void SetSdkDumpState(bool running, int result, const std::wstring& status, const std::wstring& outputDir)
    {
        std::lock_guard lock(g_sdkDumpMutex);
        g_sdkDumpRunning = running;
        g_lastSdkDumpResult = result;
        g_sdkDumpStatus = status;
        if (!outputDir.empty())
            g_lastSdkDumpOutputDir = outputDir;
    }

    bool PathExists(const fs::path& path)
    {
        std::error_code error;
        return !path.empty() && fs::exists(path, error);
    }

    std::uint64_t FileSizeOrZero(const fs::path& path)
    {
        std::error_code error;
        const std::uintmax_t size = fs::file_size(path, error);
        return error ? 0 : static_cast<std::uint64_t>(size);
    }

    std::string ReadFilePrefix(const fs::path& path, std::size_t maxBytes)
    {
        std::ifstream file;
        try
        {
            file.open(path, std::ios::binary);
            if (!file)
                return {};
        }
        catch (const std::exception& ex)
        {
            LogSdkBackendException(L"ReadFilePrefix", ex);
            return {};
        }

        std::string text(maxBytes, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(file.gcount()));
        return text;
    }

    std::uint32_t ReadCountAfterKey(const fs::path& path, const char* key)
    {
        if (!key || key[0] == '\0')
            return 0;

        const std::string text = ReadFilePrefix(path, 128 * 1024);
        if (text.empty())
            return 0;

        const std::string needle = "\"" + std::string(key) + "\"";
        std::size_t pos = text.find(needle);
        if (pos == std::string::npos)
            return 0;

        pos = text.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return 0;

        ++pos;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;

        std::uint64_t value = 0;
        bool foundDigit = false;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
        {
            foundDigit = true;
            value = (value * 10) + static_cast<std::uint64_t>(text[pos] - '0');
            if (value > UINT32_MAX)
                return UINT32_MAX;
            ++pos;
        }

        return foundDigit ? static_cast<std::uint32_t>(value) : 0;
    }

    std::uint32_t CountPatternInText(const std::string& text, const char* pattern)
    {
        if (!pattern || pattern[0] == '\0' || text.empty())
            return 0;

        std::uint32_t count = 0;
        const std::string needle(pattern);
        std::size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos)
        {
            if (count == UINT32_MAX)
                return count;

            ++count;
            pos += needle.size();
        }

        return count;
    }

    bool ContainsAny(const std::string& lowerText, std::initializer_list<const char*> needles)
    {
        for (const char* needle : needles)
        {
            if (needle && lowerText.find(needle) != std::string::npos)
                return true;
        }

        return false;
    }

    std::uint32_t ClassifyGeneratedSdkSymbol(const std::string& text, std::uint32_t baseFlags)
    {
        const std::string lower = ToLower(text);
        std::uint32_t flags = baseFlags;

        if (ContainsAny(lower, { "player", "soldier", "trooper", "character", "hero", "clientplayer", "localplayer", "networkplayer" }))
            flags |= FrostbiteGeneratedSdkSymbol_PlayerLike;
        if (ContainsAny(lower, { "actor", "gameobject", "objectentity", "blueprint" }))
            flags |= FrostbiteGeneratedSdkSymbol_ActorLike;
        if (ContainsAny(lower, { "entity", "entities", "entitylist", "entitymanager", "cliententity", "serverentity" }))
            flags |= FrostbiteGeneratedSdkSymbol_EntityLike;
        if (ContainsAny(lower, { "model", "renderable", "visual", "appearance" }))
            flags |= FrostbiteGeneratedSdkSymbol_ModelLike;
        if (ContainsAny(lower, { "mesh", "skinnedmesh", "geometry", "skeleton", "bone", "rig" }))
            flags |= FrostbiteGeneratedSdkSymbol_MeshLike;
        if (ContainsAny(lower, { "console", "cvar", "command", "exec", "cheatmanager" }))
            flags |= FrostbiteGeneratedSdkSymbol_ConsoleLike;
        if (ContainsAny(lower, { "camera", "fov", "viewmatrix", "viewprojection", "viewangle", "projecttoscreen" }))
            flags |= FrostbiteGeneratedSdkSymbol_CameraLike;
        if (ContainsAny(lower, { "transform", "position", "translation", "rotation", "vector3", "vec3", "worldspace", "worldmatrix" }))
            flags |= FrostbiteGeneratedSdkSymbol_TransformLike;
        if (ContainsAny(lower, { "time", "timescale", "deltatime", "tick", "framerate", "fps" }))
            flags |= FrostbiteGeneratedSdkSymbol_TimeLike;

        return flags;
    }

    bool HasGeneratedSdkSymbolMeaning(std::uint32_t flags)
    {
        constexpr std::uint32_t meaningful =
            FrostbiteGeneratedSdkSymbol_PlayerLike |
            FrostbiteGeneratedSdkSymbol_ActorLike |
            FrostbiteGeneratedSdkSymbol_EntityLike |
            FrostbiteGeneratedSdkSymbol_ModelLike |
            FrostbiteGeneratedSdkSymbol_MeshLike |
            FrostbiteGeneratedSdkSymbol_ConsoleLike |
            FrostbiteGeneratedSdkSymbol_CameraLike |
            FrostbiteGeneratedSdkSymbol_TransformLike |
            FrostbiteGeneratedSdkSymbol_TimeLike;
        return (flags & meaningful) != 0;
    }

    int CountJsonBraces(const std::string& text)
    {
        int depthDelta = 0;
        bool inString = false;
        bool escaped = false;

        for (char ch : text)
        {
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (ch == '\\' && inString)
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
            {
                inString = !inString;
                continue;
            }

            if (inString)
                continue;

            if (ch == '{')
                ++depthDelta;
            else if (ch == '}')
                --depthDelta;
        }

        return depthDelta;
    }

    std::string ExtractJsonString(const std::string& objectText, const char* key)
    {
        if (!key || key[0] == '\0')
            return {};

        const std::string needle = "\"" + std::string(key) + "\"";
        std::size_t pos = objectText.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = objectText.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        pos = objectText.find('"', pos + 1);
        if (pos == std::string::npos)
            return {};

        std::string value;
        bool escaped = false;
        for (++pos; pos < objectText.size(); ++pos)
        {
            const char ch = objectText[pos];
            if (escaped)
            {
                switch (ch)
                {
                case 'n':
                    value.push_back(' ');
                    break;
                case 'r':
                case 't':
                    value.push_back(' ');
                    break;
                default:
                    value.push_back(ch);
                    break;
                }
                escaped = false;
                continue;
            }

            if (ch == '\\')
            {
                escaped = true;
                continue;
            }

            if (ch == '"')
                break;

            value.push_back(ch);
        }

        return value;
    }

    std::uint32_t ExtractJsonUInt(const std::string& objectText, const char* key)
    {
        if (!key || key[0] == '\0')
            return 0;

        const std::string needle = "\"" + std::string(key) + "\"";
        std::size_t pos = objectText.find(needle);
        if (pos == std::string::npos)
            return 0;

        pos = objectText.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return 0;

        ++pos;
        while (pos < objectText.size() && std::isspace(static_cast<unsigned char>(objectText[pos])))
            ++pos;

        std::uint64_t value = 0;
        while (pos < objectText.size() && std::isdigit(static_cast<unsigned char>(objectText[pos])))
        {
            value = (value * 10) + static_cast<std::uint64_t>(objectText[pos] - '0');
            if (value > UINT32_MAX)
                return UINT32_MAX;
            ++pos;
        }

        return static_cast<std::uint32_t>(value);
    }

    std::string ExtractJsonStringArrayPreview(const std::string& objectText, const char* key, std::size_t maxItems)
    {
        if (!key || key[0] == '\0' || maxItems == 0)
            return {};

        const std::string needle = "\"" + std::string(key) + "\"";
        std::size_t pos = objectText.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = objectText.find('[', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        std::string preview;
        std::size_t itemCount = 0;
        while (pos < objectText.size() && itemCount < maxItems)
        {
            pos = objectText.find('"', pos + 1);
            if (pos == std::string::npos)
                break;

            std::string item;
            bool escaped = false;
            for (++pos; pos < objectText.size(); ++pos)
            {
                const char ch = objectText[pos];
                if (escaped)
                {
                    item.push_back(ch);
                    escaped = false;
                    continue;
                }

                if (ch == '\\')
                {
                    escaped = true;
                    continue;
                }

                if (ch == '"')
                    break;

                item.push_back(ch);
            }

            if (!item.empty())
            {
                if (!preview.empty())
                    preview += " | ";
                preview += item;
                ++itemCount;
            }

            const std::size_t arrayEnd = objectText.find(']', pos);
            const std::size_t nextQuote = objectText.find('"', pos + 1);
            if (arrayEnd != std::string::npos && (nextQuote == std::string::npos || arrayEnd < nextQuote))
                break;
        }

        return preview;
    }

    void CopyUtf8ToWideField(const std::string& value, wchar_t* outValue, std::uint32_t outValueLength)
    {
        CopyWideString(WidenAscii(value.c_str()), outValue, outValueLength);
    }

    void AddGeneratedSdkSymbol(std::vector<FrostbiteGeneratedSdkSymbolInfo>& symbols, FrostbiteGeneratedSdkSymbolInfo info)
    {
        if (symbols.size() >= kMaxGeneratedSdkSymbols)
            return;

        symbols.emplace_back(info);
    }

    void ParseGeneratedFunctionCandidate(
        const std::string& objectText,
        std::vector<FrostbiteGeneratedSdkSymbolInfo>& symbols,
        std::size_t& functionCount)
    {
        if (functionCount >= kMaxGeneratedSdkFunctionSymbols)
            return;

        const std::string module = ExtractJsonString(objectText, "module");
        const std::string category = ExtractJsonString(objectText, "primaryCategory");
        const std::string address = ExtractJsonString(objectText, "functionAddressHex");
        const std::string reasoning = ExtractJsonString(objectText, "reasoning");
        const std::string related = ExtractJsonStringArrayPreview(objectText, "relatedStrings", 3);
        const std::uint32_t score = ExtractJsonUInt(objectText, "score");

        std::string name = related.empty() ? (category + " function " + address) : related;
        if (name.empty())
            name = address.empty() ? "function candidate" : address;

        std::string searchable = module + " " + category + " " + address + " " + reasoning + " " + related;
        const std::uint32_t flags = ClassifyGeneratedSdkSymbol(searchable, FrostbiteGeneratedSdkSymbol_FunctionCandidate);
        if (!HasGeneratedSdkSymbolMeaning(flags))
            return;

        FrostbiteGeneratedSdkSymbolInfo info = {};
        CopyWideString(L"function", info.source, static_cast<std::uint32_t>(std::size(info.source)));
        CopyUtf8ToWideField(category, info.category, static_cast<std::uint32_t>(std::size(info.category)));
        CopyUtf8ToWideField(module, info.moduleName, static_cast<std::uint32_t>(std::size(info.moduleName)));
        CopyUtf8ToWideField(name, info.name, static_cast<std::uint32_t>(std::size(info.name)));
        CopyUtf8ToWideField(address, info.addressHex, static_cast<std::uint32_t>(std::size(info.addressHex)));
        CopyUtf8ToWideField(reasoning, info.detail, static_cast<std::uint32_t>(std::size(info.detail)));
        info.score = score;
        info.flags = flags;

        AddGeneratedSdkSymbol(symbols, info);
        ++functionCount;
    }

    void ParseGeneratedStringCandidate(
        const std::string& objectText,
        std::vector<FrostbiteGeneratedSdkSymbolInfo>& symbols,
        std::size_t& stringCount)
    {
        if (stringCount >= kMaxGeneratedSdkStringSymbols)
            return;

        const std::string value = ExtractJsonString(objectText, "value");
        const std::string module = ExtractJsonString(objectText, "module");
        const std::string category = ExtractJsonString(objectText, "category");
        const std::string address = ExtractJsonString(objectText, "addressHex");
        const std::string section = ExtractJsonString(objectText, "section");
        const std::uint32_t score = ExtractJsonUInt(objectText, "score");
        const std::uint32_t xrefs = ExtractJsonUInt(objectText, "xrefCount");

        std::string searchable = value + " " + module + " " + category + " " + section;
        const std::uint32_t flags = ClassifyGeneratedSdkSymbol(searchable, FrostbiteGeneratedSdkSymbol_String);
        if (!HasGeneratedSdkSymbolMeaning(flags))
            return;

        FrostbiteGeneratedSdkSymbolInfo info = {};
        CopyWideString(L"string", info.source, static_cast<std::uint32_t>(std::size(info.source)));
        CopyUtf8ToWideField(category, info.category, static_cast<std::uint32_t>(std::size(info.category)));
        CopyUtf8ToWideField(module, info.moduleName, static_cast<std::uint32_t>(std::size(info.moduleName)));
        CopyUtf8ToWideField(value, info.name, static_cast<std::uint32_t>(std::size(info.name)));
        CopyUtf8ToWideField(address, info.addressHex, static_cast<std::uint32_t>(std::size(info.addressHex)));

        std::string detail = "section=" + section + " xrefs=" + std::to_string(xrefs);
        CopyUtf8ToWideField(detail, info.detail, static_cast<std::uint32_t>(std::size(info.detail)));
        info.score = score;
        info.flags = flags;

        AddGeneratedSdkSymbol(symbols, info);
        ++stringCount;
    }

    template <typename ParseFn>
    void ParseJsonObjectArray(const fs::path& path, const char* arrayName, ParseFn&& parse)
    {
        if (!PathExists(path))
            return;

        std::ifstream file;
        try
        {
            file.open(path, std::ios::binary);
            if (!file)
                return;
        }
        catch (const std::exception& ex)
        {
            LogSdkBackendException(L"ParseJsonObjectArray", ex);
            return;
        }

        const std::string arrayNeedle = "\"" + std::string(arrayName) + "\"";
        bool inArray = false;
        int depth = 0;
        std::string objectText;
        std::string line;

        while (std::getline(file, line))
        {
            if (!inArray)
            {
                if (line.find(arrayNeedle) != std::string::npos)
                    inArray = true;
                continue;
            }

            if (depth == 0 && line.find(']') != std::string::npos && line.find('{') == std::string::npos)
                break;

            const bool startsObject = depth == 0 && line.find('{') != std::string::npos;
            if (startsObject)
                objectText.clear();

            if (depth > 0 || startsObject)
            {
                objectText += line;
                objectText.push_back('\n');
            }

            depth += CountJsonBraces(line);
            if (depth <= 0 && !objectText.empty())
            {
                depth = 0;
                parse(objectText);
                objectText.clear();
            }
        }
    }

    std::vector<FrostbiteGeneratedSdkSymbolInfo> BuildGeneratedSdkSymbols(const fs::path& outputDir)
    {
        std::vector<FrostbiteGeneratedSdkSymbolInfo> symbols;
        std::size_t functionCount = 0;
        std::size_t stringCount = 0;

        ParseJsonObjectArray(outputDir / L"FrostbiteRuntimeIntrospection.json", "functionCandidates",
            [&](const std::string& objectText) {
                ParseGeneratedFunctionCandidate(objectText, symbols, functionCount);
            });

        ParseJsonObjectArray(outputDir / L"Strings.json", "strings",
            [&](const std::string& objectText) {
                ParseGeneratedStringCandidate(objectText, symbols, stringCount);
            });

        std::sort(symbols.begin(), symbols.end(), [](const FrostbiteGeneratedSdkSymbolInfo& lhs, const FrostbiteGeneratedSdkSymbolInfo& rhs) {
            const auto rank = [](std::uint32_t flags) {
                std::uint32_t value = 0;
                if ((flags & FrostbiteGeneratedSdkSymbol_PlayerLike) != 0) value += 100;
                if ((flags & FrostbiteGeneratedSdkSymbol_ActorLike) != 0) value += 80;
                if ((flags & FrostbiteGeneratedSdkSymbol_EntityLike) != 0) value += 70;
                if ((flags & FrostbiteGeneratedSdkSymbol_ModelLike) != 0) value += 50;
                if ((flags & FrostbiteGeneratedSdkSymbol_MeshLike) != 0) value += 45;
                if ((flags & FrostbiteGeneratedSdkSymbol_CameraLike) != 0) value += 35;
                if ((flags & FrostbiteGeneratedSdkSymbol_TransformLike) != 0) value += 30;
                if ((flags & FrostbiteGeneratedSdkSymbol_ConsoleLike) != 0) value += 25;
                return value;
            };

            const std::uint32_t lhsRank = rank(lhs.flags);
            const std::uint32_t rhsRank = rank(rhs.flags);
            if (lhsRank != rhsRank)
                return lhsRank > rhsRank;
            if (lhs.score != rhs.score)
                return lhs.score > rhs.score;
            return wcscmp(lhs.name, rhs.name) < 0;
        });

        return symbols;
    }

    fs::path FindLatestGeneratedSdkDirectory()
    {
        try
        {
            const fs::path root = fs::path(BuildDefaultOutputDir()).parent_path();
            if (!PathExists(root))
                return {};

            fs::path best;
            fs::file_time_type bestTime = fs::file_time_type::min();
            int bestScore = 0;
            int skippedOtherProcessDumps = 0;
            std::wstring skippedOtherProcessSample;
            const fs::path processPath = GetProcessPath();
            const std::wstring processStem = ToLower(SanitizeFileName(processPath.empty()
                ? std::wstring{}
                : processPath.stem().wstring()));
            std::error_code error;
            for (const fs::directory_entry& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, error))
            {
                if (error)
                    break;

                std::error_code entryError;
                if (!entry.is_directory(entryError) || entryError)
                    continue;

                const fs::file_time_type writeTime = entry.last_write_time(entryError);
                if (entryError)
                    continue;

                const std::wstring folderName = ToLower(entry.path().filename().wstring());
                int score = 0;
                const bool matchesProcess = !processStem.empty() && folderName.find(processStem) != std::wstring::npos;
                if (!processStem.empty() && !matchesProcess)
                {
                    ++skippedOtherProcessDumps;
                    if (skippedOtherProcessSample.empty())
                        skippedOtherProcessSample = entry.path().filename().wstring();
                    continue;
                }

                if (matchesProcess)
                    score += 100;
                if (folderName.find(L"injected_") != std::wstring::npos)
                    score += 25;
                if (PathExists(entry.path() / L"FrostbiteRuntimeIntrospection.json"))
                    score += 20;
                if (PathExists(entry.path() / L"FrostbiteInjectedProcess.json"))
                    score += 15;
                if (PathExists(entry.path() / L"FrostbiteSDKManifest.json"))
                    score += 10;
                if (folderName == L"example")
                    score -= 100;

                if (score <= 0)
                    continue;

                if (best.empty() || score > bestScore || (score == bestScore && writeTime > bestTime))
                {
                    best = entry.path();
                    bestTime = writeTime;
                    bestScore = score;
                }
            }

            if (best.empty())
            {
                FrostbiteUniversal::Log::Write(L"Generated SDK cache prime found no matching current-process dump folder");
                if (skippedOtherProcessDumps > 0)
                {
                    std::wstringstream skipped;
                    skipped << L"Generated SDK cache skipped " << skippedOtherProcessDumps
                            << L" other-process dump folder(s); sample=" << skippedOtherProcessSample;
                    FrostbiteUniversal::Log::Write(skipped.str());
                }
            }
            return best;
        }
        catch (const std::exception& ex)
        {
            LogSdkBackendException(L"FindLatestGeneratedSdkDirectory", ex);
            return {};
        }
    }

    fs::path ResolveReloadDir(const wchar_t* outputDir)
    {
        if (outputDir && outputDir[0] != L'\0')
            return fs::path(outputDir);

        std::wstring lastOutputDir;
        {
            std::lock_guard lock(g_sdkDumpMutex);
            lastOutputDir = g_lastSdkDumpOutputDir;
        }

        if (!lastOutputDir.empty() && PathExists(lastOutputDir))
            return fs::path(lastOutputDir);

        return FindLatestGeneratedSdkDirectory();
    }

    bool ReloadGeneratedSdkDirectory(const fs::path& outputDir, int dumpResult, bool refreshLiveRuntime)
    {
        try
        {
            if (!PathExists(outputDir))
                return false;

            FrostbiteGeneratedSdkInfo info = {};
            CopyPath(outputDir, info.outputDir);
            info.lastDumpResult = dumpResult;

            const fs::path manifestPath = outputDir / L"FrostbiteSDKManifest.json";
            const fs::path runtimePath = outputDir / L"FrostbiteRuntimeIntrospection.json";
            const fs::path processPath = outputDir / L"FrostbiteInjectedProcess.json";
            const fs::path stringsPath = outputDir / L"Strings.json";
            const fs::path xrefsPath = outputDir / L"StringXrefs.json";

            if (PathExists(manifestPath))
            {
                CopyPath(manifestPath, info.manifestPath);
                const std::string manifest = ReadFilePrefix(manifestPath, 4 * 1024 * 1024);
                info.manifestGameCount = CountPatternInText(manifest, "\"title\"");
                info.manifestFileCount = CountPatternInText(manifest, "\"path\"");
            }

            if (PathExists(runtimePath))
            {
                CopyPath(runtimePath, info.runtimeIntrospectionPath);
                info.runtimeFunctionCandidateCount = ReadCountAfterKey(runtimePath, "functionCandidateCount");
            }

            if (PathExists(processPath))
            {
                CopyPath(processPath, info.injectedProcessPath);
                info.processModuleCount = ReadCountAfterKey(processPath, "moduleCount");
                const std::string processPrefix = ReadFilePrefix(processPath, 2 * 1024 * 1024);
                info.processSymbolCount = CountPatternInText(processPrefix, "\"symbolCount\"");
            }

            if (PathExists(stringsPath))
            {
                CopyPath(stringsPath, info.stringsPath);
                info.stringCount = ReadCountAfterKey(stringsPath, "stringCount");
            }

            if (PathExists(xrefsPath))
            {
                CopyPath(xrefsPath, info.stringXrefsPath);
                info.stringXrefCount = ReadCountAfterKey(xrefsPath, "xrefCount");
            }

            std::error_code error;
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(outputDir, fs::directory_options::skip_permission_denied, error))
            {
                if (error)
                    break;

                std::error_code entryError;
                if (!entry.is_regular_file(entryError) || entryError)
                    continue;

                if (info.generatedFileCount != UINT32_MAX)
                    ++info.generatedFileCount;
                info.generatedBytes += FileSizeOrZero(entry.path());
            }

            std::vector<FrostbiteGeneratedSdkSymbolInfo> generatedSymbols = BuildGeneratedSdkSymbols(outputDir);
            info.generatedSymbolCount = static_cast<std::uint32_t>((std::min)(generatedSymbols.size(), static_cast<std::size_t>(UINT32_MAX)));
            std::vector<FrostbiteGeneratedSdkSymbolInfo> sampleSymbols;
            const std::size_t sampleCount = (std::min)(generatedSymbols.size(), static_cast<std::size_t>(8));
            sampleSymbols.insert(sampleSymbols.end(), generatedSymbols.begin(), generatedSymbols.begin() + sampleCount);

            {
                std::lock_guard lock(g_sdkDumpMutex);
                info.reloadGeneration = ++g_generatedSdkReloadGeneration;
                g_generatedSdkInfo = info;
                g_generatedSdkSymbols = std::move(generatedSymbols);
                g_lastSdkDumpOutputDir = outputDir.wstring();
            }

            std::wstringstream message;
            message << L"Generated SDK reloaded into runtime cache: files=" << info.generatedFileCount
                    << L", bytes=" << info.generatedBytes
                    << L", functions=" << info.runtimeFunctionCandidateCount
                    << L", strings=" << info.stringCount
                    << L", xrefs=" << info.stringXrefCount
                    << L", classifiedSymbols=" << info.generatedSymbolCount
                    << L", output=" << outputDir.wstring();
            FrostbiteUniversal::Log::Write(message.str());

            for (const FrostbiteGeneratedSdkSymbolInfo& symbol : sampleSymbols)
            {
                std::wstringstream symbolLine;
                symbolLine << L"Generated SDK symbol candidate: source=" << symbol.source
                           << L" category=" << symbol.category
                           << L" score=" << std::dec << symbol.score
                           << L" flags=0x" << std::hex << symbol.flags
                           << L" name=" << symbol.name
                           << L" address=" << symbol.addressHex;
                FrostbiteUniversal::Log::Write(symbolLine.str());
            }

            if (refreshLiveRuntime)
                FrostbiteUniversal_Refresh();

            return true;
        }
        catch (const std::exception& ex)
        {
            LogSdkBackendException(L"ReloadGeneratedSdkDirectory", ex);
            return false;
        }
        catch (...)
        {
            FrostbiteUniversal::Log::Write(L"ReloadGeneratedSdkDirectory caught unknown C++ exception");
            return false;
        }
    }

    struct SdkDumpRequest
    {
        std::wstring outputDir;
    };

    DWORD WINAPI SdkDumpThread(void* context)
    {
        try
        {
            std::unique_ptr<SdkDumpRequest> request(static_cast<SdkDumpRequest*>(context));
            const std::wstring outputDir = request ? request->outputDir : BuildDefaultOutputDir();

            SetSdkDumpState(true, -1, L"SDK dumper running. Console output and generated reports are being written.", outputDir);
            FrostbiteUniversal::Log::Write(L"SDK backend dump started: " + outputDir);

            FrostbiteSDKGenerator_SetVerbose(1);
            const int result = FrostbiteSDKGenerator_GenerateInjectedSnapshot(outputDir.c_str());
            const bool reloaded = result == 0 && ReloadGeneratedSdkDirectory(outputDir, result, true);

            std::wstringstream status;
            status << L"SDK dumper finished with code " << result << L".";
            if (reloaded)
                status << L" Runtime cache reloaded.";
            FrostbiteUniversal::Log::Write(status.str() + L" Output: " + outputDir);
            SetSdkDumpState(false, result, status.str(), outputDir);
            return static_cast<DWORD>(result);
        }
        catch (const std::exception& ex)
        {
            LogSdkBackendException(L"SdkDumpThread", ex);
            SetSdkDumpState(false, -1, L"SDK dumper thread failed with a C++ exception.", {});
            return static_cast<DWORD>(-1);
        }
        catch (...)
        {
            FrostbiteUniversal::Log::Write(L"SdkDumpThread caught unknown C++ exception");
            SetSdkDumpState(false, -1, L"SDK dumper thread failed with an unknown C++ exception.", {});
            return static_cast<DWORD>(-1);
        }
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_RunSdkDump(const wchar_t* outputDir)
{
    try
    {
        const std::wstring resolvedOutputDir = ResolveOutputDir(outputDir);

        {
            std::lock_guard lock(g_sdkDumpMutex);
            if (g_sdkDumpRunning)
                return 0;

            g_sdkDumpRunning = true;
            g_lastSdkDumpResult = -1;
            g_sdkDumpStatus = L"SDK dumper running. Console output and generated reports are being written.";
            g_lastSdkDumpOutputDir = resolvedOutputDir;
        }

        FrostbiteUniversal::Log::Write(L"SDK backend dump started synchronously: " + resolvedOutputDir);
        FrostbiteSDKGenerator_SetVerbose(1);
        const int result = FrostbiteSDKGenerator_GenerateInjectedSnapshot(resolvedOutputDir.c_str());
        const bool reloaded = result == 0 && ReloadGeneratedSdkDirectory(resolvedOutputDir, result, true);

        std::wstringstream status;
        status << L"SDK dumper finished with code " << result << L".";
        if (reloaded)
            status << L" Runtime cache reloaded.";
        FrostbiteUniversal::Log::Write(status.str() + L" Output: " + resolvedOutputDir);
        SetSdkDumpState(false, result, status.str(), resolvedOutputDir);
        return result;
    }
    catch (const std::exception& ex)
    {
        LogSdkBackendException(L"FrostbiteUniversal_RunSdkDump", ex);
        SetSdkDumpState(false, -1, L"SDK dumper failed with a C++ exception.", {});
        return -1;
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_StartSdkDump(const wchar_t* outputDir)
{
    try
    {
        const std::wstring resolvedOutputDir = ResolveOutputDir(outputDir);

        {
            std::lock_guard lock(g_sdkDumpMutex);
            if (g_sdkDumpRunning)
                return 0;

            g_sdkDumpRunning = true;
            g_lastSdkDumpResult = -1;
            g_sdkDumpStatus = L"SDK dumper queued.";
            g_lastSdkDumpOutputDir = resolvedOutputDir;
        }

        auto request = std::make_unique<SdkDumpRequest>();
        request->outputDir = resolvedOutputDir;

        HANDLE thread = ::CreateThread(nullptr, 0, SdkDumpThread, request.get(), 0, nullptr);
        if (!thread)
        {
            const DWORD error = ::GetLastError();
            std::wstringstream status;
            status << L"SDK dumper failed to start. GetLastError=" << error << L".";
            SetSdkDumpState(false, static_cast<int>(error), status.str(), resolvedOutputDir);
            FrostbiteUniversal::Log::Write(status.str());
            return 0;
        }

        request.release();
        ::CloseHandle(thread);
        return 1;
    }
    catch (const std::exception& ex)
    {
        LogSdkBackendException(L"FrostbiteUniversal_StartSdkDump", ex);
        SetSdkDumpState(false, -1, L"SDK dumper failed to start because of a C++ exception.", {});
        return 0;
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_IsSdkDumpRunning()
{
    std::lock_guard lock(g_sdkDumpMutex);
    return g_sdkDumpRunning ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetLastSdkDumpResult()
{
    std::lock_guard lock(g_sdkDumpMutex);
    return g_lastSdkDumpResult;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetSdkDumpStatus(wchar_t* outStatus, std::uint32_t outStatusLength)
{
    if (!outStatus || outStatusLength == 0)
        return 0;

    std::lock_guard lock(g_sdkDumpMutex);
    CopyWideString(g_sdkDumpStatus, outStatus, outStatusLength);
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetLastSdkDumpOutputDir(wchar_t* outPath, std::uint32_t outPathLength)
{
    if (!outPath || outPathLength == 0)
        return 0;

    std::wstring outputDir;
    {
        std::lock_guard lock(g_sdkDumpMutex);
        outputDir = g_lastSdkDumpOutputDir;
    }

    if (outputDir.empty())
        outputDir = BuildDefaultOutputDir();

    CopyWideString(outputDir, outPath, outPathLength);
    return !outputDir.empty() ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ReloadGeneratedSdk(const wchar_t* outputDir)
{
    try
    {
        const fs::path resolvedOutputDir = ResolveReloadDir(outputDir);
        if (resolvedOutputDir.empty())
        {
            FrostbiteUniversal::Log::Write(L"Generated SDK reload skipped: no generated SDK folder found");
            return 0;
        }

        int lastResult = -1;
        {
            std::lock_guard lock(g_sdkDumpMutex);
            lastResult = g_lastSdkDumpResult;
        }

        return ReloadGeneratedSdkDirectory(resolvedOutputDir, lastResult, true) ? 1 : 0;
    }
    catch (const std::exception& ex)
    {
        LogSdkBackendException(L"FrostbiteUniversal_ReloadGeneratedSdk", ex);
        return 0;
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_PrimeGeneratedSdkCache(const wchar_t* outputDir)
{
    try
    {
        const fs::path resolvedOutputDir = ResolveReloadDir(outputDir);
        if (resolvedOutputDir.empty())
        {
            FrostbiteUniversal::Log::Write(L"Generated SDK cache prime skipped: no generated SDK folder found");
            return 0;
        }

        int lastResult = -1;
        {
            std::lock_guard lock(g_sdkDumpMutex);
            lastResult = g_lastSdkDumpResult;
        }

        return ReloadGeneratedSdkDirectory(resolvedOutputDir, lastResult, false) ? 1 : 0;
    }
    catch (const std::exception& ex)
    {
        LogSdkBackendException(L"FrostbiteUniversal_PrimeGeneratedSdkCache", ex);
        return 0;
    }
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetGeneratedSdkInfo(FrostbiteGeneratedSdkInfo* outInfo)
{
    if (!outInfo)
        return 0;

    std::lock_guard lock(g_sdkDumpMutex);
    *outInfo = g_generatedSdkInfo;
    return g_generatedSdkInfo.outputDir[0] != L'\0' ? 1 : 0;
}

FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetGeneratedSdkSymbolCount()
{
    std::lock_guard lock(g_sdkDumpMutex);
    return static_cast<std::uint32_t>((std::min)(g_generatedSdkSymbols.size(), static_cast<std::size_t>(UINT32_MAX)));
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetGeneratedSdkSymbolInfo(std::uint32_t index, FrostbiteGeneratedSdkSymbolInfo* outInfo)
{
    if (!outInfo)
        return 0;

    std::lock_guard lock(g_sdkDumpMutex);
    if (index >= g_generatedSdkSymbols.size())
        return 0;

    *outInfo = g_generatedSdkSymbols[index];
    return 1;
}
