# Building

This project is a Windows x64 Visual Studio workspace. Build from the root of the repository unless a command says otherwise.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022
- Workload: Desktop development with C++
- Windows SDK
- Git with submodule support
- Optional: Visual Studio Developer PowerShell for `dumpbin.exe`

Clone with submodules:

```powershell
git clone --recurse-submodules <your-repo-url> Frostbite
cd Frostbite
```

If you already cloned without submodules:

```powershell
git submodule update --init --recursive
```

## Build With Visual Studio

1. Open `Source\FrostbiteEngineTools.sln`.
2. Select `Release`.
3. Select `x64`.
4. Build the solution.

The solution builds:

| Project | Type | Output |
| --- | --- | --- |
| `FrostbiteUniversal` | DLL | `Source\FrostbiteUniversal\build\x64\Release\FrostbiteUniversal.dll` |
| `FrostbiteSDKGenerator` | EXE | `Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe` |
| `FrostbiteSDKGeneratorDll` | DLL | `Source\FrostbiteSDKGeneratorDll\build\x64\Release\FrostbiteSDKGenerator.dll` |

## Build The SDK Bridge Template

Most users should build the template through Visual Studio:

1. Open `Templates\FrostbiteUniversalSdkBridge\FrostbiteUniversalSdkBridge.sln`.
2. Select `Release`.
3. Select `x64`.
4. Build.

Output:

```text
Templates\FrostbiteUniversalSdkBridge\build\x64\Release\FrostbiteUniversalSdkBridge.dll
```

To point the template at a generated SDK, edit the `GeneratedSdkDir` property in `FrostbiteUniversalSdkBridge.vcxproj` or update the project's additional include directories in Visual Studio.

## Build From PowerShell

Use the Visual Studio MSBuild path that exists on your machine. Community is shown here:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Source\FrostbiteEngineTools.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Build the Visual Studio SDK bridge template:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Templates\FrostbiteUniversalSdkBridge\FrostbiteUniversalSdkBridge.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Professional and Enterprise installs use the same path shape with `Professional` or `Enterprise` instead of `Community`.

## Stage Local Tools

Compiled binaries are intentionally ignored by Git. After building, stage local copies for testing:

```powershell
New-Item -ItemType Directory -Force -Path ".\Tools" | Out-Null

Copy-Item ".\Source\FrostbiteUniversal\build\x64\Release\FrostbiteUniversal.dll" `
  ".\INJECT_THIS_FrostbiteUniversal.dll" -Force

Copy-Item ".\Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe" `
  ".\Tools\FrostbiteSDKGenerator.exe" -Force

Copy-Item ".\Source\FrostbiteSDKGeneratorDll\build\x64\Release\FrostbiteSDKGenerator.dll" `
  ".\Tools\FrostbiteSDKGenerator.dll" -Force

Copy-Item ".\Source\FrostbiteSDKGeneratorDll\build\x64\Release\FrostbiteSDKGenerator.dll" `
  ".\Tools\INJECT_THIS_FOR_SDK_DUMP_FrostbiteSDKGenerator.dll" -Force

Copy-Item ".\Source\FrostbiteSDKGenerator\Include\FrostbiteSDKGenerator.h" `
  ".\Tools\FrostbiteSDKGenerator.h" -Force

Copy-Item ".\Source\FrostbiteUniversal\Include\FrostbiteUniversal.h" `
  ".\Tools\FrostbiteUniversal.h" -Force
```

## Optional Dumpbin Support

`dumpbin.exe` can be used beside the SDK generator for extra PE import/export inspection. Open a Visual Studio Developer PowerShell, then run:

```powershell
dumpbin /headers ".\Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe"
dumpbin /imports ".\Source\FrostbiteSDKGeneratorDll\build\x64\Release\FrostbiteSDKGenerator.dll"
dumpbin /exports ".\Source\FrostbiteSDKGeneratorDll\build\x64\Release\FrostbiteSDKGenerator.dll"
```

The dumper itself should remain the main source of SDK reports. `dumpbin` is best used as a supporting audit tool.

## Common Build Issues

### MinHook headers are missing

Run:

```powershell
git submodule update --init --recursive
```

### ImGui headers are missing

Confirm the vendored ImGui folder exists:

```powershell
Test-Path ".\Source\imgui\imgui.h"
```

`FrostbiteUniversal` uses the local `Source\imgui` folder through the `UnityImGuiDir` project property.

### Linker cannot find DirectX libraries

Install the Windows SDK through Visual Studio Installer. The project links against `d3d11.lib`, `d3d12.lib`, and `dxgi.lib`.

### GitHub Actions build fails

Check the workflow log first. The CI job builds both `Source\FrostbiteEngineTools.sln` and `Templates\FrostbiteUniversalSdkBridge\FrostbiteUniversalSdkBridge.sln` in `Release|x64` using `windows-latest`.
