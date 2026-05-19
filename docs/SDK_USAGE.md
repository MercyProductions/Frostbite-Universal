# SDK Usage And Import Guide

This guide explains how to generate an SDK-style dump, how to read the important reports, and how to import the generated headers into your own C++ project.

The generated SDK is read-only metadata. It is useful for research, documentation, tooling, labels, and diagnostics. Runtime addresses are snapshot-specific and should be treated as evidence, not as a stable public ABI.

## Feature Map

| Area | What The Toolkit Provides |
| --- | --- |
| SDK generation | Static folder scans, live injected snapshots, C++ headers, JSON, Markdown reports, dashboards, labels, SQLite SQL, and ReClass notes. |
| Candidate research | Strings, xrefs, RTTI, vtables, `.pdata` function bounds, call graphs, traces, system clusters, shader maps, asset references, and run diffs. |
| Universal runtime | ImGui diagnostics, module/export/catalog views, actor/model table, timescale, skybox/fog/debug tint controls, boxes, snaplines, FOV, and view-angle preview. |
| SDK bridge template | Visual Studio project showing how to map a generated SDK into Universal through `SdkBindings.cpp`. |

## 1. Build Or Stage The Tools

Build the solution:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Source\FrostbiteEngineTools.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Stage the local tools:

```powershell
New-Item -ItemType Directory -Force -Path ".\Tools" | Out-Null
Copy-Item ".\Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe" ".\Tools\FrostbiteSDKGenerator.exe" -Force
Copy-Item ".\Source\FrostbiteSDKGenerator\Include\FrostbiteSDKGenerator.h" ".\Tools\FrostbiteSDKGenerator.h" -Force
Copy-Item ".\Source\FrostbiteUniversal\build\x64\Release\FrostbiteUniversal.dll" ".\INJECT_THIS_FrostbiteUniversal.dll" -Force
Copy-Item ".\Source\FrostbiteUniversal\Include\FrostbiteUniversal.h" ".\Tools\FrostbiteUniversal.h" -Force
```

## 2. Generate A Folder-Only Static SDK

Use this when you want PE metadata, imports, exports, dependency graphs, and file triage without loading anything into a process.

```powershell
& ".\Tools\FrostbiteSDKGenerator.exe" `
  --game-root "C:\Path\To\YourOwnedGameOrResearchBuild" `
  --out ".\GeneratedSDK\MyStaticScan"
```

Useful static reports:

| Report | Purpose |
| --- | --- |
| `FrostbiteSDKManifest.json` | Static source of truth for files/modules. |
| `FrostbiteModules.md` | Human-readable module list. |
| `FrostbiteSDK.generated.h` | Header summary of static modules. |
| `SDK/StaticScan.hpp` | Reusable C++17 static scan metadata. |

## 3. Generate A Live Runtime SDK

Use this only with an owned/local process where you have permission to load the Universal debug DLL.

The injectable Universal tool is:

```text
INJECT_THIS_FrostbiteUniversal.dll
```

When loaded into your owned process, press F4, open the SDK tab, and choose `Run Full SDK Dump`. It writes:

```text
GeneratedSDK/Injected_<process>_<pid>/
```

If your host/plugin system should call the backend manually, call:

```cpp
extern "C" __declspec(dllimport)
int __stdcall FrostbiteUniversal_StartSdkDump(const wchar_t* outputDir);

FrostbiteUniversal_StartSdkDump(L".\\GeneratedSDK\\ManualSnapshot");
```

## 4. Read The Generated Reports

Start with these files:

1. `ResearchDashboard.md`
2. `ScanSummary.md`
3. `InterestingModules.md`
4. `HighConfidenceFunctions.md`
5. `TimeCandidates.md`
6. `EnvironmentCandidates.md`
7. `FunctionTraces/Index.md`
8. `Labels/CandidateLabels_IDA.py`
9. `Labels/CandidateLabels_Ghidra.py`
10. `Labels/CandidateLabels_BinaryNinja.py`

Important machine-readable files:

