<#
.SYNOPSIS
    One-shot Winerose build: configure, build, run every test (unit, golden, arch, tools, pluginval).

.EXAMPLE
    .\scripts\build.ps1                          # vs2026 preset, Release, everything incl. pluginval
    .\scripts\build.ps1 -Config Debug -SkipPluginval
    .\scripts\build.ps1 -Preset vs2026-core      # JUCE-free layers only (no JUCE download)
    .\scripts\build.ps1 -Clean                   # delete build\<preset> first
    .\scripts\build.ps1 -SkipBenchmark           # skip CPU-threshold tests (unknown hardware, e.g. CI)

.NOTES
    Works in Windows PowerShell 5.1 and PowerShell 7. Finds a CMake new enough for the chosen generator,
    preferring the one bundled with Visual Studio (CMake 4.0 has no "Visual Studio 18 2026" generator).
    pluginval is downloaded once into build\tools\pluginval.
#>
[CmdletBinding()]
param(
    [ValidateSet('vs2026', 'vs2026-core', 'vs2022', 'ninja', 'ninja-core')]
    [string]$Preset = 'vs2026',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$SkipPluginval,
    [switch]$SkipBenchmark,   # CPU thresholds only mean something on known hardware (CI uses this)

    [ValidateRange(1, 10)]
    [int]$Strictness = 10
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build\$Preset"

function Get-CMakeVersion([string]$exe) {
    $line = (& $exe --version 2>$null | Select-Object -First 1)
    if ($line -match '(\d+)\.(\d+)\.(\d+)') { return [version]"$($Matches[1]).$($Matches[2]).$($Matches[3])" }
    return [version]'0.0.0'
}

function Find-CMake([version]$minimum) {
    $candidates = @()
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { $candidates += $onPath.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        foreach ($vs in (& $vswhere -all -products '*' -property installationPath)) {
            $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $bundled) { $candidates += $bundled }
        }
    }

    $best = $candidates | Sort-Object { Get-CMakeVersion $_ } -Descending | Select-Object -First 1
    if (-not $best) { throw 'CMake not found. Install CMake or Visual Studio with the "C++ CMake tools" component.' }
    $version = Get-CMakeVersion $best
    if ($version -lt $minimum) { throw "Found CMake $version at $best; preset '$Preset' needs >= $minimum." }
    return $best
}

function Invoke-Checked([string]$what, [scriptblock]$block) {
    Write-Host "==> $what" -ForegroundColor Cyan
    & $block
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

function Get-Pluginval {
    $dir = Join-Path $RepoRoot 'build\tools\pluginval'
    $exe = Join-Path $dir 'pluginval.exe'
    if (-not (Test-Path $exe)) {
        Write-Host '==> Downloading pluginval' -ForegroundColor Cyan
        New-Item -ItemType Directory -Force $dir | Out-Null
        $zip = Join-Path $dir 'pluginval.zip'
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/Tracktion/pluginval/releases/latest/download/pluginval_Windows.zip' -OutFile $zip
        Expand-Archive -Force $zip $dir
        Remove-Item $zip
    }
    return $exe
}

$minimum = if ($Preset -like 'vs2026*') { [version]'4.2.0' } else { [version]'3.25.0' }
$cmake = Find-CMake $minimum
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
Write-Host "Using CMake $(Get-CMakeVersion $cmake): $cmake"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "==> Removing $BuildDir" -ForegroundColor Cyan
    Remove-Item -Recurse -Force $BuildDir
}

$configureArgs = @('--preset', $Preset)
$wantsPluginval = -not $SkipPluginval -and -not $SkipTests -and ($Preset -notlike '*-core')
if ($wantsPluginval) {
    $configureArgs += "-DWINEROSE_PLUGINVAL=$(Get-Pluginval)"
    $configureArgs += "-DWINEROSE_PLUGINVAL_STRICTNESS=$Strictness"
} else {
    $configureArgs += '-DWINEROSE_PLUGINVAL='
}

Push-Location $RepoRoot
try {
    Invoke-Checked "Configure ($Preset)" { & $cmake @configureArgs }
    Invoke-Checked "Build ($Config)"     { & $cmake --build $BuildDir --config $Config --parallel }
    if (-not $SkipTests) {
        $ctestArgs = @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure')
        if ($SkipBenchmark) { $ctestArgs += @('-LE', 'benchmark') }
        Invoke-Checked "Test ($Config)"  { & $ctest @ctestArgs }
    }
} finally {
    Pop-Location
}

$artefacts = Join-Path $BuildDir "src\plugin\Winerose_artefacts\$Config"
if (Test-Path $artefacts) { Write-Host "Plugin artefacts: $artefacts" -ForegroundColor Green }
Write-Host 'Done.' -ForegroundColor Green
