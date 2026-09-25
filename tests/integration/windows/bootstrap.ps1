# Exercises native Windows bootstrap using the same five-file verified release subset as POSIX.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")


function New-BootstrapSource {
    param([Parameter(Mandatory = $true)][string]$Path)

    New-PrivateTestDirectory -Path $Path
    $binary = Join-Path $Path "cup-windows-x64.exe"
    $license = Join-Path $Path "LICENSE"
    $notices = Join-Path $Path "THIRD_PARTY_NOTICES.txt"
    $catalog = Join-Path $Path "catalog.cfg"
    $release = Join-Path $Path "release.txt"

    Copy-Item -LiteralPath $Script:CupTestExecutable -Destination $binary
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "LICENSE") -Destination $license
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "scripts\dependencies\THIRD_PARTY_NOTICES.txt") `
        -Destination $notices
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "tests\fixtures\catalog.cfg") `
        -Destination $catalog

    $version = (Get-Content -LiteralPath (Join-Path $Script:CupTestProjectRoot "VERSION") -Raw).Trim()
    $commit = "0123456789abcdef0123456789abcdef01234567"
    Write-Utf8NoBom -Path $release -Lines @(
        "format=2",
        "version=$version",
        "commit=$commit",
        "root_layout=2",
        "catalog_format=1",
        "asset_count=4",
        "asset.0.name=LICENSE",
        "asset.0.sha256=$(Get-Sha256Lower -Path $license)",
        "asset.1.name=THIRD_PARTY_NOTICES.txt",
        "asset.1.sha256=$(Get-Sha256Lower -Path $notices)",
        "asset.2.name=catalog.cfg",
        "asset.2.sha256=$(Get-Sha256Lower -Path $catalog)",
        "asset.3.name=cup-windows-x64.exe",
        "asset.3.sha256=$(Get-Sha256Lower -Path $binary)"
    )
}

function Test-StagingEmpty {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    return @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue).Count -eq 0
}

function Invoke-Bootstrap {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Base,
        [switch]$ExpectFailure
    )

    $result = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @("--internal-bootstrap", $Source, $Base) `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($ExpectFailure) {
        if ($result.ExitCode -eq 0) { Fail-Test "bootstrap unexpectedly accepted source: $Source" }
    } elseif ($result.ExitCode -ne 0) {
        Fail-Test "bootstrap failed [$($result.ExitCode)]`n$($result.Output)"
    }
    return $result
}

