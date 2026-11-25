# ==================================================================================================
# CoralGraphics standalone build
# ==================================================================================================
# Configures, builds and tests this repository as a standalone project:
#
#   .\scripts\build.ps1                         # Release, tests on
#   .\scripts\build.ps1 -Configuration Debug    # Debug
#   .\scripts\build.ps1 -NoTests                # skip the suites (and GoogleTest)
#   .\scripts\build.ps1 -Clean                  # remove the build directory first
#
# A dependency declared in .gitmodules is fetched automatically when it is missing, so a fresh
# checkout needs one command. Rerun .\scripts\bootstrap.ps1 explicitly to update the dependencies.
#
# Extra CMake arguments go through -CMakeArgs:
#
#   .\scripts\build.ps1 -CMakeArgs '-DCORAL_BACKEND_SUPPORTS_METAL=OFF'
# ==================================================================================================

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$NoTests,
    [switch]$NoBootstrap,
    [switch]$Clean,
    [string]$BuildDir = 'build',
    [int]$Jobs = 0,
    [string[]]$CMakeArgs = @(),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArguments = @()
)

$ErrorActionPreference = 'Stop'

if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$Tests = if ($NoTests) { 'OFF' } else { 'ON' }

function Write-Step([string]$Message) {
    Write-Host $Message
}

function Fail([string]$Message) {
    throw "error: $Message"
}

function Invoke-Native {
    param([string[]]$Arguments)

    $command = $Arguments[0]
    $rest = @()
    if ($Arguments.Count -gt 1) {
        $rest = $Arguments[1..($Arguments.Count - 1)]
    }
    & $command @rest
    if ($LASTEXITCODE -ne 0) {
        Fail "$($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Test-DependenciesMissing {
    if (-not (Test-Path (Join-Path $Root 'external/CoralCore/CMakeLists.txt'))) { return $true }
    if (-not (Test-Path (Join-Path $Root 'third_party/libz/coral/CMakeLists.txt'))) { return $true }
    if (-not (Test-Path (Join-Path $Root 'third_party/libyuv/CMakeLists.txt'))) { return $true }
    if (-not $NoTests -and -not (Test-Path (Join-Path $Root 'third_party/libgtest/coral/CMakeLists.txt'))) {
        return $true
    }
    return $false
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail 'cmake is required and was not found on PATH'
}

# --------------------------------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------------------------------
if (Test-DependenciesMissing) {
    if ($NoBootstrap) {
        Fail 'dependencies are missing and -NoBootstrap was given; run .\scripts\bootstrap.ps1'
    }
    Write-Step 'Some dependencies declared in .gitmodules are missing; fetching them first.'
    & (Join-Path $PSScriptRoot 'bootstrap.ps1')
    if ($LASTEXITCODE -ne 0) {
        Fail "bootstrap.ps1 failed with exit code $LASTEXITCODE"
    }
}

# --------------------------------------------------------------------------------------------------
# Configure, build, test
# --------------------------------------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildPath)) {
    Write-Step "Removing $BuildPath"
    Remove-Item -LiteralPath $BuildPath -Recurse -Force
}

Write-Step "Configuring CoralGraphics ($Configuration, tests=$Tests)"
$configure = @(
    'cmake', '-S', $Root, '-B', $BuildPath,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCORAL_BUILD_TESTS=$Tests"
)
$extraCMakeArgs = @($CMakeArgs) + @($RemainingArguments)
if ($extraCMakeArgs.Count -gt 0) {
    $configure += $extraCMakeArgs
}
Invoke-Native -Arguments $configure

$parallel = if ($Jobs -gt 0) { @('--parallel', "$Jobs") } else { @('--parallel') }
Write-Step 'Building'
Invoke-Native -Arguments (@('cmake', '--build', $BuildPath, '--config', $Configuration) + $parallel)

if (-not $NoTests) {
    Write-Step 'Running the test suites'
    # Only CoralGraphics' own suites and demos. A standalone configure also adds the dependency
    # repositories' tests, and several of those are registered without the executables this build
    # produces; ctest reports them as Not Run, which is not a failure of this repository.
    $testFilter = '^(test_(graphics|backend|geometry|effect)|effect_|mac_demo_verify|graphics_demo_|vulkan_demo_|linux_demo_|lottie_player_|fuzz_parsers_smoke)'
    Invoke-Native -Arguments @('ctest', '--test-dir', $BuildPath, '-C', $Configuration, '-R', $testFilter, '--output-on-failure')
}

Write-Step ''
Write-Step "Done. The build is in $BuildPath"
