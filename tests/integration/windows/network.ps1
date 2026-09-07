# Verifies package download through a loopback address, checksum rejection,
# and decompressed metadata size limits on Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

$server = $null

function Publish-NetworkPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [string]$ServerRoot
    )

    New-TestPackage -Component 'compiler' -Tool 'clang' -Version $Version -Entries @('clang')
    $platform = 'windows-x64'
    $packageName = "clang-$Version-$platform-$platform"
    $cacheDir = Join-Path $Script:CupTestHome (
        ".cup\cache\compiler\clang\$platform\$platform\$Version")
    $releaseDir = Join-Path $ServerRoot "$Version-$platform-$platform"
    New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $cacheDir "$packageName.zip") -Destination $releaseDir
    Copy-Item -LiteralPath (Join-Path $cacheDir 'SHA256SUMS') -Destination $releaseDir
    Remove-Item -LiteralPath $cacheDir -Recurse -Force
    return [pscustomobject]@{
        PackageName = $packageName
        ReleaseDir = $releaseDir
    }
}

try {
    Initialize-TestEnvironment -Name 'network' -ExecutablePath $CupExecutablePath
    # Initialize the isolated runtime; repair behavior is owned by repair.ps1.
    Invoke-Cup -CommandArgs @('repair') | Out-Null
    $helper = Get-TestHelperPath -Name 'network-helper'

    $serverRoot = Join-Path $Script:CupTestRoot 'server'
    $readyFile = Join-Path $Script:CupTestRoot 'server.ready'
    $stdoutFile = Join-Path $Script:CupTestRoot 'server.stdout'
    $stderrFile = Join-Path $Script:CupTestRoot 'server.stderr'
    New-Item -ItemType Directory -Force -Path $serverRoot | Out-Null

    $compressedVersion = '97.0.4'
    $compressedPath =
        "/compressed-limit/$compressedVersion-windows-x64-windows-x64/SHA256SUMS"
    $arguments = @(
        'http-server', '--root', $serverRoot, '--port', '0',
        '--ready-file', $readyFile,
        '--gzip-path', $compressedPath, '--gzip-bytes', '4194305'
    )
    $server = Start-TestHelperProcess -FilePath $helper `
        -ArgumentList $arguments `
        -WorkingDirectory $Script:CupTestRoot `
        -RedirectStandardOutput $stdoutFile `
        -RedirectStandardError $stderrFile

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ($true) {
        if ($server.HasExited) {
            $errorText = if (Test-Path -LiteralPath $stderrFile) {
                Get-Content -LiteralPath $stderrFile -Raw
            } else { '' }
            Fail-Test "local HTTP fixture exited before becoming ready`n$errorText"
        }
        if (Test-Path -LiteralPath $readyFile -PathType Leaf) {
            $readyItem = Get-Item -LiteralPath $readyFile -ErrorAction SilentlyContinue
            if ($null -ne $readyItem -and $readyItem.Length -gt 0) {
                break
            }
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            Fail-Test 'local HTTP fixture did not become ready'
        }
        Start-Sleep -Milliseconds 50
    }
    $portText = (Get-Content -LiteralPath $readyFile -Raw).Trim()
    $port = 0
    if (-not [int]::TryParse($portText, [ref]$port) -or
        $port -lt 1 -or $port -gt 65535) {
        Fail-Test "invalid local HTTP port: $portText"
    }

    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'url_template' -Mode 'Replace' -Value (
            "http://127.0.0.1:$port/{version}-{host_platform}-{target_platform}/" +
            'clang-{version}-{host_platform}-{target_platform}.{format}')
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'checksum_url_template' -Mode 'Replace' -Value (
            "http://127.0.0.1:$port/{version}-{host_platform}-{target_platform}/SHA256SUMS")

    $env:CUP_INSTALL_ALLOW_INSECURE = '1'
    $env:NO_PROXY = '127.0.0.1'

    $validVersion = '97.0.1'
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'available_versions' -Value $validVersion -Mode 'Prepend'
    Publish-NetworkPackage -Version $validVersion -ServerRoot $serverRoot | Out-Null

    Write-Host '==> Downloading a package through the loopback address...'
    Invoke-Cup -CommandArgs @('install', 'compiler', "clang@$validVersion") | Out-Null

    $badVersion = '97.0.2'
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'available_versions' -Value $badVersion -Mode 'Prepend'
    $badPackage = Publish-NetworkPackage -Version $badVersion -ServerRoot $serverRoot
    Write-Utf8NoBom -Path (Join-Path $badPackage.ReleaseDir 'SHA256SUMS') -Lines @(
        ('0' * 64) + "  $($badPackage.PackageName).zip")

    Write-Host '==> Rejecting a package whose downloaded checksum does not match...'
    $failure = Invoke-Cup -CommandArgs @('install', 'compiler', "clang@$badVersion") `
        -ExpectFailure
    Assert-Contains $failure 'downloaded package failed SHA-256 verification'
    $badCache = Join-Path $Script:CupTestHome (
        ".cup\cache\compiler\clang\windows-x64\windows-x64\$badVersion")
    Assert-PathMissing (Join-Path $badCache "$($badPackage.PackageName).zip")
    Assert-NotContains (Invoke-Cup -CommandArgs @('list', 'compiler')) `
        "compiler:clang@$badVersion"
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup\transaction.txt')
    Assert-CupHealthy

    $invalidChecksumVersion = '97.0.3'
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'available_versions' -Value $invalidChecksumVersion -Mode 'Prepend'
    $invalidChecksumPackage = Publish-NetworkPackage `
        -Version $invalidChecksumVersion -ServerRoot $serverRoot
    Write-Utf8NoBom -Path (Join-Path $invalidChecksumPackage.ReleaseDir 'SHA256SUMS') -Lines @(
        ('0' * 64) + " x$($invalidChecksumPackage.PackageName).zip")

    Write-Host '==> Rejecting malformed downloaded checksum metadata with a diagnostic...'
    $invalidChecksumFailure = Assert-CupStatus `
        -CommandArgs @('install', 'compiler', "clang@$invalidChecksumVersion") `
        -ExpectedStatus 4 `
        -ExpectedText 'Error: downloaded SHA256SUMS metadata is invalid.'
    Assert-NotContains $invalidChecksumFailure 'Downloaded package archive.'
    $invalidChecksumCache = Join-Path $Script:CupTestHome (
        ".cup\cache\compiler\clang\windows-x64\windows-x64\$invalidChecksumVersion")
    Assert-PathMissing (Join-Path $invalidChecksumCache 'SHA256SUMS')
    Assert-PathMissing (Join-Path $invalidChecksumCache `
        "$($invalidChecksumPackage.PackageName).zip")
    Assert-CupHealthy

    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'available_versions' -Value $compressedVersion -Mode 'Prepend'
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'checksum_url_template' -Mode 'Replace' -Value (
            "http://127.0.0.1:$port/compressed-limit/" +
            '{version}-{host_platform}-{target_platform}/SHA256SUMS')

    Write-Host '==> Rejecting checksum metadata whose decompressed body exceeds the limit...'
    $compressedFailure = Invoke-Cup `
        -CommandArgs @('install', 'compiler', "clang@$compressedVersion") `
        -ExpectFailure
    Assert-Contains $compressedFailure 'download exceeded the configured size limit'
    $compressedCache = Join-Path $Script:CupTestHome (
        ".cup\cache\compiler\clang\windows-x64\windows-x64\$compressedVersion")
    Assert-PathMissing (Join-Path $compressedCache 'SHA256SUMS')
    Assert-NotContains (Invoke-Cup -CommandArgs @('list', 'compiler')) `
        "compiler:clang@$compressedVersion"
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup\transaction.txt')
    Assert-CupHealthy

    Write-Host 'Windows network integration tests passed.'
} finally {
    if ($null -ne $server) {
        if (-not $server.HasExited) {
            Stop-TestProcessTree -Process $server -WaitMilliseconds 5000
        }
        $server.Dispose()
    }
    Remove-TestEnvironment
}
