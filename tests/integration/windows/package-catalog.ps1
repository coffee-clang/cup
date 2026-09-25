# Exercises Windows development-catalog fallback and the concrete artifact schema through the real CLI.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name 'package-catalog' -ExecutablePath $CupExecutablePath
    $catalog = Join-Path $Script:CupTestDevRoot 'config\catalog.cfg'

    # Source-only discovery uses the local development catalog without creating a runtime root.
    Invoke-Cup -CommandArgs @('search', 'compiler') | Out-Null
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup')

    Write-Utf8NoBom -Path $catalog -Lines @(
        'format=1',
        'revision=1',
        'update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg',
        'package.0.component=compiler',
        'package.0.tool=clang',
        'package.0.host=windows-x64',
        'package.0.target=windows-x64',
        'package.0.version=98.0.1',
        'package.0.stable=true',
        'package.0.artifact.0.format=zip',
        'package.0.artifact.0.url=https://example.invalid/clang-98.0.1-windows-x64-windows-x64.zip',
        ('package.0.artifact.0.sha256=' + ('0' * 64)))
    $output = Invoke-Cup -CommandArgs @('search', 'compiler')
    Assert-Contains $output '98.0.1'
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup')

    $missingSha = Get-Content -LiteralPath $catalog | Where-Object {
        -not $_.StartsWith('package.0.artifact.0.sha256=', [StringComparison]::Ordinal)
    }
    Write-Utf8NoBom -Path $catalog -Lines $missingSha
    Assert-CupStatus -CommandArgs @('search', 'compiler') -ExpectedStatus 4 | Out-Null

    Write-Utf8NoBom -Path $catalog -Lines @(
        'format=1',
        'revision=1',
        'update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg',
        'package.0.component=compiler',
        'package.0.tool=clang',
        'package.0.host=windows-x64',
        'package.0.target=windows-x64',
        'package.0.version=98.0.1',
        'package.0.stable=true',
        'package.0.artifact.0.format=zip',
        'package.0.artifact.0.url=http://example.invalid/clang-98.0.1-windows-x64-windows-x64.zip',
        ('package.0.artifact.0.sha256=' + ('0' * 64)))
    Assert-CupStatus -CommandArgs @('search', 'compiler') -ExpectedStatus 4 | Out-Null

    Write-Utf8NoBom -Path $catalog -Lines @(
        'format=1',
        'revision=2',
        'update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg',
        'package.0.component=compiler',
        'package.0.tool=clang',
        'package.0.host=windows-x64',
        'package.0.target=windows-x64',
        'package.0.version=future-1',
        'package.0.stable=true',
        'package.0.artifact.0.format=zip',
        'package.0.artifact.0.url=https://example.invalid/clang-future-1-windows-x64-windows-x64.zip',
        ('package.0.artifact.0.sha256=' + ('0' * 64)))
    $future = Invoke-Cup -CommandArgs @('search', 'compiler')
    Assert-NotContains $future 'future-1'
    Assert-PathMissing (Join-Path $Script:CupTestHome '.cup')

    Write-Host 'Windows package-catalog tests passed.'
} finally {
    Remove-TestEnvironment
}
