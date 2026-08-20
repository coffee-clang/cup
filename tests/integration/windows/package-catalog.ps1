# Exercises Windows development-catalog fallback, checksum-schema and secure-URL validation
# through the real CLI. Installed-source precedence is covered by the catalog unit suite.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name "package-catalog" -ExecutablePath $CupExecutablePath
    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $original = Get-Content -LiteralPath $catalog

    $removed = $false
    $missingChecksum = foreach ($line in $original) {
        if (-not $removed -and $line.Contains(".checksum_url_template=")) {
            $removed = $true
        } else {
            $line
        }
    }
    if (-not $removed) {
        Fail-Test "could not remove checksum_url_template"
    }
    Write-Utf8NoBom -Path $catalog -Lines $missingChecksum
    Assert-Contains (Invoke-Cup -CommandArgs @("search") -ExpectFailure) `
        "is missing one or more required fields"

    $changed = $false
    $insecure = foreach ($line in $original) {
        if (-not $changed -and $line.Contains(".checksum_url_template=https:")) {
            $changed = $true
            $line.Replace("=https:", "=http:")
        } else {
            $line
        }
    }
    if (-not $changed) {
        Fail-Test "could not alter checksum URL"
    }
    Write-Utf8NoBom -Path $catalog -Lines $insecure
    Assert-Contains (Invoke-Cup -CommandArgs @("search") -ExpectFailure) `
        "catalog URL templates must use HTTPS"

    Write-Host "Windows package-catalog tests passed."
} finally {
    Remove-TestEnvironment
}
