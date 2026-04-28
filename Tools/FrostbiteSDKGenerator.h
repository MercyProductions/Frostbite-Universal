#pragma once

#include <cstdint>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#if defined(FROSTBITE_SDK_GENERATOR_EXPORTS)
#define FROSTBITE_SDK_GENERATOR_API extern "C" __declspec(dllexport)
#elif defined(FROSTBITE_SDK_GENERATOR_IMPORTS)
#define FROSTBITE_SDK_GENERATOR_API extern "C" __declspec(dllimport)
#else
#define FROSTBITE_SDK_GENERATOR_API extern "C"
#endif

// Opens or attaches a console so DLL callers can see live SDK generation output.
FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_OpenConsole();

// Enables or disables verbose progress printing for generator calls.
FROSTBITE_SDK_GENERATOR_API void FrostbiteSDKGenerator_SetVerbose(int verbose);

// Generates from the current process folder and writes to outputDir.
// If outputDir is null or empty, GeneratedSDK is created in the current working directory.
FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_GenerateDefault(const wchar_t* outputDir);

// Captures a live-process SDK snapshot from the current process only.
// This is the function the DLL runs automatically when injected.
// It enumerates loaded modules, PE exports/imports, DbgHelp/PDB symbols, MSVC RTTI/vtables,
// and optional owned-project introspection exports when available.
FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_GenerateInjectedSnapshot(const wchar_t* outputDir);

// Generates from an explicit list of game roots.
// Return codes match FrostbiteSDKGenerator.exe:
// 0 = success, 2 = no valid game roots, 3 = output folder error, 4 = write failure, 10 = unexpected exception.
FROSTBITE_SDK_GENERATOR_API int FrostbiteSDKGenerator_GenerateFromRoots(
    const wchar_t** gameRoots,
    std::uint32_t gameRootCount,
    const wchar_t* outputDir,
    int includeThirdParty,
    int includeAntiCheat,
    std::uint32_t maxExportsPerModule);

enum FrostbiteSDKValueType : std::uint32_t
{
    FrostbiteSDKValue_Unknown = 0,
    FrostbiteSDKValue_Bool = 1,
    FrostbiteSDKValue_Int32 = 2,
    FrostbiteSDKValue_UInt32 = 3,
    FrostbiteSDKValue_Int64 = 4,
    FrostbiteSDKValue_UInt64 = 5,
    FrostbiteSDKValue_Float = 6,
    FrostbiteSDKValue_Double = 7,
    FrostbiteSDKValue_String = 8,
    FrostbiteSDKValue_Vector2 = 9,
    FrostbiteSDKValue_Vector3 = 10,
    FrostbiteSDKValue_Vector4 = 11,
    FrostbiteSDKValue_Color = 12,
    FrostbiteSDKValue_Pointer = 13
};

enum FrostbiteSDKIntrospectionFlags : std::uint32_t
{
    FrostbiteSDKFlag_None = 0,
    FrostbiteSDKFlag_ReadOnly = 1u << 0,
    FrostbiteSDKFlag_RuntimeWritable = 1u << 1,
    FrostbiteSDKFlag_Render = 1u << 2,
    FrostbiteSDKFlag_Time = 1u << 3,
    FrostbiteSDKFlag_Physics = 1u << 4,
    FrostbiteSDKFlag_Debug = 1u << 5
};

struct FrostbiteSDKCVarInfo
{
    std::uint32_t size;
    wchar_t name[128];
    wchar_t category[64];
    wchar_t typeName[32];
    std::uint32_t valueType;
    wchar_t currentValue[128];
    wchar_t defaultValue[128];
    wchar_t description[256];
    std::uint64_t address;
    std::uint32_t flags;
};

struct FrostbiteSDKSystemInfo
{
    std::uint32_t size;
    wchar_t name[128];
    wchar_t kind[64];
    wchar_t module[128];
    wchar_t description[256];
    std::uint64_t address;
    std::uint32_t flags;
};

struct FrostbiteSDKEnvironmentInfo
{
    std::uint32_t size;
    wchar_t name[128];
    wchar_t system[128];
    wchar_t typeName[32];
    std::uint32_t valueType;
    wchar_t currentValue[128];
    wchar_t description[256];
    std::uint64_t address;
    std::uint32_t flags;
};

struct FrostbiteSDKFieldInfo
{
    std::uint32_t size;
    wchar_t name[128];
    wchar_t typeName[128];
    std::uint32_t offset;
    std::uint32_t sizeBytes;
    std::uint32_t flags;
};

struct FrostbiteSDKTypeInfo
{
    std::uint32_t size;
    wchar_t namespaceName[128];
    wchar_t name[128];
    wchar_t kind[64];
    std::uint32_t sizeBytes;
    std::uint32_t fieldCount;
    std::uint64_t address;
    std::uint32_t flags;
};

// Optional owned-project provider exports that the injected generator will consume when present:
//   std::uint32_t __stdcall FrostbiteSDK_GetCVarCount();
//   int __stdcall FrostbiteSDK_GetCVarInfo(std::uint32_t index, FrostbiteSDKCVarInfo* outInfo);
//   std::uint32_t __stdcall FrostbiteSDK_GetSystemCount();
//   int __stdcall FrostbiteSDK_GetSystemInfo(std::uint32_t index, FrostbiteSDKSystemInfo* outInfo);
//   std::uint32_t __stdcall FrostbiteSDK_GetEnvironmentCount();
//   int __stdcall FrostbiteSDK_GetEnvironmentInfo(std::uint32_t index, FrostbiteSDKEnvironmentInfo* outInfo);
//   std::uint32_t __stdcall FrostbiteSDK_GetTypeCount();
//   int __stdcall FrostbiteSDK_GetTypeInfo(std::uint32_t index, FrostbiteSDKTypeInfo* outInfo);
//   int __stdcall FrostbiteSDK_GetFieldInfo(std::uint32_t typeIndex, std::uint32_t fieldIndex, FrostbiteSDKFieldInfo* outInfo);
