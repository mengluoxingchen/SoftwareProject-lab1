[CmdletBinding()]
param([switch]$Fix)

. (Join-Path $PSScriptRoot 'Tools.ps1')
$tools = Get-LabTools
if (-not (Test-Path -LiteralPath $tools.ClangFormat)) { throw 'clang-format was not found in Visual Studio.' }
$sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $tools.Source 'src'), (Join-Path $tools.Source 'tests') -Recurse -File | Where-Object { $_.Extension -in '.cpp', '.h', '.hpp' })
foreach ($sourceFile in $sourceFiles) {
    if ($Fix) { & $tools.ClangFormat -i $sourceFile.FullName }
    else { & $tools.ClangFormat --dry-run --Werror $sourceFile.FullName }
    if ($LASTEXITCODE -ne 0) { throw "Formatting failed: $($sourceFile.FullName)" }
}
Write-Output ('Formatting passed for ' + $sourceFiles.Count + ' C++ files.')

