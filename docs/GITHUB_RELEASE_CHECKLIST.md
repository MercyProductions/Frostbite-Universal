# GitHub Release Checklist

Use this before the first push and before each public release.

## Before First Push

1. Pick a repository name.
2. Decide whether the repository is private or public.
3. Choose a license before making a public open-source release. If no license is added, the code remains all rights reserved by default.
4. Keep generated SDK dumps, logs, and compiled binaries out of Git.
5. Verify the ignored files:

   ```powershell
   cd <repo-root>
   git status --ignored -s
   ```

6. Confirm submodules:

   ```powershell
   git submodule status
   ```

7. Build locally:

   ```powershell
   & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
     ".\Source\FrostbiteEngineTools.sln" `
     /m /p:Configuration=Release /p:Platform=x64 /v:minimal

   & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
     ".\Templates\FrostbiteUniversalSdkBridge\FrostbiteUniversalSdkBridge.sln" `
     /m /p:Configuration=Release /p:Platform=x64 /v:minimal
   ```

## Initialize Git

If this folder is not already a repository:

```powershell
cd <repo-root>
git init
git add .gitattributes .gitignore .gitmodules README.md CONTRIBUTING.md SECURITY.md THIRD_PARTY_NOTICES.md docs .github Source FrostbiteUniversal_Bridge.ini Tools
git status
git commit -m "Prepare Frostbite toolkit for GitHub"
```

If `Source\ThirdParty\minhook` appears as a submodule entry, that is expected.

## Connect To GitHub

Create an empty repository on GitHub, then:

```powershell
git branch -M main
git remote add origin https://github.com/MercyProductions/Frostbite-Universal.git
git push -u origin main
```

## Release Artifacts

Compiled binaries are ignored in Git. If you want to publish binaries, attach them to a GitHub Release instead of committing them.

Suggested release package:

```text
FrostbiteToolkit_<version>/
  FrostbiteUniversal.dll
  FrostbiteSDKGenerator.exe
  FrostbiteSDKGenerator.dll
  FrostbiteUniversal.h
  FrostbiteSDKGenerator.h
  README_TOOLS.txt
  SDKGENERATOR_RUNTIME_INTROSPECTION.md
  PROJECT_BRIDGE_EXAMPLE.md
  THIRD_PARTY_NOTICES.md
```

Do not publish:

- `GeneratedSDK/` dumps from proprietary games
- `Logs/` from live runs
- Local absolute-path configuration
- Crash dumps
- Private game assets or protected metadata

## Release Validation

Before uploading a release zip:

1. Build `Release|x64`.
2. Build the SDK bridge template in `Release|x64`.
3. Run the SDK generator against an owned test folder or owned project.
4. Confirm `GeneratedSDK/<run>/ResearchDashboard.md` exists.
5. Confirm the generated SDK headers include useful module/candidate data.
6. Confirm Universal's Project tab still exposes the documented bridge/template controls.
7. Confirm labels/scripts are generated if enabled.
8. Confirm no private paths or proprietary dumps are included in the release zip.
9. Include third-party notices.
