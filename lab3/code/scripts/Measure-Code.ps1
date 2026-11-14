[CmdletBinding()]
param([string]$OutputDirectory)

. (Join-Path $PSScriptRoot 'Tools.ps1')
$taskTools = Get-LabTools
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $taskTools.Root 'build\verification' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Measure-CppLines([string]$Path) {
    $taskBlock = $false
    $taskRawEnd = $null
    $taskQuote = [char]0
    $taskCode = 0; $taskComment = 0; $taskBlank = 0
    $taskLines = [System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8)
    foreach ($taskLine in $taskLines) {
        $taskHasCode = $false
        $taskIndex = 0
        while ($taskIndex -lt $taskLine.Length) {
            if ($taskRawEnd) {
                $taskHasCode = $true
                $taskEnd = $taskLine.IndexOf($taskRawEnd, $taskIndex, [StringComparison]::Ordinal)
                if ($taskEnd -lt 0) { break }
                $taskIndex = $taskEnd + $taskRawEnd.Length
                $taskRawEnd = $null
                continue
            }
            if ($taskBlock) {
                $taskEnd = $taskLine.IndexOf('*/', $taskIndex, [StringComparison]::Ordinal)
                if ($taskEnd -lt 0) { break }
                $taskIndex = $taskEnd + 2
                $taskBlock = $false
                continue
            }
            $taskChar = $taskLine[$taskIndex]
            if ($taskQuote -ne [char]0) {
                $taskHasCode = $true
                if ($taskChar -eq '\') { $taskIndex += 2; continue }
                if ($taskChar -eq $taskQuote) { $taskQuote = [char]0 }
                $taskIndex++
                continue
            }
            if ([char]::IsWhiteSpace($taskChar)) { $taskIndex++; continue }
            $taskTail = $taskLine.Substring($taskIndex)
            if ($taskTail.StartsWith('//', [StringComparison]::Ordinal)) { break }
            if ($taskTail.StartsWith('/*', [StringComparison]::Ordinal)) {
                $taskBlock = $true; $taskIndex += 2; continue
            }
            $taskHasCode = $true
            if ($taskTail -match '^R"([^\s()\\]{0,16})\(') {
                $taskRawEnd = ')' + $matches[1] + '"'
                $taskIndex += $matches[0].Length
                continue
            }
            if ($taskChar -eq '"' -or $taskChar -eq "'") { $taskQuote = $taskChar }
            $taskIndex++
        }
        if ([string]::IsNullOrWhiteSpace($taskLine)) { $taskBlank++ }
        elseif ($taskHasCode) { $taskCode++ }
        else { $taskComment++ }
    }
    return [pscustomobject]@{Total=$taskLines.Length; Code=$taskCode; Comment=$taskComment; Blank=$taskBlank}
}

$taskRows = @(foreach ($taskDirectory in @('src','tests')) {
    Get-ChildItem -LiteralPath (Join-Path $taskTools.Source $taskDirectory) -Recurse -File |
        Where-Object { $_.Extension -in '.cpp','.h','.hpp' } |
        Sort-Object FullName | ForEach-Object {
            $taskRelative = $_.FullName.Substring($taskTools.Source.Length + 1).Replace('\','/')
            $taskCategory = if ($taskDirectory -eq 'tests') { 'tests' } elseif ($taskRelative.StartsWith('src/ui/')) { 'handwritten_ui' } else { 'business_and_infrastructure' }
            $taskCounts = Measure-CppLines $_.FullName
            [pscustomobject]@{Path=$taskRelative; Category=$taskCategory; Total=$taskCounts.Total; Code=$taskCounts.Code; Comment=$taskCounts.Comment; Blank=$taskCounts.Blank}
        }
})
$taskGroups = @(foreach ($taskCategory in @('business_and_infrastructure','handwritten_ui','tests')) {
    $taskGroup = @($taskRows | Where-Object { $_.Category -eq $taskCategory })
    [pscustomobject]@{
        Category=$taskCategory; Files=$taskGroup.Count
        Total=($taskGroup | Measure-Object Total -Sum).Sum
        Code=($taskGroup | Measure-Object Code -Sum).Sum
        Comment=($taskGroup | Measure-Object Comment -Sum).Sum
        Blank=($taskGroup | Measure-Object Blank -Sum).Sum
    }
})
$taskProduction = @($taskRows | Where-Object { $_.Category -ne 'tests' })
$taskReport = [ordered]@{
    Scope='src and tests C++ files only; no MOC/RCC/build output, dependencies or generated GUI files'
    Method='Physical lines; code has non-comment tokens, comments inside ordinary/raw strings are code. Blank lines and comment-only lines are separate. Mixed code/comment lines count as code.'
    ProductionCode=($taskProduction | Measure-Object Code -Sum).Sum
    BusinessCode=($taskGroups | Where-Object Category -eq 'business_and_infrastructure').Code
    RequiredMinimumExclusive=1500
    ExceedsMinimumWithoutUiOrTests=(($taskGroups | Where-Object Category -eq 'business_and_infrastructure').Code -gt 1500)
    Groups=$taskGroups; Files=$taskRows
}
$taskRows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'code-size-files.csv') -NoTypeInformation -Encoding UTF8
$taskReport | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'code-size.json') -Encoding UTF8
$taskGroups | Format-Table -AutoSize
Write-Output ("Production nonblank/non-comment lines: " + $taskReport.ProductionCode)
if (-not $taskReport.ExceedsMinimumWithoutUiOrTests) { throw 'Business and infrastructure code alone does not exceed 1500 lines.' }
