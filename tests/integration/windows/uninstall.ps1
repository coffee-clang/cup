# Purpose: Exercises the public detached Windows uninstall workflow.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

try {
    Initialize-TestEnvironment -Name "uninstall" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Write-Utf8NoBom -Path (Join-Path $cupRoot "components\fixture.txt") -Lines @("fixture")
    $output = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $output "Uninstall started. The PATH entry was not removed."

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $leftovers = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        $leftovers = @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force `
            -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall.*" })
        if (-not (Test-Path -LiteralPath $cupRoot) -and $leftovers.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-PathMissing $cupRoot
    $leftovers = @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall.*" })
    if ($leftovers.Count -ne 0) {
        Fail-Test "uninstall helper left staging behind: $($leftovers[0].FullName)"
    }

    Write-Host "Windows uninstall tests passed."
} finally {
    Remove-TestEnvironment
}
