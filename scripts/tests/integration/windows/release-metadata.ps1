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

    Write-Host "Windows release metadata tests passed."
} finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
