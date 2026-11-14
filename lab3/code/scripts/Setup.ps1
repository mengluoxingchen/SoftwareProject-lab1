[CmdletBinding()]
param()

. (Join-Path $PSScriptRoot 'Tools.ps1')
$tools = Get-LabTools
$downloadRoot = Join-Path $tools.Root '.tools\downloads'
$qtInstallRoot = Join-Path $tools.Root '.tools\qt\6.8.3\msvc2022_64'
New-Item -ItemType Directory -Path $downloadRoot, $qtInstallRoot -Force | Out-Null

# The Qt SDK stays inside the project. No machine PATH or registry settings are changed.
$repository = 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_683/qt6_683/qt.qt6.683.win64_msvc2022_64/'
$archiveName = '6.8.3-0-202503201308qtbase-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z'
$archivePath = Join-Path $downloadRoot $archiveName
$configPath = Join-Path $tools.Qt 'lib\cmake\Qt6\Qt6Config.cmake'
$expectedHash = 'ab0bd2551618f7128ef30c6d74fe45b6a76e2296'

if (-not (Test-Path -LiteralPath $configPath)) {
    $ProgressPreference = 'SilentlyContinue'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Output 'Reading the official Qt package checksum...'
    $checksumResponse = Invoke-WebRequest -Uri ($repository + $archiveName + '.sha1') -UseBasicParsing -TimeoutSec 60
    $checksumText = if ($checksumResponse.Content -is [byte[]]) {
        [System.Text.Encoding]::ASCII.GetString($checksumResponse.Content)
    } else { [string]$checksumResponse.Content }
    $publishedHash = $checksumText.Trim().ToLowerInvariant()
    if ($publishedHash -ne $expectedHash) { throw 'The official Qt archive checksum differs from the pinned package.' }
    $hasVerifiedArchive = (Test-Path -LiteralPath $archivePath) -and ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA1).Hash.ToLowerInvariant() -eq $expectedHash)
    if (-not $hasVerifiedArchive) {
        Write-Output 'Downloading the Qt 6.8.3 MSVC x64 base SDK (about 38 MB)...'
        Invoke-WebRequest -Uri ($repository + $archiveName) -OutFile $archivePath -UseBasicParsing -TimeoutSec 600
    }
    if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA1).Hash.ToLowerInvariant() -ne $expectedHash) {
        throw 'Qt archive checksum mismatch; the package will not be extracted.'
    }
    Write-Output 'Extracting Qt with Windows tar...'
    & "$env:SystemRoot\System32\tar.exe" -xf $archivePath -C $qtInstallRoot
    if ($LASTEXITCODE -ne 0) { throw 'Qt extraction failed.' }
    if (-not (Test-Path -LiteralPath $configPath)) { throw "Qt package layout differs from the expected SDK location: $configPath" }
}

if (Test-Path -LiteralPath $archivePath) {
    if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA1).Hash.ToLowerInvariant() -ne $expectedHash) {
        throw 'The cached Qt archive does not match the verified package.'
    }
    @{
        qtVersion = '6.8.3'
        architecture = 'msvc2022_64'
        archive = $archiveName
        officialSha1 = $expectedHash
        localSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        source = $repository
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $downloadRoot 'qt-package-manifest.json') -Encoding UTF8
}

$licenseRoot = Join-Path $tools.Root 'licenses\qt'
New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
foreach ($license in @(
    @{ Name = 'LGPL-3.0.txt'; Url = 'https://www.gnu.org/licenses/lgpl-3.0.txt' },
    @{ Name = 'GPL-3.0.txt'; Url = 'https://www.gnu.org/licenses/gpl-3.0.txt' }
)) {
    $licensePath = Join-Path $licenseRoot $license.Name
    if (-not (Test-Path -LiteralPath $licensePath)) {
        Invoke-WebRequest -Uri $license.Url -OutFile $licensePath -UseBasicParsing -TimeoutSec 60
    }
}

$tools = Initialize-LabEnvironment
& $tools.CMake --version
& (Join-Path $tools.Qt 'bin\qmake.exe') -query QT_VERSION
if (Test-Path -LiteralPath $tools.Git) { & $tools.Git --version }
Write-Output 'Environment is ready. Run scripts\Build.ps1 next.'

