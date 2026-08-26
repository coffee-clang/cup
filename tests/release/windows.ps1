# Validates one completed Windows release candidate, native
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

$originalLocation = (Get-Location).Path
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$ReleaseDir = (Resolve-Path -LiteralPath $ReleaseDir).Path
$temporaryParent = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [System.IO.Path]::GetTempPath()
} else {
    $env:RUNNER_TEMP
}
$testWorkRoot = $null

function Stop-ReleaseProcessTree {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [ValidateRange(1, 60000)]
        [int]$WaitMilliseconds = 10000
    )

    if ($Process.HasExited) {
        return
    }

    $treeStopped = $false
    try {
        & taskkill.exe /PID $Process.Id /T /F 2>&1 | Out-Null
        $treeStopped = ($LASTEXITCODE -eq 0)
    } catch {
        $treeStopped = $false
    }
    if (-not $treeStopped -and -not $Process.HasExited) {
        try {
            $Process.Kill()
        } catch {
            # Cleanup is best effort.
        }
    }
    if (-not $Process.WaitForExit($WaitMilliseconds) -and -not $Process.HasExited) {
        try {
            $Process.Kill()
        } catch {
            # Cleanup is best effort.
        }
        [void]$Process.WaitForExit($WaitMilliseconds)
    }
}

function Write-CanonicalAsciiLines {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Lines
    )

    $text = if ($Lines.Count -eq 0) { '' } else { ($Lines -join "`n") + "`n" }
    [IO.File]::WriteAllText($Path, $text, [Text.Encoding]::ASCII)
}

function Get-CanonicalAsciiLines {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release text asset is not a regular file: $($item.Name)"
    }

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -eq 0 -or $bytes[$bytes.Length - 1] -ne 10) {
        throw "Release text asset is not canonical: $($item.Name)"
    }
    foreach ($byte in $bytes) {
        if ($byte -ne 10 -and ($byte -lt 32 -or $byte -gt 126)) {
            throw "Release text asset contains non-canonical bytes: $($item.Name)"
        }
    }

    $parts = [Text.Encoding]::ASCII.GetString($bytes).Split([char]10)
    if ($parts[$parts.Length - 1].Length -ne 0) {
        throw "Release text asset is not canonical: $($item.Name)"
    }
    return @($parts[0..($parts.Length - 2)])
}

# Run child PowerShell scripts while preserving expected stderr and exit status.
function Invoke-PowerShellScript {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScriptPath,
        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    $id = [Guid]::NewGuid().ToString('N')
    $stdoutPath = Join-Path $testWorkRoot "powershell-$id.stdout"
    $stderrPath = Join-Path $testWorkRoot "powershell-$id.stderr"

    $process = $null
    try {
        $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`""
        $process = Start-Process -FilePath 'powershell.exe' `
            -ArgumentList $arguments `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -WorkingDirectory $WorkingDirectory `
            -NoNewWindow `
            -PassThru
        [void]$process.Handle
        if (-not $process.WaitForExit(300000)) {
            Stop-ReleaseProcessTree -Process $process
            throw "PowerShell release fixture timed out: $ScriptPath"
        }
        $process.WaitForExit()

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
        if ($null -ne $process) {
            $process.Dispose()
        }
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

# Verify that the candidate directory contains only the exact public asset set.
function Assert-ExactCandidateFiles {
    $expected = @(
        'packages.cfg', 'install.cfg', 'release.txt', 'provenance.txt',
        'THIRD_PARTY_NOTICES.txt', 'install.sh', 'install.ps1',
        'SHA256SUMS.common', 'SHA256SUMS.windows-x64', 'cup-windows-x64.exe'
    )
    $entries = @(Get-ChildItem -LiteralPath $ReleaseDir -Force)
    if ($entries.Count -ne $expected.Count) {
        throw "Windows release candidate does not contain the exact public asset set"
    }
    foreach ($entry in $entries) {
        if (($expected -cnotcontains $entry.Name) -or $entry.PSIsContainer -or
            ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $entry.Length -le 0) {
            throw "Invalid Windows release candidate entry: $($entry.Name)"
        }
    }
}

# Verify one canonical checksum document and every referenced digest.
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

    $lines = @(Get-CanonicalAsciiLines -Path $checksumPath)
    if ($lines.Count -ne $ExpectedNames.Count) {
        throw "Unexpected checksum entry count in ${ChecksumFile}"
    }

    for ($index = 0; $index -lt $ExpectedNames.Count; $index++) {
        $expectedName = $ExpectedNames[$index]
        $match = [regex]::Match($lines[$index], '^([0-9a-f]{64})  ([^\s]+)$')
        if (-not $match.Success -or $match.Groups[2].Value -cne $expectedName) {
            throw "Non-canonical checksum entry in ${ChecksumFile}: $expectedName"
        }

        $expectedHash = $match.Groups[1].Value
        $path = Join-Path $Directory $expectedName
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Checksum entry references missing file: $expectedName"
        }

        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -cne $expectedHash) {
            throw "Checksum mismatch for ${expectedName}: expected $expectedHash, got $actualHash"
        }
    }
}

function Assert-ChecksumFixtureRejected {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory,
        [Parameter(Mandatory = $true)]
        [string]$ChecksumFile,
        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedNames,
        [Parameter(Mandatory = $true)]
        [string]$CaseName,
        [Parameter(Mandatory = $true)]
        [string]$ExpectedMessage
    )

    $accepted = $false
    try {
        Assert-ChecksumFile -Directory $Directory -ChecksumFile $ChecksumFile `
            -ExpectedNames $ExpectedNames
        $accepted = $true
    } catch {
        if (-not $_.Exception.Message.Contains($ExpectedMessage)) {
            throw "Checksum fixture '$CaseName' failed for the wrong reason: $($_.Exception.Message)"
        }
    }
    if ($accepted) {
        throw "Checksum validator accepted non-canonical fixture: $CaseName"
    }
}

