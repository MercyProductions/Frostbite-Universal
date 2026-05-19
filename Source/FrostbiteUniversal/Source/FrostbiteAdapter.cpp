#include "FrostbiteUniversal.h"
#include "FrostbiteLog.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr std::uint32_t kMaxEntities = 8192;

    std::mutex g_adapterMutex;
    FrostbiteViewProjectionProviderCallback g_matrixProvider = nullptr;
    void* g_matrixProviderUserData = nullptr;
    FrostbiteViewportProviderCallback g_viewportProvider = nullptr;
    void* g_viewportProviderUserData = nullptr;
    FrostbiteMatrix4x4 g_viewProjection = {};
    FrostbiteViewport g_viewport = {};
    FrostbiteAdapterTiming g_timing = {};
    bool g_hasMatrix = false;
    bool g_hasViewport = false;
    bool g_entityProviderRegistered = false;

    bool IsFinite(float value)
    {
        return std::isfinite(value);
    }

    bool IsValidViewport(const FrostbiteViewport& viewport)
    {
        return IsFinite(viewport.x) &&
            IsFinite(viewport.y) &&
            IsFinite(viewport.width) &&
            IsFinite(viewport.height) &&
            viewport.width > 1.0f &&
            viewport.height > 1.0f;
    }

    bool IsValidMatrix(const FrostbiteMatrix4x4& matrix)
    {
        bool anyNonZero = false;
        for (float value : matrix.m)
        {
            if (!IsFinite(value))
                return false;
            anyNonZero = anyNonZero || std::fabs(value) > 0.000001f;
        }
        return anyNonZero;
    }

    LARGE_INTEGER NowCounter()
    {
        LARGE_INTEGER value = {};
        ::QueryPerformanceCounter(&value);
        return value;
    }

    double ElapsedMs(LARGE_INTEGER start, LARGE_INTEGER end)
    {
        LARGE_INTEGER frequency = {};
        ::QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart == 0)
            return 0.0;
        return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) /
            static_cast<double>(frequency.QuadPart);
    }

    template <std::size_t N>
    void CopyWide(wchar_t (&dest)[N], const std::wstring& value)
    {
        wcsncpy_s(dest, value.c_str(), _TRUNCATE);
    }

    std::wstring ToLower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(::towlower(ch));
        });
        return value;
    }

    bool Contains(const std::wstring& value, const wchar_t* needle)
    {
        return needle && ToLower(value).find(ToLower(needle)) != std::wstring::npos;
    }

    std::string WideToUtf8(const wchar_t* value)
    {
        if (!value || value[0] == L'\0')
            return {};

        const int size = ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1)
            return {};

        std::string result(static_cast<std::size_t>(size - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
        return result;
    }

    void CopyUtf8ToWide(const std::string& value, wchar_t* outValue, std::uint32_t outValueLength)
    {
        if (!outValue || outValueLength == 0)
            return;

        if (value.empty())
        {
            outValue[0] = L'\0';
            return;
        }

        const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0)
        {
            outValue[0] = L'\0';
            return;
        }

        std::wstring wide(static_cast<std::size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), size);
        wcsncpy_s(outValue, outValueLength, wide.c_str(), _TRUNCATE);
    }

    std::string JsonEscape(const wchar_t* value)
    {
        const std::string utf8 = WideToUtf8(value);
        std::string escaped;
        for (char ch : utf8)
        {
            switch (ch)
            {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
            }
        }
        return escaped;
    }

    bool ProjectWithConvention(
        const FrostbiteMatrix4x4& matrix,
        const FrostbiteViewport& viewport,
        const FrostbiteVec3& world,
        bool columnMajor,
        bool openGlDepth,
        bool yFlip,
        FrostbiteProjectedPoint& outPoint)
    {
        const float* m = matrix.m;
        float clipX = 0.0f;
        float clipY = 0.0f;
        float clipZ = 0.0f;
        float clipW = 0.0f;

        if (columnMajor)
        {
            clipX = world.x * m[0] + world.y * m[4] + world.z * m[8] + m[12];
            clipY = world.x * m[1] + world.y * m[5] + world.z * m[9] + m[13];
            clipZ = world.x * m[2] + world.y * m[6] + world.z * m[10] + m[14];
            clipW = world.x * m[3] + world.y * m[7] + world.z * m[11] + m[15];
        }
        else
        {
            clipX = world.x * m[0] + world.y * m[1] + world.z * m[2] + m[3];
            clipY = world.x * m[4] + world.y * m[5] + world.z * m[6] + m[7];
            clipZ = world.x * m[8] + world.y * m[9] + world.z * m[10] + m[11];
            clipW = world.x * m[12] + world.y * m[13] + world.z * m[14] + m[15];
        }

        if (!IsFinite(clipX) || !IsFinite(clipY) || !IsFinite(clipZ) || !IsFinite(clipW) || std::fabs(clipW) < 0.00001f)
            return false;

        const float ndcX = clipX / clipW;
        const float ndcY = clipY / clipW;
        const float ndcZ = clipZ / clipW;
        if (!IsFinite(ndcX) || !IsFinite(ndcY) || !IsFinite(ndcZ))
            return false;

        outPoint.x = viewport.x + ((ndcX * 0.5f) + 0.5f) * viewport.width;
        outPoint.y = viewport.y + (yFlip ? ((ndcY * 0.5f) + 0.5f) : (0.5f - (ndcY * 0.5f))) * viewport.height;
        outPoint.depth = ndcZ;

        const bool depthClipped = openGlDepth ? (ndcZ < -1.0f || ndcZ > 1.0f) : (ndcZ < 0.0f || ndcZ > 1.0f);
        const bool xyClipped = ndcX < -1.0f || ndcX > 1.0f || ndcY < -1.0f || ndcY > 1.0f;
        outPoint.clipped = (xyClipped || depthClipped || clipW < 0.0f) ? 1 : 0;
        return IsFinite(outPoint.x) && IsFinite(outPoint.y);
    }

    bool ProjectWithMatrix(
        const FrostbiteMatrix4x4& matrix,
        const FrostbiteViewport& viewport,
        const FrostbiteVec3& world,
        FrostbiteProjectedPoint& outPoint)
    {
        if (!IsValidMatrix(matrix) || !IsValidViewport(viewport))
            return false;

        const std::uint32_t flags = matrix.flags;
        const bool wantsColumn = (flags & FrostbiteMatrix_ColumnMajor) != 0;
        const bool wantsRow = (flags & FrostbiteMatrix_RowMajor) != 0;
        const bool openGlDepth = (flags & FrostbiteMatrix_OpenGLDepth) != 0 || (flags & FrostbiteMatrix_D3DDepth) == 0;
        const bool yFlip = (flags & FrostbiteMatrix_YFlip) != 0;

        if (wantsColumn || wantsRow)
            return ProjectWithConvention(matrix, viewport, world, wantsColumn, openGlDepth, yFlip, outPoint);

        FrostbiteProjectedPoint row = {};
        if (ProjectWithConvention(matrix, viewport, world, false, openGlDepth, yFlip, row) && !row.clipped)
        {
            outPoint = row;
            return true;
        }

        FrostbiteProjectedPoint column = {};
        if (ProjectWithConvention(matrix, viewport, world, true, openGlDepth, yFlip, column))
        {
            outPoint = column;
            return true;
        }

        if (ProjectWithConvention(matrix, viewport, world, false, openGlDepth, yFlip, row))
        {
            outPoint = row;
            return true;
        }

        return false;
    }

    FrostbiteVec3 MakeVec3(const float value[3])
    {
        return { value[0], value[1], value[2] };
    }

    void BuildWorldBounds(const FrostbiteActorModelInfo& item, FrostbiteVec3& outMin, FrostbiteVec3& outMax)
    {
        outMin = MakeVec3(item.boundsMin);
        outMax = MakeVec3(item.boundsMax);

        const bool boundsEmpty =
            std::fabs(outMin.x) < 0.00001f &&
            std::fabs(outMin.y) < 0.00001f &&
            std::fabs(outMin.z) < 0.00001f &&
            std::fabs(outMax.x) < 0.00001f &&
            std::fabs(outMax.y) < 0.00001f &&
            std::fabs(outMax.z) < 0.00001f;

        if (!boundsEmpty)
            return;

        const FrostbiteVec3 center = MakeVec3(item.position);
        FrostbiteVec3 half = {
            std::fabs(item.size[0]) * 0.5f,
            std::fabs(item.size[1]) * 0.5f,
            std::fabs(item.size[2]) * 0.5f
        };

        if (half.x < 0.0001f && half.y < 0.0001f && half.z < 0.0001f)
        {
            const float radius = item.radius > 0.0001f ? item.radius : 32.0f;
            half = { radius, radius, radius };
        }

        outMin = { center.x - half.x, center.y - half.y, center.z - half.z };
        outMax = { center.x + half.x, center.y + half.y, center.z + half.z };
    }

    bool ProjectActor(
        const FrostbiteMatrix4x4& matrix,
        const FrostbiteViewport& viewport,
        FrostbiteActorModelInfo& item,
        bool& outClipped)
    {
        outClipped = false;

        FrostbiteProjectedPoint center = {};
        if (!ProjectWithMatrix(matrix, viewport, MakeVec3(item.position), center))
            return false;

        item.screenPosition[0] = center.x;
        item.screenPosition[1] = center.y;
        item.screenDepth = center.depth;
        outClipped = center.clipped != 0;

        FrostbiteVec3 minWorld = {};
        FrostbiteVec3 maxWorld = {};
        BuildWorldBounds(item, minWorld, maxWorld);

        const FrostbiteVec3 corners[8] = {
            { minWorld.x, minWorld.y, minWorld.z },
            { maxWorld.x, minWorld.y, minWorld.z },
            { minWorld.x, maxWorld.y, minWorld.z },
            { maxWorld.x, maxWorld.y, minWorld.z },
            { minWorld.x, minWorld.y, maxWorld.z },
            { maxWorld.x, minWorld.y, maxWorld.z },
            { minWorld.x, maxWorld.y, maxWorld.z },
            { maxWorld.x, maxWorld.y, maxWorld.z }
        };

        bool anyCorner = false;
        bool anyUnclipped = center.clipped == 0;
        float minX = center.x;
        float minY = center.y;
        float maxX = center.x;
        float maxY = center.y;
        for (const FrostbiteVec3& corner : corners)
        {
            FrostbiteProjectedPoint point = {};
            if (!ProjectWithMatrix(matrix, viewport, corner, point))
                continue;

            anyCorner = true;
            anyUnclipped = anyUnclipped || point.clipped == 0;
            minX = (std::min)(minX, point.x);
            minY = (std::min)(minY, point.y);
            maxX = (std::max)(maxX, point.x);
            maxY = (std::max)(maxY, point.y);
        }

        if (anyCorner)
        {
            item.screenBoundsMin[0] = minX;
            item.screenBoundsMin[1] = minY;
            item.screenBoundsMax[0] = maxX;
            item.screenBoundsMax[1] = maxY;
        }
        else
        {
            item.screenBoundsMin[0] = center.x - 12.0f;
            item.screenBoundsMin[1] = center.y - 24.0f;
            item.screenBoundsMax[0] = center.x + 12.0f;
            item.screenBoundsMax[1] = center.y + 24.0f;
        }

        if (anyUnclipped)
            item.flags |= FrostbiteActorModel_HasScreenProjection;
        else
            item.flags &= ~FrostbiteActorModel_HasScreenProjection;

        outClipped = !anyUnclipped;
        return true;
    }

    void ProjectActorModelCache()
    {
        FrostbiteMatrix4x4 matrix = {};
        FrostbiteViewport viewport = {};
        {
            std::lock_guard lock(g_adapterMutex);
            matrix = g_viewProjection;
            viewport = g_viewport;
        }

        const std::uint32_t available = FrostbiteUniversal_GetActorModelCount();
        const std::uint32_t count = (std::min)(available, kMaxEntities);
        std::vector<FrostbiteActorModelInfo> actors(count);
        const std::uint32_t copied = FrostbiteUniversal_CopyActorModelList(actors.data(), count);
        actors.resize(copied);

        std::uint32_t projected = 0;
        std::uint32_t clipped = 0;

        if (IsValidMatrix(matrix) && IsValidViewport(viewport))
        {
            for (FrostbiteActorModelInfo& actor : actors)
            {
                bool actorClipped = false;
                if (!ProjectActor(matrix, viewport, actor, actorClipped))
                    continue;

                if (actorClipped)
                    ++clipped;
                else
                    ++projected;
            }

            FrostbiteUniversal_ClearActorModelList();
            for (const FrostbiteActorModelInfo& actor : actors)
                FrostbiteUniversal_AddActorModelInfo(&actor);
        }

        std::lock_guard lock(g_adapterMutex);
        g_timing.entityCount = copied;
        g_timing.projectedCount = projected;
        g_timing.clippedCount = clipped;
    }

    std::wstring DetectRendererBackend()
    {
        bool hasDxgi = false;
        bool hasD3D11 = false;
        bool hasD3D12 = false;
        bool hasVulkan = false;
        bool hasOpenGl = false;
        bool hasRenderCore = false;

        const std::uint32_t count = FrostbiteUniversal_GetModuleCount();
        for (std::uint32_t index = 0; index < count; ++index)
        {
            FrostbiteModuleInfo module = {};
            if (!FrostbiteUniversal_GetModuleInfo(index, &module))
                continue;

            const std::wstring name = module.name;
            hasDxgi = hasDxgi || Contains(name, L"dxgi");
            hasD3D11 = hasD3D11 || Contains(name, L"d3d11") || Contains(name, L"dx11");
            hasD3D12 = hasD3D12 || Contains(name, L"d3d12") || Contains(name, L"dx12");
            hasVulkan = hasVulkan || Contains(name, L"vulkan");
            hasOpenGl = hasOpenGl || Contains(name, L"opengl");
            hasRenderCore = hasRenderCore || (module.flags & FrostbiteModule_RenderCore2) != 0;
        }

        if (hasD3D12)
            return L"Direct3D12";
        if (hasD3D11 || (hasDxgi && hasRenderCore))
            return L"Direct3D11";
        if (hasVulkan)
            return L"Vulkan";
        if (hasOpenGl)
            return L"OpenGL";
        if (hasRenderCore)
            return L"Frostbite render core";
        return L"Unknown";
    }

    std::string ExtractJsonString(const std::string& object, const char* key)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return {};
        pos = object.find(':', pos);
        pos = object.find('"', pos);
        if (pos == std::string::npos)
            return {};
        ++pos;

        std::string result;
        bool escaped = false;
        for (; pos < object.size(); ++pos)
        {
            const char ch = object[pos];
            if (escaped)
            {
                result.push_back(ch);
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
            result.push_back(ch);
        }
        return result;
    }

    bool ExtractJsonNumber(const std::string& object, const char* key, float& outValue)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return false;
        pos = object.find(':', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        const float value = std::strtof(object.c_str() + pos + 1, &end);
        if (end == object.c_str() + pos + 1 || !IsFinite(value))
            return false;
        outValue = value;
        return true;
    }

    bool ExtractJsonUInt64(const std::string& object, const char* key, std::uint64_t& outValue)
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return false;
        pos = object.find(':', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        const unsigned long long value = std::strtoull(object.c_str() + pos + 1, &end, 10);
        if (end == object.c_str() + pos + 1)
            return false;
        outValue = static_cast<std::uint64_t>(value);
        return true;
    }

    bool ExtractJsonUInt32(const std::string& object, const char* key, std::uint32_t& outValue)
    {
        std::uint64_t value = 0;
        if (!ExtractJsonUInt64(object, key, value))
            return false;
        outValue = static_cast<std::uint32_t>((std::min)(value, static_cast<std::uint64_t>(UINT32_MAX)));
        return true;
    }

    bool ExtractJsonVec3(const std::string& object, const char* key, float outValue[3])
    {
        const std::string token = std::string("\"") + key + "\"";
        std::size_t pos = object.find(token);
        if (pos == std::string::npos)
            return false;
        pos = object.find('[', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        outValue[0] = std::strtof(object.c_str() + pos + 1, &end);
        if (!end || *end == '\0')
            return false;
        outValue[1] = std::strtof(end + 1, &end);
        if (!end || *end == '\0')
            return false;
        outValue[2] = std::strtof(end + 1, &end);
        return IsFinite(outValue[0]) && IsFinite(outValue[1]) && IsFinite(outValue[2]);
    }

    bool ExtractJsonMatrix(const std::string& json, FrostbiteMatrix4x4& outMatrix)
    {
        std::size_t pos = json.find("\"m\"");
        if (pos == std::string::npos)
            return false;
        pos = json.find('[', pos);
        if (pos == std::string::npos)
            return false;

        char* end = nullptr;
        const char* cursor = json.c_str() + pos + 1;
        for (float& value : outMatrix.m)
        {
            value = std::strtof(cursor, &end);
            if (end == cursor || !IsFinite(value))
                return false;
            cursor = end + 1;
        }

        std::uint32_t flags = 0;
        if (ExtractJsonUInt32(json, "matrixFlags", flags))
            outMatrix.flags = flags;
        return true;
    }

    std::vector<std::string> ExtractObjectArray(const std::string& json, const char* arrayName)
    {
        std::vector<std::string> objects;
        const std::string token = std::string("\"") + arrayName + "\"";
        std::size_t pos = json.find(token);
        if (pos == std::string::npos)
            return objects;
        pos = json.find('[', pos);
        if (pos == std::string::npos)
            return objects;

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t objectStart = std::string::npos;
        for (; pos < json.size(); ++pos)
        {
            const char ch = json[pos];
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
            {
                if (depth == 0)
                    objectStart = pos;
                ++depth;
            }
            else if (ch == '}')
            {
                --depth;
                if (depth == 0 && objectStart != std::string::npos)
                {
                    objects.push_back(json.substr(objectStart, pos - objectStart + 1));
                    objectStart = std::string::npos;
                }
            }
            else if (ch == ']' && depth == 0)
            {
                break;
            }
        }

        return objects;
    }
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_RegisterEntityProvider(FrostbiteActorModelProviderCallback callback, void* userData)
{
    FrostbiteUniversal_SetActorModelProvider(callback, userData);
    std::lock_guard lock(g_adapterMutex);
    g_entityProviderRegistered = callback != nullptr;
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_RegisterViewProjectionProvider(FrostbiteViewProjectionProviderCallback callback, void* userData)
{
    std::lock_guard lock(g_adapterMutex);
    g_matrixProvider = callback;
    g_matrixProviderUserData = userData;
    FrostbiteUniversal::Log::Write(callback
        ? L"View-projection provider registered"
        : L"View-projection provider cleared");
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_RegisterViewportProvider(FrostbiteViewportProviderCallback callback, void* userData)
{
    std::lock_guard lock(g_adapterMutex);
    g_viewportProvider = callback;
    g_viewportProviderUserData = userData;
    FrostbiteUniversal::Log::Write(callback
        ? L"Viewport provider registered"
        : L"Viewport provider cleared");
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_UpdateProviders()
{
    FrostbiteViewProjectionProviderCallback matrixProvider = nullptr;
    void* matrixUserData = nullptr;
    FrostbiteViewportProviderCallback viewportProvider = nullptr;
    void* viewportUserData = nullptr;
    {
        std::lock_guard lock(g_adapterMutex);
        matrixProvider = g_matrixProvider;
        matrixUserData = g_matrixProviderUserData;
        viewportProvider = g_viewportProvider;
        viewportUserData = g_viewportProviderUserData;
    }

    double entityMs = 0.0;
    double matrixMs = 0.0;
    double viewportMs = 0.0;

    if (viewportProvider)
    {
        FrostbiteViewport viewport = {};
        const LARGE_INTEGER start = NowCounter();
        const int result = viewportProvider(&viewport, viewportUserData);
        viewportMs = ElapsedMs(start, NowCounter());
        if (result && IsValidViewport(viewport))
            FrostbiteUniversal_SubmitViewport(&viewport);
    }

    if (matrixProvider)
    {
        FrostbiteMatrix4x4 matrix = {};
        const LARGE_INTEGER start = NowCounter();
        const int result = matrixProvider(&matrix, matrixUserData);
        matrixMs = ElapsedMs(start, NowCounter());
        if (result && IsValidMatrix(matrix))
            FrostbiteUniversal_SubmitViewProjection(&matrix);
    }

    const LARGE_INTEGER start = NowCounter();
    const int entityCount = FrostbiteUniversal_RefreshActorModelList();
    entityMs = ElapsedMs(start, NowCounter());
    ProjectActorModelCache();

    {
        std::lock_guard lock(g_adapterMutex);
        g_timing.entityProviderMs = entityMs;
        g_timing.matrixProviderMs = matrixMs;
        g_timing.viewportProviderMs = viewportMs;
        g_timing.entityCount = static_cast<std::uint32_t>(entityCount < 0 ? 0 : entityCount);
        ++g_timing.frameId;
    }

    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SubmitEntitySnapshots(const FrostbiteActorModelInfo* entities, std::uint32_t count)
{
    if (!entities && count != 0)
        return 0;

    const std::uint32_t clamped = (std::min)(count, kMaxEntities);
    FrostbiteUniversal_ClearActorModelList();
    for (std::uint32_t index = 0; index < clamped; ++index)
        FrostbiteUniversal_AddActorModelInfo(&entities[index]);

    ProjectActorModelCache();
    {
        std::lock_guard lock(g_adapterMutex);
        g_timing.entityCount = clamped;
        ++g_timing.frameId;
    }
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SubmitViewProjection(const FrostbiteMatrix4x4* matrix)
{
    if (!matrix || !IsValidMatrix(*matrix))
        return 0;

    {
        std::lock_guard lock(g_adapterMutex);
        g_viewProjection = *matrix;
        g_hasMatrix = true;
    }

    ProjectActorModelCache();
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_SubmitViewport(const FrostbiteViewport* viewport)
{
    if (!viewport || !IsValidViewport(*viewport))
        return 0;

    {
        std::lock_guard lock(g_adapterMutex);
        g_viewport = *viewport;
        g_hasViewport = true;
    }

    ProjectActorModelCache();
    return 1;
}

FROSTBITEUNIVERSAL_API std::uint32_t FrostbiteUniversal_GetEntityCount()
{
    return FrostbiteUniversal_GetActorModelCount();
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetEntitySnapshot(std::uint32_t index, FrostbiteActorModelInfo* outInfo)
{
    return FrostbiteUniversal_GetActorModelInfo(index, outInfo);
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetAdapterTiming(FrostbiteAdapterTiming* outTiming)
{
    if (!outTiming)
        return 0;

    std::lock_guard lock(g_adapterMutex);
    *outTiming = g_timing;
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_GetCapabilityInfo(FrostbiteCapabilityInfo* outInfo)
{
    if (!outInfo)
        return 0;

    FrostbiteRuntimeInfo runtime = {};
    FrostbiteUniversal_GetRuntimeInfo(&runtime);

    FrostbiteGeneratedSdkInfo sdkInfo = {};
    const bool sdkLoaded = FrostbiteUniversal_GetGeneratedSdkInfo(&sdkInfo) != 0;

    std::lock_guard lock(g_adapterMutex);
    *outInfo = {};
    outInfo->frostbiteDetected = FrostbiteUniversal_IsFrostbiteProcess();
    outInfo->dataDirectoryFound = (runtime.flags & FrostbiteRuntime_HasDataDirectory) ? 1 : 0;
    outInfo->initFsFound = (runtime.flags & FrostbiteRuntime_HasInitFs) ? 1 : 0;
    outInfo->layoutTocFound = (runtime.flags & FrostbiteRuntime_HasLayoutToc) ? 1 : 0;
    outInfo->tocArchivesFound = (runtime.flags & FrostbiteRuntime_HasTocArchives) ? 1 : 0;
    outInfo->casArchivesFound = (runtime.flags & FrostbiteRuntime_HasCasArchives) ? 1 : 0;
    outInfo->engineBuildInfoFound = (runtime.flags & FrostbiteRuntime_HasEngineBuildInfo) ? 1 : 0;
    outInfo->renderCoreFound = (runtime.flags & FrostbiteRuntime_HasRenderCore2) ? 1 : 0;
    outInfo->exportsFound = FrostbiteUniversal_GetExportCount() > 0 ? 1 : 0;
    outInfo->imguiAvailable = FrostbiteUniversal_HasSharedImGui();
    outInfo->overlayRunning = FrostbiteUniversal_OverlayIsRunning();
    outInfo->entityProviderRegistered = (g_entityProviderRegistered || FrostbiteUniversal_HasActorModelBridge()) ? 1 : 0;
    outInfo->viewProjectionProviderRegistered = g_matrixProvider ? 1 : 0;
    outInfo->viewportProviderRegistered = g_viewportProvider ? 1 : 0;
    outInfo->viewportValid = (g_hasViewport && IsValidViewport(g_viewport)) ? 1 : 0;
    outInfo->matrixValid = (g_hasMatrix && IsValidMatrix(g_viewProjection)) ? 1 : 0;
    outInfo->snapshotReady = FrostbiteUniversal_GetActorModelCount() > 0 ? 1 : 0;
    outInfo->generatedSdkLoaded = sdkLoaded ? 1 : 0;
    outInfo->sdkDumpRunning = FrostbiteUniversal_IsSdkDumpRunning();

    FrostbiteProjectedPoint point = {};
    const FrostbiteVec3 origin = { 0.0f, 0.0f, 0.0f };
    outInfo->w2sProjectionWorking = (g_hasMatrix && g_hasViewport && ProjectWithMatrix(g_viewProjection, g_viewport, origin, point)) ? 1 : 0;
    CopyWide(outInfo->rendererBackend, DetectRendererBackend());

    std::wstringstream details;
    details << L"modules " << runtime.moduleCount
            << L", frostbite modules " << runtime.frostbiteModuleCount
            << L", exports " << FrostbiteUniversal_GetExportCount()
            << L", entities " << FrostbiteUniversal_GetActorModelCount()
            << L", projected " << g_timing.projectedCount
            << L", clipped " << g_timing.clippedCount
            << L", sdk cache " << (sdkLoaded ? L"loaded" : L"not loaded");
    CopyWide(outInfo->details, details.str());
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_ProjectWorldToScreen(const FrostbiteVec3* world, FrostbiteProjectedPoint* outPoint)
{
    if (!world || !outPoint)
        return 0;

    std::lock_guard lock(g_adapterMutex);
    *outPoint = {};
    return (g_hasMatrix && g_hasViewport && ProjectWithMatrix(g_viewProjection, g_viewport, *world, *outPoint)) ? 1 : 0;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_WriteSnapshotJson(const wchar_t* path)
{
    if (!path || path[0] == L'\0')
        return 0;

    FrostbiteMatrix4x4 matrix = {};
    FrostbiteViewport viewport = {};
    FrostbiteAdapterTiming timing = {};
    {
        std::lock_guard lock(g_adapterMutex);
        matrix = g_viewProjection;
        viewport = g_viewport;
        timing = g_timing;
    }

    const std::uint32_t count = FrostbiteUniversal_GetActorModelCount();
    std::vector<FrostbiteActorModelInfo> actors(count);
    const std::uint32_t copied = FrostbiteUniversal_CopyActorModelList(actors.data(), count);
    actors.resize(copied);

    std::ofstream out(std::filesystem::path(path), std::ios::trunc | std::ios::binary);
    if (!out)
        return 0;

    out << "{\n";
    out << "  \"engine\": \"Frostbite\",\n";
    out << "  \"frameId\": " << timing.frameId << ",\n";
    out << "  \"viewport\": { \"x\": " << viewport.x << ", \"y\": " << viewport.y
        << ", \"width\": " << viewport.width << ", \"height\": " << viewport.height << " },\n";
    out << "  \"matrixFlags\": " << matrix.flags << ",\n";
    out << "  \"m\": [";
    for (std::size_t index = 0; index < 16; ++index)
    {
        if (index)
            out << ", ";
        out << matrix.m[index];
    }
    out << "],\n";
    out << "  \"timing\": { \"entityProviderMs\": " << timing.entityProviderMs
        << ", \"matrixProviderMs\": " << timing.matrixProviderMs
        << ", \"viewportProviderMs\": " << timing.viewportProviderMs
        << ", \"projected\": " << timing.projectedCount
        << ", \"clipped\": " << timing.clippedCount << " },\n";
    out << "  \"entities\": [\n";
    for (std::size_t index = 0; index < actors.size(); ++index)
    {
        const FrostbiteActorModelInfo& actor = actors[index];
        out << "    { \"id\": " << actor.id
            << ", \"actorName\": \"" << JsonEscape(actor.actorName)
            << "\", \"className\": \"" << JsonEscape(actor.className)
            << "\", \"modelName\": \"" << JsonEscape(actor.modelName)
            << "\", \"assetPath\": \"" << JsonEscape(actor.assetPath)
            << "\", \"position\": [" << actor.position[0] << ", " << actor.position[1] << ", " << actor.position[2]
            << "], \"boundsMin\": [" << actor.boundsMin[0] << ", " << actor.boundsMin[1] << ", " << actor.boundsMin[2]
            << "], \"boundsMax\": [" << actor.boundsMax[0] << ", " << actor.boundsMax[1] << ", " << actor.boundsMax[2]
            << "], \"size\": [" << actor.size[0] << ", " << actor.size[1] << ", " << actor.size[2]
            << "], \"radius\": " << actor.radius
            << ", \"flags\": " << actor.flags
            << ", \"likelyPlayerScore\": " << actor.likelyPlayerScore << " }";
        if (index + 1 < actors.size())
            out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return 1;
}

FROSTBITEUNIVERSAL_API int FrostbiteUniversal_LoadSnapshotJson(const wchar_t* path)
{
    if (!path || path[0] == L'\0')
        return 0;

    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in)
        return 0;

    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    FrostbiteViewport viewport = {};
    ExtractJsonNumber(json, "x", viewport.x);
    ExtractJsonNumber(json, "y", viewport.y);
    ExtractJsonNumber(json, "width", viewport.width);
    ExtractJsonNumber(json, "height", viewport.height);

    FrostbiteMatrix4x4 matrix = {};
    const bool hasMatrix = ExtractJsonMatrix(json, matrix);

    std::vector<FrostbiteActorModelInfo> actors;
    for (const std::string& object : ExtractObjectArray(json, "entities"))
    {
        FrostbiteActorModelInfo actor = {};
        ExtractJsonUInt64(object, "id", actor.id);
        CopyUtf8ToWide(ExtractJsonString(object, "actorName"), actor.actorName, static_cast<std::uint32_t>(std::size(actor.actorName)));
        CopyUtf8ToWide(ExtractJsonString(object, "className"), actor.className, static_cast<std::uint32_t>(std::size(actor.className)));
        CopyUtf8ToWide(ExtractJsonString(object, "modelName"), actor.modelName, static_cast<std::uint32_t>(std::size(actor.modelName)));
        CopyUtf8ToWide(ExtractJsonString(object, "assetPath"), actor.assetPath, static_cast<std::uint32_t>(std::size(actor.assetPath)));
        ExtractJsonVec3(object, "position", actor.position);
        ExtractJsonVec3(object, "boundsMin", actor.boundsMin);
        ExtractJsonVec3(object, "boundsMax", actor.boundsMax);
        ExtractJsonVec3(object, "size", actor.size);
        ExtractJsonNumber(object, "radius", actor.radius);
        std::uint32_t flags = 0;
        ExtractJsonUInt32(object, "flags", flags);
        actor.flags = flags;
        ExtractJsonNumber(object, "likelyPlayerScore", actor.likelyPlayerScore);
        actors.push_back(actor);
        if (actors.size() >= kMaxEntities)
            break;
    }

    if (IsValidViewport(viewport))
        FrostbiteUniversal_SubmitViewport(&viewport);
    if (hasMatrix && IsValidMatrix(matrix))
        FrostbiteUniversal_SubmitViewProjection(&matrix);
    FrostbiteUniversal_SubmitEntitySnapshots(actors.data(), static_cast<std::uint32_t>(actors.size()));
    return 1;
}

FROSTBITEUNIVERSAL_API void FrostbiteUniversal_PrintCurrentEntities()
{
    const std::uint32_t count = FrostbiteUniversal_GetActorModelCount();
    std::vector<FrostbiteActorModelInfo> actors(count);
    const std::uint32_t copied = FrostbiteUniversal_CopyActorModelList(actors.data(), count);
    actors.resize(copied);

    HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    for (const FrostbiteActorModelInfo& actor : actors)
    {
        std::ostringstream line;
        line << "[FrostbiteUniversal] Entity id=" << actor.id
             << " actor=\"" << WideToUtf8(actor.actorName)
             << "\" class=\"" << WideToUtf8(actor.className)
             << "\" model=\"" << WideToUtf8(actor.modelName)
             << "\" position=(" << actor.position[0] << ", " << actor.position[1] << ", " << actor.position[2] << ")"
             << " flags=0x" << std::hex << actor.flags << std::dec
             << " score=" << actor.likelyPlayerScore << "\n";
        const std::string text = line.str();
        ::OutputDebugStringA(text.c_str());
        if (output && output != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            if (!::WriteConsoleA(output, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr))
                ::WriteFile(output, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
    }
}
