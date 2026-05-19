[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$GameRoot,
    [string]$GeneratorPath,
    [string]$OutputDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'GeneratedSDK\StaticGameRootScan'),
    [int]$MaxExportsPerModule = 160,
    [switch]$IncludeThirdParty,
    [switch]$IncludeAntiCheat
)

$ErrorActionPreference = 'Stop'

function Resolve-GeneratorPath {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        return $ExplicitPath
    }

    $projectRoot = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $projectRoot 'Tools\FrostbiteSDKGenerator.exe'),
        (Join-Path $projectRoot 'Source\FrostbiteSDKGenerator\build\x64\Release\FrostbiteSDKGenerator.exe'),
        (Join-Path $projectRoot 'Source\FrostbiteSDKGenerator\x64\Release\FrostbiteSDKGenerator.exe')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw 'FrostbiteSDKGenerator.exe was not found. Build it or pass -GeneratorPath.'
}

$GeneratorPath = Resolve-GeneratorPath -ExplicitPath $GeneratorPath
if (-not (Test-Path -LiteralPath $GeneratorPath -PathType Leaf)) {
    throw "FrostbiteSDKGenerator.exe does not exist: $GeneratorPath"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$args = @()
foreach ($root in $GameRoot) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        Write-Warning "Game root does not exist and will still be passed for generator reporting: $root"
    }

    $args += '--game-root'
    $args += $root
}

$args += '--out'
$args += $OutputDir
$args += '--max-exports'
$args += [string]$MaxExportsPerModule

if ($IncludeThirdParty) {
    $args += '--include-third-party'
}

if ($IncludeAntiCheat) {
    $args += '--include-anti-cheat'
}

Write-Host "Running FrostbiteSDKGenerator:"
$quotedArgs = $args | ForEach-Object { '"' + $_ + '"' }
Write-Host ('  "' + $GeneratorPath + '" ' + ($quotedArgs -join ' '))

& $GeneratorPath @args
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    throw "FrostbiteSDKGenerator exited with code $exitCode"
}

Write-Host "Frostbite static game-root scan complete."
Write-Host "Output: $OutputDir"
