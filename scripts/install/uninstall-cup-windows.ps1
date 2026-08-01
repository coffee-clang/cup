# Purpose: Detached Windows helper that removes the canonical CUP root after the parent exits.
# Inputs: selected root, copied helper path, parent identity and inherited handles.

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
$script:JournalRoot = ""
$script:TemporaryName = ""
$script:Token = ""
$script:FailureStage = "handoff"

function Get-FileSystemItemOrNull([string]$Path) {
    return Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Get-NormalizedFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "path must not be empty"
    }

    $candidate = $Path.Replace('/', '\')
    if ($candidate.StartsWith("\\?\UNC\", [StringComparison]::OrdinalIgnoreCase)) {
        $candidate = "\\" + $candidate.Substring(8)
    } elseif ($candidate.StartsWith("\\?\", [StringComparison]::OrdinalIgnoreCase)) {
        $candidate = $candidate.Substring(4)
    } elseif ($candidate.StartsWith("\\.\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "device paths are not accepted"
    }
    return [IO.Path]::GetFullPath($candidate)
}

function Write-UninstallJournal(
    [string]$Root,
    [string]$Phase,
    [string]$Stage,
    [int]$ErrorCode
) {
    $rootItem = Get-FileSystemItemOrNull $Root
    if ($null -eq $rootItem -or -not $rootItem.PSIsContainer -or
        ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "uninstall journal root is unavailable"
    }
    $journal = Join-Path $Root "transaction.txt"
    $temporary = Join-Path $Root (
        ".uninstall-transaction." + [Guid]::NewGuid().ToString("N")
    )
    $content = @(
        "format=1"
        "operation=uninstall"
        "phase=$Phase"
        "temporary_name=$($script:TemporaryName)"
        "token=$($script:Token)"
        "stage=$Stage"
        "error=$ErrorCode"
        ""
    ) -join "`n"
    try {
        [IO.File]::WriteAllText($temporary, $content, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $journal -PathType Leaf) {
            [IO.File]::Replace($temporary, $journal, $null, $true)
        } else {
            [IO.File]::Move($temporary, $journal)
        }
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
    }
}

function Write-UninstallFailure {
    if ([string]::IsNullOrWhiteSpace($script:JournalRoot) -or
        [string]::IsNullOrWhiteSpace($script:TemporaryName) -or
        [string]::IsNullOrWhiteSpace($script:Token)) {
        return
    }
    try {
        Write-UninstallJournal $script:JournalRoot "failed" $script:FailureStage 1
    } catch {
        # Preserve the original failure; no secondary best-effort file is created.
    }
}

function Remove-TreeNoFollow([string]$Path) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item) { return }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        if ($item.PSIsContainer) {
            [IO.Directory]::Delete($item.FullName, $false)
        } else {
            [IO.File]::Delete($item.FullName)
        }
        return
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
        $attributes = $item.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly)
        [IO.File]::SetAttributes($item.FullName, $attributes)
        $item = Get-FileSystemItemOrNull $Path
        if ($null -eq $item) { return }
    }
    if (-not $item.PSIsContainer) {
        [IO.File]::Delete($item.FullName)
        return
    }
    foreach ($child in @(Get-ChildItem -LiteralPath $item.FullName -Force)) {
        Remove-TreeNoFollow $child.FullName
    }
    [IO.Directory]::Delete($item.FullName, $false)
}

