param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $root "Source\FrostbiteUniversal\FrostbiteUniversal.sln"
$dll = Join-Path $root "Source\FrostbiteUniversal\build\x64\$Configuration\FrostbiteUniversal.dll"

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($install) {
            $candidate = Join-Path $install "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $pathCandidate = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($pathCandidate) { return $pathCandidate.Source }

    throw "MSBuild.exe was not found. Run from a Visual Studio Developer PowerShell or install Build Tools."
}

function Find-Dumpbin {
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($dumpbin) { return $dumpbin.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($install) {
            $toolsRoot = Join-Path $install "VC\Tools\MSVC"
            $latestTools = Get-ChildItem $toolsRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
            if ($latestTools) {
                $candidate = Join-Path $latestTools.FullName "bin\Hostx64\x64\dumpbin.exe"
                if (Test-Path $candidate) { return $candidate }
            }
        }
    }

    throw "dumpbin.exe was not found. Run from a Visual Studio Developer PowerShell or install VC tools."
}

$msbuild = Find-MSBuild
Write-Host "Building $Configuration x64 with $msbuild"
& $msbuild $solution /m /restore /p:Configuration=$Configuration /p:Platform=x64
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $dll)) {
    throw "Expected DLL was not produced: $dll"
}

$dumpbin = Find-Dumpbin
$exports = & $dumpbin /nologo /exports $dll
$requiredExports = @(
    "FrostbiteUniversal_Initialize",
    "FrostbiteUniversal_GetExport",
    "FrostbiteUniversal_RunSdkDump",
    "FrostbiteUniversal_ReloadGeneratedSdk",
    "FrostbiteUniversal_RegisterEntityProvider",
    "FrostbiteUniversal_RegisterViewProjectionProvider",
    "FrostbiteUniversal_RegisterViewportProvider",
    "FrostbiteUniversal_UpdateProviders",
    "FrostbiteUniversal_ProjectWorldToScreen",
    "FrostbiteUniversal_WriteSnapshotJson",
    "FrostbiteUniversal_LoadSnapshotJson",
    "FrostbiteUniversal_GetCapabilityInfo"
)

foreach ($name in $requiredExports) {
    if (-not ($exports -match [regex]::Escape($name))) {
        throw "Required export missing: $name"
    }
}

Write-Host "Verified $dll"
