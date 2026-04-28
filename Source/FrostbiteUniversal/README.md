# FrostbiteUniversal

`FrostbiteUniversal` builds the runtime diagnostics DLL:

```text
Source/FrostbiteUniversal/build/x64/Release/FrostbiteUniversal.dll
```

Build from the repository root:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Source\FrostbiteEngineTools.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

The public API is declared in:

```text
Source/FrostbiteUniversal/Include/FrostbiteUniversal.h
```

## Features

- Self-hosted ImGui overlay with F4 visibility toggle.
- Console/log initialization for runtime diagnostics.
- Runtime module, export, catalog, and report APIs.
- Owned-project bridge through explicit `FrostbiteGame_*` exports.
- Project tab controls for timescale, skybox tint, fog tint, debug material tint, wireframe/debug flags, FOV, and view-angle preview.
- Live actor/model table with actor name, class, model, asset path, position, computed size, radius, and flags.
- Optional projected debug overlays for model boxes, snaplines, and selected view target markers.

## SDK Bridge Template

If you already have a generated SDK, start here:

```text
Templates/FrostbiteUniversalSdkBridge/README.md
```

The template gives users a copyable bridge with the exact `FrostbiteGame_*` exports Universal expects. Users only need to edit `SdkBindings.cpp` to map their SDK's world/entity, timescale, sky/environment, fog, and debug visualization systems.

The template includes a working sample backend for timescale, skybox tint, fog tint, debug material tint, actor/model location data, model boxes, snaplines, FOV, and view-angle preview. It is useful for checking that Universal's Project tab is connected before replacing the sample state with real SDK calls.

For the full project walkthrough, read the root `README.md` and `docs/SDK_USAGE.md`.
