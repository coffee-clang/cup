# Purpose: Validates one completed Windows release candidate, native
# executable, and generated installer.

param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseDir,
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [Parameter(Mandatory = $true)]
    [string]$SourceSha
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = (Get-Location).Path
$ReleaseDir = (Resolve-Path -LiteralPath $ReleaseDir).Path
$originalLocation = $projectRoot

# Run child PowerShell scripts without turning expected native stderr into a terminating error.
function Invoke-PowerShellScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    $id = [Guid]::NewGuid().ToString('N')
    $stdoutPath = Join-Path $env:RUNNER_TEMP "cup-powershell-$PID-$id.stdout"
    $stderrPath = Join-Path $env:RUNNER_TEMP "cup-powershell-$PID-$id.stderr"

    try {
        $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`""
        $process = Start-Process -FilePath 'powershell.exe' `
            -ArgumentList $arguments `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -WorkingDirectory $WorkingDirectory `
            -NoNewWindow `
            -Wait `
            -PassThru

        $output = @()
        if (Test-Path -LiteralPath $stdoutPath) {
            $output += @(Get-Content -LiteralPath $stdoutPath)
        }
        if (Test-Path -LiteralPath $stderrPath) {
            $output += @(Get-Content -LiteralPath $stderrPath)
        }

        return [PSCustomObject]@{
            ExitCode = $process.ExitCode
            Output = $output
        }
    } finally {
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

# Verify that each checksum file names exactly the expected immutable assets.
function Get-NextTestVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CurrentVersion
    )

    if ($CurrentVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw "Invalid release version for update fixture: $CurrentVersion"
    }
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $patch = [int]$Matches[3]
    $candidates = @(
        "$major.$minor.$($patch + 1)",
        "$major.$($minor + 1).0",
        "$($major + 1).0.0"
    )
    foreach ($candidate in $candidates) {
        if ($candidate.Length -eq $CurrentVersion.Length) {
            return $candidate
        }
    }
    throw "Could not create a same-length update version from $CurrentVersion"
}

function Assert-ChecksumFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumFile,
        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedNames
    )

    $checksumPath = Join-Path $Directory $ChecksumFile
    if (-not (Test-Path -LiteralPath $checksumPath)) {
        throw "Missing checksum file: $ChecksumFile"
    }

    $seen = @{}
    foreach ($line in Get-Content -LiteralPath $checksumPath) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9A-Fa-f]{64})\s+\*?(.+)$') {
            throw "Invalid checksum line in ${ChecksumFile}: $line"
        }

        $expectedHash = $Matches[1].ToLowerInvariant()
        $name = $Matches[2]
        if ($name.Contains('/') -or $name.Contains('\\') -or $name.Contains('..')) {
            throw "Unsafe checksum entry in ${ChecksumFile}: $name"
        }
        if ($ExpectedNames -notcontains $name) {
            throw "Unexpected checksum entry in ${ChecksumFile}: $name"
        }

        $path = Join-Path $Directory $name
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Checksum entry references missing file: $name"
        }

        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Checksum mismatch for ${name}: expected $expectedHash, got $actualHash"
        }
        $seen[$name] = $true
    }

    foreach ($name in $ExpectedNames) {
        if (-not $seen.ContainsKey($name)) {
            throw "Missing checksum entry in ${ChecksumFile}: $name"
        }
    }
}

