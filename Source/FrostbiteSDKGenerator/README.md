# FrostbiteSDKGenerator

`FrostbiteSDKGenerator` builds the read-only SDK/report generator:

```text
Source/FrostbiteSDKGenerator/build/x64/Release/FrostbiteSDKGenerator.exe
```

The same backend is embedded by `FrostbiteUniversal` for the ImGui SDK tab:

```text
Source/FrostbiteUniversal/build/x64/Release/FrostbiteUniversal.dll
```

The public C API is declared in:

```text
Source/FrostbiteSDKGenerator/Include/FrostbiteSDKGenerator.h
```

## Features

- Static folder scan mode for owned game folders/research builds.
- Live injected snapshot mode for owned/local processes.
- Console progress output in embedded Universal-backend mode.
- Module, section, import, export, symbol, PDB, RTTI, vtable, and virtual slot capture.
- ASCII/UTF-16 string discovery and read-only xref analysis.
- `.pdata` function boundary recovery.
- Candidate discovery and scoring for time/tick, visual environment, rendering, physics, entity, audio, shader, asset, and debug systems.
- Annotated function traces, caller/callee graph output, system clusters, enum/table detection, name decomposition, shader pipeline maps, EBX/TOC/CAS-adjacent metadata references, and run diffing.
- Export targets for C++ headers, JSON, Markdown, HTML dashboard, SQLite SQL, IDA, Ghidra, Binary Ninja, and ReClass notes.

The dumper is discovery-based. It does not require provider exports, does not patch process memory, and does not bypass anti-cheat or protected files.

## Backend Entry Points

```cpp
FrostbiteSDKGenerator_OpenConsole();
FrostbiteSDKGenerator_SetVerbose(1);
FrostbiteSDKGenerator_GenerateDefault(L".\\GeneratedSDK");
```

Verbose mode is on by default for embedded backend calls. It opens or attaches a console and prints scanning, include/skip, export-scan, RTTI/vtable reconstruction, `.pdata` function-bound recovery, read-only string/xref discovery, call-graph mapping, asset-reference scanning, and file-write progress in real time.

## Build

From the repository root:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Source\FrostbiteEngineTools.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Or open `Source/FrostbiteEngineTools.sln` in Visual Studio and build `Release|x64`.

## Run

Static folder scan:

```powershell
& ".\Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe" `
  --game-root "C:\Path\To\YourOwnedGameOrResearchBuild" `
  --out ".\GeneratedSDK\StaticScan"
```

Query existing injected reports:

```powershell
& ".\Tools\FrostbiteSDKGenerator.exe" --out ".\GeneratedSDK\Injected_<process>_<pid>" --query TimeScale
& ".\Tools\FrostbiteSDKGenerator.exe" --out ".\GeneratedSDK\Injected_<process>_<pid>" --trace 0x14048a430
& ".\Tools\FrostbiteSDKGenerator.exe" --diff dump_menu.json dump_ingame.json
```

For the full walkthrough, read:

```text
README.md
docs/SDK_USAGE.md
Tools/SDKGENERATOR_RUNTIME_INTROSPECTION.md
```
