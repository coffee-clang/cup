# Purpose: Installs one immutable official cup bootstrap under the canonical Windows user root.
# The generated release version, tag and commit select and verify all downloaded assets.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoOwner = "coffee-clang"
$RepoName = "cup"
$ReleaseVersion = "@CUP_RELEASE_VERSION@"
$ReleaseTag = "@CUP_RELEASE_TAG@"
$ReleaseCommit = "@CUP_RELEASE_COMMIT@"
$DefaultBaseUrl = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag"
$BaseUrl = if ([string]::IsNullOrWhiteSpace($env:CUP_INSTALL_BASE_URL)) {
    $DefaultBaseUrl
} else {
    $env:CUP_INSTALL_BASE_URL.TrimEnd('/')
}

$UserProfilePath = ""
$CupRoot = ""
$CupBinDir = ""
$CupConfigDir = ""
$CupHelpersDir = ""
$CupExe = ""
$PackagesCfg = ""
$InstallPolicy = ""
$CommonChecksums = ""
$Platform = "windows-x64"
$PlatformChecksums = ""
$UninstallScript = ""
$UpdateHelper = ""
$CupAsset = "cup-windows-x64.exe"
$Staging = ""
$UninstallMarker = ""
$CupAvailableInPath = $false

function Fail([string]$Message) {
    throw "Error: $Message"
}
function Write-Info([string]$Message) {
    Write-Host $Message
}

# Release identity, platform and transport validation.
function Assert-InstallerIdentity {
    $PlaceholderMarker = '@' + 'CUP_RELEASE_'
    if ($ReleaseVersion.Contains($PlaceholderMarker) -or
        $ReleaseTag.Contains($PlaceholderMarker) -or
        $ReleaseCommit.Contains($PlaceholderMarker)) {
        Fail "installer was not prepared for a concrete release"
    }
    if ($ReleaseVersion -notmatch '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$') {
        Fail "installer has an invalid release version"
    }
    if ($ReleaseTag -cne "v$ReleaseVersion") {
        Fail "installer release tag does not match its version"
    }
    if ($ReleaseCommit -notmatch '^[0-9a-f]{40}$') {
        Fail "installer has an invalid release commit"
    }
}

function Assert-BaseUrl {
    try {
        $uri = [Uri]$BaseUrl
    } catch {
        Fail "installer base URL is invalid"
    }
    if (-not $uri.IsAbsoluteUri -or -not [string]::IsNullOrEmpty($uri.UserInfo)) {
        Fail "installer base URL is invalid"
    }
    if ($uri.Scheme -ceq 'https') {
        return
    }
    if ($uri.Scheme -ceq 'http' -and $uri.IsLoopback -and
        $env:CUP_INSTALL_ALLOW_INSECURE -ceq '1') {
        return
    }
    Fail "installer base URL must use HTTPS"
}

function Assert-DownloadUri([Uri]$Uri) {
    if ($Uri.Scheme -ceq 'https') {
        return
    }
    if ($Uri.Scheme -ceq 'http' -and $Uri.IsLoopback -and
        $env:CUP_INSTALL_ALLOW_INSECURE -ceq '1') {
        return
    }
    Fail "download redirected to an insecure URL: $Uri"
}

function Test-WindowsX64 {
    if ($PSVersionTable.PSEdition -eq "Core" -and -not $IsWindows) {
        Fail "this installer supports Windows only"
    }
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE) -or
        -not [System.IO.Path]::IsPathRooted($env:USERPROFILE)) {
        Fail "the Windows user profile could not be determined as an absolute path"
    }
    $profilePath = [System.IO.Path]::GetFullPath($env:USERPROFILE).TrimEnd([char[]]'\/')
    $profileRoot = [System.IO.Path]::GetPathRoot($profilePath).TrimEnd([char[]]'\/')
    if ($profilePath -ieq $profileRoot) {
        Fail "USERPROFILE must not be a volume root"
    }
    $script:UserProfilePath = $profilePath
    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
    if ($arch.ToString() -ne "X64") {
        Fail "unsupported architecture: $arch. This installer supports x64 only"
    }
}

