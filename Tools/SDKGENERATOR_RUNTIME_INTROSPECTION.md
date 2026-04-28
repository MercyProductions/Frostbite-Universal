# Frostbite SDK Generator Runtime Introspection

This SDK generator now has three runtime research layers:

1. Passive metadata extraction from the process that loaded the DLL.
2. Discovery-based runtime introspection from strings, xrefs, nearby calls, nearby floats, and math-op proximity.
3. Candidate validation/mapping from PE `.pdata` function bounds, local caller/callee graph edges, runtime read-only value snapshots, asset-reference scans, and run-to-run diffs.

It intentionally does not implement arbitrary memory writes, stealth scanning, anti-debug bypass logic, or anti-cheat bypass logic.

## Current Feature Summary

- Static and injected SDK generation.
- Loaded module, import, export, PDB, symbol, section, RTTI, vtable, and virtual slot reports.
- String-first runtime discovery with ASCII/UTF-16 strings, categories, xrefs, nearby calls, nearby floats, and candidate scoring.
- Function validation with PE `.pdata` bounds, caller/callee graph edges, annotated traces, float classification, and tiered candidate reports.
- System clustering for Time, Tick, Physics, Visual Environment, Sky/Lighting, Shader Pipeline, Entity/Component, Audio, and Asset Loading.
- EBX/TOC/CAS-adjacent metadata string/reference scans without decrypting or unpacking protected content.
- SDK exports for C++ headers, JSON, Markdown, HTML dashboard, SQLite SQL, IDA, Ghidra, Binary Ninja, and ReClass notes.

## Output Files

Injected DLL output folder:

```text
GeneratedSDK\Injected_<process>_<pid>
```

Important files:

```text
FrostbiteInjectedProcess.json
FrostbiteInjectedSymbols.md
FrostbiteInjectedSDK.generated.h
FrostbiteRuntimeIntrospection.json
FrostbiteRuntimeIntrospection.md
Strings.json
StringXrefs.json
TimeCandidates.md
EnvironmentCandidates.md
HighConfidenceFunctions.md
ResearchDashboard.md
ResearchDashboard.html
CandidateCallGraph.json
RuntimeValueWatchers.md
WatchReport.md
WatchReport.json
SystemClusters.md
RunDiff.md
AssetReferences.json
AssetReferences.md
AssetReferenceGraph.md
BundleMap.md
TypeDescriptorReport.md
EnumTables.md
NameDecomposition.md
ShaderPipelineMap.md
Research.sqlite.sql
ReClassNotes.md
FunctionTraces\Index.md
Labels\CandidateLabels_IDA.py
Labels\CandidateLabels_Ghidra.py
Labels\CandidateLabels_BinaryNinja.py
SDK\Confirmed.generated.h
SDK\Candidates.generated.h
SDK\Noise.generated.h
FrostbiteTypes.md
InterestingModules.md
ScanSummary.md
SDK.generated.h
FrostbiteTypes.generated.h
SDK\SDK.hpp
SDK\LiveProcess.hpp
SDK\RuntimeIntrospection.hpp
SDK\StaticScan.hpp
```

`FrostbiteInjectedProcess.json` contains modules, imports, exports, PDB/DbgHelp symbols, MSVC RTTI classes, vtables, and virtual slots.

`FrostbiteRuntimeIntrospection.json` contains discovery-based function candidates. It does not depend on provider exports or a known CVar registry.

`Strings.json` and `StringXrefs.json` contain read-only string discovery from game/Frostbite-owned modules and safe RIP-relative xrefs from executable sections.

`ResearchDashboard.md` is the best first file. It summarizes the highest-scoring Time, Environment, Rendering, Physics, Entity, and Audio targets, plus the next recommended manual reversing steps.

`TimeCandidates.md`, `EnvironmentCandidates.md`, and `HighConfidenceFunctions.md` are candidate reports. They do not claim to have found real CVars or environment managers.

`FunctionTraces` contains focused trace reports for the strongest functions. Each page shows the bounded function range, related strings, xref addresses, nearby float/data evidence, callers, and callees.

`WatchReport.md`, `WatchReport.json`, and `RuntimeValueWatchers.md` sample suspected read-only float/global addresses over time. They classify values as static, frame timer, counter, state value, config constant, or unknown. Pause/menu/level sensitivity is determined by comparing multiple runs with `RunDiff.md` or `--diff`.

