# Exercises the public detached Windows uninstall workflow.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

$componentDenyRule = $null
$fixtureDenyRule = $null
$blockedComponents = $null
$blockedFixture = $null
$failedResidue = $null
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

function Get-SystemSortProcessIds {
    $systemDirectory = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
    if ([string]::IsNullOrWhiteSpace($systemDirectory)) {
        return @()
    }
    $expectedPath = [IO.Path]::GetFullPath((Join-Path $systemDirectory "sort.exe"))
    $matches = [System.Collections.Generic.List[int]]::new()
    foreach ($process in Get-Process -Name "sort" -ErrorAction SilentlyContinue) {
        try {
            if ([string]::Equals(
                    [IO.Path]::GetFullPath($process.Path),
                    $expectedPath,
                    [StringComparison]::OrdinalIgnoreCase)) {
                $matches.Add($process.Id)
            }
        } catch {
            continue
        }
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

    $carrierPids = @(Get-SystemSortProcessIds | Where-Object {
        $carrierBaselinePids -notcontains $_
    })
    $carrierText = if ($carrierPids.Count -eq 0) { "none" } else { $carrierPids -join "," }
    $lines.Add("New System32 sort.exe carrier candidates: $carrierText")
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

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    Write-Utf8NoBom -Path (Join-Path $cupRoot "components\fixture.txt") -Lines @("fixture")
    $carrierBaselinePids = @(Get-SystemSortProcessIds)
    $output = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $output "Uninstall started. The PATH entry was not removed."
    Wait-ForCleanUninstall -CanonicalRoot $cupRoot

    # Force native cleanup to fail after detach. Deny DELETE on the file and FILE_DELETE_CHILD on
    # its parent explicitly; Windows permits deletion when either right is available. Verify the
    # fault injection before invoking cup so a broken fixture cannot be mistaken for a cup failure.
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $blockedComponents = Join-Path $cupRoot "components"
    $blockedFixture = Join-Path $blockedComponents "fixture.txt"
    New-Item -ItemType Directory -Force -Path (Join-Path $cupRoot "bin") | Out-Null
    Write-Utf8NoBom -Path $blockedFixture -Lines @("fixture")
    Copy-Item -LiteralPath $Script:CupTestExecutable `
        -Destination (Join-Path $cupRoot "bin\cup.exe") -Force

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $componentDenyRule = [Security.AccessControl.FileSystemAccessRule]::new(
        $identity,
        [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles,
        [Security.AccessControl.AccessControlType]::Deny)
    $fixtureDenyRule = [Security.AccessControl.FileSystemAccessRule]::new(
        $identity,
        [Security.AccessControl.FileSystemRights]::Delete,
        [Security.AccessControl.AccessControlType]::Deny)

    $acl = Get-Acl -LiteralPath $blockedComponents
    [void]$acl.AddAccessRule($componentDenyRule)
    Set-Acl -LiteralPath $blockedComponents -AclObject $acl
    $acl = Get-Acl -LiteralPath $blockedFixture
    [void]$acl.AddAccessRule($fixtureDenyRule)
    Set-Acl -LiteralPath $blockedFixture -AclObject $acl

    $deleteWasDenied = $false
    try {
        [System.IO.File]::Delete($blockedFixture)
    } catch [System.UnauthorizedAccessException] {
        $deleteWasDenied = $true
    }
    if (-not $deleteWasDenied -or
        -not (Test-Path -LiteralPath $blockedFixture -PathType Leaf)) {
        Fail-Test "cleanup-failure ACL fixture did not reliably deny deletion"
    }

    $carrierBaselinePids = @(Get-SystemSortProcessIds)
    $failedOutput = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $failedOutput "Uninstall started. The PATH entry was not removed."

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
    Assert-PathExists (Join-Path $failedResidue "components\fixture.txt")
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

    # Root recovery evidence and temporary-helper lifetime are separate invariants. Even when
    # managed cleanup fails, the DELETE_ON_CLOSE carrier must release the helper after child exit.
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
    # Remove the injected ACLs from every root location that can legitimately own them. Do not
    # depend on the test having recognized the journal correctly: teardown must not mask the
    # original assertion if the root detached but its recovery evidence was malformed.
    $aclRoots = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $blockedComponents) {
        $canonicalAclRoot = Split-Path -Parent $blockedComponents
        if (-not [string]::IsNullOrWhiteSpace($canonicalAclRoot)) {
            $aclRoots.Add($canonicalAclRoot)
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($Script:CupTestHome)) {
        foreach ($detached in @(Get-DetachedUninstallRoots)) {
            if ($aclRoots -notcontains $detached.FullName) {
                $aclRoots.Add($detached.FullName)
            }
        }
    }

    foreach ($aclRoot in $aclRoots) {
        $aclComponents = Join-Path $aclRoot "components"
        $aclFixture = Join-Path $aclComponents "fixture.txt"

        if ($null -ne $fixtureDenyRule -and
            (Test-Path -LiteralPath $aclFixture -PathType Leaf)) {
            try {
                $acl = Get-Acl -LiteralPath $aclFixture
                [void]$acl.RemoveAccessRuleSpecific($fixtureDenyRule)
                Set-Acl -LiteralPath $aclFixture -AclObject $acl
            } catch {
                # Cleanup is best effort; Remove-TestEnvironment remains the final authority.
            }
        }
        if ($null -ne $componentDenyRule -and
            (Test-Path -LiteralPath $aclComponents -PathType Container)) {
            try {
                $acl = Get-Acl -LiteralPath $aclComponents
                [void]$acl.RemoveAccessRuleSpecific($componentDenyRule)
                Set-Acl -LiteralPath $aclComponents -AclObject $acl
            } catch {
                # Cleanup is best effort; Remove-TestEnvironment remains the final authority.
            }
        }
    }
    Remove-TestEnvironment
}
