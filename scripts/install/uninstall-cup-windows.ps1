# Purpose: Detached Windows helper that removes the canonical cup root after
# the parent process exits.
# Inputs: canonical root, copied helper path, parent identity and inherited handles.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupRoot,

    [Parameter(Mandatory = $true)]
    [string]$SelfPath,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [int]::MaxValue)]
    [int]$ParentPid,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [UInt64]::MaxValue)]
    [UInt64]$ParentHandle,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [UInt64]::MaxValue)]
    [UInt64]$ReadyHandle
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-FileSystemItemOrNull([string]$Path) {
    return Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Get-NormalizedFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "path must not be empty"
    }

    $candidate = $Path.Replace('/', '\')
    if ($candidate.StartsWith(
        "\\?\UNC\",
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        $candidate = "\\" + $candidate.Substring(8)
    } elseif ($candidate.StartsWith(
        "\\?\",
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        $candidate = $candidate.Substring(4)
    } elseif ($candidate.StartsWith(
        "\\.\",
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "device paths are not accepted"
    }

    return [System.IO.Path]::GetFullPath($candidate)
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
    $userProfile = (Get-NormalizedFullPath $env:USERPROFILE).TrimEnd([char[]]'\/')
    $profileRoot = [System.IO.Path]::GetPathRoot($userProfile).TrimEnd([char[]]'\/')
    if ($userProfile -ieq $profileRoot) {
        throw "USERPROFILE must not be a volume root"
    }

    $expectedRoot = Get-NormalizedFullPath (Join-Path $userProfile ".cup")
    $expectedRoot = $expectedRoot.TrimEnd([char[]]'\/')
    $requestedRoot = Get-NormalizedFullPath $CupRoot
    $requestedRoot = $requestedRoot.TrimEnd([char[]]'\/')
    $requestedSelf = Get-NormalizedFullPath $SelfPath
    $runningSelf = Get-NormalizedFullPath $PSCommandPath

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

    $readySafeHandle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new(
        [IntPtr]::new([Int64]$ReadyHandle),
        $true
    )
    $readyStream = [System.IO.FileStream]::new(
        $readySafeHandle,
        [System.IO.FileAccess]::Write
    )
    try {
        $readyStream.WriteByte([byte][char]'R')
        $readyStream.Flush()
    } finally {
        $readyStream.Dispose()
    }

    $parentSafeHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new(
        [IntPtr]::new([Int64]$ParentHandle),
        $true
    )
    $parentWait = [System.Threading.EventWaitHandle]::new(
        $false,
        [System.Threading.EventResetMode]::AutoReset
    )
    $parentWait.SafeWaitHandle = $parentSafeHandle
    try {
        $null = $parentWait.WaitOne()
    } finally {
        $parentWait.Dispose()
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
