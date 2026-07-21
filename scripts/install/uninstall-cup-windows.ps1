# Purpose: Detached Windows helper that removes the canonical cup root after
# the parent process exits.
# Inputs: canonical root, copied helper path and parent process id.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupRoot,

    [Parameter(Mandatory = $true)]
    [string]$SelfPath,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$ParentPid
)

$ErrorActionPreference = "Stop"

try {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        throw "USERPROFILE is not set"
    }

    $expectedRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $env:USERPROFILE ".cup")
    ).TrimEnd('\')
    $requestedRoot = [System.IO.Path]::GetFullPath($CupRoot).TrimEnd('\')
    $requestedSelf = [System.IO.Path]::GetFullPath($SelfPath)
    $runningSelf = [System.IO.Path]::GetFullPath($PSCommandPath)

    if (-not $requestedRoot.Equals(
        $expectedRoot,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "refusing to remove a non-canonical cup root"
    }
    if (-not $requestedSelf.Equals(
        $runningSelf,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "self path does not match the running uninstall helper"
    }

    try {
        Wait-Process -Id $ParentPid
    } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        # The parent already exited before the helper started waiting.
    }

    if (Test-Path -LiteralPath $requestedRoot) {
        $stagingRoot = Join-Path $env:USERPROFILE (
            ".cup-uninstall." + [Guid]::NewGuid().ToString("N")
        )
        $stagedCupRoot = Join-Path $stagingRoot "root"

        [System.IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
        try {
            [System.IO.Directory]::Move($requestedRoot, $stagedCupRoot)
        } catch {
            if (Test-Path -LiteralPath $stagingRoot) {
                Remove-Item -LiteralPath $stagingRoot -Force
            }
            throw "could not detach $requestedRoot`: $_"
        }

        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }

    if (Test-Path -LiteralPath $requestedSelf -PathType Leaf) {
        Remove-Item -LiteralPath $requestedSelf -Force
    }

    exit 0
} catch {
    Write-Error $_
    exit 1
}
