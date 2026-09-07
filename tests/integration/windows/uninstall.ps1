# Exercises the public detached Windows uninstall workflow.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

$carrierBaselinePids = @()

function Get-DetachedUninstallRoots {
    return @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force -Directory `
        -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -like ".cup-uninstall-*"
        })
}

function Get-UninstallHelperFiles {
    return @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force -File `
        -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -like ".cup-uninstall-helper-*"
        })
}

function Get-ProcessIdsForExecutable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $target = [IO.Path]::GetFullPath($Path)
    $matches = [System.Collections.Generic.List[int]]::new()
    foreach ($process in Get-Process -ErrorAction SilentlyContinue) {
        try {
            $processPath = $process.Path
            if (-not [string]::IsNullOrWhiteSpace($processPath) -and
                [string]::Equals(
                    [IO.Path]::GetFullPath($processPath),
                    $target,
                    [StringComparison]::OrdinalIgnoreCase)) {
                $matches.Add($process.Id)
            }
        } catch {
            continue
        }
    }
    return @($matches.ToArray())
}

function Get-CleanupCarrierProcessIds {
    $matches = [System.Collections.Generic.List[int]]::new()
    try {
        foreach ($process in Get-CimInstance Win32_Process `
            -Filter "Name = 'powershell.exe'" -ErrorAction Stop) {
            if (-not [string]::IsNullOrWhiteSpace($process.CommandLine) -and
                $process.CommandLine.IndexOf(
                    "CUP_UNINSTALL_CLEANUP_CARRIER=1",
                    [StringComparison]::Ordinal) -ge 0) {
                $matches.Add([int]$process.ProcessId)
            }
        }
    } catch {
        return @()
    }
    return @($matches.ToArray())
}

function Read-JournalForDiagnostics {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $journal = Join-Path $Root "transaction.txt"
    if (-not (Test-Path -LiteralPath $journal -PathType Leaf)) {
        return "<missing>"
    }
    try {
        return (Get-Content -LiteralPath $journal -Raw -ErrorAction Stop).TrimEnd()
    } catch {
        return "<unreadable: $($_.Exception.Message)>"
    }
}

function Get-UninstallDiagnostics {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CanonicalRoot
    )

    $lines = [System.Collections.Generic.List[string]]::new()
    $canonicalState = if (Test-Path -LiteralPath $CanonicalRoot -PathType Container) {
        "present"
    } else {
        "missing"
    }
    $lines.Add("Canonical root: $canonicalState ($CanonicalRoot)")
    if ($canonicalState -eq "present") {
        $lines.Add("Canonical journal:")
        $lines.Add((Read-JournalForDiagnostics -Root $CanonicalRoot))
    }

    $detachedRoots = @(Get-DetachedUninstallRoots)
    if ($detachedRoots.Count -eq 0) {
        $lines.Add("Detached roots: <none>")
    } else {
        $lines.Add("Detached roots:")
        foreach ($root in $detachedRoots) {
            $lines.Add("- $($root.FullName)")
            $lines.Add((Read-JournalForDiagnostics -Root $root.FullName))
        }
    }

    $helpers = @(Get-UninstallHelperFiles)
    if ($helpers.Count -eq 0) {
        $lines.Add("Temporary helpers: <none>")
    } else {
        $lines.Add("Temporary helpers:")
        foreach ($helper in $helpers) {
            $pids = @(Get-ProcessIdsForExecutable -Path $helper.FullName)
            $pidText = if ($pids.Count -eq 0) { "none" } else { $pids -join "," }
            $lines.Add("- $($helper.FullName) (running-pids=$pidText)")
        }
    }

    $carrierPids = @(Get-CleanupCarrierProcessIds | Where-Object {
        $carrierBaselinePids -notcontains $_
    })
    $carrierText = if ($carrierPids.Count -eq 0) { "none" } else { $carrierPids -join "," }
    $lines.Add("New uninstall cleanup carrier candidates: $carrierText")
    return ($lines -join "`n")
}

function Wait-ForCleanUninstall {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CanonicalRoot,

        [ValidateRange(1, 120)]
        [int]$TimeoutSeconds = 20
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $detachedRoots = @(Get-DetachedUninstallRoots)
        $helpers = @(Get-UninstallHelperFiles)
        if (-not (Test-Path -LiteralPath $CanonicalRoot) -and
            $detachedRoots.Count -eq 0 -and
            $helpers.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 100
    }

    Fail-Test (
        "uninstall did not fully clean up within $TimeoutSeconds seconds`n" +
        (Get-UninstallDiagnostics -CanonicalRoot $CanonicalRoot))
}

try {
    Initialize-TestEnvironment -Name "uninstall" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $uninstallStartedMessage =
        "Uninstall handoff accepted; cleanup continues in the background. " +
        "You can close this terminal. The PATH entry was not removed."

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Write-Utf8NoBom -Path (Join-Path $cupRoot "components\fixture.txt") -Lines @("fixture")
    $carrierBaselinePids = @(Get-CleanupCarrierProcessIds)
    $output = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $output $uninstallStartedMessage
    Assert-Contains $output "Recovery path if cleanup fails: "
    Assert-Contains $output ".cup-uninstall-"
    Wait-ForCleanUninstall -CanonicalRoot $cupRoot

    Write-Host "Windows uninstall tests passed."
} finally {
    Remove-TestEnvironment
}
