[CmdletBinding()]
param([string]$DataDirectory)

. (Join-Path $PSScriptRoot 'Tools.ps1')
$tools = Initialize-LabEnvironment
$application = Join-Path $tools.Root 'build\msvc-release\shared_ledger.exe'
if (-not (Test-Path -LiteralPath $application)) { throw 'Executable not found. Run scripts\Build.ps1 first.' }
$applicationArguments = @()
if ($DataDirectory) { $applicationArguments += @('--data-dir', [System.IO.Path]::GetFullPath($DataDirectory)) }
& $application @applicationArguments
if ($LASTEXITCODE -ne 0) { throw "Application exited with code $LASTEXITCODE" }