function Test-InstallerMetadataFailure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [string[]]$Lines,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedMessage
    )

    $fixtureName = "metadata-$Name"
    $fixture = Join-Path $root $fixtureName
    $profile = Join-Path $env:RUNNER_TEMP "cup-installer-$fixtureName-$PID"
    $saved = @{}
    foreach ($variable in @(
        'USERPROFILE',
        'CUP_INSTALL_ALLOW_INSECURE',
        'CUP_INSTALL_BASE_URL',
        'CUP_INSTALL_NO_PATH_PROMPT'
    )) {
        $item = Get-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
        $saved[$variable] = if ($null -eq $item) { $null } else { $item.Value }
    }

    try {
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $profile -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path $fixture | Out-Null
        New-Item -ItemType Directory -Path $profile | Out-Null

        foreach ($asset in @(
            'cup-windows-x64.exe',
            'packages.cfg',
            'install.cfg',
            'uninstall.ps1',
            'SHA256SUMS.common'
        )) {
            Copy-Item -LiteralPath (Join-Path $ReleaseDir $asset) -Destination $fixture
        }
        Set-Content -LiteralPath (Join-Path $fixture 'release.txt') `
            -Value $Lines -Encoding Ascii

        $platformNames = @('cup-windows-x64.exe', 'uninstall.ps1', 'release.txt')
        $checksumLines = foreach ($asset in $platformNames) {
            $hash = (Get-FileHash -LiteralPath (Join-Path $fixture $asset) `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $asset"
        }
        Set-Content -LiteralPath (Join-Path $fixture 'SHA256SUMS.windows-x64') `
            -Value $checksumLines -Encoding Ascii

        $env:USERPROFILE = $profile
        $env:CUP_INSTALL_ALLOW_INSECURE = '1'
        $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port/$fixtureName"
        $env:CUP_INSTALL_NO_PATH_PROMPT = '1'

        $result = Invoke-PowerShellScript `
            -ScriptPath (Join-Path $ReleaseDir 'install.ps1') `
            -WorkingDirectory $profile
        $status = $result.ExitCode
        $text = $result.Output -join [Environment]::NewLine
        if ($status -eq 0) {
            throw "Metadata diagnostic case unexpectedly succeeded: $Name"
        }
        if ($text -notlike "*$ExpectedMessage*") {
            throw "Metadata diagnostic case '$Name' was not explained:`n$text"
        }
    } finally {
        foreach ($variable in $saved.Keys) {
            if ($null -eq $saved[$variable]) {
                Remove-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
            } else {
                Set-Item -LiteralPath "Env:$variable" -Value $saved[$variable]
            }
        }
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $profile -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Validate the candidate checksums, metadata and native executable.
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.common" `
    -ExpectedNames @("packages.cfg", "install.cfg", "install.sh", "install.ps1")
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.windows-x64" `
    -ExpectedNames @("cup-windows-x64.exe", "uninstall.ps1", "release.txt")

$releaseMetadataPath = Join-Path $ReleaseDir "release.txt"
$releaseMetadata = @(Get-Content -LiteralPath $releaseMetadataPath)
$expectedMetadata = @(
    "format=1",
    "version=$Version",
    "commit=$SourceSha"
)
if ($releaseMetadata.Count -ne $expectedMetadata.Count) {
    throw "release.txt must contain exactly three lines"
}
for ($i = 0; $i -lt $expectedMetadata.Count; $i++) {
    if ($releaseMetadata[$i] -ne $expectedMetadata[$i]) {
        throw "Unexpected release.txt line $($i + 1): $($releaseMetadata[$i])"
    }
}

$binary = (Resolve-Path (Join-Path $ReleaseDir "cup-windows-x64.exe")).Path
$actual = & $binary --version
if ($LASTEXITCODE -ne 0) {
    throw "Release candidate --version failed with exit code $LASTEXITCODE"
}
if ($actual -ne "cup $Version") {
    throw "Unexpected version: $actual"
}

# Serve the candidate locally and smoke-test the generated installer in a fresh profile.
$port = 0
$root = $ReleaseDir
if ($env:CUP_TEST_CONFIGURATION) {
    $configuration = $env:CUP_TEST_CONFIGURATION
} else {
    $configuration = "development"
}
$helper = Join-Path $projectRoot "build\windows-x64\$configuration\tests\helpers\network-helper.exe"
if (-not (Test-Path -LiteralPath $helper)) {
    throw "HTTP test helper is not built: $helper"
}
$ready = Join-Path $env:RUNNER_TEMP "cup-http-ready-$PID"
Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
$serverArgs = @('http-server', '--root', $root, '--port', "$port", '--ready-file', $ready)
$server = Start-Process -FilePath $helper `
    -ArgumentList $serverArgs `
    -PassThru `
    -WindowStyle Hidden
$testProfile = Join-Path $env:RUNNER_TEMP "cup-installer-profile-$PID"
$originalEnvironment = @{}
foreach ($name in @(
    "USERPROFILE",
    "CUP_INSTALL_ALLOW_INSECURE",
    "CUP_INSTALL_BASE_URL",
    "CUP_INSTALL_NO_PATH_PROMPT"
)) {
    $item = Get-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    $originalEnvironment[$name] = if ($null -eq $item) { $null } else { $item.Value }
}

try {
    $serverReady = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            if (-not (Test-Path -LiteralPath $ready)) {
                throw "not ready"
            }
            $portText = (Get-Content -LiteralPath $ready -Raw).Trim()
            $parsedPort = 0
            if (-not [int]::TryParse($portText, [ref]$parsedPort) -or
                $parsedPort -lt 1 -or $parsedPort -gt 65535) {
                throw "invalid server port"
            }
            $port = $parsedPort
            Invoke-WebRequest -UseBasicParsing `
                -Uri "http://127.0.0.1:$port/release.txt" | Out-Null
            $serverReady = $true
            break
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $serverReady) {
        throw "HTTP test helper did not become ready"
    }

    Test-InstallerMetadataFailure -Name 'invalid-version' -Lines @(
        'format=1',
        'version=0.2',
        "commit=$SourceSha"
    ) -ExpectedMessage "release metadata version is invalid; expected 'MAJOR.MINOR.PATCH'"
    Test-InstallerMetadataFailure -Name 'version-mismatch' -Lines @(
        'format=1',
        'version=0.2.1',
        "commit=$SourceSha"
    ) -ExpectedMessage "release metadata version mismatch: expected '$Version', received '0.2.1'"
    Test-InstallerMetadataFailure -Name 'commit-mismatch' -Lines @(
        'format=1',
        "version=$Version",
        'commit=fedcba9876543210fedcba9876543210fedcba98'
    ) -ExpectedMessage (
        "release metadata commit mismatch: expected '$SourceSha', received " +
        "'fedcba9876543210fedcba9876543210fedcba98'"
    )

    if (Test-Path -LiteralPath $testProfile) {
        Remove-Item -LiteralPath $testProfile -Recurse -Force
    }
    New-Item -ItemType Directory -Path $testProfile | Out-Null

    $env:USERPROFILE = $testProfile
    $env:CUP_INSTALL_ALLOW_INSECURE = "1"
    $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port"
    $env:CUP_INSTALL_NO_PATH_PROMPT = "1"

    $installResult = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $testProfile
    $installText = $installResult.Output -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($installText)) {
        Write-Host $installText
    }
    if ($installResult.ExitCode -ne 0) {
        throw "Windows installer failed with exit code $($installResult.ExitCode)`n$installText"
    }

    # Exercise the installed release from outside the source checkout, matching the POSIX test.
    Set-Location -LiteralPath $testProfile
    $installed = Join-Path $env:USERPROFILE ".cup\bin\cup.exe"
    if (-not (Test-Path -LiteralPath $installed -PathType Leaf)) {
        throw "Windows installer did not create $installed"
    }
    $installedVersion = & $installed --version
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup --version failed with exit code $LASTEXITCODE"
    }
    if ($installedVersion -ne "cup $Version") {
        throw "Unexpected installed version: $installedVersion"
    }
    $candidateHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    $installedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($installedHash -ne $candidateHash) {
        throw "Installed cup does not match the tested release candidate"
    }
    $doctorOutput = @(& $installed doctor 2>&1)
    $doctorStatus = $LASTEXITCODE
    $doctorText = $doctorOutput -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($doctorText)) {
        Write-Host $doctorText
    }
    if ($doctorStatus -ne 0) {
        throw "Installed cup doctor failed with exit code $doctorStatus`n$doctorText"
    }
    if ($doctorText -like "*development cup assets*" -or
        $doctorText -like "*development catalog*") {
        throw "Official Windows installation unexpectedly used development cup assets"
    }
    if ($doctorText -notlike "*Doctor found no issues.*") {
        throw "Installed cup doctor did not report a healthy release installation"
    }

    # Repair may recreate mutable runtime paths, but it must preserve the installed
    # executable exactly as POSIX repair preserves ~/.cup/bin/cup.
    $binaryHashBeforeRepair = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    $stagingDirectory = Join-Path $env:USERPROFILE ".cup\staging"
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force -ErrorAction SilentlyContinue
    $repairOutput = @(& $installed repair 2>&1)
    $repairStatus = $LASTEXITCODE
    $repairText = $repairOutput -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($repairText)) {
        Write-Host $repairText
    }
    if ($repairStatus -ne 0) {
        throw "Installed cup repair failed with exit code $repairStatus`n$repairText"
    }
    if (-not (Test-Path -LiteralPath $stagingDirectory -PathType Container)) {
        throw "cup repair did not recreate the staging directory"
    }
    $binaryHashAfterRepair = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($binaryHashAfterRepair -ne $binaryHashBeforeRepair) {
        throw "cup repair replaced the running executable"
    }
    $versionAfterRepair = & $installed --version
    if ($LASTEXITCODE -ne 0 -or $versionAfterRepair -ne "cup $Version") {
        throw "Installed cup was not usable after repair"
    }

    # An official installation can restore its packaged uninstall asset without changing cup.exe.
    $uninstallAsset = Join-Path $env:USERPROFILE ".cup\helpers\uninstall.ps1"
    Remove-Item -LiteralPath $uninstallAsset -Force
    $assetRepairOutput = @(& $installed repair 2>&1)
    $assetRepairStatus = $LASTEXITCODE
    $assetRepairText = $assetRepairOutput -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($assetRepairText)) {
        Write-Host $assetRepairText
    }
    if ($assetRepairStatus -ne 0) {
        throw "Installed cup asset repair failed with exit code $assetRepairStatus`n$assetRepairText"
    }
    if ($assetRepairText -notlike "*Restoring uninstall script.*") {
        throw "Installed cup repair did not report restoring uninstall.ps1"
    }
    if (-not (Test-Path -LiteralPath $uninstallAsset -PathType Leaf)) {
        throw "Installed cup repair did not restore uninstall.ps1"
    }
    if (-not (Get-Item -LiteralPath $uninstallAsset).IsReadOnly) {
        throw "Installed cup repair did not restore read-only uninstall.ps1"
    }
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
        $binaryHashBeforeRepair) {
        throw "Installed cup asset repair changed cup.exe"
    }

    # A completion marker is accepted only for a complete installed generation. The failed
    # recovery must preserve both cup.exe and the staging evidence.
    $bootstrapStaging = Join-Path $env:USERPROFILE ".cup\.bootstrap"
    $committedMarker = Join-Path $bootstrapStaging "committed"
    $updateHelper = Join-Path $env:USERPROFILE ".cup\helpers\cup-update-helper.exe"
    $savedUpdateHelper = Join-Path $env:USERPROFILE "saved-cup-update-helper.exe"
    New-Item -ItemType Directory -Path $bootstrapStaging | Out-Null
    New-Item -ItemType File -Path $committedMarker | Out-Null
    Move-Item -LiteralPath $updateHelper -Destination $savedUpdateHelper
    $incompleteResult = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $testProfile
    if ($incompleteResult.ExitCode -eq 0) {
        throw "Incomplete committed Windows bootstrap staging unexpectedly succeeded"
    }
    if (($incompleteResult.Output -join "`n") -notlike
        "*completed bootstrap staging does not match a complete installed generation*") {
        throw "Incomplete committed Windows bootstrap failure was not explained"
    }
    if (-not (Test-Path -LiteralPath $committedMarker -PathType Leaf)) {
        throw "Incomplete committed Windows bootstrap staging was not preserved"
    }
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
        $binaryHashBeforeRepair) {
        throw "Incomplete committed Windows bootstrap recovery changed cup.exe"
    }
    Move-Item -LiteralPath $savedUpdateHelper -Destination $updateHelper

    # Reinstall the same tested candidate, complete cleanup and keep the executable valid.
    $reinstallResult = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $testProfile
    $reinstallText = $reinstallResult.Output -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($reinstallText)) {
        Write-Host $reinstallText
    }
    if ($reinstallResult.ExitCode -ne 0) {
        throw "Windows reinstall failed with exit code $($reinstallResult.ExitCode)`n$reinstallText"
    }
    if (Test-Path -LiteralPath $bootstrapStaging) {
        throw "Windows reinstall did not remove completed bootstrap staging"
    }
    $binaryHashAfterReinstall = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($binaryHashAfterReinstall -ne $candidateHash) {
        throw "Windows reinstall changed the tested release executable"
    }
    $versionAfterReinstall = & $installed --version
    if ($LASTEXITCODE -ne 0 -or $versionAfterReinstall -ne "cup $Version") {
        throw "Installed cup was not usable after reinstall"
    }

    # A local immutable release fixture exercises the complete detached update path. The binary
    # patcher changes only same-length embedded version strings, preserving the tested executable.
    $nextVersion = Get-NextTestVersion -CurrentVersion $Version
    $updateRoot = Join-Path $ReleaseDir "update-fixture"
    $versionRoot = Join-Path $updateRoot $nextVersion
    $patchHelper = Join-Path $projectRoot `
        "build\windows-x64\$configuration\tests\helpers\binary-patch.exe"
    if (-not (Test-Path -LiteralPath $patchHelper -PathType Leaf)) {
        throw "Binary patch helper is not built: $patchHelper"
    }
    Remove-Item -LiteralPath $updateRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $versionRoot -Force | Out-Null

    foreach ($asset in @("packages.cfg", "install.cfg", "install.sh", "install.ps1", "uninstall.ps1")) {
        Copy-Item -LiteralPath (Join-Path $ReleaseDir $asset) -Destination $versionRoot
    }
    $updatedBinary = Join-Path $versionRoot "cup-windows-x64.exe"
    & $patchHelper $binary $updatedBinary $Version $nextVersion | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Binary patch helper failed with exit code $LASTEXITCODE"
    }

    $updatedMetadata = @(
        "format=1",
        "version=$nextVersion",
        "commit=$SourceSha"
    )
    Set-Content -LiteralPath (Join-Path $versionRoot "release.txt") `
        -Value $updatedMetadata -Encoding ascii
    Copy-Item -LiteralPath (Join-Path $versionRoot "release.txt") `
        -Destination (Join-Path $updateRoot "release.txt")

    $commonLines = foreach ($asset in @("packages.cfg", "install.cfg", "install.sh", "install.ps1")) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $versionRoot $asset) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $asset"
    }
    Set-Content -LiteralPath (Join-Path $versionRoot "SHA256SUMS.common") `
        -Value $commonLines -Encoding ascii
    $platformLines = foreach ($asset in @("cup-windows-x64.exe", "uninstall.ps1", "release.txt")) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $versionRoot $asset) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $asset"
    }
    Set-Content -LiteralPath (Join-Path $versionRoot "SHA256SUMS.windows-x64") `
        -Value $platformLines -Encoding ascii

    $updatedVersionOutput = & $updatedBinary --version
    if ($LASTEXITCODE -ne 0 -or $updatedVersionOutput -ne "cup $nextVersion") {
        throw "Patched update executable did not expose version $nextVersion"
    }

    $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port/update-fixture"
    $updateOutput = @(& $installed update cup 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "cup update cup failed with exit code $LASTEXITCODE`n$($updateOutput -join "`n")"
    }
    $updateText = $updateOutput -join "`n"
    if ($updateText -notlike "*Verified update from cup $Version to $nextVersion scheduled.*") {
        throw "cup update cup did not report the scheduled version transition`n$updateText"
    }

    $updateResult = Join-Path $env:USERPROFILE ".cup\cup-update-result.txt"
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline -and
        -not (Test-Path -LiteralPath $updateResult -PathType Leaf)) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $updateResult -PathType Leaf)) {
        throw "cup update helper did not publish a result"
    }
    $resultText = Get-Content -LiteralPath $updateResult -Raw
    foreach ($expected in @("status=success", "error=0", "version=$nextVersion")) {
        if ($resultText -notmatch "(?m)^$([regex]::Escape($expected))`r?$" ) {
            throw "cup update result is missing '$expected'`n$resultText"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $env:USERPROFILE ".cup\transaction.txt")) {
        throw "successful cup update left a transaction journal"
    }
    $installedUpdatedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    $fixtureUpdatedHash = (Get-FileHash -LiteralPath $updatedBinary -Algorithm SHA256).Hash
    if ($installedUpdatedHash -ne $fixtureUpdatedHash) {
        throw "installed cup does not match the verified update executable"
    }
    $updatedInstalledVersion = & $installed --version
    if ($LASTEXITCODE -ne 0 -or $updatedInstalledVersion -ne "cup $nextVersion") {
        throw "installed cup was not usable after update"
    }
    $updatedDoctorOutput = @(& $installed doctor 2>&1)
    $updatedDoctorStatus = $LASTEXITCODE
    $updatedDoctorText = $updatedDoctorOutput -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($updatedDoctorText)) {
        Write-Host $updatedDoctorText
    }
    if ($updatedDoctorStatus -ne 0) {
        throw "updated cup doctor failed with exit code $updatedDoctorStatus`n$updatedDoctorText"
    }
    if ($updatedDoctorText -like "*development cup assets*" -or
        $updatedDoctorText -like "*development catalog*") {
        throw "Updated Windows release unexpectedly used development cup assets"
    }
    $staleUpdate = @(Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE ".cup\staging") `
        -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "cup-update-*" })
    if ($staleUpdate.Count -ne 0) {
        throw "successful cup update left staging behind: $($staleUpdate[0].FullName)"
    }

    # The assembled candidate performs its detached uninstall smoke test.
    $uninstallOutput = & $installed uninstall --yes 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup uninstall failed with exit code $LASTEXITCODE"
    }
    if (($uninstallOutput -join "`n") -notlike
        "*Uninstall started. The PATH entry was not removed.*") {
        throw "Installed cup uninstall did not report detached removal"
    }
    $cupRoot = Join-Path $env:USERPROFILE ".cup"
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $residues = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        $residues = @(Get-ChildItem -LiteralPath $env:USERPROFILE -Force `
            -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall.*" })
        if (-not (Test-Path -LiteralPath $cupRoot) -and $residues.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $cupRoot) {
        throw "Release uninstall did not remove the cup root"
    }
    $residues = @(Get-ChildItem -LiteralPath $env:USERPROFILE -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall.*" })
    if ($residues.Count -ne 0) {
        throw "Release uninstall left staging behind: $($residues[0].FullName)"
    }
} finally {
    Set-Location -LiteralPath $originalLocation
    foreach ($name in $originalEnvironment.Keys) {
        $value = $originalEnvironment[$name]
        if ($null -eq $value) {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item -LiteralPath "Env:$name" -Value $value
        }
    }

    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $server.WaitForExit()
    }
    Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testProfile -Recurse -Force -ErrorAction SilentlyContinue
}