| File | Use |
| --- | --- |
| `FrostbiteInjectedProcess.json` | Loaded modules, exports, imports, symbols, RTTI, vtables. |
| `FrostbiteRuntimeIntrospection.json` | String/xref/function-candidate source of truth. |
| `Strings.json` | Discovered ASCII and UTF-16 strings. |
| `StringXrefs.json` | Read-only string references. |
| `CandidateCallGraph.json` | Caller/callee graph evidence. |
| `WatchReport.json` | Read-only sampled value classification. |
| `AssetReferences.json` | Local metadata/string references for assets and descriptors. |

## 5. Import The SDK Into Your Own C++ Project

The reusable SDK is generated here:

```text
GeneratedSDK/Injected_<process>_<pid>/SDK/
```

It normally contains:

```text
SDK.hpp
LiveProcess.hpp
RuntimeIntrospection.hpp
StaticScan.hpp
Confirmed.generated.h
Candidates.generated.h
Noise.generated.h
```

### Option A: Copy The Generated SDK Folder

Copy:

```text
GeneratedSDK/Injected_<process>_<pid>/SDK/
```

into your own project, for example:

```text
YourProject/external/FrostbiteGeneratedSDK/
```

Then include:

```cpp
#include "SDK.hpp"
```

Set your include directory to:

```text
YourProject/external/FrostbiteGeneratedSDK
```

### Option B: Reference The SDK In Place

Add this include directory to your project:

```text
Frostbite/GeneratedSDK/Injected_<process>_<pid>/SDK
```

Then include:

```cpp
#include "SDK.hpp"
```

If you instead add the dump root as an include directory:

```text
Frostbite/GeneratedSDK/Injected_<process>_<pid>
```

include it as:

```cpp
#include "SDK/SDK.hpp"
```

## 6. Visual Studio Setup

In your consuming project:

1. Open project properties.
2. Set `Configuration` to `All Configurations`.
3. Set `Platform` to `x64`.
4. Go to `C/C++ > General > Additional Include Directories`.
5. Add the generated `SDK` folder path.
6. Go to `C/C++ > Language > C++ Language Standard`.
7. Select `ISO C++17 Standard` or newer.
8. No linker input is required for the generated SDK headers.

## 7. Minimal Usage Example

```cpp
#include <cstddef>
#include <iostream>
#include "SDK.hpp"

int main()
{
    std::cout << "Loaded modules:\n";
    for (const auto& module : FrostbiteSDK::Live::Modules)
    {
        std::cout << "  " << module.name
                  << " base=0x" << std::hex << module.baseAddress
                  << " exports=" << std::dec << module.exportCount
                  << "\n";
    }

    std::cout << "\nRuntime candidates:\n";
    for (const auto& candidate : FrostbiteSDK::Runtime::FunctionCandidates)
    {
        std::cout << "  score=" << candidate.score
                  << " module=" << candidate.module
                  << " category=" << candidate.primaryCategory
                  << " address=0x" << std::hex << candidate.functionAddress
                  << std::dec << "\n";
    }
}
```

## 8. Using IDA, Ghidra, Or Binary Ninja Labels

Open the generated `Labels` folder:

```text
GeneratedSDK/Injected_<process>_<pid>/Labels/
```

Use the script that matches your tool:

| Tool | File |
| --- | --- |
| IDA | `CandidateLabels_IDA.py` |
| Ghidra | `CandidateLabels_Ghidra.py` |
| Binary Ninja | `CandidateLabels_BinaryNinja.py` |

These scripts label candidate functions only. They do not patch or modify the target binary.

## 9. Query Existing Dumps

Search generated reports:

```powershell
& ".\Tools\FrostbiteSDKGenerator.exe" `
  --out ".\GeneratedSDK\Injected_<process>_<pid>" `
  --query TimeScale
```

Trace a candidate address:

```powershell
& ".\Tools\FrostbiteSDKGenerator.exe" `
  --out ".\GeneratedSDK\Injected_<process>_<pid>" `
  --trace 0x14048a430
```

Diff two reports:

```powershell
& ".\Tools\FrostbiteSDKGenerator.exe" --diff dump_menu.json dump_ingame.json
```

## 10. Using The Owned-Project Runtime Bridge

`FrostbiteUniversal` can communicate with your own game/project through explicit exports. This is opt-in.

The easiest starting point is the full SDK bridge template:

```text
Templates/FrostbiteUniversalSdkBridge/
```

Read:

```text
Templates/FrostbiteUniversalSdkBridge/README.md
```