`Labels` contains IDA, Ghidra, and Binary Ninja label scripts for the ranked candidate functions.

`AssetReferences.md`, `AssetReferenceGraph.md`, `BundleMap.md`, `TypeDescriptorReport.md`, and `AssetReferences.json` sample EBX/TOC/CAS-adjacent files for type descriptors, asset paths, GUIDs, bundle names, level names, and component names. It does not decrypt, decompress, unpack, or modify assets.

`EnumTables.md` and `NameDecomposition.md` break apart Frostbite-like names such as `EcsVisualEnvironmentBlendMode_Add` into prefix/system, kind, value, and category.

`ShaderPipelineMap.md` groups render strings into likely GI/surfel, reflections, irradiance, shadows, fog, atmosphere, exposure, material, transparency, emissive, and postprocess passes.

## Architecture

### Phase 1: Loaded Module Capture

The DLL snapshots only the process it is loaded into:

1. `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId())`
2. Skip known anti-cheat modules.
3. Parse PE exports/imports from each module on disk.
4. Load DbgHelp/PDB symbols for each module when available.
5. Run MSVC RTTI/vtable reconstruction against loaded image sections.

### Phase 2: Symbol And PDB Harvesting

DbgHelp is initialized with:

```cpp
SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
SymInitialize(GetCurrentProcess(), nullptr, FALSE);
```

For each module:

```cpp
DWORD64 base = SymLoadModuleExW(process, nullptr, modulePath, nullptr, moduleBase, imageSize, nullptr, 0);
SymEnumSymbols(process, base, nullptr, EnumSymbolCallback, &context);
```

The generator records:

```cpp
struct SymbolRecord
{
    std::string name;
    std::string undecoratedName;
    std::string signature;
    std::uint64_t address;
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t tag;
    bool isFunction;
    bool hasSignature;
};
```

### Phase 3: MSVC RTTI And VTable Reconstruction

The current implementation targets normal MSVC x64 RTTI:

```cpp
struct CompleteObjectLocator64
{
    std::uint32_t signature;       // usually 1 on x64
    std::uint32_t offset;
    std::uint32_t cdOffset;
    std::int32_t typeDescriptorRva;
    std::int32_t classDescriptorRva;
    std::int32_t selfRva;
};

struct ClassHierarchyDescriptor64
{
    std::uint32_t signature;
    std::uint32_t attributes;
    std::uint32_t numBaseClasses;
    std::int32_t baseClassArrayRva;
};

struct BaseClassDescriptor64
{
    std::int32_t typeDescriptorRva;
    std::uint32_t numContainedBases;
    std::int32_t mdisp;
    std::int32_t pdisp;
    std::int32_t vdisp;
    std::uint32_t attributes;
    std::int32_t classHierarchyDescriptorRva;
};
```

Algorithm:

1. Enumerate readable non-discardable PE sections as data ranges.
2. Enumerate executable PE sections as code ranges.
3. Walk data ranges pointer-by-pointer.
4. Treat each pointer as a possible `CompleteObjectLocator64`.
5. Validate:
   - Pointer is inside a readable module section.
   - `signature == 1`.
   - `base + selfRva == CompleteObjectLocator address`.
   - `typeDescriptorRva` and `classDescriptorRva` resolve inside the module.
   - TypeDescriptor name looks like MSVC class/struct RTTI, such as `.?AVGameWorld@@`.
6. Treat the address immediately after the COL pointer as a vtable start.
7. Count contiguous vtable entries while each slot points into executable code.
8. Resolve each virtual slot to a DbgHelp symbol when possible.
9. Emit the class record and vtable slots.

Generated shape:

```cpp
struct ClassInfo
{
    const char* ownerModule;
    const char* name;
    const char* decoratedName;
    const char* bases;
    std::uint64_t typeDescriptor;
    std::uint64_t completeObjectLocator;
    std::uint64_t classHierarchyDescriptor;
    std::uint64_t vtable;
    std::uint32_t objectOffset;
    std::uint32_t hierarchyAttributes;
    std::uint32_t virtualFunctionCount;
};
```

RTTI can recover class names, inheritance names, and virtual function slots. It cannot recover non-virtual field offsets unless your build exposes PDB field records or the provider reflection ABI below.

### Phase 4: Candidate Validation And Mapping

The runtime discovery layer no longer stops at "string near code." It now validates candidates with:

