# Exercises ZIP validation and malicious archive rejection on Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")
. (Join-Path $PSScriptRoot "..\..\support\windows\archive-fixtures.ps1")

try {
    Initialize-TestEnvironment -Name "archive-safety" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    # Invalid cached archives refresh through a bounded loopback target so this
    # native integration never contacts the public release service.
    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $updatedCatalog = foreach ($line in (Get-Content -LiteralPath $catalog)) {
        if ($line.StartsWith(
                "compiler.clang.windows-x64.windows-x64.url_template=",
                [System.StringComparison]::Ordinal)) {
            "compiler.clang.windows-x64.windows-x64.url_template=" +
                "https://127.0.0.1:1/{version}-{host_platform}-{target_platform}/" +
                "clang-{version}-{host_platform}-{target_platform}.{format}"
        } elseif ($line.StartsWith(
                "compiler.clang.windows-x64.windows-x64.checksum_url_template=",
                [System.StringComparison]::Ordinal)) {
            "compiler.clang.windows-x64.windows-x64.checksum_url_template=" +
                "https://127.0.0.1:1/{version}-{host_platform}-{target_platform}/SHA256SUMS"
        } else {
            $line
        }
    }
    Write-Utf8NoBom -Path $catalog -Lines $updatedCatalog

    $caseVersion = "30.1.1"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $caseVersion `
        -Mode "Prepend"
    $casePackage = "clang-$caseVersion-windows-x64-windows-x64"
    $caseEntries = @(
        [pscustomobject]@{
            Name = "$casePackage/bin/CLANG.cmd"
            Content = "collision`n"
        }
    )
    $caseFixture = New-CustomZipPackage -Version $caseVersion -ExtraEntries $caseEntries
    Assert-ZipContainsExactEntries -Archive $caseFixture.Archive -Names @(
        "$casePackage/bin/clang.cmd",
        "$casePackage/bin/CLANG.cmd")
    [void](Assert-InstallRejected $caseVersion)

    $traversalVersion = "30.1.2"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $traversalVersion `
        -Mode "Prepend"
    $traversalPackage = "clang-$traversalVersion-windows-x64-windows-x64"
    $traversalEntries = @(
        [pscustomobject]@{
            Name = "$traversalPackage/../escape.txt"
            Content = "escape`n"
        }
    )
    $traversalFixture = New-CustomZipPackage `
        -Version $traversalVersion `
        -ExtraEntries $traversalEntries
    Assert-ZipContainsExactEntries -Archive $traversalFixture.Archive -Names @(
        "$traversalPackage/../escape.txt")
    [void](Assert-InstallRejected $traversalVersion)
    $escapedPath = Join-Path $cupRoot (
        "components\compiler\clang\windows-x64\windows-x64\escape.txt")
    Assert-PathMissing $escapedPath

    $backslashVersion = "30.1.5"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $backslashVersion `
        -Mode "Prepend"
    $backslashPackage = "clang-$backslashVersion-windows-x64-windows-x64"
    $backslashEntry = "$backslashPackage/bin\escape.cmd"
    $backslashEntries = @(
        [pscustomobject]@{
            Name = $backslashEntry
            Content = "escape`n"
        }
    )
    $backslashFixture = New-CustomZipPackage `
        -Version $backslashVersion `
        -ExtraEntries $backslashEntries
    Assert-ZipContainsExactEntries -Archive $backslashFixture.Archive -Names @(
        $backslashEntry)
    [void](Assert-InstallRejected $backslashVersion)

    $mismatchVersion = "30.1.3"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $mismatchVersion `
        -Mode "Prepend"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "default_format" `
        -Value "tar.gz" `
        -Mode "Replace"
    $mismatchFixture = New-CustomZipPackage -Version $mismatchVersion -ExtraEntries @()
    $mismatchArchive = Join-Path (Split-Path -Parent $mismatchFixture.Archive) `
        "$($mismatchFixture.PackageName).tar.gz"
    Move-Item -LiteralPath $mismatchFixture.Archive -Destination $mismatchArchive
    $mismatchHash = (Get-FileHash `
        -LiteralPath $mismatchArchive `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path (Split-Path -Parent $mismatchArchive) "SHA256SUMS") -Lines @(
        "$mismatchHash  $(Split-Path -Leaf $mismatchArchive)")
    $mismatchOutput = Assert-InstallRejected $mismatchVersion
    Assert-Contains $mismatchOutput "failed to download"

    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "default_format" `
        -Value "zip" `
        -Mode "Replace"
    $invalidVersion = "30.1.4"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $invalidVersion `
        -Mode "Prepend"
    $invalidPackage = "clang-$invalidVersion-windows-x64-windows-x64"
    $invalidCache = Join-Path $cupRoot (
        "cache\compiler\clang\windows-x64\windows-x64\$invalidVersion")
    New-Item -ItemType Directory -Force -Path $invalidCache | Out-Null
    $invalidArchive = Join-Path $invalidCache "$invalidPackage.zip"
    Set-Content -LiteralPath $invalidArchive -Encoding ascii -Value "not a zip archive"
    $invalidHash = (Get-FileHash `
        -LiteralPath $invalidArchive `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path $invalidCache "SHA256SUMS") -Lines @(
        "$invalidHash  $(Split-Path -Leaf $invalidArchive)")
    $invalidOutput = Assert-InstallRejected $invalidVersion
    Assert-Contains $invalidOutput "failed to download"

    Assert-CupHealthy
    Write-Host "Windows archive safety tests passed."
} finally {
    Remove-TestEnvironment
}
