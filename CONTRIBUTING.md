# Contributing

Thanks for helping improve the toolkit. Keep contributions practical, documented, and inside the project scope.

## Good Contributions

- Read-only scanner improvements
- Safer PE parsing
- Better string, RTTI, xref, and function-boundary analysis
- Better SDK header and JSON generation
- Better report formatting
- Better IDA, Ghidra, Binary Ninja, SQLite, or ReClass exports
- Better owned-project Universal bridge examples and SDK template adapters
- Better actor/model projection, debug visualization, FOV, and editor camera examples for owned projects
- Build fixes and GitHub Actions improvements
- Documentation and examples for owned/local research workflows

## Contributions Not Accepted

- Anti-cheat bypasses
- Stealth or evasion logic
- Anti-debug bypass logic
- Protected-file decryption or unpacking
- Networked cheating features
- Instructions for unauthorized use against third-party online games
- Commits containing generated dumps, logs, crash dumps, or compiled binaries

## Local Build Check

Before opening a pull request:

```powershell
git submodule update --init --recursive

& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Source\FrostbiteEngineTools.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal

& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  ".\Templates\FrostbiteUniversalSdkBridge\FrostbiteUniversalSdkBridge.sln" `
  /m /p:Configuration=Release /p:Platform=x64 /v:minimal
```

## Pull Request Checklist

- The change is read-only or clearly limited to owned-project diagnostics.
- No generated SDK dumps are committed.
- No logs, crash dumps, or local absolute-path configs are committed.
- Third-party licenses are preserved.
- Documentation is updated when behavior changes.
- The main solution and SDK bridge template build in `Release|x64`.