function Initialize-InstallationPaths {
    $script:CupRoot = Join-Path $script:UserProfilePath ".cup"
    $script:CupBinDir = Join-Path $script:CupRoot "bin"
    $script:CupConfigDir = Join-Path $script:CupRoot "config"
    $script:CupHelpersDir = Join-Path $script:CupRoot "helpers"
    $script:CupExe = Join-Path $script:CupBinDir "cup.exe"
    $script:PackagesCfg = Join-Path $script:CupConfigDir "packages.cfg"
    $script:InstallPolicy = Join-Path $script:CupConfigDir "install.cfg"
    $script:CommonChecksums = Join-Path $script:CupConfigDir "SHA256SUMS.common"
    $script:PlatformChecksums = Join-Path $script:CupConfigDir "SHA256SUMS.$Platform"
    $script:UninstallScript = Join-Path $script:CupHelpersDir "uninstall.ps1"
    $script:UpdateHelper = Join-Path $script:CupHelpersDir "cup-update-helper.exe"
    $script:Staging = Join-Path $script:CupRoot ".bootstrap"
    $script:UninstallMarker = Join-Path $script:CupRoot "uninstall.pending"
}

# Download and strict checksum validation.
function Download-File([string]$Url, [string]$Output) {
    try {
        $response = Invoke-WebRequest -Uri $Url -OutFile $Output `
            -UseBasicParsing -PassThru -ErrorAction Stop
        $finalUri = $null
        if ($null -ne $response.BaseResponse) {
            if ($null -ne $response.BaseResponse.ResponseUri) {
                $finalUri = $response.BaseResponse.ResponseUri
            } elseif ($null -ne $response.BaseResponse.RequestMessage) {
                $finalUri = $response.BaseResponse.RequestMessage.RequestUri
            }
        }
        if ($null -ne $finalUri) {
            Assert-DownloadUri $finalUri
        }
    } catch {
        Fail "failed to download $Url"
    }
    $file = Get-Item -LiteralPath $Output
    if ($file.Length -le 0) {
        Fail "downloaded file is empty: $Url"
    }
}

function Read-ChecksumEntries([string]$ChecksumFile) {
    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $ChecksumFile) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(\S+)$') {
            Fail "invalid checksum file: $ChecksumFile"
        }
        $entries.Add([pscustomobject]@{
            Hash = $Matches[1].ToLowerInvariant()
            Name = $Matches[2]
        })
    }
    return $entries
}

function Assert-ChecksumNames(
    [string]$ChecksumFile,
    [string[]]$ExpectedNames
) {
    $entries = @(Read-ChecksumEntries $ChecksumFile)
    if ($entries.Count -ne $ExpectedNames.Count) {
        Fail "checksum file contains an unexpected number of entries: $ChecksumFile"
    }
    foreach ($expectedName in $ExpectedNames) {
        $matching = @($entries | Where-Object { $_.Name -ceq $expectedName })
        if ($matching.Count -ne 1) {
            Fail "checksum entry is missing or duplicated: $expectedName"
        }
    }
}

function Assert-NamedChecksum(
    [string]$Directory,
    [string]$ChecksumFile,
    [string]$ExpectedName
) {
    $entries = @(Read-ChecksumEntries $ChecksumFile)
    $matching = @($entries | Where-Object { $_.Name -ceq $ExpectedName })
    if ($matching.Count -ne 1) {
        Fail "checksum entry is missing or duplicated: $ExpectedName"
    }
    $path = Join-Path $Directory $ExpectedName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        Fail "checksum asset is missing: $ExpectedName"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $matching[0].Hash) {
        Fail "checksum verification failed for $ExpectedName"
    }
}

function Assert-Checksums(
    [string]$Directory,
    [string]$ChecksumFile,
    [string[]]$ExpectedNames
) {
    Assert-ChecksumNames -ChecksumFile $ChecksumFile -ExpectedNames $ExpectedNames
    foreach ($expectedName in $ExpectedNames) {
        Assert-NamedChecksum -Directory $Directory -ChecksumFile $ChecksumFile `
            -ExpectedName $expectedName
    }
}

