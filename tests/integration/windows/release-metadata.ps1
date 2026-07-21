# Purpose: Exercises Windows installer parsing and validation of generated release metadata.

param([Parameter(Mandatory = $true)][string]$CupExecutablePath)
. (Join-Path $PSScriptRoot "common.ps1")

if ([string]::IsNullOrWhiteSpace($CupExecutablePath)) {
    Fail-Test "cup executable path is empty"
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$installerPath = Join-Path $projectRoot "scripts\install\install-cup-windows.ps1"
$installer = Get-Content -LiteralPath $installerPath -Raw
$entryPoint = $installer.LastIndexOf("`ntry {")
if ($entryPoint -lt 0) {
    Fail-Test "failed to isolate Windows installer functions"
}
Invoke-Expression $installer.Substring(0, $entryPoint)

$root = New-IsolatedTestRoot -Name "release metadata"
try {
    function Write-Metadata([string]$Name, [string[]]$Lines) {
        $path = Join-Path $root $Name
        Set-Content -LiteralPath $path -Encoding ascii -Value $Lines
        return $path
    }

    function Assert-MetadataRejected([string]$Path) {
        try {
            Assert-ReleaseMetadata $Path
        } catch {
            return
        }
        Fail-Test "invalid release metadata unexpectedly succeeded: $Path"
    }

    $valid = Write-Metadata "valid.txt" @(
        "format=1", "version=0.2.0", "commit=abcdef0")
    Assert-ReleaseMetadata $valid

    Assert-MetadataRejected (Write-Metadata "leading-zero.txt" @(
        "format=1", "version=00.2.0", "commit=abcdef0"))
    Assert-MetadataRejected (Write-Metadata "non-ascii-digit.txt" @(
        "format=1", "version=٠.2.0", "commit=abcdef0"))
    Assert-MetadataRejected (Write-Metadata "too-large.txt" @(
        "format=1", "version=1000000.2.0", "commit=abcdef0"))
    Assert-MetadataRejected (Write-Metadata "plain.txt" @("0.2.0"))
    Assert-MetadataRejected (Write-Metadata "duplicate.txt" @(
        "format=1", "version=0.2.0", "version=0.2.1", "commit=abcdef0"))
    Assert-MetadataRejected (Write-Metadata "extra.txt" @(
        "format=1", "version=0.2.0", "commit=abcdef0", "unknown=value"))
    Assert-MetadataRejected (Write-Metadata "bad-commit.txt" @(
        "format=1", "version=0.2.0", "commit=not-a-commit"))


    $script:BaseUrl = "https://example.invalid/releases"
    Assert-BaseUrl
    $script:BaseUrl = "http://example.invalid/releases"
    try {
        Assert-BaseUrl
        Fail-Test "external HTTP installer URL unexpectedly succeeded"
    } catch {
        if ($_.Exception.Message -like 'TEST FAILED:*') { throw }
    }
    $script:BaseUrl = "http://127.0.0.1:8080"
    Remove-Item Env:CUP_INSTALL_ALLOW_INSECURE -ErrorAction SilentlyContinue
    try {
        Assert-BaseUrl
        Fail-Test "loopback HTTP installer URL without opt-in unexpectedly succeeded"
    } catch {
        if ($_.Exception.Message -like 'TEST FAILED:*') { throw }
    }
    $env:CUP_INSTALL_ALLOW_INSECURE = "1"
    Assert-BaseUrl
    Remove-Item Env:CUP_INSTALL_ALLOW_INSECURE -ErrorAction SilentlyContinue
    $script:BaseUrl = "https://user@example.invalid/releases"
    try {
        Assert-BaseUrl
        Fail-Test "installer URL containing credentials unexpectedly succeeded"
    } catch {
        if ($_.Exception.Message -like 'TEST FAILED:*') { throw }
    }

    $checksumDir = Join-Path $root "checksums"
    New-Item -ItemType Directory -Path $checksumDir | Out-Null
    foreach ($name in @("packages.cfg", "install.cfg", "install.sh", "install.ps1")) {
        Set-Content -LiteralPath (Join-Path $checksumDir $name) -Encoding ascii -Value $name
    }
    $checksumLines = foreach ($name in @("packages.cfg", "install.cfg", "install.sh", "install.ps1")) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $checksumDir $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $name"
    }
    $checksumFile = Join-Path $checksumDir "SHA256SUMS.common"
    Write-Utf8NoBom -Path $checksumFile -Lines $checksumLines
    Assert-ChecksumNames -ChecksumFile $checksumFile `
        -ExpectedNames @("packages.cfg", "install.cfg", "install.sh", "install.ps1")
    Assert-NamedChecksum -Directory $checksumDir -ChecksumFile $checksumFile `
        -ExpectedName "install.ps1"

    $savedProfile = $env:USERPROFILE
    try {
        $env:USERPROFILE = [System.IO.Path]::GetPathRoot($savedProfile)
        try {
            Test-WindowsX64
            Fail-Test "volume-root USERPROFILE unexpectedly succeeded"
        } catch {
            if ($_.Exception.Message -like 'TEST FAILED:*') { throw }
        }
    } finally {
        $env:USERPROFILE = $savedProfile
    }

    Write-Host "Windows release metadata tests passed."
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