That template already exports the default `FrostbiteGame_*` functions and keeps your project-specific SDK mapping in `SdkBindings.cpp`.

Include:

```cpp
#include "FrostbiteUniversal.h"
```

Export the default bridge functions from your owned process or bridge DLL:

```cpp
extern "C" __declspec(dllexport)
std::uint32_t __stdcall FrostbiteGame_GetActorModelCount();

extern "C" __declspec(dllexport)
int __stdcall FrostbiteGame_GetActorModelInfo(std::uint32_t index, FrostbiteActorModelInfo* outInfo);

extern "C" __declspec(dllexport)
void __stdcall FrostbiteGame_SetTimescale(float timescale);

extern "C" __declspec(dllexport)
float __stdcall FrostbiteGame_GetTimescale();
```

Optional feature-state exports are declared in:

```text
Source/FrostbiteUniversal/Include/FrostbiteUniversal.h
```

If you use different names, map them in:

```text
FrostbiteUniversal_Bridge.ini
```

Example:

```ini
[BridgeExports]
ModuleName=
GetActorModelCount=MyGame_GetActorModelCount
GetActorModelInfo=MyGame_GetActorModelInfo
SetTimescale=MyGame_SetTimescale
GetTimescale=MyGame_GetTimescale
ApplyFeatures=MyGame_ApplyUniversalFeatures
GetFeatureState=MyGame_GetUniversalFeatureState
SetSkyboxTint=MyGame_SetSkyboxTint
SetDebugMaterialTint=MyGame_SetDebugMaterialTint
SetFogTint=MyGame_SetFogTint
```

### Using An SDK You Already Generated

Point your project or bridge DLL at the generated SDK include folder:

```text
GeneratedSDK/Injected_<process>_<pid>/SDK/
```

Then edit:

```text
Templates/FrostbiteUniversalSdkBridge/src/SdkBindings.cpp
```

Replace the TODO sections with your SDK calls:

| Adapter Function | Map To Your SDK |
| --- | --- |
| `GetActorModelCount` | World/entity/component list count |
| `GetActorModel` | Actor name, class name, model name, asset path, position, flags |
| `SetTimescale` / `GetTimescale` | Time, tick, simulation, or world time system |
| `ApplySkyboxTint` | Visual environment, sky, or lighting system |
| `ApplyModelDebugTint` | Owned debug/editor material visualization |
| `ApplyFogTint` | Fog, atmosphere, or environment settings |
| `ReadFeatureState` | Current live values, if your SDK exposes them |

If your generated SDK is metadata-only, use it as a map for names, offsets, and candidate functions, then call your own safe project wrappers from `SdkBindings.cpp`.

The template also includes a working fallback backend. Before a real SDK is connected, `Refresh Actors/Models` shows sample rows for:

- `Sdk::TimeSystem`
- `Sdk::VisualEnvironment`
- `Sdk::FogComponent`

Changing timescale updates a derived `scaledFrameDelta = 1/60 * timescale`, and skybox/fog controls update the sample state. The template actor/model cache stores world position, rotation, scale, bounds min/max, computed size, radius, and optional 2D screen projection in a `std::vector<ActorModel>`.

For a real SDK, replace `RefreshActorModelCacheLocked()` with your world/entity/model traversal. Universal will display position, size, radius, debug boxes, and snaplines in the Project tab after `Refresh Actors/Models`.

The template also includes FOV and view-angle preview fields. Wire those to your owned debug camera or editor camera callbacks when you want the Project tab to demonstrate camera/FOV integration.

Most users should build the template through Visual Studio:

```text
Templates/FrostbiteUniversalSdkBridge/FrostbiteUniversalSdkBridge.sln
```

When your real SDK is connected, set `AEGIS_TEMPLATE_ENABLE_SAMPLE_DATA=0` in the Visual Studio preprocessor definitions, or build the optional CMake target with `-DAEGIS_TEMPLATE_ENABLE_SAMPLE_DATA=OFF`.

## 11. What Not To Commit

Do not commit:

- `GeneratedSDK/`
- `Logs/`
- built `.dll`, `.exe`, `.pdb`, `.lib`, `.exp`, `.ilk` files
- private game dumps
- proprietary game files
- crash dumps

Use GitHub Releases for built binaries if you want to distribute a compiled snapshot.