function Assert-ReleaseMetadata(
    [string]$Path,
    [string]$ExpectedVersion = "",
    [string]$ExpectedCommit = ""
) {
    $lines = @(Get-Content -LiteralPath $Path)
    if ($lines.Count -ne 3) {
        Fail "invalid release metadata: $Path"
    }

    $values = @{}
    foreach ($line in $lines) {
        if ($line -notmatch '^([^=]+)=(.+)$' -or $values.ContainsKey($Matches[1])) {
            Fail "invalid release metadata: $Path"
        }
        $values[$Matches[1]] = $Matches[2]
    }

    if ($values.Count -ne 3 -or $values['format'] -cne '1' -or
        $values['version'] -notmatch '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$' -or
        $values['commit'] -notmatch '^[0-9a-f]{7,40}$') {
        Fail "invalid release metadata: $Path"
    }
    if ($ExpectedVersion -ne "" -and $values['version'] -cne $ExpectedVersion) {
        Fail "release metadata version does not match this installer: $Path"
    }
    if ($ExpectedCommit -ne "" -and $values['commit'] -cne $ExpectedCommit) {
        Fail "release metadata commit does not match this installer: $Path"
    }
}

# Optional user PATH integration.
function Test-CupBinInUserPath {
    $path = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($null -eq $path) {
        return $false
    }
    foreach ($entry in ($path -split ';')) {
        if ($entry.TrimEnd('\') -ieq $CupBinDir.TrimEnd('\')) {
            return $true
        }
    }
    return $false
}

function Add-CupToUserPath {
    if (Test-CupBinInUserPath) {
        $script:CupAvailableInPath = $true
        Write-Info "cup bin directory is already in the user PATH."
        return
    }
    if ($env:CUP_INSTALL_NO_PATH_PROMPT -eq "1") {
        Write-Info "PATH not modified. Add this directory manually when needed: $CupBinDir"
        return
    }
    $answer = Read-Host "Add $CupBinDir to your user PATH? [y/N]"
    if ($answer -match '^(y|yes)$') {
        $current = [Environment]::GetEnvironmentVariable("Path", "User")
        $newPath = if ([string]::IsNullOrWhiteSpace($current)) {
            $CupBinDir
        } else {
            "$current;$CupBinDir"
        }
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        $script:CupAvailableInPath = $true
        Write-Info "User PATH updated. Open a new terminal to use cup from PATH."
    } else {
        Write-Info "PATH not modified. Add this directory manually when needed: $CupBinDir"
    }
}

# File attributes, reparse-point checks and transactional replacement.
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

function Clear-ReadOnly([string]$Path) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        return
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
        $attributes = $item.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly)
        [System.IO.File]::SetAttributes($item.FullName, $attributes)
    }
}

function Assert-RealDirectory([string]$Path) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item) {
        return
    }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail "managed directory is a reparse point: $Path"
    }
    if (-not $item.PSIsContainer) {
        Fail "managed path is not a directory: $Path"
    }
}

function Set-BootstrapPermissions([bool]$RequireAssets = $false) {
    $allAssets = @(
        $PackagesCfg,
        $InstallPolicy,
        $CommonChecksums,
        $PlatformChecksums,
        $UninstallScript,
        $UpdateHelper,
        $CupExe
    )
    if ($RequireAssets) {
        foreach ($path in $allAssets) {
            $item = Get-FileSystemItemOrNull $path
            if ($null -eq $item -or $item.PSIsContainer -or
                ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Fail "installed bootstrap asset is missing or unsafe: $path"
            }
        }
    }

    foreach ($path in @(
        $PackagesCfg,
        $InstallPolicy,
        $CommonChecksums,
        $PlatformChecksums,
        $UninstallScript
    )) {
        $item = Get-FileSystemItemOrNull $path
        if ($null -ne $item -and -not $item.PSIsContainer -and
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            Set-ItemProperty -LiteralPath $path -Name IsReadOnly -Value $true
        }
    }
}

