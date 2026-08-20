# Verifies package download through a local hostname and checksum
# rejection on Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

$server = $null
$originalEnvironment = @{}
foreach ($name in @('CUP_INSTALL_ALLOW_INSECURE', 'NO_PROXY', 'no_proxy')) {
    $item = Get-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
    $originalEnvironment[$name] = if ($null -eq $item) { $null } else { $item.Value }
}

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
    $configuration = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_CONFIGURATION)) {
        'development'
    } else {
        $env:CUP_TEST_CONFIGURATION
    }
    $helper = Join-Path $Script:CupTestBuildRoot (
        "windows-x64\$configuration\tests\helpers\network-helper.exe")
    Assert-PathExists $helper

    $serverRoot = Join-Path $Script:CupTestRoot 'server'
    $readyFile = Join-Path $Script:CupTestRoot 'server.ready'
    $stdoutFile = Join-Path $Script:CupTestRoot 'server.stdout'
    $stderrFile = Join-Path $Script:CupTestRoot 'server.stderr'
    New-Item -ItemType Directory -Force -Path $serverRoot | Out-Null

    $arguments = @(
        'http-server', '--root', $serverRoot, '--port', '0',
        '--ready-file', $readyFile
    ) | ForEach-Object { ConvertTo-NativeArgument -Argument $_ }
    $server = Start-Process -FilePath $helper `
        -ArgumentList ($arguments -join ' ') `
        -WorkingDirectory $Script:CupTestRoot `
        -RedirectStandardOutput $stdoutFile `
        -RedirectStandardError $stderrFile `
        -NoNewWindow -PassThru

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
            "http://localhost:$port/{version}-{host_platform}-{target_platform}/" +
            'clang-{version}-{host_platform}-{target_platform}.{format}')
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'checksum_url_template' -Mode 'Replace' -Value (
            "http://localhost:$port/{version}-{host_platform}-{target_platform}/SHA256SUMS")

    $env:CUP_INSTALL_ALLOW_INSECURE = '1'
    $env:NO_PROXY = 'localhost,127.0.0.1'
    $env:no_proxy = 'localhost,127.0.0.1'

    $validVersion = '97.0.1'
    Set-PackageCatalogField -Component 'compiler' -Tool 'clang' `
        -Field 'available_versions' -Value $validVersion -Mode 'Prepend'
    Publish-NetworkPackage -Version $validVersion -ServerRoot $serverRoot | Out-Null

    Write-Host '==> Downloading a package through the local hostname...'
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

    Write-Host 'Windows network integration tests passed.'
} finally {
    if ($null -ne $server) {
        if (-not $server.HasExited) {
            try {
                & taskkill.exe /PID $server.Id /T /F 2>&1 | Out-Null
            } catch {
                # Cleanup is best effort.
            }
            [void]$server.WaitForExit(5000)
        }
        $server.Dispose()
    }
    foreach ($name in $originalEnvironment.Keys) {
        if ($null -eq $originalEnvironment[$name]) {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item -LiteralPath "Env:$name" -Value $originalEnvironment[$name]
        }
    }
    Remove-TestEnvironment
}
