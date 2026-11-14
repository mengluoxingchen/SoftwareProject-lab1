$ErrorActionPreference = 'Stop'
$script:LabProjectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Get-LabTools {
    $vsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vsWhere)) {
        throw 'Visual Studio Installer was not found. Install the Desktop development with C++ workload.'
    }
    $vsRoot = & $vsWhere -all -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'No Visual Studio installation with C++ tools was found.' }
    $vsRoot = @($vsRoot)[0].Trim()
    $cmakeRoot = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $gitPath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe'
    if (-not (Test-Path -LiteralPath $gitPath)) {
        $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
        if ($gitCommand) { $gitPath = $gitCommand.Source }
    }
    $qtRoot = $env:QT_ROOT
    $sourceRoot = Join-Path $script:LabProjectRoot 'code'
    if (Test-Path -LiteralPath (Join-Path $script:LabProjectRoot 'CMakeLists.txt')) {
        $sourceRoot = $script:LabProjectRoot
    }
    if (-not $qtRoot) { $qtRoot = Join-Path $script:LabProjectRoot '.tools\qt\6.8.3\msvc2022_64' }
    return [PSCustomObject]@{
        Root = $script:LabProjectRoot
        Source = $sourceRoot
        VisualStudio = $vsRoot
        DevCommand = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
        CMake = Join-Path $cmakeRoot 'CMake\bin\cmake.exe'
        CTest = Join-Path $cmakeRoot 'CMake\bin\ctest.exe'
        Ninja = Join-Path $cmakeRoot 'Ninja\ninja.exe'
        Git = $gitPath
        ClangFormat = Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin\clang-format.exe'
        Qt = [System.IO.Path]::GetFullPath($qtRoot)
    }
}

function Initialize-LabEnvironment {
    $tools = Get-LabTools
    foreach ($toolPath in @($tools.DevCommand, $tools.CMake, $tools.Ninja)) {
        if (-not (Test-Path -LiteralPath $toolPath)) { throw "Required tool not found: $toolPath" }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $tools.Qt 'lib\cmake\Qt6\Qt6Config.cmake'))) {
        throw 'Qt SDK not found. Run scripts\Setup.ps1 first, or set QT_ROOT to an MSVC x64 Qt SDK.'
    }
    # Import the compiler and Windows SDK environment only into this process.
    $devLine = 'call "' + $tools.DevCommand + '" -arch=x64 -host_arch=x64 >nul && set'
    $devEnvironment = & $env:ComSpec /d /s /c $devLine
    if ($LASTEXITCODE -ne 0) { throw 'Visual Studio environment initialization failed.' }
    foreach ($entry in $devEnvironment) {
        if ($entry -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    $env:QT_ROOT = $tools.Qt
    # Keep compiler dependency messages stable for CMake/Ninja on Chinese Windows.
    $env:VSLANG = '1033'
    $env:QT_PLUGIN_PATH = Join-Path $tools.Qt 'plugins'
    $env:PATH = (Join-Path $tools.Qt 'bin') + ';' + (Split-Path $tools.CMake) + ';' + (Split-Path $tools.Ninja) + ';' + (Split-Path $tools.Git) + ';' + $env:PATH
    return $tools
}

function Get-LabCompilerIncludePrefix($tools) {
    $probeDirectory = Join-Path $tools.Root 'build\configuration'
    New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
    $probePath = Join-Path $probeDirectory 'msvc-includes.cpp'
    [System.IO.File]::WriteAllText($probePath, "#include <stddef.h>`r`n", [System.Text.Encoding]::ASCII)
    $compilerOutput = & cl.exe /nologo /showIncludes /Zs $probePath 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'MSVC dependency-output probe failed.' }
    foreach ($compilerLine in $compilerOutput) {
        if ([string]$compilerLine -match '^(.*?)[A-Za-z]:[\\/].*stddef\.h') { return $matches[1] }
    }
    throw 'Could not detect the compiler include prefix required by Ninja.'
}

