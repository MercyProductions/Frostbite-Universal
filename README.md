# Frostbite-Universal

Frostbite-Universal is a Windows C++17 workspace for owned-process Frostbite research, local debugging, SDK-style metadata generation, and ImGui-based project diagnostics.

The repository contains two main Visual Studio projects plus a reusable SDK bridge template:

| Project | Output | Purpose |
| --- | --- | --- |
| `FrostbiteSDKGenerator` | `FrostbiteSDKGenerator.exe` | Standalone read-only SDK/report dumper for owned/local process folders. |
| `FrostbiteUniversal` | `FrostbiteUniversal.dll` | Universal ImGui debug tool. It embeds the SDK generator backend so the in-game SDK tab can run the full dumper and write the same generated reports. |

The `FrostbiteUniversalSdkBridge` template is separate from the main solution and shows how to wire an existing generated SDK into Universal through clean `FrostbiteGame_*` exports.

This project is intended for local research, modding documentation, debugging, and owned-project analysis only. It does not include anti-cheat bypasses, stealth/evasion logic, protected-file decryption, or memory patching guidance.

## Feature Overview

### FrostbiteUniversal

- Self-hosted ImGui overlay with F4 toggle.
- Runtime process/module/export/catalog reporting.
- Owned-project bridge through explicit exported functions.
- Timescale, skybox tint, fog tint, debug material tint, and wireframe/debug flags.
- Actor/model table with name, class, asset path, position, size, radius, flags, and optional screen projection.
- Template debug drawing for model boxes and snaplines.
- Standard adapter diagnostics: `FrostbiteUniversal_RegisterEntityProvider`, `FrostbiteUniversal_RegisterViewProjectionProvider`, `FrostbiteUniversal_RegisterViewportProvider`, provider timing, W2S projection, JSON frame snapshots, and offline replay through `FrostbiteUniversal_LoadSnapshotJson`.
- FOV override and view-angle preview fields for owned debug/editor cameras.
- Local logs and reports under ignored `Logs/` output.

### FrostbiteSDKGenerator

- Standalone EXE mode and embedded Universal-backend mode.
- Static folder scan and live injected snapshot modes.
- PE module/import/export analysis.
- String discovery, UTF-16/ASCII scanning, xref discovery, RTTI/vtable cleanup, `.pdata` function bounds, candidate scoring, call graph output, function traces, and system clustering.
- Candidate reports for time/tick, visual environment, rendering, physics, entity, audio, assets, shader pipelines, EBX/TOC/CAS-adjacent references, enum/table names, and noise filtering.
- Export targets for C++ headers, JSON, Markdown, HTML dashboard, SQLite SQL, IDA, Ghidra, Binary Ninja, and ReClass notes.

### SDK Bridge Template

- Visual Studio `.sln` first; CMake is optional.
- `SdkBindings.cpp` is the only file most users need to customize.
- Shows a clean `std::vector<ActorModel>` cache for actors/models.
- Demonstrates world position, rotation, scale, bounds min/max, computed size/radius, screen projection, boxes, snaplines, FOV, view-angle preview, and feature callbacks.

## Quick Start

1. Clone with submodules:

   ```powershell
   git clone --recurse-submodules <your-repo-url> Frostbite
   cd Frostbite
   ```

2. Install prerequisites:

   - Windows 10/11 x64
   - Visual Studio 2022 with "Desktop development with C++"
   - Windows SDK
   - PowerShell 5+ or PowerShell 7+
   - Optional: `dumpbin.exe` from Visual Studio Developer tools