1. PE `.pdata` unwind records to recover bounded x64 function ranges.
2. Direct `E8 rel32` call scanning inside game/Frostbite-owned executable sections.
3. Candidate caller/callee graphs normalized back to `.pdata` function starts when possible.
4. Tiering:
   - `Confirmed`: high score, `.pdata` bounds, repeated xrefs, and graph or float/math evidence.
   - `Strong Candidate`: useful bounded function with xref evidence.
   - `Weak Candidate`: interesting evidence but insufficient validation.
   - `Noise`: fallback or low-confidence records.
5. System clustering for Time, Environment, Rendering, Physics, Entity, and Audio.
6. Float constant classification for values commonly seen in time/render/math code: frame deltas, tick rates, degree/radian conversion, identity and multiplier constants.

All of this is read-only. The dumper does not patch code, write process memory, bypass anti-cheat, or hide scanning activity.

## Legacy Optional Metadata ABI

The discovery reports do not require this ABI and the injected runtime introspection path no longer calls it. It is kept only as an optional way for an owned test project to export exact metadata if you choose to do that later.

Include:

```cpp
#include "FrostbiteSDKGenerator.h"
```

The dumper automatically searches every loaded module for these exports.

### CVars

Exports:

```cpp
extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteSDK_GetCVarCount();

extern "C" __declspec(dllexport)
int __stdcall FrostbiteSDK_GetCVarInfo(std::uint32_t index, FrostbiteSDKCVarInfo* outInfo);
```

Example:

```cpp
extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteSDK_GetCVarCount()
{
    return static_cast<std::uint32_t>(g_CVars.size());
}

extern "C" __declspec(dllexport)
int __stdcall FrostbiteSDK_GetCVarInfo(std::uint32_t index, FrostbiteSDKCVarInfo* outInfo)
{
    if (!outInfo || index >= g_CVars.size())
        return 0;

    const auto& cvar = g_CVars[index];
    ZeroMemory(outInfo, sizeof(*outInfo));
    outInfo->size = sizeof(*outInfo);
    wcscpy_s(outInfo->name, cvar.Name.c_str());
    wcscpy_s(outInfo->category, cvar.Category.c_str());
    wcscpy_s(outInfo->typeName, L"float");
    outInfo->valueType = FrostbiteSDKValue_Float;
    swprintf_s(outInfo->currentValue, L"%.4f", cvar.CurrentFloat);
    swprintf_s(outInfo->defaultValue, L"%.4f", cvar.DefaultFloat);
    wcscpy_s(outInfo->description, cvar.Description.c_str());
    outInfo->address = reinterpret_cast<std::uint64_t>(&cvar.CurrentFloat);
    outInfo->flags = FrostbiteSDKFlag_RuntimeWritable | FrostbiteSDKFlag_Time;
    return 1;
}
```

Good CVar categories:

```text
time
render
physics
camera
debug
world
```

Useful names to expose from your own project:

```text
time.timescale
time.tickrate
time.deltaTime
camera.fov
render.skyboxIntensity
render.fogDensity
physics.gravity
```

### Systems

Exports:

```cpp
extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteSDK_GetSystemCount();

extern "C" __declspec(dllexport)
int __stdcall FrostbiteSDK_GetSystemInfo(std::uint32_t index, FrostbiteSDKSystemInfo* outInfo);
```

Use this for owned project systems such as:

```text
GameWorld
ClientGameContext
RenderSystem
EntityManager
LevelSystem
SceneSystem
EnvironmentManager
TimeSystem
```

### Render And Environment Values

Exports:

```cpp
extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteSDK_GetEnvironmentCount();

extern "C" __declspec(dllexport)
int __stdcall FrostbiteSDK_GetEnvironmentInfo(std::uint32_t index, FrostbiteSDKEnvironmentInfo* outInfo);
```

Use this for values like:

```text
skybox.texture
skybox.tint
skybox.intensity
lighting.sunDirection
lighting.sunColor
hdr.exposure
fog.color
fog.density
atmosphere.heightFog
```

### Reflection Type And Field Layouts

Exports:

```cpp
extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteSDK_GetTypeCount();

extern "C" __declspec(dllexport)
int __stdcall FrostbiteSDK_GetTypeInfo(std::uint32_t index, FrostbiteSDKTypeInfo* outInfo);

extern "C" __declspec(dllexport)
int __stdcall FrostbiteSDK_GetFieldInfo(
    std::uint32_t typeIndex,
    std::uint32_t fieldIndex,
    FrostbiteSDKFieldInfo* outInfo);
```

