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
Set-StrictMode -Version Latest

function Get-FileSystemItemOrNull([string]$Path) {
    return Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Remove-TreeNoFollow([string]$Path) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item) { return }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        if ($item.PSIsContainer) {
            [System.IO.Directory]::Delete($item.FullName, $false)
        } else {
            [System.IO.File]::Delete($item.FullName)
        }
        return
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
        $attributes = $item.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly)
        [System.IO.File]::SetAttributes($item.FullName, $attributes)
        $item = Get-FileSystemItemOrNull $Path
        if ($null -eq $item) { return }
    }
    if (-not $item.PSIsContainer) {
        [System.IO.File]::Delete($item.FullName)
        return
    }
    foreach ($child in @(Get-ChildItem -LiteralPath $item.FullName -Force)) {
        Remove-TreeNoFollow $child.FullName
    }
    [System.IO.Directory]::Delete($item.FullName, $false)
}

try {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE) -or
        -not [System.IO.Path]::IsPathRooted($env:USERPROFILE)) {
        throw "USERPROFILE must contain an absolute path"
    }
    $userProfile = [System.IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd([char[]]'\/')
    $profileRoot = [System.IO.Path]::GetPathRoot($userProfile).TrimEnd([char[]]'\/')
    if ($userProfile -ieq $profileRoot) {
        throw "USERPROFILE must not be a volume root"
    }

    $expectedRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $userProfile ".cup")
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

    $selfItem = Get-FileSystemItemOrNull $requestedSelf
    if ($null -eq $selfItem -or $selfItem.PSIsContainer -or
        ($selfItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "the running uninstall helper is not a regular file"
    }
    $rootItem = Get-FileSystemItemOrNull $requestedRoot
    if ($null -ne $rootItem -and
        (-not $rootItem.PSIsContainer -or
         ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "canonical cup root is not a real directory"
    }
    $markerPath = Join-Path $requestedRoot "uninstall.pending"
    $markerItem = Get-FileSystemItemOrNull $markerPath
    if ($null -eq $markerItem -or $markerItem.PSIsContainer -or
        ($markerItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "uninstall marker is missing or invalid"
    }
    $markerLines = @(Get-Content -LiteralPath $markerPath)
    if ($markerLines.Count -ne 1 -or
        $markerLines[0] -cne "parent_pid=$ParentPid") {
        throw "uninstall marker does not match the parent process"
    }

    try {
        Wait-Process -Id $ParentPid
    } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
        # The parent already exited before the helper started waiting.
    }

    if (Test-Path -LiteralPath $requestedRoot) {
        $stagingRoot = Join-Path $userProfile (
            ".cup-uninstall." + [Guid]::NewGuid().ToString("N")
        )
        $stagedCupRoot = Join-Path $stagingRoot "root"

        [System.IO.Directory]::CreateDirectory($stagingRoot) | Out-Null
        try {
            [System.IO.Directory]::Move($requestedRoot, $stagedCupRoot)
        } catch {
            if (Test-Path -LiteralPath $stagingRoot) {
                Remove-TreeNoFollow $stagingRoot
            }
            throw "could not detach $requestedRoot`: $_"
        }

        Remove-TreeNoFollow $stagingRoot
    }

    if (Test-Path -LiteralPath $requestedSelf -PathType Leaf) {
        Remove-Item -LiteralPath $requestedSelf -Force
    }

    exit 0
} catch {
    Write-Error $_
    exit 1
}