try {
    Initialize-TestEnvironment -Name "bootstrap" -ExecutablePath $CupExecutablePath

    $source = Join-Path $Script:CupTestRoot "source"
    New-BootstrapSource -Path $source
    $result = Invoke-Bootstrap -Source $source -Base $Script:CupTestHome
    Assert-Contains $result.Output "Verified cup"
    Assert-Contains $result.Output "CUP_BOOTSTRAP_ROOT="

    $root = Join-Path $Script:CupTestHome ".cup"
    foreach ($relative in @(
        "root.txt", "cup.lock", "state.txt", "bin\cup.exe", "release.txt", "LICENSE",
        "THIRD_PARTY_NOTICES.txt", "config\catalog.cfg"
    )) {
        Assert-PathExists (Join-Path $root $relative)
    }
    Assert-PathMissing (Join-Path $root "transaction.txt")
    Assert-PathMissing (Join-Path $root "helpers\update-helper.exe")
    if (-not (Test-StagingEmpty -Path (Join-Path $root "staging"))) {
        Fail-Test "successful fresh bootstrap left staging residue"
    }
    if (@(Get-ChildItem -LiteralPath $Script:CupTestHome -Directory -Filter ".cup-install-*" `
            -ErrorAction SilentlyContinue).Count -ne 0) {
        Fail-Test "successful fresh bootstrap left private-root residue"
    }

    $installed = Join-Path $root "bin\cup.exe"
    Assert-Equals (Get-Sha256Lower -Path $installed) (Get-Sha256Lower -Path $Script:CupTestExecutable)
    $version = Invoke-NativeProcess -FilePath $installed -Arguments @("--version") `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($version.ExitCode -ne 0) { Fail-Test "installed bootstrap binary failed --version" }

    # Existing-root reinstall is synchronous and preserves non-generation runtime state.
    $config = Invoke-NativeProcess -FilePath $installed `
        -Arguments @("config", "set", "compiler", "clang") `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($config.ExitCode -ne 0) { Fail-Test "could not create bootstrap preference fixture" }
    $preference = Join-Path $root "config\preferences.txt"
    $state = Join-Path $root "state.txt"
    $catalog = Join-Path $root "config\catalog.cfg"
    $preferenceBefore = Get-Sha256Lower -Path $preference
    $stateBefore = Get-Sha256Lower -Path $state
    $catalogBefore = Get-Sha256Lower -Path $catalog
    $cache = Join-Path $root "cache"
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $cacheObject = Join-Path $cache "preserved-object"
    Write-Utf8NoBom -Path $cacheObject -Lines @("keep-me")
    $cacheBefore = Get-Sha256Lower -Path $cacheObject

    $secondSource = Join-Path $Script:CupTestRoot "second-source"
    New-BootstrapSource -Path $secondSource
    $second = Invoke-Bootstrap -Source $secondSource -Base $Script:CupTestHome
    Assert-Contains $second.Output "Verified cup"
    Assert-PathMissing (Join-Path $root "transaction.txt")
    if (-not (Test-StagingEmpty -Path (Join-Path $root "staging"))) {
        Fail-Test "successful existing-root reinstall left staging residue"
    }
    Assert-Equals (Get-Sha256Lower -Path $preference) $preferenceBefore
    Assert-Equals (Get-Sha256Lower -Path $state) $stateBefore
    Assert-Equals (Get-Sha256Lower -Path $catalog) $catalogBefore
    Assert-Equals (Get-Sha256Lower -Path $cacheObject) $cacheBefore

    # Reinstall preserves an unsupported future catalog and refuses the operation.
    $futureBase = Join-Path $Script:CupTestRoot 'future-base'
    New-Item -ItemType Directory -Path $futureBase | Out-Null
    $futureSource = Join-Path $Script:CupTestRoot 'future-source'
    New-BootstrapSource -Path $futureSource
    [void](Invoke-Bootstrap -Source $futureSource -Base $futureBase)
    $futureRoot = Join-Path $futureBase '.cup'
    $futureCatalog = Join-Path $futureRoot 'config\catalog.cfg'
    Write-Utf8NoBom -Path $futureCatalog -Lines @(
        'format=2',
        'revision=9',
        'update_url=https://example.invalid/catalog.cfg'
    )
    $futureCatalogHash = Get-Sha256Lower -Path $futureCatalog
    $futureBinary = Join-Path $futureRoot 'bin\cup.exe'
    $futureBinaryHash = Get-Sha256Lower -Path $futureBinary
    [void](Invoke-Bootstrap -Source $futureSource -Base $futureBase -ExpectFailure)
    Assert-Equals (Get-Sha256Lower -Path $futureCatalog) $futureCatalogHash
    Assert-Equals (Get-Sha256Lower -Path $futureBinary) $futureBinaryHash
    Assert-PathMissing "$futureCatalog.invalid"

    # Reinstall preserves malformed catalog evidence and restores the release snapshot.
    $malformedBase = Join-Path $Script:CupTestRoot 'malformed-base'
    New-Item -ItemType Directory -Path $malformedBase | Out-Null
    $malformedSource = Join-Path $Script:CupTestRoot 'malformed-source'
    New-BootstrapSource -Path $malformedSource
    [void](Invoke-Bootstrap -Source $malformedSource -Base $malformedBase)
    $malformedCatalog = Join-Path $malformedBase '.cup\config\catalog.cfg'
    Write-Utf8NoBom -Path $malformedCatalog -Lines @('invalid=1')
    [void](Invoke-Bootstrap -Source $malformedSource -Base $malformedBase)
    Assert-Equals (Get-Sha256Lower -Path $malformedCatalog) `
        (Get-Sha256Lower -Path (Join-Path $malformedSource 'catalog.cfg'))
    Assert-PathExists "$malformedCatalog.invalid"
    Assert-Contains (Get-Content -LiteralPath "$malformedCatalog.invalid" -Raw) 'invalid=1'

    # Reinstall restores a missing managed binary.
    $repairBase = Join-Path $Script:CupTestRoot 'binary-repair-base'
    New-Item -ItemType Directory -Path $repairBase | Out-Null
    $repairSource = Join-Path $Script:CupTestRoot 'binary-repair-source'
    New-BootstrapSource -Path $repairSource
    [void](Invoke-Bootstrap -Source $repairSource -Base $repairBase)
    $repairBinary = Join-Path $repairBase '.cup\bin\cup.exe'
    Remove-Item -LiteralPath $repairBinary -Force
    [void](Invoke-Bootstrap -Source $repairSource -Base $repairBase)
    Assert-Equals (Get-Sha256Lower -Path $repairBinary) `
        (Get-Sha256Lower -Path (Join-Path $repairSource 'cup-windows-x64.exe'))

    # Reinstall refuses a newer installed generation.
    $downgradeBase = Join-Path $Script:CupTestRoot 'downgrade-base'
    New-Item -ItemType Directory -Path $downgradeBase | Out-Null
    $downgradeSource = Join-Path $Script:CupTestRoot 'downgrade-source'
    New-BootstrapSource -Path $downgradeSource
    [void](Invoke-Bootstrap -Source $downgradeSource -Base $downgradeBase)
    $downgradeRoot = Join-Path $downgradeBase '.cup'
    $downgradeRelease = Join-Path $downgradeRoot 'release.txt'
    $releaseLines = @(Get-Content -LiteralPath $downgradeRelease)
    for ($i = 0; $i -lt $releaseLines.Count; $i++) {
        if ($releaseLines[$i].StartsWith('version=')) { $releaseLines[$i] = 'version=999.0.0' }
    }
    Write-Utf8NoBom -Path $downgradeRelease -Lines $releaseLines
    $downgradeBinary = Join-Path $downgradeRoot 'bin\cup.exe'
    $downgradeBinaryHash = Get-Sha256Lower -Path $downgradeBinary
    $downgrade = Invoke-Bootstrap -Source $downgradeSource -Base $downgradeBase -ExpectFailure
    Assert-Contains $downgrade.Output 'downgrade refused'
    Assert-Equals (Get-Sha256Lower -Path $downgradeBinary) $downgradeBinaryHash

    # A quiescent root can be relocated and the installer reuses that selected root.
    $relocatedBase = Join-Path $Script:CupTestRoot "relocated base"
    New-Item -ItemType Directory -Path $relocatedBase | Out-Null
    $relocatedRoot = Join-Path $relocatedBase ".cup"
    Move-Item -LiteralPath $root -Destination $relocatedRoot
    $relocatedBinary = Join-Path $relocatedRoot "bin\cup.exe"
    $version = Invoke-NativeProcess -FilePath $relocatedBinary -Arguments @("--version") `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($version.ExitCode -ne 0) { Fail-Test "relocated bootstrap binary failed --version" }
    Assert-PathMissing $root

    $thirdSource = Join-Path $Script:CupTestRoot "third-source"
    New-BootstrapSource -Path $thirdSource
    [void](Invoke-Bootstrap -Source $thirdSource -Base $relocatedBase)
    Assert-PathMissing $root
    Assert-PathExists (Join-Path $relocatedRoot "bin\cup.exe")
    Assert-PathMissing (Join-Path $relocatedRoot "transaction.txt")

    # Exact-set and authenticated-byte failures occur before any root mutation.
    $invalidHome = Join-Path $Script:CupTestRoot "invalid-home"
    New-Item -ItemType Directory -Path $invalidHome | Out-Null
    $invalidSource = Join-Path $Script:CupTestRoot "invalid-source"
    New-BootstrapSource -Path $invalidSource
    Write-Utf8NoBom -Path (Join-Path $invalidSource "extra.txt") -Lines @("extra")
    [void](Invoke-Bootstrap -Source $invalidSource -Base $invalidHome -ExpectFailure)
    Assert-PathMissing (Join-Path $invalidHome ".cup")
    Assert-PathMissing (Join-Path $invalidHome ".coffee-cup")

    Remove-Item -LiteralPath (Join-Path $invalidSource "extra.txt") -Force
    Add-Content -LiteralPath (Join-Path $invalidSource "catalog.cfg") -Value "tampered" -Encoding ascii
    [void](Invoke-Bootstrap -Source $invalidSource -Base $invalidHome -ExpectFailure)
    Assert-PathMissing (Join-Path $invalidHome ".cup")
    Assert-PathMissing (Join-Path $invalidHome ".coffee-cup")

    Write-Host "Windows bootstrap integration tests passed."
} finally {
    Remove-TestEnvironment
}