function Remove-DetachedCupRoot([string]$Root) {
    # Preserve the ownership marker, canonical executable and transaction until every unrelated
    # entry is gone. A failed cleanup then remains recognizable by the installer.
    foreach ($child in @(Get-ChildItem -LiteralPath $Root -Force)) {
        if ($child.Name.Equals("transaction.txt", [StringComparison]::OrdinalIgnoreCase) -or
            $child.Name.Equals("root.txt", [StringComparison]::OrdinalIgnoreCase) -or
            $child.Name.Equals("bin", [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        Remove-TreeNoFollow $child.FullName
    }

    $bin = Join-Path $Root "bin"
    foreach ($child in @(Get-ChildItem -LiteralPath $bin -Force)) {
        if ($child.Name.Equals("cup.exe", [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        Remove-TreeNoFollow $child.FullName
    }

    [IO.File]::Delete((Join-Path $bin "cup.exe"))
    [IO.Directory]::Delete($bin, $false)
    [IO.File]::Delete((Join-Path $Root "root.txt"))
    [IO.File]::Delete((Join-Path $Root "transaction.txt"))
    [IO.Directory]::Delete($Root, $false)
}

try {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE) -or
        -not [IO.Path]::IsPathRooted($env:USERPROFILE)) {
        throw "USERPROFILE must contain an absolute path"
    }
    $userProfile = (Get-NormalizedFullPath $env:USERPROFILE).TrimEnd([char[]]'\/')
    $profileRoot = [IO.Path]::GetPathRoot($userProfile).TrimEnd([char[]]'\/')
    if ($userProfile -ieq $profileRoot) {
        throw "USERPROFILE must not be a volume root"
    }

    $primaryRoot = (Get-NormalizedFullPath (Join-Path $userProfile ".cup")).TrimEnd([char[]]'\/')
    $alternativeRoot = (
        Get-NormalizedFullPath (Join-Path $userProfile ".coffee-cup")
    ).TrimEnd([char[]]'\/')
    $requestedRoot = (Get-NormalizedFullPath $CupRoot).TrimEnd([char[]]'\/')
    $script:JournalRoot = $requestedRoot
    $requestedSelf = Get-NormalizedFullPath $SelfPath
    $runningSelf = Get-NormalizedFullPath $PSCommandPath

    if (-not $requestedRoot.Equals($primaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
        -not $requestedRoot.Equals($alternativeRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to remove an unsupported cup root"
    }
    if (-not $requestedSelf.Equals($runningSelf, [StringComparison]::OrdinalIgnoreCase)) {
        throw "self path does not match the running uninstall helper"
    }

    $selfItem = Get-FileSystemItemOrNull $requestedSelf
    if ($null -eq $selfItem -or $selfItem.PSIsContainer -or
        ($selfItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "the running uninstall helper is not a regular file"
    }
    $rootItem = Get-FileSystemItemOrNull $requestedRoot
    if ($null -eq $rootItem -or -not $rootItem.PSIsContainer -or
        ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "canonical cup root is not a real directory"
    }

    $rootMarkerPath = Join-Path $requestedRoot "root.txt"
    $rootMarkerItem = Get-FileSystemItemOrNull $rootMarkerPath
    if ($null -eq $rootMarkerItem -or $rootMarkerItem.PSIsContainer -or
        ($rootMarkerItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "cup root marker is missing or invalid"
    }
    $rootMarkerLines = @(Get-Content -LiteralPath $rootMarkerPath)
    if ($rootMarkerLines.Count -ne 3 -or
        $rootMarkerLines[0] -cne "format=1" -or
        $rootMarkerLines[1] -cne "product=coffee-clang/cup" -or
        $rootMarkerLines[2] -cne "layout=1") {
        throw "cup root marker is missing or invalid"
    }

    $journalPath = Join-Path $requestedRoot "transaction.txt"
    $journalItem = Get-FileSystemItemOrNull $journalPath
    if ($null -eq $journalItem -or $journalItem.PSIsContainer -or
        ($journalItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "uninstall transaction is missing or invalid"
    }
    $journalLines = @(Get-Content -LiteralPath $journalPath)
    if ($journalLines.Count -ne 7 -or
        $journalLines[0] -cne "format=1" -or
        $journalLines[1] -cne "operation=uninstall" -or
        $journalLines[2] -cne "phase=scheduled" -or
        -not $journalLines[3].StartsWith("temporary_name=", [StringComparison]::Ordinal) -or
        -not $journalLines[4].StartsWith("token=", [StringComparison]::Ordinal) -or
        $journalLines[5] -cne "stage=handoff" -or
        $journalLines[6] -cne "error=0") {
        throw "uninstall transaction is missing or invalid"
    }
    $script:TemporaryName = $journalLines[3].Substring("temporary_name=".Length)
    $script:Token = $journalLines[4].Substring("token=".Length)
    if ($script:Token -notmatch '^[A-Za-z0-9_-]+$' -or
        $script:TemporaryName -cne ".cup-uninstall.$($script:Token)") {
        throw "uninstall transaction identity is invalid"
    }
    $detachedRoot = Join-Path $userProfile $script:TemporaryName
    if (Test-Path -LiteralPath $detachedRoot) {
        throw "uninstall destination already exists"
    }

    Write-UninstallJournal $requestedRoot "scheduled" "parent-wait" 0

    $readySafeHandle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new(
        [IntPtr]::new([Int64]$ReadyHandle),
        $true
    )
    $readyStream = [IO.FileStream]::new($readySafeHandle, [IO.FileAccess]::Write)
    try {
        $readyStream.WriteByte([byte][char]'R')
        $readyStream.Flush()
    } finally {
        $readyStream.Dispose()
    }
    $script:FailureStage = "parent-wait"

    $parentSafeHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new(
        [IntPtr]::new([Int64]$ParentHandle),
        $true
    )
    $parentWait = [Threading.EventWaitHandle]::new(
        $false,
        [Threading.EventResetMode]::AutoReset
    )
    $parentWait.SafeWaitHandle = $parentSafeHandle
    try {
        $null = $parentWait.WaitOne()
    } finally {
        $parentWait.Dispose()
    }

    Write-UninstallJournal $requestedRoot "detaching" "detach" 0
    $script:FailureStage = "detach"
    [IO.Directory]::Move($requestedRoot, $detachedRoot)
    $script:JournalRoot = $detachedRoot

    $script:FailureStage = "cleanup"
    Write-UninstallJournal $detachedRoot "failed" "cleanup" 1
    Remove-DetachedCupRoot $detachedRoot

    if (Test-Path -LiteralPath $requestedSelf -PathType Leaf) {
        Remove-Item -LiteralPath $requestedSelf -Force
    }
    exit 0
} catch {
    Write-UninstallFailure
    Write-Error $_
    exit 1
}
