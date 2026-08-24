# Exercises the public detached Windows uninstall workflow.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

$denyRule = $null
$blockedComponents = $null
$failedResidue = $null

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
            -ErrorAction SilentlyContinue | Where-Object {
                $_.Name -like ".cup-uninstall-*"
            })
        if (-not (Test-Path -LiteralPath $cupRoot) -and $leftovers.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $cupRoot) {
        $journalPath = Join-Path $cupRoot "transaction.txt"
        $journalText = if (Test-Path -LiteralPath $journalPath -PathType Leaf) {
            Get-Content -LiteralPath $journalPath -Raw -ErrorAction SilentlyContinue
        } else {
            "<missing>"
        }
        $leftoverText = if ($leftovers.Count -eq 0) {
            "<none>"
        } else {
            ($leftovers | ForEach-Object { $_.FullName }) -join "`n"
        }
        $message = "uninstall did not detach the canonical root within 20 seconds: $cupRoot`n" +
            "Canonical journal:`n$journalText`n" +
            "Detached residue candidates:`n$leftoverText"
        Fail-Test $message
    }
    $leftovers = @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force `
        -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -like ".cup-uninstall-*"
        })
    if ($leftovers.Count -ne 0) {
        Fail-Test "uninstall helper left staging behind: $($leftovers[0].FullName)"
    }

    # A native cleanup failure must preserve transaction.txt while managed residue remains. Use a
    # Windows ACL denial here rather than a deep PowerShell-created path: the test must exercise CUP's
    # native cleanup failure, not PowerShell 5.1 long-path behavior.
    Invoke-Cup -CommandArgs @("repair") | Out-Null
    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $blockedComponents = Join-Path $cupRoot "components"
    New-Item -ItemType Directory -Force -Path (Join-Path $cupRoot "bin") | Out-Null
    Write-Utf8NoBom -Path (Join-Path $blockedComponents "fixture.txt") -Lines @("fixture")
    Copy-Item -LiteralPath $Script:CupTestExecutable `
        -Destination (Join-Path $cupRoot "bin\cup.exe") -Force

    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $rights = [Security.AccessControl.FileSystemRights]::Delete -bor `
        [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles
    $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor `
        [Security.AccessControl.InheritanceFlags]::ObjectInherit
    $denyRule = [Security.AccessControl.FileSystemAccessRule]::new(
        $identity,
        $rights,
        $inheritance,
        [Security.AccessControl.PropagationFlags]::None,
        [Security.AccessControl.AccessControlType]::Deny)
    $acl = Get-Acl -LiteralPath $blockedComponents
    [void]$acl.AddAccessRule($denyRule)
    Set-Acl -LiteralPath $blockedComponents -AclObject $acl

    $failedOutput = Invoke-Cup -CommandArgs @("uninstall", "--yes")
    Assert-Contains $failedOutput "Uninstall started. The PATH entry was not removed."

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline) {
        $candidate = @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force `
            -ErrorAction SilentlyContinue | Where-Object {
                $_.Name -like ".cup-uninstall-*"
            } |
            Select-Object -First 1)
        if ($candidate.Count -eq 1) {
            $journal = Join-Path $candidate[0].FullName "transaction.txt"
            if (Test-Path -LiteralPath $journal -PathType Leaf) {
                $journalText = Get-Content -LiteralPath $journal -Raw
                if ($journalText.Contains("phase=detaching") -and
                    $journalText.Contains("stage=detach")) {
                    $failedResidue = $candidate[0].FullName
                    break
                }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $failedResidue) {
        Fail-Test "failed uninstall did not leave an identifiable detached residue"
    }
    Assert-PathMissing $cupRoot
    Assert-PathExists (Join-Path $failedResidue "transaction.txt")
    Assert-PathExists (Join-Path $failedResidue "components\fixture.txt")

    # Cleanup failure belongs to the detached managed root, not to the temporary executable's
    # lifetime. The deferred DELETE_ON_CLOSE carrier must still disappear after the helper exits.
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $helperResidue = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        $helperResidue = @(Get-ChildItem -LiteralPath $Script:CupTestHome -Force `
            -ErrorAction SilentlyContinue | Where-Object {
                $_.Name -like ".cup-uninstall-helper-*"
            })
        if ($helperResidue.Count -eq 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($helperResidue.Count -ne 0) {
        Fail-Test "failed uninstall left temporary helper behind: $($helperResidue[0].FullName)"
    }

    Write-Host "Windows uninstall tests passed."
} finally {
    if ($null -ne $denyRule) {
        $aclPath = if ($null -ne $failedResidue) {
            Join-Path $failedResidue "components"
        } else {
            $blockedComponents
        }
        if ($null -ne $aclPath -and (Test-Path -LiteralPath $aclPath -PathType Container)) {
            try {
                $acl = Get-Acl -LiteralPath $aclPath
                [void]$acl.RemoveAccessRuleSpecific($denyRule)
                Set-Acl -LiteralPath $aclPath -AclObject $acl
            } catch {
                # Cleanup is best effort.
            }
        }
    }
    Remove-TestEnvironment
}
