# The detached Windows helper waits for the parent, detaches the canonical cup root, then removes the detached tree.
# It receives the selected root, copied helper path and inherited parent-lifetime handles.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupRoot,

    [Parameter(Mandatory = $true)]
    [string]$SelfPath,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [UInt64]::MaxValue)]
    [UInt64]$ParentHandle,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [UInt64]::MaxValue)]
    [UInt64]$ReadyHandle,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [UInt64]::MaxValue)]
    [UInt64]$LeaseHandle
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$script:JournalRoot = ""
$script:TemporaryName = ""
$script:Token = ""
$script:FailureStage = "handoff"
$script:Lease = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new(
    [IntPtr]::new([Int64]$LeaseHandle),
    $true
)

function Close-UninstallLease {
    if ($null -ne $script:Lease -and -not $script:Lease.IsClosed) {
        $script:Lease.Dispose()
    }
}

function Get-FileSystemItemOrNull([string]$Path) {
    return Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Read-CanonicalAsciiLines(
    [string]$Path,
    [int]$MaximumBytes = 65536
) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item -or $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "expected a regular non-reparse file: $Path"
    }

    if ($item.Length -le 0 -or $item.Length -gt $MaximumBytes) {
        throw "canonical text file has an invalid size: $Path"
    }

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ne $item.Length -or $bytes[$bytes.Length - 1] -ne 10) {
        throw "canonical text file must end with exactly represented LF lines: $Path"
    }
    foreach ($byte in $bytes) {
        if ($byte -ne 10 -and ($byte -lt 32 -or $byte -gt 126)) {
            throw "canonical text file contains a non-printable byte: $Path"
        }
    }

    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    $parts = $text.Split([char]10)
    if ($parts[$parts.Length - 1].Length -ne 0) {
        throw "canonical text file has an invalid final line: $Path"
    }

    if ($parts.Length -eq 1) {
        return @()
    }
    return @($parts[0..($parts.Length - 2)])
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
        $bytes = [Text.Encoding]::ASCII.GetBytes($content)
        $stream = [IO.FileStream]::new(
            $temporary,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None
        )
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        } finally {
            $stream.Dispose()
        }

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
    if ($null -eq $item) {
        return
    }

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
        if ($null -eq $item) {
            return
        }
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
    # entry is gone. A failed cleanup then retains enough evidence to identify this detached tree.
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

