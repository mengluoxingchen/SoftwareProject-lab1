[CmdletBinding()]
param([switch]$SkipTests, [switch]$Fresh, [switch]$Rebuild)

. (Join-Path $PSScriptRoot 'Tools.ps1')
$tools = Initialize-LabEnvironment
$includePrefix = Get-LabCompilerIncludePrefix $tools
Push-Location $tools.Source
try {
    $configureArguments = @('--preset', 'msvc-release')
    $configureArguments += "-DLEDGER_MSVC_INCLUDE_PREFIX=$includePrefix"
    if ($Fresh) { $configureArguments += '--fresh' }
    & $tools.CMake @configureArguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    $buildArguments = @('--build', '--preset', 'msvc-release', '--parallel', '4')
    if ($Rebuild) { $buildArguments += '--clean-first' }
    & $tools.CMake @buildArguments
    if ($LASTEXITCODE -ne 0) { throw 'C++ build failed.' }
    if (-not $SkipTests) {
        & $tools.CTest --preset msvc-release
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
} finally { Pop-Location }

