# Exercises the public detached Windows uninstall workflow.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

$failedResidue = $null
$carrierBaselinePids = @()
$longPathFixtureRelative = "components\cleanup-long-path"

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

function Convert-ToExtendedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $full = if ([IO.Path]::IsPathRooted($Path)) { $Path } else { [IO.Path]::GetFullPath($Path) }
    if ($full.StartsWith('\\')) {
        return '\\?\UNC\' + $full.Substring(2)
    }
    return '\\?\' + $full
}

function New-CleanupLongPathFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CanonicalRoot
    )

    $current = Join-Path $CanonicalRoot $longPathFixtureRelative
    [void][IO.Directory]::CreateDirectory((Convert-ToExtendedPath -Path $current))

    # Keep recursion shallow enough for sanitizer builds while making the ASCII path comfortably
    # exceed CUP's 1024-byte internal path buffer. Cleanup therefore fails after detach at the
    # path-representation boundary rather than because of ACLs, open handles or timing.
    for ($index = 0; $index -lt 18; $index++) {
        $segment = ('segment-{0:D2}-' -f $index) + ('x' * 52)
        $current = Join-Path $current $segment
        [void][IO.Directory]::CreateDirectory((Convert-ToExtendedPath -Path $current))
    }

    $full = $current
    if ($full.Length -le 1100 -or
        -not [IO.Directory]::Exists((Convert-ToExtendedPath -Path $current))) {
        Fail-Test "cleanup long-path fixture was not created beyond the CUP path bound"
    }
}

function Remove-CleanupLongPathFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $fixture = Join-Path $Root $longPathFixtureRelative
    $extended = Convert-ToExtendedPath -Path $fixture
    if ([IO.Directory]::Exists($extended)) {
        [IO.Directory]::Delete($extended, $true)
    }
}

function Test-DetachedUninstallJournal {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.DirectoryInfo]$Candidate
    )

    if (-not $Candidate.Name.StartsWith(
            ".cup-uninstall-", [StringComparison]::Ordinal) -or
        $Candidate.Name.Length -le 15) {
        return $false
    }
    $journal = Join-Path $Candidate.FullName "transaction.txt"
    if (-not (Test-Path -LiteralPath $journal -PathType Leaf)) {
        return $false
    }

    $token = $Candidate.Name.Substring(15)
    $expected = @(
        "format=1",
        "operation=uninstall",
        "phase=detaching",
        "temporary_name=$($Candidate.Name)",
        "token=$token",
        "stage=detach",
        "error=0"
    )
    $actual = @(Get-Content -LiteralPath $journal -ErrorAction Stop)
    if ($actual.Count -ne $expected.Count) {
        return $false
    }
    foreach ($line in $expected) {
        if ($actual -cnotcontains $line) {
            return $false
        }
    }
    return $true
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
        "Uninstall started; cleanup continues in the background. " +
        "You can close this terminal. The PATH entry was not removed."

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Write-Utf8NoBom -Path (Join-Path $cupRoot "components\fixture.txt") -Lines @("fixture")
    $carrierBaselinePids = @(Get-CleanupCarrierProcessIds)
    $output = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $output $uninstallStartedMessage
    Wait-ForCleanUninstall -CanonicalRoot $cupRoot

    # Force cleanup to fail deterministically after detach with a shallow tree whose full path
    # exceeds CUP's internal path representation. This avoids ACL, open-handle and timing effects.
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    New-CleanupLongPathFixture -CanonicalRoot $cupRoot

    $carrierBaselinePids = @(Get-CleanupCarrierProcessIds)
    $failedOutput = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $failedOutput $uninstallStartedMessage

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        $matchingRoots = [System.Collections.Generic.List[string]]::new()
        foreach ($candidate in @(Get-DetachedUninstallRoots)) {
            if (Test-DetachedUninstallJournal -Candidate $candidate) {
                $matchingRoots.Add($candidate.FullName)
            }
        }
        if ($matchingRoots.Count -gt 1) {
            Fail-Test (
                "failed uninstall left multiple journal-bearing detached roots`n" +
                (Get-UninstallDiagnostics -CanonicalRoot $cupRoot))
        }
        if ($matchingRoots.Count -eq 1) {
            $failedResidue = $matchingRoots[0]
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $failedResidue) {
        Fail-Test (
            "failed uninstall did not leave the expected detached recovery evidence`n" +
            (Get-UninstallDiagnostics -CanonicalRoot $cupRoot))
    }

    Assert-PathMissing $cupRoot
    Assert-PathExists (Join-Path $failedResidue "transaction.txt")
    Assert-PathExists (Join-Path $failedResidue $longPathFixtureRelative)
    $detachedRoots = @(Get-DetachedUninstallRoots)
    if ($detachedRoots.Count -ne 1 -or
        -not [string]::Equals(
            $detachedRoots[0].FullName,
            $failedResidue,
            [StringComparison]::OrdinalIgnoreCase)) {
        Fail-Test (
            "failed uninstall did not preserve exactly one owned detached root`n" +
            (Get-UninstallDiagnostics -CanonicalRoot $cupRoot))
    }

    # Recovery evidence and temporary-helper lifetime are separate invariants. Even when managed
    # cleanup fails, the process-wait carrier must release the helper after actual child exit.
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (@(Get-UninstallHelperFiles).Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (@(Get-UninstallHelperFiles).Count -ne 0) {
        Fail-Test (
            "failed uninstall left a temporary helper behind`n" +
            (Get-UninstallDiagnostics -CanonicalRoot $cupRoot))
    }

    Write-Host "Windows uninstall tests passed."
} finally {
    # The deliberate long-path residue is outside CUP's normal path representation. Remove just
    # that fixture through an extended path before the shared test-environment teardown.
    if (-not [string]::IsNullOrWhiteSpace($Script:CupTestHome)) {
        $canonical = Join-Path $Script:CupTestHome ".cup"
        try {
            Remove-CleanupLongPathFixture -Root $canonical
        } catch {
            # Preserve the primary test result; shared teardown remains the final cleanup authority.
        }
        foreach ($detached in @(Get-DetachedUninstallRoots)) {
            try {
                Remove-CleanupLongPathFixture -Root $detached.FullName
            } catch {
                # Preserve the primary test result; shared teardown remains the final cleanup authority.
            }
        }
    }
    Remove-TestEnvironment
}