function Get-UninstallContext(
    [string]$RequestedCupRoot,
    [string]$RequestedSelfPath
) {
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE) -or
        -not [IO.Path]::IsPathRooted($env:USERPROFILE)) {
        throw "USERPROFILE must contain an absolute path"
    }

    $userProfile = (Get-NormalizedFullPath $env:USERPROFILE).TrimEnd([char[]]'\/')
    $profileRoot = [IO.Path]::GetPathRoot($userProfile).TrimEnd([char[]]'\/')
    if ($userProfile -ieq $profileRoot) {
        throw "USERPROFILE must not be a volume root"
    }

    $primaryRoot = (
        Get-NormalizedFullPath (Join-Path $userProfile ".cup")
    ).TrimEnd([char[]]'\/')
    $fallbackRoot = (
        Get-NormalizedFullPath (Join-Path $userProfile ".coffee-cup")
    ).TrimEnd([char[]]'\/')
    $requestedRoot = (Get-NormalizedFullPath $RequestedCupRoot).TrimEnd([char[]]'\/')
    $requestedSelf = Get-NormalizedFullPath $RequestedSelfPath
    $runningSelf = Get-NormalizedFullPath $PSCommandPath

    if (-not $requestedRoot.Equals($primaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
        -not $requestedRoot.Equals($fallbackRoot, [StringComparison]::OrdinalIgnoreCase)) {
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

    return [pscustomobject]@{
        UserProfile = $userProfile
        Root = $requestedRoot
        Self = $requestedSelf
    }
}

function Assert-CupRootMarker([string]$Root) {
    $lines = @(Read-CanonicalAsciiLines (Join-Path $Root "root.txt") 1024)
    if ($lines.Count -ne 3 -or
        $lines[0] -cne "format=1" -or
        $lines[1] -cne "product=coffee-clang/cup" -or
        $lines[2] -cne "layout=1") {
        throw "cup root marker is missing or invalid"
    }
}

function Read-UninstallIdentity([string]$Root) {
    $lines = @(Read-CanonicalAsciiLines (Join-Path $Root "transaction.txt") 8192)
    if ($lines.Count -ne 7 -or
        $lines[0] -cne "format=1" -or
        $lines[1] -cne "operation=uninstall" -or
        $lines[2] -cne "phase=scheduled" -or
        -not $lines[3].StartsWith("temporary_name=", [StringComparison]::Ordinal) -or
        -not $lines[4].StartsWith("token=", [StringComparison]::Ordinal) -or
        $lines[5] -cne "stage=handoff" -or
        $lines[6] -cne "error=0") {
        throw "uninstall transaction is missing or invalid"
    }

    $temporaryName = $lines[3].Substring("temporary_name=".Length)
    $token = $lines[4].Substring("token=".Length)
    if ($token.Length -eq 0 -or $token.Length -ge 256 -or
        $token -notmatch '^[A-Za-z0-9_.-]+$' -or
        ($temporaryName -cne ".cup-uninstall.$token" -and
         $temporaryName -cne ".cup-uninstall-$token")) {
        throw "uninstall transaction identity is invalid"
    }

    return [pscustomobject]@{
        TemporaryName = $temporaryName
        Token = $token
    }
}

function Signal-UninstallReady([UInt64]$Handle) {
    $safeHandle = [Microsoft.Win32.SafeHandles.SafeFileHandle]::new(
        [IntPtr]::new([Int64]$Handle),
        $true
    )
    $stream = [IO.FileStream]::new($safeHandle, [IO.FileAccess]::Write)
    try {
        $stream.WriteByte([byte][char]'R')
        $stream.Flush()
    } finally {
        $stream.Dispose()
    }
}

function Wait-ForParentExit([UInt64]$Handle) {
    $safeHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new(
        [IntPtr]::new([Int64]$Handle),
        $true
    )
    $wait = [Threading.EventWaitHandle]::new(
        $false,
        [Threading.EventResetMode]::AutoReset
    )
    $wait.SafeWaitHandle = $safeHandle
    try {
        $null = $wait.WaitOne()
    } finally {
        $wait.Dispose()
    }
}

function Invoke-DetachedUninstall {
    $context = Get-UninstallContext $CupRoot $SelfPath
    $script:JournalRoot = $context.Root

    Assert-CupRootMarker $context.Root
    $identity = Read-UninstallIdentity $context.Root
    $script:TemporaryName = $identity.TemporaryName
    $script:Token = $identity.Token

    $detachedRoot = Join-Path $context.UserProfile $script:TemporaryName
    if (Test-Path -LiteralPath $detachedRoot) {
        throw "uninstall destination already exists"
    }

    Write-UninstallJournal $context.Root "scheduled" "parent-wait" 0
    Signal-UninstallReady $ReadyHandle

    $script:FailureStage = "parent-wait"
    Wait-ForParentExit $ParentHandle

    Write-UninstallJournal $context.Root "detaching" "detach" 0
    $script:FailureStage = "detach"
    [IO.Directory]::Move($context.Root, $detachedRoot)
    $script:JournalRoot = $detachedRoot

    # The canonical root no longer exists. Release cup.lock before deleting the detached tree.
    Close-UninstallLease

    $script:FailureStage = "cleanup"
    Write-UninstallJournal $detachedRoot "failed" "cleanup" 1
    Remove-DetachedCupRoot $detachedRoot

    if (Test-Path -LiteralPath $context.Self -PathType Leaf) {
        Remove-Item -LiteralPath $context.Self -Force
    }
}

try {
    Invoke-DetachedUninstall
    exit 0
} catch {
    Write-UninstallFailure
    Write-Error $_
    exit 1
} finally {
    Close-UninstallLease
}