function Test-ChecksumFileAssertions {
    $fixture = Join-Path $temporaryParent `
        ("cup-release-checksum-test-" + [Guid]::NewGuid().ToString('N'))
    $checksumFile = 'SHA256SUMS.fixture'
    $checksumPath = Join-Path $fixture $checksumFile
    $expectedNames = @('asset-a.txt', 'asset-b.txt')

    try {
        New-Item -ItemType Directory -Path $fixture | Out-Null
        Write-CanonicalAsciiLines -Path (Join-Path $fixture $expectedNames[0]) `
            -Lines @('asset-a')
        Write-CanonicalAsciiLines -Path (Join-Path $fixture $expectedNames[1]) `
            -Lines @('asset-b')

        $hashA = (Get-FileHash -LiteralPath (Join-Path $fixture $expectedNames[0]) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $hashB = (Get-FileHash -LiteralPath (Join-Path $fixture $expectedNames[1]) `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        $validLines = @(
            "$hashA  $($expectedNames[0])",
            "$hashB  $($expectedNames[1])"
        )

        Write-CanonicalAsciiLines -Path $checksumPath -Lines $validLines
        Assert-ChecksumFile -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames

        Write-CanonicalAsciiLines -Path $checksumPath -Lines @(
            $validLines[0], $validLines[0])
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'duplicate entry' `
            -ExpectedMessage 'Non-canonical checksum entry'

        Write-CanonicalAsciiLines -Path $checksumPath -Lines @(
            "$hashA  ASSET-A.TXT", $validLines[1])
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'wrong-case filename' `
            -ExpectedMessage 'Non-canonical checksum entry'

        Write-CanonicalAsciiLines -Path $checksumPath -Lines @(
            "$($hashA.ToUpperInvariant())  $($expectedNames[0])", $validLines[1])
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'uppercase hash' `
            -ExpectedMessage 'Non-canonical checksum entry'

        Write-CanonicalAsciiLines -Path $checksumPath -Lines @(
            "$hashA $($expectedNames[0])", $validLines[1])
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'non-canonical spacing' `
            -ExpectedMessage 'Non-canonical checksum entry'

        Write-CanonicalAsciiLines -Path $checksumPath -Lines @(
            $validLines[1], $validLines[0])
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'wrong ordering' `
            -ExpectedMessage 'Non-canonical checksum entry'

        [IO.File]::WriteAllText(
            $checksumPath, ($validLines -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'CRLF bytes' `
            -ExpectedMessage 'contains non-canonical bytes'

        [IO.File]::WriteAllText(
            $checksumPath, ($validLines -join "`n"), [Text.Encoding]::ASCII)
        Assert-ChecksumFixtureRejected -Directory $fixture -ChecksumFile $checksumFile `
            -ExpectedNames $expectedNames -CaseName 'missing final LF' `
            -ExpectedMessage 'is not canonical'
    } finally {
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
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
    $profile = Join-Path $testWorkRoot "installer-$fixtureName"
    $saved = @{}
    foreach ($variable in @(
        'USERPROFILE',
        'CUP_INSTALL_ALLOW_INSECURE',
        'CUP_INSTALL_BASE_URL'
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
            'install.sh',
            'install.ps1',
            'SHA256SUMS.common'
        )) {
            Copy-Item -LiteralPath (Join-Path $ReleaseDir $asset) -Destination $fixture
        }
        Write-CanonicalAsciiLines -Path (Join-Path $fixture 'release.txt') `
            -Lines $Lines

        $platformNames = @('cup-windows-x64.exe', 'release.txt', 'SHA256SUMS.common')
        $checksumLines = foreach ($asset in $platformNames) {
            $hash = (Get-FileHash -LiteralPath (Join-Path $fixture $asset) `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $asset"
        }
        Write-CanonicalAsciiLines `
            -Path (Join-Path $fixture 'SHA256SUMS.windows-x64') `
            -Lines $checksumLines

        $env:USERPROFILE = $profile
        $env:CUP_INSTALL_ALLOW_INSECURE = '1'
        $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port/$fixtureName"

        $result = Invoke-PowerShellScript `
            -ScriptPath (Join-Path $ReleaseDir 'install.ps1') `
            -WorkingDirectory $profile
        $status = $result.ExitCode
        $text = $result.Output -join [Environment]::NewLine
        if ($status -eq 0) {
            throw "Metadata diagnostic case unexpectedly succeeded: $Name"
        }
        $normalizedText = [regex]::Replace($text, '\s+', ' ')
        $normalizedExpected = [regex]::Replace($ExpectedMessage, '\s+', ' ')
        if ($normalizedText.IndexOf(
                $normalizedExpected,
                [StringComparison]::Ordinal
            ) -lt 0) {
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

# A final partial transfer window must be checked when EOF arrives. Every
# individual read below completes within the one-second test threshold, so only
# the accumulated final-window check can reject the response.
function Test-InstallerFinalLowSpeedWindow {
    $fixture = Join-Path $testWorkRoot 'final-low-speed'
    $profile = Join-Path $fixture 'profile'
    $ready = Join-Path $fixture 'ready.txt'
    $installer = Join-Path $fixture 'install.ps1'
    $serverProcess = $null
    $saved = @{}

    if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
        throw "Low-speed HTTP helper is missing: $helper"
    }
    New-Item -ItemType Directory -Path $profile -Force | Out-Null

    $installerText = [IO.File]::ReadAllText((Join-Path $ReleaseDir 'install.ps1'))
    $installerText = $installerText.Replace('$LowSpeedSeconds = 30', '$LowSpeedSeconds = 1')
    if ($installerText -notlike '*$LowSpeedSeconds = 1*') {
        throw 'Could not prepare the low-speed installer fixture'
    }
    [IO.File]::WriteAllText($installer, $installerText, [Text.Encoding]::UTF8)

    foreach ($variable in @(
        'USERPROFILE',
        'CUP_INSTALL_ALLOW_INSECURE',
        'CUP_INSTALL_BASE_URL'
    )) {
        $item = Get-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
        $saved[$variable] = if ($null -eq $item) { $null } else { $item.Value }
    }

    try {
        $serverArguments = "slow-http-server --ready-file `"$ready`""
        $serverProcess = Start-Process -FilePath $helper `
            -ArgumentList $serverArguments `
            -PassThru `
            -WindowStyle Hidden

        $slowPort = 0
        for ($attempt = 0; $attempt -lt 50; $attempt++) {
            if (Test-Path -LiteralPath $ready) {
                $portText = (Get-Content -LiteralPath $ready -Raw).Trim()
                if ([int]::TryParse($portText, [ref]$slowPort) -and $slowPort -gt 0) {
                    break
                }
            }
            Start-Sleep -Milliseconds 100
        }
        if ($slowPort -le 0) {
            throw 'Low-speed HTTP fixture did not become ready'
        }

        $env:USERPROFILE = $profile
        $env:CUP_INSTALL_ALLOW_INSECURE = '1'
        $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$slowPort"

        $result = Invoke-PowerShellScript -ScriptPath $installer -WorkingDirectory $profile
        $text = $result.Output -join [Environment]::NewLine
        if ($result.ExitCode -eq 0) {
            throw 'Final low-speed response unexpectedly succeeded'
        }
        if ($text -notlike '*remained below the minimum transfer speed*') {
            throw "Final low-speed response was not rejected by the transfer policy:`n$text"
        }
    } finally {
        foreach ($variable in $saved.Keys) {
            if ($null -eq $saved[$variable]) {
                Remove-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
            } else {
                Set-Item -LiteralPath "Env:$variable" -Value $saved[$variable]
            }
        }
        if ($null -ne $serverProcess) {
            if (-not $serverProcess.HasExited) {
                Stop-ReleaseProcessTree -Process $serverProcess
            }
            $serverProcess.Dispose()
        }
        Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# Validate the candidate checksums, metadata and native executable.
Test-ChecksumFileAssertions
Assert-ExactCandidateFiles
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.common" `
    -ExpectedNames @("packages.cfg", "install.cfg", "install.sh", "install.ps1")
Assert-ChecksumFile -Directory $ReleaseDir -ChecksumFile "SHA256SUMS.windows-x64" `
    -ExpectedNames @("cup-windows-x64.exe", "release.txt", "SHA256SUMS.common")

$releaseMetadataPath = Join-Path $ReleaseDir "release.txt"
$releaseMetadata = @(Get-CanonicalAsciiLines -Path $releaseMetadataPath)
$expectedMetadata = @(
    "format=1",
    "version=$Version",
    "commit=$SourceSha"
)
if ($releaseMetadata.Count -ne $expectedMetadata.Count) {
    throw "release.txt must contain exactly three lines"
}
for ($i = 0; $i -lt $expectedMetadata.Count; $i++) {
    if ($releaseMetadata[$i] -cne $expectedMetadata[$i]) {
        throw "Unexpected release.txt line $($i + 1): $($releaseMetadata[$i])"
    }
}

$binary = (Resolve-Path (Join-Path $ReleaseDir "cup-windows-x64.exe")).Path
$actual = & $binary --version
if ($LASTEXITCODE -ne 0) {
    throw "Release candidate --version failed with exit code $LASTEXITCODE"
}
if ($actual -cne "cup $Version") {
    throw "Unexpected version: $actual"
}

# Serve the candidate locally and smoke-test the generated installer in a fresh profile.
$port = 0
if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_SERVER_ROOT)) {
    throw "CUP_TEST_SERVER_ROOT is required"
}
$root = (Resolve-Path -LiteralPath $env:CUP_TEST_SERVER_ROOT).Path
if ($env:CUP_TEST_CONFIGURATION) {
    $configuration = $env:CUP_TEST_CONFIGURATION
} else {
    $configuration = "development"
}
$testBuildRoot = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_BUILD_ROOT)) {
    Join-Path $projectRoot 'build'
} else {
    $env:CUP_TEST_BUILD_ROOT
}
$helper = Join-Path $testBuildRoot "windows-x64\$configuration\tests\helpers\network-helper.exe"
if (-not (Test-Path -LiteralPath $helper)) {
    throw "HTTP test helper is not built: $helper"
}
$server = $null
$originalEnvironment = @{}

try {
    $testWorkRoot = Join-Path $temporaryParent `
        ("cup-release-test-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testWorkRoot | Out-Null

    $ready = Join-Path $testWorkRoot "http-ready"
    $serverArgs = "http-server --root `"$root`" --port $port --ready-file `"$ready`""
    $server = Start-Process -FilePath $helper `
        -ArgumentList $serverArgs `
        -PassThru `
        -WindowStyle Hidden
    $testProfile = Join-Path $testWorkRoot "installer profile"
    $foreignProfile = Join-Path $testWorkRoot "foreign-profile"
    foreach ($name in @(
        "USERPROFILE",
        "CUP_INSTALL_ALLOW_INSECURE",
        "CUP_INSTALL_BASE_URL"
    )) {
        $item = Get-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        $originalEnvironment[$name] = if ($null -eq $item) { $null } else { $item.Value }
    }

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

    # Prove the documented MSYS2/Git-Bash handoff with the real shell, cygpath and PowerShell.
    $shell = Get-Command sh.exe -ErrorAction Stop
    $cygpath = Get-Command cygpath.exe -ErrorAction Stop
    $shellProfile = Join-Path $testWorkRoot 'shell handoff profile'
    New-Item -ItemType Directory -Path $shellProfile | Out-Null
    $installSh = @(& $cygpath.Source -u (Join-Path $ReleaseDir 'install.sh'))
    if ($LASTEXITCODE -ne 0 -or $installSh.Count -ne 1) {
        throw 'cygpath could not translate the public shell installer path'
    }
    $env:USERPROFILE = $shellProfile
    $env:CUP_INSTALL_ALLOW_INSECURE = '1'
    $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port"
    $shellInstallOutput = @(& $shell.Source $installSh[0] 2>&1)
    $shellInstallStatus = $LASTEXITCODE
    if ($shellInstallStatus -ne 0) {
        throw (
            "Windows shell handoff failed with exit code $shellInstallStatus`n" +
            ($shellInstallOutput -join [Environment]::NewLine))
    }
    $shellInstalled = Join-Path $shellProfile '.cup\bin\cup.exe'
    $shellVersion = & $shellInstalled --version
    if ($LASTEXITCODE -ne 0 -or $shellVersion -cne "cup $Version") {
        throw 'Windows shell handoff did not install the expected cup version'
    }
    $candidateHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
    $shellHash = (Get-FileHash -LiteralPath $shellInstalled -Algorithm SHA256).Hash
    if ($shellHash -ne $candidateHash) {
        throw 'Windows shell handoff did not install the tested release candidate bytes'
    }
    Remove-Item -LiteralPath $shellProfile -Recurse -Force

    if (Test-Path -LiteralPath $testProfile) {
        Remove-Item -LiteralPath $testProfile -Recurse -Force
    }
    New-Item -ItemType Directory -Path $testProfile | Out-Null

    $env:USERPROFILE = $testProfile
    $env:CUP_INSTALL_ALLOW_INSECURE = "1"
    $env:CUP_INSTALL_BASE_URL = "http://127.0.0.1:$port"

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
    if ($installedVersion -cne "cup $Version") {
        throw "Unexpected installed version: $installedVersion"
    }
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

    # Preserve an unrelated .cup and validate the complete release lifecycle from
    # the stable .coffee-cup fallback.
    Remove-Item -LiteralPath $foreignProfile -Recurse -Force -ErrorAction SilentlyContinue
    $foreignPrimary = Join-Path $foreignProfile ".cup"
    New-Item -ItemType Directory -Path $foreignPrimary -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $foreignPrimary "foreign.txt") `
        -Value "unrelated" -Encoding Ascii
    $env:USERPROFILE = $foreignProfile
    $foreignInstall = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $foreignProfile
    if ($foreignInstall.ExitCode -ne 0) {
        throw (
            "Fallback-root installer failed with exit code $($foreignInstall.ExitCode)`n" +
            ($foreignInstall.Output -join [Environment]::NewLine))
    }
    $foreignRoot = Join-Path $foreignProfile ".coffee-cup"
    $foreignInstalled = Join-Path $foreignRoot "bin\cup.exe"
    $foreignMarker = @(Get-Content -LiteralPath (Join-Path $foreignRoot "root.txt"))
    if ($foreignMarker.Count -ne 3 -or
        $foreignMarker[0] -cne "format=1" -or
        $foreignMarker[1] -cne "product=coffee-clang/cup" -or
        $foreignMarker[2] -cne "layout=1") {
        throw "Fallback-root marker is invalid"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $foreignPrimary "foreign.txt") -PathType Leaf)) {
        throw "Installer modified the unrelated .cup directory"
    }
    $foreignVersion = & $foreignInstalled --version
    if ($LASTEXITCODE -ne 0 -or $foreignVersion -cne "cup $Version") {
        throw "Fallback-root cup was not usable"
    }
    $foreignDoctor = @(& $foreignInstalled doctor 2>&1)
    if ($LASTEXITCODE -ne 0 -or
        ($foreignDoctor -join [Environment]::NewLine) -notlike "*Doctor found no issues.*") {
        throw "Fallback-root cup doctor did not report a healthy installation"
    }
    $foreignUninstall = @(& $foreignInstalled uninstall --yes 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Fallback-root uninstall failed`n$($foreignUninstall -join "`n")"
    }
    $foreignDeadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $foreignResidues = @(Get-ChildItem -LiteralPath $foreignProfile -Force `
            -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall-*" })
        if (-not (Test-Path -LiteralPath $foreignRoot) -and $foreignResidues.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $foreignDeadline)
    if ((Test-Path -LiteralPath $foreignRoot) -or $foreignResidues.Count -ne 0) {
        throw "Fallback-root uninstall did not complete cleanly"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $foreignPrimary "foreign.txt") -PathType Leaf)) {
        throw "Fallback-root uninstall modified the unrelated .cup directory"
    }
    # A recognizable root with a corrupt ownership marker must block the installer
    # without selecting or creating the fallback root.
    $corruptProfile = Join-Path $testWorkRoot "corrupt-root-profile"
    $corruptRoot = Join-Path $corruptProfile ".cup"
    foreach ($directory in @("components", "staging", "cache")) {
        New-Item -ItemType Directory -Force -Path (Join-Path $corruptRoot $directory) |
            Out-Null
    }
    Write-CanonicalAsciiLines -Path (Join-Path $corruptRoot "state.txt") `
        -Lines @("format=1")
    Set-Content -LiteralPath (Join-Path $corruptRoot "root.txt") `
        -Value "corrupt" -Encoding Ascii
    $corruptStateHash = (Get-FileHash -LiteralPath (Join-Path $corruptRoot "state.txt") `
        -Algorithm SHA256).Hash
    $corruptMarkerHash = (Get-FileHash -LiteralPath (Join-Path $corruptRoot "root.txt") `
        -Algorithm SHA256).Hash
    $env:USERPROFILE = $corruptProfile
    $corruptInstall = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $corruptProfile
    if ($corruptInstall.ExitCode -eq 0 -or
        ($corruptInstall.Output -join [Environment]::NewLine) -notlike `
            "*cup root marker is invalid for the recognized root*") {
        throw "Windows installer accepted a recognized root with a corrupt marker"
    }
    if ((Get-FileHash -LiteralPath (Join-Path $corruptRoot "state.txt") `
            -Algorithm SHA256).Hash -ne $corruptStateHash -or
        (Get-FileHash -LiteralPath (Join-Path $corruptRoot "root.txt") `
            -Algorithm SHA256).Hash -ne $corruptMarkerHash) {
        throw "Windows installer modified the corrupt root"
    }
    if (Test-Path -LiteralPath (Join-Path $corruptProfile ".coffee-cup")) {
        throw "Windows installer created the fallback root after a corrupt marker"
    }

    # A superficially shaped uninstall sibling is not ownership proof. Installation ignores it
    # and must not modify it while selecting the normal canonical root.
    $residueProfile = Join-Path $testWorkRoot "unowned-residue-profile"
    $residueRoot = Join-Path $residueProfile ".cup-uninstall-fixture"
    New-Item -ItemType Directory -Force -Path (Join-Path $residueRoot "bin") | Out-Null
    Set-Content -LiteralPath (Join-Path $residueRoot "bin\cup.exe") `
        -Value "binary" -Encoding Ascii
    Write-CanonicalAsciiLines -Path (Join-Path $residueRoot "transaction.txt") -Lines @(
        "format=1",
        "operation=uninstall",
        "phase=detaching",
        "temporary_name=.cup-uninstall-fixture",
        "token=fixture",
        "stage=detach",
        "error=0"
    )
    $residueBinaryHash = (Get-FileHash -LiteralPath (Join-Path $residueRoot "bin\cup.exe") `
        -Algorithm SHA256).Hash
    $residueJournalHash = (Get-FileHash -LiteralPath (Join-Path $residueRoot "transaction.txt") `
        -Algorithm SHA256).Hash
    $env:USERPROFILE = $residueProfile
    $residueInstall = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $residueProfile
    if ($residueInstall.ExitCode -ne 0) {
        throw (
            "Windows installer was blocked by an unrelated uninstall sibling`n" +
            ($residueInstall.Output -join [Environment]::NewLine))
    }
    if ((Get-FileHash -LiteralPath (Join-Path $residueRoot "bin\cup.exe") `
            -Algorithm SHA256).Hash -ne $residueBinaryHash -or
        (Get-FileHash -LiteralPath (Join-Path $residueRoot "transaction.txt") `
            -Algorithm SHA256).Hash -ne $residueJournalHash) {
        throw "Windows installer modified an unrelated uninstall sibling"
    }
    $residueCup = Join-Path $residueProfile ".cup\bin\cup.exe"
    $residueVersion = & $residueCup --version
    if ($LASTEXITCODE -ne 0 -or $residueVersion -cne "cup $Version") {
        throw "Windows installer did not create a usable canonical root beside the unrelated sibling"
    }

    $env:USERPROFILE = $testProfile

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
    if ($LASTEXITCODE -ne 0 -or $versionAfterRepair -cne "cup $Version") {
        throw "Installed cup was not usable after repair"
    }

    # The update helper is derived from the installed executable, not a release asset. Repair
    # regenerates a missing copy without changing the running executable.
    $updateHelper = Join-Path $env:USERPROFILE ".cup\helpers\update-helper.exe"
    Remove-Item -LiteralPath $updateHelper -Force
    $helperRepairOutput = @(& $installed repair 2>&1)
    $helperRepairStatus = $LASTEXITCODE
    $helperRepairText = $helperRepairOutput -join [Environment]::NewLine
    if (-not [string]::IsNullOrEmpty($helperRepairText)) {
        Write-Host $helperRepairText
    }
    if ($helperRepairStatus -ne 0) {
        throw (
            "Installed cup helper repair failed with exit code " +
            "$helperRepairStatus`n$helperRepairText")
    }
    if ($helperRepairText -notlike
        "*Regenerated native update helper from the installed executable.*") {
        throw "Installed cup repair did not report regenerating update-helper.exe"
    }
    if (-not (Test-Path -LiteralPath $updateHelper -PathType Leaf)) {
        throw "Installed cup repair did not regenerate update-helper.exe"
    }
    if ((Get-FileHash -LiteralPath $updateHelper -Algorithm SHA256).Hash -ne
        $binaryHashBeforeRepair) {
        throw "Regenerated update-helper.exe does not match cup.exe"
    }
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
        $binaryHashBeforeRepair) {
        throw "Installed cup helper repair changed cup.exe"
    }

    # A malformed canonical journal blocks bootstrap before any managed mutation.
    # The failed transport preserves the journal evidence and the installed executable.
    $transaction = Join-Path $env:USERPROFILE ".cup\transaction.txt"
    [IO.File]::WriteAllText($transaction, "invalid=1`n", [Text.Encoding]::ASCII)
    $incompleteResult = Invoke-PowerShellScript `
        -ScriptPath (Join-Path $ReleaseDir "install.ps1") `
        -WorkingDirectory $testProfile
    if ($incompleteResult.ExitCode -eq 0) {
        throw "Windows bootstrap unexpectedly ignored a malformed canonical transaction"
    }
    $incompleteText = $incompleteResult.Output -join [Environment]::NewLine
    $normalizedIncompleteText = [regex]::Replace($incompleteText, '\s+', ' ')
    if ($normalizedIncompleteText.IndexOf(
            'verified cup bootstrap transaction was rejected',
            [StringComparison]::Ordinal
        ) -lt 0) {
        throw "Malformed canonical transaction failure was not explained"
    }
    if (-not (Test-Path -LiteralPath $transaction -PathType Leaf) -or
        [IO.File]::ReadAllText($transaction) -cne "invalid=1`n") {
        throw "Malformed canonical transaction evidence was not preserved"
    }
    if ((Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash -ne
        $binaryHashBeforeRepair) {
        throw "Rejected Windows bootstrap changed cup.exe"
    }
    Remove-Item -LiteralPath $transaction -Force

    # Reinstall the same tested candidate without journal or staging residue.
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
    if (Test-Path -LiteralPath $transaction) {
        throw "Windows reinstall left a canonical transaction"
    }
    $staging = Join-Path $env:USERPROFILE ".cup\staging"
    if ((Get-ChildItem -LiteralPath $staging -Force | Measure-Object).Count -ne 0) {
        throw "Windows reinstall left canonical staging residue"
    }
    $binaryHashAfterReinstall = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    if ($binaryHashAfterReinstall -ne $candidateHash) {
        throw "Windows reinstall changed the tested release executable"
    }
    $versionAfterReinstall = & $installed --version
    if ($LASTEXITCODE -ne 0 -or $versionAfterReinstall -cne "cup $Version") {
        throw "Installed cup was not usable after reinstall"
    }
    $helperHashBeforeUpdate = (Get-FileHash -LiteralPath $updateHelper -Algorithm SHA256).Hash
    if ($helperHashBeforeUpdate -ne $candidateHash) {
        throw "Windows reinstall did not derive the update helper from cup.exe"
    }

    # The private server fixture contains a genuine newer official build produced before this runner.
    $updateRoot = Join-Path $root 'update-fixture'
    $updateMetadata = Get-CanonicalAsciiLines (Join-Path $updateRoot 'release.txt')
    if ($updateMetadata.Count -ne 3 -or $updateMetadata[0] -cne 'format=1' -or
        $updateMetadata[2] -cne "commit=$SourceSha" -or
        -not $updateMetadata[1].StartsWith('version=', [StringComparison]::Ordinal)) {
        throw 'Update fixture release metadata is invalid'
    }
    $nextVersion = $updateMetadata[1].Substring('version='.Length)
    $versionRoot = Join-Path $updateRoot $nextVersion
    $updatedBinary = Join-Path $versionRoot 'cup-windows-x64.exe'
    $updatedVersionOutput = & $updatedBinary --version
    if ($LASTEXITCODE -ne 0 -or $updatedVersionOutput -cne "cup $nextVersion") {
        throw "Genuine update executable did not expose version $nextVersion"
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

    $transactionPath = Join-Path $env:USERPROFILE ".cup\transaction.txt"
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $updatedInstalledVersion = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not (Test-Path -LiteralPath $transactionPath -PathType Leaf)) {
            try {
                $candidateVersion = @(& $installed --version 2>$null)
                if ($LASTEXITCODE -eq 0 -and $candidateVersion -ceq "cup $nextVersion") {
                    & $installed doctor *> $null
                    if ($LASTEXITCODE -eq 0) {
                        $updatedInstalledVersion = $candidateVersion
                        break
                    }
                }
            } catch {
                # The helper may be between atomic replacement steps; retry until the deadline.
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $updatedInstalledVersion) {
        throw "cup update helper did not finalize version $nextVersion"
    }
    if (Test-Path -LiteralPath $transactionPath) {
        throw "successful cup update left a transaction journal"
    }
    $installedUpdatedHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    $fixtureUpdatedHash = (Get-FileHash -LiteralPath $updatedBinary -Algorithm SHA256).Hash
    if ($installedUpdatedHash -ne $fixtureUpdatedHash) {
        throw "installed cup does not match the verified update executable"
    }
    $helperHashAfterUpdate = (Get-FileHash -LiteralPath $updateHelper -Algorithm SHA256).Hash
    if ($helperHashAfterUpdate -ne $helperHashBeforeUpdate -or
        $helperHashAfterUpdate -eq $installedUpdatedHash) {
        throw "derived update helper did not remain the previous runner after update"
    }
    if ($updatedInstalledVersion -cne "cup $nextVersion") {
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
    $uninstallStartedMessage =
        "Uninstall started; cleanup continues in the background. " +
        "You can close this terminal. The PATH entry was not removed."
    $uninstallOutput = & $installed uninstall --yes 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Installed cup uninstall failed with exit code $LASTEXITCODE"
    }
    if (($uninstallOutput -join "`n") -notlike "*$uninstallStartedMessage*") {
        throw "Installed cup uninstall did not report detached removal"
    }
    $cupRoot = Join-Path $env:USERPROFILE ".cup"
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $residues = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        $residues = @(Get-ChildItem -LiteralPath $env:USERPROFILE -Force `
            -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall-*" })
        if (-not (Test-Path -LiteralPath $cupRoot) -and $residues.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $cupRoot) {
        throw "Release uninstall did not remove the cup root"
    }
    $residues = @(Get-ChildItem -LiteralPath $env:USERPROFILE -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall-*" })
    if ($residues.Count -ne 0) {
        throw "Release uninstall left staging behind: $($residues[0].FullName)"
    }

    # Run negative transport and metadata policy cases after the complete positive
    # release lifecycle so one diagnostic-policy failure cannot hide later
    # installer/runtime compatibility failures in the same native run.
    Test-InstallerFinalLowSpeedWindow

    Test-InstallerMetadataFailure -Name 'format-key-case' -Lines @(
        'Format=1',
        "version=$Version",
        "commit=$SourceSha"
    ) -ExpectedMessage 'release metadata has an unsupported format'
    Test-InstallerMetadataFailure -Name 'version-key-case' -Lines @(
        'format=1',
        "Version=$Version",
        "commit=$SourceSha"
    ) -ExpectedMessage 'release metadata version does not match the installer'
    Test-InstallerMetadataFailure -Name 'commit-key-case' -Lines @(
        'format=1',
        "version=$Version",
        "Commit=$SourceSha"
    ) -ExpectedMessage 'release metadata commit does not match the installer'
    $mismatchedVersion = if ($Version -eq '0.0.0') { '0.0.1' } else { '0.0.0' }
    Test-InstallerMetadataFailure -Name 'version-mismatch' -Lines @(
        'format=1',
        "version=$mismatchedVersion",
        "commit=$SourceSha"
    ) -ExpectedMessage 'release metadata version does not match the installer'
    Test-InstallerMetadataFailure -Name 'commit-mismatch' -Lines @(
        'format=1',
        "version=$Version",
        'commit=fedcba9876543210fedcba9876543210fedcba98'
    ) -ExpectedMessage 'release metadata commit does not match the installer'
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

    if ($null -ne $server) {
        if (-not $server.HasExited) {
            Stop-ReleaseProcessTree -Process $server
        }
        $server.Dispose()
    }
    if ($null -ne $testWorkRoot) {
        Remove-Item -LiteralPath $testWorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