Example field output:

```cpp
outInfo->offset = offsetof(MyGame::TimeSystem, m_timescale);
outInfo->sizeBytes = sizeof(float);
wcscpy_s(outInfo->name, L"m_timescale");
wcscpy_s(outInfo->typeName, L"float");
```

This is the cleanest way to generate field offsets. RTTI gives inheritance and vtables, but not reliable data-member offsets.

## Stability Rules

The discovery engine uses guarded read-only memory reads:

```cpp
__try
{
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}
__except (EXCEPTION_EXECUTE_HANDLER)
{
    return false;
}
```

No process memory is written.

## Recommended Build Flow

1. Build your game with RTTI enabled when possible.
2. Keep PDB files next to your binaries when you want richer function names and field/type symbol data.
3. Inject:

```text
Tools\INJECT_THIS_FOR_SDK_DUMP_FrostbiteSDKGenerator.dll
```

4. Watch the console output.
5. Open:

```text
GeneratedSDK\Injected_<process>_<pid>\ResearchDashboard.md
GeneratedSDK\Injected_<process>_<pid>\FrostbiteRuntimeIntrospection.md
GeneratedSDK\Injected_<process>_<pid>\TimeCandidates.md
GeneratedSDK\Injected_<process>_<pid>\EnvironmentCandidates.md
GeneratedSDK\Injected_<process>_<pid>\HighConfidenceFunctions.md
GeneratedSDK\Injected_<process>_<pid>\FunctionTraces\Index.md
GeneratedSDK\Injected_<process>_<pid>\RuntimeValueWatchers.md
GeneratedSDK\Injected_<process>_<pid>\WatchReport.md
GeneratedSDK\Injected_<process>_<pid>\EnumTables.md
GeneratedSDK\Injected_<process>_<pid>\ShaderPipelineMap.md
GeneratedSDK\Injected_<process>_<pid>\TypeDescriptorReport.md
```

6. Reuse:

```cpp
#include "SDK/SDK.hpp"
```

7. Query generated reports from the CLI:

```powershell
FrostbiteSDKGenerator.exe --out "GeneratedSDK\Injected_<process>_<pid>" --query TimeScale
FrostbiteSDKGenerator.exe --out "GeneratedSDK\Injected_<process>_<pid>" --query VisualEnvironment
FrostbiteSDKGenerator.exe --out "GeneratedSDK\Injected_<process>_<pid>" --trace 0x14048a430
FrostbiteSDKGenerator.exe --diff dump_menu.json dump_ingame.json
FrostbiteSDKGenerator.exe --watch watchlist.json
```

## What This Does Not Do

It does not:

- Patch game code.
- Write arbitrary memory.
- Bypass anti-debug or anti-cheat systems.
- Guess hidden Frostbite singleton addresses in stripped third-party games.
- Treat a pattern scan hit as a safe typed pointer.

It does:

- Classify modules as `GameSpecific`, `FrostbiteLikely`, `ThirdParty`, `WindowsSystem`, or `Unknown`.
- Scan readable sections in game/Frostbite-owned modules for relevant strings.
- Categorize strings as `Time`, `Rendering`, `Environment`, `Entity`, `Physics`, `Audio`, `UI`, `Debug`, `Console/CVar`, or `Asset/Resource`.
- Scan executable sections for RIP-relative references to interesting strings.
- Recover function boundaries from PE `.pdata` unwind information.
- Build a local direct caller/callee graph for high-confidence candidates.
- Classify float constants that often indicate frame delta, tick rate, degree/radian conversion, and multiplier math.
- Export candidate labels for IDA, Ghidra, and Binary Ninja.
- Diff candidate addresses against the previous injected run.
- Sample EBX/TOC/CAS-adjacent files for type descriptors, asset paths, GUIDs, bundle names, and component names.
- Detect enum/lookup-table string groups and decompose Frostbite-style names.
- Generate Markdown, HTML, SQL import, ReClass notes, and tiered C++ headers.
- Score findings from 0-100 so time/render/environment leads rise above Windows/Steam/NVIDIA/CRT noise.
- Write focused reports that help decide where to reverse next.

For an owned project, use the ranked discovery reports first. If you later want exact names/field layouts from your own engine code, the legacy metadata ABI can still provide that, but the candidate engine does not depend on it.
