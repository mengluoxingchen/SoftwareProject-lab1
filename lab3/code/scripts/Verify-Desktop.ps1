[CmdletBinding()]
param([switch]$Packaged, [switch]$IdentityFlows, [switch]$ExpenseFlows, [switch]$WorkflowFlows, [switch]$SettlementFlows, [switch]$FinanceFlows, [switch]$MembershipFlows, [switch]$QueryFlows, [switch]$RecoveryFlows, [switch]$SecurityFlows)

. (Join-Path $PSScriptRoot 'Tools.ps1')
$tools = Initialize-LabEnvironment
$verificationRoot = Join-Path $tools.Root 'build\verification'
New-Item -ItemType Directory -Path $verificationRoot -Force | Out-Null
if ($SecurityFlows -or $IdentityFlows -or $ExpenseFlows -or $WorkflowFlows -or $SettlementFlows -or $FinanceFlows -or $MembershipFlows -or $QueryFlows -or $RecoveryFlows) {
    if ($Packaged) { throw 'The desktop scenario tests run from the development build.' }
    if (([int]$SecurityFlows.IsPresent + [int]$IdentityFlows.IsPresent + [int]$ExpenseFlows.IsPresent + [int]$WorkflowFlows.IsPresent + [int]$SettlementFlows.IsPresent + [int]$FinanceFlows.IsPresent + [int]$MembershipFlows.IsPresent + [int]$QueryFlows.IsPresent + [int]$RecoveryFlows.IsPresent) -gt 1) { throw 'Choose one desktop scenario suite per run.' }
    $env:LEDGER_SCREENSHOT_DIR = $verificationRoot
    $scenario = if ($SecurityFlows) { 'security' } elseif ($RecoveryFlows) { 'recovery' } elseif ($QueryFlows) { 'query' } elseif ($MembershipFlows) { 'membership' } elseif ($FinanceFlows) { 'finance' } elseif ($SettlementFlows) { 'settlement' } elseif ($WorkflowFlows) { 'workflow' } elseif ($ExpenseFlows) { 'expense' } else { 'identity' }
    $testProgram = Join-Path $tools.Root "build\msvc-release\ui_${scenario}_tests.exe"
    $testResult = Join-Path $verificationRoot "ui-$scenario-test-results.xml"
    $testProcess = Start-Process -FilePath $testProgram -ArgumentList @('-platform', 'windows', '-o', ('"' + $testResult + ',junitxml"')) -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput (Join-Path $verificationRoot "$scenario-ui.stdout.log") -RedirectStandardError (Join-Path $verificationRoot "$scenario-ui.stderr.log")
    if ($testProcess.ExitCode -ne 0) {
        Get-Content -LiteralPath $testResult, (Join-Path $verificationRoot "$scenario-ui.stderr.log") -ErrorAction SilentlyContinue
        throw "Desktop $scenario scenarios failed."
    }
    Write-Output "Desktop $scenario scenarios passed: $testResult"
    return
}
$suffix = if ($Packaged) { 'packaged' } else { 'development' }
$application = if ($Packaged) {
    Join-Path $tools.Root 'dist\SharedLedger\shared_ledger.exe'
} else { Join-Path $tools.Root 'build\msvc-release\shared_ledger.exe' }
if (-not (Test-Path -LiteralPath $application)) { throw "Executable not found: $application" }
$screenshot = Join-Path $verificationRoot ("desktop-$suffix.png")
$dataDirectory = Join-Path $verificationRoot ("data-$suffix")

if ($Packaged) {
    # Ensure the deployed app does not accidentally use the development Qt DLLs or plugins.
    $env:QT_PLUGIN_PATH = ''
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
}
# Verify the real Windows platform, including its system font fallback.
$arguments = @('--platform', 'windows', '--smoke-test', '--data-dir', ('"' + $dataDirectory + '"'), '--screenshot', ('"' + $screenshot + '"'))
$process = Start-Process -FilePath $application -ArgumentList $arguments -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput (Join-Path $verificationRoot "$suffix.stdout.log") -RedirectStandardError (Join-Path $verificationRoot "$suffix.stderr.log")
if ($process.ExitCode -ne 0) {
    Get-Content -LiteralPath (Join-Path $verificationRoot "$suffix.stderr.log")
    throw "Desktop startup verification failed with exit code $($process.ExitCode)."
}
if (-not (Test-Path -LiteralPath $screenshot)) { throw 'No desktop screenshot was produced.' }
if (-not (Test-Path -LiteralPath (Join-Path $dataDirectory 'shared_ledger.sqlite3'))) { throw 'No SQLite data file was created.' }
Write-Output "Desktop startup passed: $application"
Write-Output "Screenshot: $screenshot"
