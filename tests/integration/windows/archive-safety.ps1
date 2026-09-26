# Exercises ZIP validation and malicious archive rejection on Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function Assert-InstallRejected(
    [string]$Version,
    [string]$ExpectedDiagnostic = "",
    [string[]]$ExtraArgs = @()
) {
    $arguments = @('install', 'compiler', "clang@$Version") + $ExtraArgs
    $output = Invoke-Cup -CommandArgs $arguments -ExpectFailure
    Assert-NotContains $output 'Cached package is invalid; downloading it again...'
    Assert-NotContains $output '==> Validating package...'
    if (-not [string]::IsNullOrEmpty($ExpectedDiagnostic)) {
        Assert-Contains $output $ExpectedDiagnostic
    }
    Assert-NotContains `
        (Invoke-Cup -CommandArgs @('list', 'compiler')) `
        "compiler: clang@$Version"
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup\transaction.txt')
    return $output
}

try {
    Initialize-TestEnvironment -Name 'archive-safety' -ExecutablePath $CupExecutablePath

    $cupRoot = Join-Path $Script:CupTestHome '.cup'

    $caseVersion = '30.1.1'
    $casePackage = "clang-$caseVersion-windows-x64-windows-x64"
    [void](New-ZipPackageFixture `
        -Version $caseVersion `
        -ExtraPath "$casePackage/bin/CLANG.cmd" `
        -ExtraContent "collision`n")
    [void](Assert-InstallRejected $caseVersion `
        'archive contains a duplicate, case-colliding, or path-type-colliding path')

    $traversalVersion = '30.1.2'
    $traversalPackage = "clang-$traversalVersion-windows-x64-windows-x64"
    [void](New-ZipPackageFixture `
        -Version $traversalVersion `
        -ExtraPath "$traversalPackage/../escape.txt" `
        -ExtraContent "escape`n")
    [void](Assert-InstallRejected $traversalVersion 'archive contains an unsafe path')
    $escapedPath = Join-Path $cupRoot 'components\compiler\clang\windows-x64\escape.txt'
    Assert-PathMissing $escapedPath

    $backslashVersion = '30.1.3'
    $backslashPackage = "clang-$backslashVersion-windows-x64-windows-x64"
    $backslashEntry = "$backslashPackage/bin\escape.cmd"
    [void](New-ZipPackageFixture `
        -Version $backslashVersion `
        -ExtraPath $backslashEntry `
        -ExtraContent "escape`n")
    [void](Assert-InstallRejected $backslashVersion `
        'archive contains multiple or unsafe top-level roots')

    $mismatchVersion = '30.1.4'
    $mismatchFixture = New-ZipPackageFixture -Version $mismatchVersion
    Set-PackageCatalogArtifact `
        -Tool 'clang' `
        -Version $mismatchVersion `
        -Format 'tar.gz' `
        -Url "https://example.invalid/$($mismatchFixture.PackageName).tar.gz" `
        -Sha256 $mismatchFixture.Sha256
    [void](Assert-InstallRejected `
        -Version $mismatchVersion `
        -ExtraArgs @('--format', 'tar.gz'))

    $invalidVersion = '30.1.5'
    $invalidPackage = "clang-$invalidVersion-windows-x64-windows-x64"
    Ensure-FixtureRuntimeRoot
    $invalidArtifact = Join-Path $Script:CupTestRoot "artifacts\$invalidPackage.zip"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $invalidArtifact) | Out-Null
    [IO.File]::WriteAllText($invalidArtifact, 'not a zip archive', [Text.Encoding]::ASCII)
    $invalidHash = Get-Sha256Lower -Path $invalidArtifact
    $cacheRoot = Join-Path $cupRoot 'cache'
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
    Copy-Item -LiteralPath $invalidArtifact -Destination (Join-Path $cacheRoot $invalidHash) -Force
    Add-PackageCatalogRecord `
        -Component 'compiler' `
        -Tool 'clang' `
        -Version $invalidVersion `
        -Format 'zip' `
        -Url "https://example.invalid/$invalidPackage.zip" `
        -Sha256 $invalidHash
    [void](Assert-InstallRejected $invalidVersion)

    Assert-CupHealthy
    Write-Host 'Windows archive safety tests passed.'
} finally {
    Remove-TestEnvironment
}
