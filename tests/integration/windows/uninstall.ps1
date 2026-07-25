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
    while ((Test-Path -LiteralPath $cupRoot) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    Assert-PathMissing $cupRoot

    $leftovers = @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force `
        -ErrorAction SilentlyContinue | Where-Object { $_.Name -like ".cup-uninstall.*" })
    if ($leftovers.Count -ne 0) {
        Fail-Test "uninstall helper left its staging directory behind"
    }

    Write-Host "Windows uninstall tests passed."
} finally {
    Remove-TestEnvironment
}
