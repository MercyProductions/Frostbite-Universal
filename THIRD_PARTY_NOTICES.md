# Third-Party Notices

This repository includes or references third-party components. Keep their license files intact when redistributing source or release packages.

## Dear ImGui

- Location: `Source/imgui`
- License file: `Source/imgui/LICENSE.txt`
- Purpose: Immediate-mode UI used by `FrostbiteUniversal`.

## MinHook

- Location: `Source/ThirdParty/minhook`
- Source: `https://github.com/TsudaKageyu/minhook.git`
- License file: `Source/ThirdParty/minhook/LICENSE.txt`
- Purpose: Windows API hook helper used by the runtime diagnostics DLL.

`Source/ThirdParty/minhook` is configured as a Git submodule. Clone with `--recurse-submodules` or run `git submodule update --init --recursive`.

## Platform SDKs

The project links against Windows and DirectX SDK libraries such as `d3d11.lib`, `d3d12.lib`, and `dxgi.lib`. These are development prerequisites and are not vendored into this repository.

## Release Packaging

When publishing a binary release, include this notice file and preserve third-party license files.
