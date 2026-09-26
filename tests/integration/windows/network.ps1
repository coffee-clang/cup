# Verifies package download through loopback, artifact digest rejection,
# and decompressed catalog metadata size limits on Windows.

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
        [string]$ServerRoot,

        [Parameter(Mandatory = $true)]
        [int]$Port
    )

    New-TestPackage -Component 'compiler' -Tool 'clang' -Version $Version -Entries @('clang')
    $platform = 'windows-x64'
    $packageName = "clang-$Version-$platform-$platform"
    $archive = Join-Path $Script:CupTestRoot "artifacts\$packageName.zip"
    $sha256 = Get-Sha256Lower -Path $archive
    $serverArchive = Join-Path $ServerRoot "$packageName.zip"
    Copy-Item -LiteralPath $archive -Destination $serverArchive -Force

    $cacheObject = Join-Path $Script:CupTestHome ".cup\cache\$sha256"
    Remove-Item -LiteralPath $cacheObject -Force -ErrorAction SilentlyContinue
    Set-PackageCatalogArtifact `
        -Tool 'clang' `
        -Version $Version `
        -Format 'zip' `
        -Url "http://127.0.0.1:$Port/$packageName.zip" `
        -Sha256 $sha256

    return [pscustomobject]@{
        PackageName = $packageName
        Archive = $archive
        Sha256 = $sha256
        CacheObject = $cacheObject
    }
}


function Publish-NewerCatalogSnapshot {
    param([Parameter(Mandatory = $true)][string]$ServerRoot)

    $runtimeCatalog = Join-Path $Script:CupTestHome '.cup\config\catalog.cfg'
    [string[]]$lines = Get-Content -LiteralPath $runtimeCatalog
    $revision = $null
    $updated = foreach ($line in $lines) {
        if ($line -match '^revision=([0-9]+)$') {
            $revision = [int]$Matches[1]
            "revision=$($revision + 1)"
        } else {
            $line
        }
    }
    if ($null -eq $revision) { Fail-Test 'runtime catalog has no canonical revision' }
    $remoteCatalog = Join-Path $ServerRoot 'catalog.cfg'
    Write-Utf8NoBom -Path $remoteCatalog -Lines $updated
    return $remoteCatalog
}

try {
    Initialize-TestEnvironment -Name 'network' -ExecutablePath $CupExecutablePath
    Ensure-FixtureRuntimeRoot
    $helper = Get-TestHelperPath -Name 'network-helper'

    $serverRoot = Join-Path $Script:CupTestRoot 'server'
    $readyFile = Join-Path $Script:CupTestRoot 'server.ready'
    $stdoutFile = Join-Path $Script:CupTestRoot 'server.stdout'
    $stderrFile = Join-Path $Script:CupTestRoot 'server.stderr'
    New-Item -ItemType Directory -Force -Path $serverRoot | Out-Null

    $oversizedCatalogPath = '/catalog-too-large'
    $arguments = @(
        'http-server', '--root', $serverRoot, '--port', '0',
        '--ready-file', $readyFile,
        '--gzip-path', $oversizedCatalogPath, '--gzip-bytes', '4194305'
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

    $env:CUP_INSTALL_ALLOW_INSECURE = '1'
    $env:NO_PROXY = '127.0.0.1'
    $env:no_proxy = '127.0.0.1'

    $validVersion = '97.0.1'
    [void](Publish-NetworkPackage -Version $validVersion -ServerRoot $serverRoot -Port $port)

    Write-Host '==> Downloading a concrete package artifact through loopback...'
    $installResult = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('install', 'compiler', "clang@$validVersion") `
        -WorkingDirectory $Script:CupTestDevRoot
    Assert-Equals ([string]$installResult.ExitCode) '0'
    Assert-Contains $installResult.Stdout "Installed compiler clang@$validVersion"
    Assert-NotContains $installResult.Stdout '==>'
    foreach ($phase in @(
        '==> Resolving clang@97.0.1...',
        '==> Downloading package...',
        '==> Extracting package...',
        '==> Validating package...',
        '==> Installing package...')) {
        Assert-Contains $installResult.Stderr $phase
    }
    Assert-Contains (Invoke-Cup -CommandArgs @('list', 'compiler')) `
        "compiler: clang@$validVersion"
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup\transaction.txt')
    Assert-CupHealthy

    Write-Host '==> Confirming an exact installed package needs no network access...'
    Set-PackageCatalogUpdateUrl -Url 'http://127.0.0.1:1/catalog.cfg'
    $noOpInstall = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('install', 'compiler', "clang@$validVersion") `
        -WorkingDirectory $Script:CupTestDevRoot
    Assert-Equals ([string]$noOpInstall.ExitCode) '0'
    Assert-Contains $noOpInstall.Stdout "clang@$validVersion is already installed"
    Assert-NotContains $noOpInstall.Stderr 'Refreshing catalog'

    Write-Host '==> Refreshing and reusing the live catalog through loopback...'
    $catalogUrl = "http://127.0.0.1:$port/catalog.cfg"
    Set-PackageCatalogUpdateUrl -Url $catalogUrl
    $remoteCatalog = Publish-NewerCatalogSnapshot -ServerRoot $serverRoot
    $runtimeCatalog = Join-Path $Script:CupTestHome '.cup\config\catalog.cfg'
    $catalogUpdate = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('update', 'catalog') -WorkingDirectory $Script:CupTestDevRoot
    Assert-Equals ([string]$catalogUpdate.ExitCode) '0'
    Assert-Equals $catalogUpdate.Stdout 'Catalog updated.'
    Assert-Contains $catalogUpdate.Stderr '==> Refreshing catalog...'
    Assert-Equals (Get-Sha256Lower -Path $runtimeCatalog) `
        (Get-Sha256Lower -Path $remoteCatalog)

    $catalogCurrent = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('update', 'catalog') -WorkingDirectory $Script:CupTestDevRoot
    Assert-Equals ([string]$catalogCurrent.ExitCode) '0'
    Assert-Equals $catalogCurrent.Stdout 'Catalog is already current.'
    Assert-Contains $catalogCurrent.Stderr '==> Refreshing catalog...'

    Write-Host '==> Refreshing search best-effort while keeping results on stdout...'
    $remoteCatalog = Publish-NewerCatalogSnapshot -ServerRoot $serverRoot
    $searchResult = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('search', 'compiler') -WorkingDirectory $Script:CupTestDevRoot
    Assert-Equals ([string]$searchResult.ExitCode) '0'
    Assert-Contains $searchResult.Stdout "Available tools for component 'compiler'"
    Assert-NotContains $searchResult.Stdout '==>'
    Assert-Contains $searchResult.Stderr '==> Refreshing catalog...'
    Assert-Equals (Get-Sha256Lower -Path $runtimeCatalog) `
        (Get-Sha256Lower -Path $remoteCatalog)

    Set-PackageCatalogUpdateUrl -Url 'http://127.0.0.1:1/catalog.cfg'
    $searchFallback = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('search', 'compiler') -WorkingDirectory $Script:CupTestDevRoot
    Assert-Equals ([string]$searchFallback.ExitCode) '0'
    Assert-Contains $searchFallback.Stdout "Available tools for component 'compiler'"
    Assert-Contains $searchFallback.Stderr `
        'Warning: catalog refresh failed; showing the local catalog.'
    Set-PackageCatalogUpdateUrl -Url $catalogUrl

    $badVersion = '97.0.2'
    $badPackage = Publish-NetworkPackage -Version $badVersion -ServerRoot $serverRoot -Port $port
    $badExpectedSha = '0' * 64
    Remove-Item -LiteralPath $badPackage.CacheObject -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $Script:CupTestHome ".cup\cache\$badExpectedSha") `
        -Force -ErrorAction SilentlyContinue
    Set-PackageCatalogArtifact `
        -Tool 'clang' `
        -Version $badVersion `
        -Format 'zip' `
        -Url "http://127.0.0.1:$port/$($badPackage.PackageName).zip" `
        -Sha256 $badExpectedSha

    Write-Host '==> Rejecting an artifact whose bytes do not match the catalog digest...'
    $failure = Invoke-Cup -CommandArgs @('install', 'compiler', "clang@$badVersion") `
        -ExpectFailure
    Assert-Contains $failure 'downloaded package failed SHA-256 verification'
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\cache\$badExpectedSha")
    Assert-NotContains (Invoke-Cup -CommandArgs @('list', 'compiler')) `
        "compiler: clang@$badVersion"
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup\transaction.txt')
    Assert-CupHealthy

    Write-Host '==> Rejecting catalog metadata whose decompressed body exceeds the metadata limit...'
    Set-PackageCatalogUpdateUrl -Url "http://127.0.0.1:$port$oversizedCatalogPath"
    $runtimeCatalog = Join-Path $Script:CupTestHome '.cup\config\catalog.cfg'
    $catalogBefore = Get-Sha256Lower -Path $runtimeCatalog
    $catalogFailure = Invoke-Cup -CommandArgs @('update', 'catalog') -ExpectFailure
    Assert-Contains $catalogFailure 'download exceeded the configured size limit'
    Assert-Equals $catalogBefore (Get-Sha256Lower -Path $runtimeCatalog) `
        'failed catalog refresh changed the local snapshot'
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