3. Build everything:

   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
     ".\Source\FrostbiteEngineTools.sln" `
     /m /p:Configuration=Release /p:Platform=x64 /v:minimal

   & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
     ".\Templates\FrostbiteUniversalSdkBridge\FrostbiteUniversalSdkBridge.sln" `
     /m /p:Configuration=Release /p:Platform=x64 /v:minimal
   ```

4. Stage local binaries if you want the same folder layout used by the docs:

   ```powershell
   New-Item -ItemType Directory -Force -Path ".\Tools" | Out-Null
   Copy-Item ".\Source\FrostbiteUniversal\build\x64\Release\FrostbiteUniversal.dll" ".\INJECT_THIS_FrostbiteUniversal.dll" -Force
   Copy-Item ".\Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe" ".\Tools\FrostbiteSDKGenerator.exe" -Force
  Copy-Item ".\Source\FrostbiteSDKGenerator\Include\FrostbiteSDKGenerator.h" ".\Tools\FrostbiteSDKGenerator.h" -Force
  Copy-Item ".\Source\FrostbiteUniversal\Include\FrostbiteUniversal.h" ".\Tools\FrostbiteUniversal.h" -Force
   ```

5. Read the SDK guide:

   - [SDK usage and import guide](docs/SDK_USAGE.md)
   - [Build guide](docs/BUILDING.md)
   - [GitHub release checklist](docs/GITHUB_RELEASE_CHECKLIST.md)
   - [Safety and scope](docs/SAFETY_AND_SCOPE.md)

6. Verify the DLL and required exports:

   ```powershell
   .\Scripts\VerifyBuild.ps1 -Configuration Release
   .\Scripts\VerifyBuild.ps1 -Configuration Debug
   ```

## Static Game-Root Scans

`FrostbiteSDKGenerator` already supports repeated `--game-root` arguments. The wrapper script keeps those runs repeatable:

```powershell
.\Scripts\Invoke-FrostbiteGameRootScan.ps1 -GameRoot "D:\Games\Some Frostbite Game"
```

Use `-IncludeThirdParty` when you need dependency modules in the static report. `-IncludeAntiCheat` is intentionally opt-in and should only be used for owned/offline research scopes.

## Repository Layout

```text
.
|-- Source/
|   |-- FrostbiteEngineTools.sln
|   |-- FrostbiteUniversal/
|   |-- FrostbiteSDKGenerator/
|   |-- imgui/
|   `-- ThirdParty/minhook/
|-- Tools/                  # Local staged tools; binaries are ignored by git
|-- Templates/
|   `-- FrostbiteUniversalSdkBridge/
|-- GeneratedSDK/           # Local dump output; ignored by git
|-- Logs/                   # Local runtime logs; ignored by git
|-- docs/
|-- FrostbiteUniversal_Bridge.ini
`-- README.md
```

## What Gets Generated

The SDK dumper writes output under:

```text
GeneratedSDK/Injected_<process>_<pid>/
```

Open these first:

| Report | Why it matters |
| --- | --- |
| `ResearchDashboard.md` | Top runtime candidates and best next targets. |
| `ScanSummary.md` | What was scanned and how many findings were produced. |
| `InterestingModules.md` | Ranked game/engine modules worth reversing. |
| `TimeCandidates.md` | TimeScale, tick, delta-time, and simulation leads. |
| `EnvironmentCandidates.md` | Skybox, fog, exposure, lighting, and environment leads. |
| `HighConfidenceFunctions.md` | Strong candidate functions with evidence. |
| `FunctionTraces/Index.md` | Annotated trace reports for selected functions. |
| `Labels/` | IDA, Ghidra, and Binary Ninja label scripts. |
| `SDK/SDK.hpp` | Header-only entry point for importing generated metadata. |

## Importing The Generated SDK

The generated SDK is header-only. After a dump, copy or reference:

```text
GeneratedSDK/Injected_<process>_<pid>/SDK/
```

Then include it from a C++17 project:

```cpp
#include "SDK/SDK.hpp"

int main()
{
    for (const auto& module : FrostbiteSDK::Live::Modules)
    {
        // module.name, module.path, module.baseAddress, etc.
    }
}
```

The full step-by-step version is in [docs/SDK_USAGE.md](docs/SDK_USAGE.md).

## FrostbiteUniversal SDK Template

If you already have a generated SDK and want `FrostbiteUniversal` to talk to your owned project, use the bridge template:

- [Templates/FrostbiteUniversalSdkBridge/README.md](Templates/FrostbiteUniversalSdkBridge/README.md)
- [Templates/FrostbiteUniversalSdkBridge/FrostbiteUniversalSdkBridge.sln](Templates/FrostbiteUniversalSdkBridge/FrostbiteUniversalSdkBridge.sln)

The template gives you the exact exported functions Universal expects and a single adapter file, `SdkBindings.cpp`, where you map your SDK's world, entity, timescale, sky/environment, fog, and debug material systems.

It also ships with a small working sample backend for timescale, skybox tint, fog tint, debug material tint, actor/model location data, model boxes, snaplines, FOV, and view-angle preview. The Project tab can show position, size, radius, and projected debug overlays before users replace the sample backend with real SDK calls.

## GitHub Notes

- Built DLLs, EXEs, logs, and generated SDK dumps are intentionally ignored.
- Publish built binaries through GitHub Releases instead of committing them.
- `Source/ThirdParty/minhook` is configured as a submodule.
- Dear ImGui is vendored under `Source/imgui`; keep its license file intact.
- See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

No project license has been selected yet. Choose a license before making a public open-source release; without one, the repository remains all rights reserved by default.

## Support Boundary

Issues and pull requests should stay within read-only local research and owned-project workflows. Do not submit features for anti-cheat bypass, stealth/evasion, unauthorized multiplayer use, protected-file decryption, or target memory patching.