# Transactional bootstrap asset backup, commit and recovery.
function Get-Assets {
    return @(
        @{
            Key = "manifest"
            Source = Join-Path $Staging "packages.cfg"
            Destination = $PackagesCfg
        },
        @{
            Key = "install-config"
            Source = Join-Path $Staging "install.cfg"
            Destination = $InstallPolicy
        },
        @{
            Key = "common-checksums"
            Source = Join-Path $Staging "SHA256SUMS.common"
            Destination = $CommonChecksums
        },
        @{
            Key = "platform-checksums"
            Source = Join-Path $Staging "SHA256SUMS.$Platform"
            Destination = $PlatformChecksums
        },
        @{
            Key = "uninstall"
            Source = Join-Path $Staging "uninstall.ps1"
            Destination = $UninstallScript
        },
        @{
            Key = "update-helper"
            Source = Join-Path $Staging "cup-update-helper.exe"
            Destination = $UpdateHelper
        },
        @{
            Key = "binary"
            Source = Join-Path $Staging $CupAsset
            Destination = $CupExe
        }
    )
}

function Restore-Asset([hashtable]$Asset) {
    $backup = Join-Path (Join-Path $Staging "backup") $Asset.Key
    $absent = "$backup.absent"
    $installed = Join-Path (Join-Path $Staging "installed") $Asset.Key
    $backupItem = Get-FileSystemItemOrNull $backup
    $absentItem = Get-FileSystemItemOrNull $absent
    $installedItem = Get-FileSystemItemOrNull $installed
    $destinationItem = Get-FileSystemItemOrNull $Asset.Destination

    foreach ($entry in @(
        @{ Name = "bootstrap backup"; Item = $backupItem },
        @{ Name = "bootstrap absent marker"; Item = $absentItem },
        @{ Name = "bootstrap installed marker"; Item = $installedItem }
    )) {
        if ($null -ne $entry.Item -and
            ($entry.Item.PSIsContainer -or
             ($entry.Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            Fail "$($entry.Name) is not a regular file: $($entry.Item.FullName)"
        }
    }
    if ($null -ne $backupItem -and $null -ne $absentItem) {
        Fail "bootstrap asset has both backup and absent markers: $($Asset.Key)"
    }
    if ($null -ne $installedItem -and
        $null -eq $backupItem -and $null -eq $absentItem) {
        Fail "bootstrap replacement has no backup evidence: $($Asset.Key)"
    }
    if ($null -ne $destinationItem -and
        ($destinationItem.PSIsContainer -or
         ($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        Fail "installed bootstrap asset is not a regular file: $($Asset.Destination)"
    }

    if ($null -ne $backupItem) {
        Clear-ReadOnly $Asset.Destination
        if ($null -ne $destinationItem) {
            [System.IO.File]::Replace($backup, $Asset.Destination, $null, $true)
        } else {
            [System.IO.File]::Move($backup, $Asset.Destination)
        }
    } elseif ($null -ne $absentItem -and $null -ne $installedItem) {
        if ($null -ne $destinationItem) {
            Clear-ReadOnly $Asset.Destination
            [System.IO.File]::Delete($Asset.Destination)
        }
    } elseif ($null -ne $absentItem -and $null -ne $destinationItem) {
        Fail "uncommitted bootstrap asset unexpectedly exists: $($Asset.Destination)"
    }
}

function Recover-Staging {
    if (-not (Test-Path -LiteralPath $Staging -PathType Container)) {
        return
    }
    $commitMarker = Join-Path $Staging "committed"
    $markerItem = Get-FileSystemItemOrNull $commitMarker
    if ($null -ne $markerItem) {
        if ($markerItem.PSIsContainer -or
            ($markerItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "bootstrap commit marker is not a regular file: $commitMarker"
        }
        try {
            Set-BootstrapPermissions -RequireAssets $true
        } catch {
            Fail "completed bootstrap staging does not match a complete installed generation"
        }
        Write-Info "Finishing cleanup from a completed cup bootstrap installation."
        Remove-TreeNoFollow $Staging
        return
    }
    Write-Info "Recovering an interrupted cup bootstrap installation."
    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($asset in (Get-Assets)) {
        try {
            Restore-Asset $asset
        } catch {
            $errors.Add($_.Exception.Message)
        }
    }
    try {
        Set-BootstrapPermissions
    } catch {
        $errors.Add($_.Exception.Message)
    }

    if ($errors.Count -gt 0) {
        $details = $errors -join [Environment]::NewLine
        Fail (
            "the previous bootstrap installation could not be recovered; " +
            "staging was preserved at $Staging$([Environment]::NewLine)$details"
        )
    }
    Remove-TreeNoFollow $Staging
}

function Backup-Asset([hashtable]$Asset) {
    $backup = Join-Path (Join-Path $Staging "backup") $Asset.Key
    $item = Get-FileSystemItemOrNull $Asset.Destination
    if ($null -ne $item) {
        if ($item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "existing bootstrap asset is not a regular file: $($Asset.Destination)"
        }
        Clear-ReadOnly $Asset.Destination
        Copy-Item -LiteralPath $Asset.Destination -Destination $backup -Force
    } else {
        New-Item -ItemType File -Path "$backup.absent" | Out-Null
    }
}

function Commit-Asset([hashtable]$Asset) {
    $sourceItem = Get-FileSystemItemOrNull $Asset.Source
    if ($null -eq $sourceItem -or $sourceItem.PSIsContainer -or
        ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail "bootstrap staged asset is not a regular file: $($Asset.Source)"
    }

    $destinationItem = Get-FileSystemItemOrNull $Asset.Destination
    if ($null -ne $destinationItem -and
        ($destinationItem.PSIsContainer -or
         ($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        Fail "installed bootstrap asset is not a regular file: $($Asset.Destination)"
    }

    $installed = Join-Path (Join-Path $Staging "installed") $Asset.Key
    New-Item -ItemType File -Path $installed | Out-Null
    if ($null -ne $destinationItem) {
        Clear-ReadOnly $Asset.Destination
        [System.IO.File]::Replace($Asset.Source, $Asset.Destination, $null, $true)
    } else {
        [System.IO.File]::Move($Asset.Source, $Asset.Destination)
    }
}

# Residue cleanup accepts only strongly validated cup uninstall staging.
function Remove-ValidatedUninstallResidues {
    $residues = @(Get-ChildItem -LiteralPath $UserProfilePath -Force |
        Where-Object { $_.Name -like '.cup-uninstall.*' })
    foreach ($residue in $residues) {
        if (-not $residue.PSIsContainer -or
            ($residue.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        $children = @(Get-ChildItem -LiteralPath $residue.FullName -Force)
        $stagedRoot = Join-Path $residue.FullName "root"
        $marker = Join-Path $stagedRoot "uninstall.pending"
        $binary = Join-Path (Join-Path $stagedRoot "bin") "cup.exe"
        if ($children.Count -ne 1 -or $children[0].Name -cne 'root' -or
            -not (Test-Path -LiteralPath $stagedRoot -PathType Container) -or
            -not (Test-Path -LiteralPath $marker -PathType Leaf) -or
            -not (Test-Path -LiteralPath $binary -PathType Leaf)) {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        foreach ($path in @($stagedRoot, $marker, $binary)) {
            $item = Get-Item -LiteralPath $path -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
            }
        }
        $markerLines = @(Get-Content -LiteralPath $marker)
        if ($markerLines.Count -ne 1 -or
            $markerLines[0] -cnotmatch '^parent_pid=[1-9][0-9]*$') {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        Write-Info "Removing validated uninstall residue: $($residue.FullName)"
        Remove-TreeNoFollow $residue.FullName
    }
}

# Private root ACL creation and validation.
function Set-PrivateDirectory([string]$Path) {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $userSid = $identity.User
    $systemSid = [System.Security.Principal.SecurityIdentifier]::new(
        [System.Security.Principal.WellKnownSidType]::LocalSystemSid, $null)
    $adminSid = [System.Security.Principal.SecurityIdentifier]::new(
        [System.Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)
    $acl = [System.Security.AccessControl.DirectorySecurity]::new()
    $acl.SetOwner($userSid)
    $acl.SetAccessRuleProtection($true, $false)
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
    foreach ($sid in @($userSid, $systemSid, $adminSid)) {
        $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
            $sid,
            [System.Security.AccessControl.FileSystemRights]::FullControl,
            $inheritance,
            [System.Security.AccessControl.PropagationFlags]::None,
            [System.Security.AccessControl.AccessControlType]::Allow)
        [void]$acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Assert-PrivateDirectory([string]$Path) {
    Assert-RealDirectory $Path
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $allowed = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    [void]$allowed.Add($identity.User.Value)
    [void]$allowed.Add(([System.Security.Principal.SecurityIdentifier]::new(
        [System.Security.Principal.WellKnownSidType]::LocalSystemSid, $null)).Value)
    [void]$allowed.Add(([System.Security.Principal.SecurityIdentifier]::new(
        [System.Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)).Value)

    $acl = Get-Acl -LiteralPath $Path
    if (-not $acl.AreAccessRulesProtected) {
        Fail "cup root ACL inherits permissions from its parent: $Path"
    }
    $ownerSid = ([System.Security.Principal.NTAccount]::new($acl.Owner)).Translate(
        [System.Security.Principal.SecurityIdentifier])
    if ($ownerSid.Value -cne $identity.User.Value) {
        Fail "cup root is not owned by the current user: $Path"
    }
    foreach ($rule in $acl.Access) {
        $sid = $rule.IdentityReference.Translate(
            [System.Security.Principal.SecurityIdentifier])
        if ($rule.IsInherited -or
            $rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow -or
            -not $allowed.Contains($sid.Value)) {
            Fail "cup root has an unsafe ACL entry: $Path"
        }
    }
}


# Keep filesystem preparation, transfer, verification and transaction phases
# separate so Main expresses the recovery-safe installation order directly.
function Initialize-InstallationDirectories {
    Assert-RealDirectory $CupRoot
    New-Item -ItemType Directory -Force -Path $CupRoot | Out-Null
    Set-PrivateDirectory $CupRoot
    Assert-PrivateDirectory $CupRoot

    foreach ($directory in @($CupBinDir, $CupConfigDir, $CupHelpersDir)) {
        Assert-RealDirectory $directory
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        Assert-RealDirectory $directory
    }
}

function Receive-BootstrapAssets {
    Download-File "$BaseUrl/$CupAsset" (Join-Path $Staging $CupAsset)
    Download-File "$BaseUrl/packages.cfg" (Join-Path $Staging "packages.cfg")
    Download-File "$BaseUrl/install.cfg" (Join-Path $Staging "install.cfg")
    Download-File "$BaseUrl/uninstall.ps1" (Join-Path $Staging "uninstall.ps1")
    Download-File "$BaseUrl/release.txt" (Join-Path $Staging "release.txt")
    Download-File "$BaseUrl/SHA256SUMS.$Platform" (Join-Path $Staging "SHA256SUMS.$Platform")
    Download-File "$BaseUrl/SHA256SUMS.common" (Join-Path $Staging "SHA256SUMS.common")
}

function Assert-BootstrapAssets {
    Assert-Checksums -Directory $Staging `
        -ChecksumFile (Join-Path $Staging "SHA256SUMS.$Platform") `
        -ExpectedNames @($CupAsset, "uninstall.ps1", "release.txt")

    $commonChecksumFile = Join-Path $Staging "SHA256SUMS.common"
    Assert-ChecksumNames -ChecksumFile $commonChecksumFile `
        -ExpectedNames @("packages.cfg", "install.cfg", "install.sh", "install.ps1")
    Assert-NamedChecksum -Directory $Staging -ChecksumFile $commonChecksumFile `
        -ExpectedName "packages.cfg"
    Assert-NamedChecksum -Directory $Staging -ChecksumFile $commonChecksumFile `
        -ExpectedName "install.cfg"
    Assert-ReleaseMetadata `
        (Join-Path $Staging "release.txt") $ReleaseVersion $ReleaseCommit

    Copy-Item -LiteralPath (Join-Path $Staging $CupAsset) `
        -Destination (Join-Path $Staging "cup-update-helper.exe")
}

function Backup-BootstrapAssets {
    foreach ($asset in (Get-Assets)) {
        Backup-Asset $asset
    }
}

function Commit-BootstrapAssets {
    foreach ($asset in (Get-Assets)) {
        Commit-Asset $asset
    }
}

function Invoke-BootstrapRollback {
    $rollbackErrors = [System.Collections.Generic.List[string]]::new()
    foreach ($asset in (Get-Assets)) {
        try {
            Restore-Asset $asset
        } catch {
            $rollbackErrors.Add($_.Exception.Message)
        }
    }
    try {
        Set-BootstrapPermissions
    } catch {
        $rollbackErrors.Add($_.Exception.Message)
    }

    if ($rollbackErrors.Count -gt 0) {
        $details = $rollbackErrors -join [Environment]::NewLine
        throw (
            "rollback was incomplete; staging was preserved at $Staging" +
            "$([Environment]::NewLine)$details"
        )
    }
    Remove-TreeNoFollow $Staging
}

# Main installation pipeline: discover, download, verify, commit and report.
function Main {
    Assert-InstallerIdentity
    Assert-BaseUrl
    Test-WindowsX64
    Initialize-InstallationPaths
    Remove-ValidatedUninstallResidues
    Initialize-InstallationDirectories

    Assert-RealDirectory $Staging
    Recover-Staging
    New-Item -ItemType Directory -Path $Staging | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Staging "backup") | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Staging "installed") | Out-Null
    $committed = $false

    try {
        Write-Info "Installing cup into $CupRoot"
        Receive-BootstrapAssets
        Assert-BootstrapAssets
        Backup-BootstrapAssets
        Commit-BootstrapAssets
        Set-BootstrapPermissions -RequireAssets $true

        if (Test-Path -LiteralPath $UninstallMarker) {
            Remove-Item -LiteralPath $UninstallMarker -Force
        }
        New-Item -ItemType File -Path (Join-Path $Staging "committed") | Out-Null
        $committed = $true
    } finally {
        if (-not $committed -and
            (Test-Path -LiteralPath $Staging -PathType Container)) {
            Invoke-BootstrapRollback
        }
    }

    try {
        Remove-TreeNoFollow $Staging
    } catch {
        [Console]::Error.WriteLine(
            "Warning: cup was installed, but completed bootstrap staging could not be removed.")
    }
    Write-Info "cup installed successfully."
    Write-Info "Binary: $CupExe"
    Write-Info "Package catalog: $PackagesCfg"
    Write-Info "Install configuration: $InstallPolicy"
    Write-Info "Checksums: $CommonChecksums"
    Write-Info "           $PlatformChecksums"
    Write-Info "Uninstall: $UninstallScript"
    Add-CupToUserPath
    if ($CupAvailableInPath) {
        Write-Info "Test with: cup help"
    } else {
        Write-Info "Test with: & `"$CupExe`" help"
    }
}

try {
    Main
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
