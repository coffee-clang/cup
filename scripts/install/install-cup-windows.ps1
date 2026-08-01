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
$RootMarker = ""
$CupAvailableInPath = $false
# PowerShell converts $null to an empty string for .NET string parameters.
# NullString preserves the real null required to omit File.Replace backups.
$NoFileReplaceBackup =
    [System.Management.Automation.Language.NullString]::Value

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

function Test-RootMarker([string]$Candidate) {
    $marker = Join-Path $Candidate "root.txt"
    $item = Get-FileSystemItemOrNull $marker
    if ($null -eq $item -or $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        return $false
    }
    try {
        $lines = @(Get-Content -LiteralPath $marker -ErrorAction Stop)
    } catch {
        return $false
    }
    return $lines.Count -eq 3 -and
        $lines[0] -ceq "format=1" -and
        $lines[1] -ceq "product=coffee-clang/cup" -and
        $lines[2] -ceq "layout=1"
}

function Test-RealPathKind([string]$Path, [bool]$Directory) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        return $false
    }
    return $item.PSIsContainer -eq $Directory
}

function Read-LegacyRecords([string]$Path) {
    if (-not (Test-RealPathKind $Path $false)) {
        throw "not a regular file"
    }
    $records = [Collections.Generic.List[object]]::new()
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($physicalLine in [IO.File]::ReadAllLines($Path)) {
        $line = $physicalLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith('#', [StringComparison]::Ordinal)) {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -le 0 -or $separator -eq $line.Length - 1) {
            throw "invalid key/value record"
        }
        $key = $line.Substring(0, $separator).Trim()
        $value = $line.Substring($separator + 1).Trim()
        if ($key.Length -eq 0 -or $value.Length -eq 0 -or -not $seen.Add($key)) {
            throw "invalid or duplicate key"
        }
        $records.Add([pscustomobject]@{ Key = $key; Value = $value })
    }
    if ($records.Count -eq 0) {
        throw "empty document"
    }
    return ,$records
}

function Test-SafeIdentifier([string]$Value) {
    return $Value -cmatch '^[A-Za-z0-9][A-Za-z0-9._+\-]{0,126}$'
}

function Test-CanonicalName([string]$Value) {
    return (Test-SafeIdentifier $Value) -and $Value -cnotmatch '[A-Z]'
}

function Test-SupportedPlatform([string]$Value) {
    return $Value -cin @(
        'linux-x64', 'linux-arm64', 'windows-x64', 'macos-x64', 'macos-arm64'
    )
}

function Test-SupportedComponent([string]$Value) {
    return $Value -cin @(
        'compiler', 'debugger', 'linker', 'formatter', 'linter',
        'language-server', 'analyzer'
    )
}

function Get-ToolComponent([string]$Tool) {
    switch -CaseSensitive ($Tool) {
        'gcc' { return 'compiler' }
        'clang' { return 'compiler' }
        'gdb' { return 'debugger' }
        'lldb' { return 'debugger' }
        'lld' { return 'linker' }
        'ld' { return 'linker' }
        'clang-format' { return 'formatter' }
        'clang-tidy' { return 'linter' }
        'clangd' { return 'language-server' }
        'valgrind' { return 'analyzer' }
        default { return $null }
    }
}

function Test-ToolComponent([string]$Component, [string]$Tool) {
    $actual = Get-ToolComponent $Tool
    return $null -ne $actual -and $actual -ceq $Component
}

function Test-LegacyList(
    [string]$Value,
    [ValidateSet('identifier', 'component', 'tool', 'format')][string]$Kind,
    [string]$Expected = ''
) {
    $items = @($Value.Split(',') | ForEach-Object { $_.Trim() })
    if ($items.Count -eq 0 -or $items -contains '') {
        return $false
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $seenComponents = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $containsExpected = [string]::IsNullOrEmpty($Expected)
    foreach ($item in $items) {
        if (-not (Test-CanonicalName $item) -or -not $seen.Add($item)) {
            return $false
        }
        if ($item -ceq $Expected) {
            $containsExpected = $true
        }
        switch ($Kind) {
            'component' {
                if (-not (Test-SupportedComponent $item)) { return $false }
            }
            'tool' {
                $component = Get-ToolComponent $item
                if ($null -eq $component -or -not $seenComponents.Add($component)) {
                    return $false
                }
            }
            'format' {
                if ($item -cnotin @('tar.xz', 'tar.gz', 'zip')) { return $false }
            }
        }
    }
    return $containsExpected
}

function Test-LegacyUrlTemplate([string]$Value, [bool]$PackageUrl) {
    if (-not $Value.StartsWith('https://', [StringComparison]::Ordinal) -or
        $Value -match '\s') {
        return $false
    }
    $allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($name in @('tool', 'host_platform', 'target_platform', 'version')) {
        [void]$allowed.Add($name)
    }
    if ($PackageUrl) { [void]$allowed.Add('format') }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($match in [regex]::Matches($Value, '\{([^{}]+)\}')) {
        $name = $match.Groups[1].Value
        if (-not $allowed.Contains($name)) { return $false }
        [void]$seen.Add($name)
    }
    $withoutPlaceholders = [regex]::Replace($Value, '\{[^{}]+\}', '')
    if ($withoutPlaceholders.Contains('{') -or $withoutPlaceholders.Contains('}')) {
        return $false
    }
    foreach ($required in @('host_platform', 'target_platform', 'version')) {
        if (-not $seen.Contains($required)) { return $false }
    }
    return (-not $PackageUrl -and -not $seen.Contains('format')) -or
        ($PackageUrl -and $seen.Contains('format'))
}

function Test-LegacyCatalog([string]$Path) {
    try { $records = Read-LegacyRecords $Path } catch { return $false }
    $tuples = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
    $allowedFields = @(
        'stable_version', 'available_versions', 'default_format', 'formats',
        'url_template', 'checksum_url_template'
    )
    foreach ($record in $records) {
        $parts = @($record.Key.Split('.'))
        if ($parts.Count -ne 5) { return $false }
        $component, $tool, $hostPlatform, $targetPlatform, $field = $parts
        if (-not (Test-SupportedComponent $component) -or
            -not (Test-ToolComponent $component $tool) -or
            -not (Test-SupportedPlatform $hostPlatform) -or
            -not (Test-SupportedPlatform $targetPlatform) -or
            $field -cnotin $allowedFields) {
            return $false
        }
        $tupleKey = "$component.$tool.$hostPlatform.$targetPlatform"
        if (-not $tuples.ContainsKey($tupleKey)) {
            $tuples[$tupleKey] = [Collections.Generic.Dictionary[string, string]]::new(
                [StringComparer]::Ordinal
            )
        }
        $fields = $tuples[$tupleKey]
        if ($fields.ContainsKey($field)) { return $false }
        $fields[$field] = $record.Value
    }
    if ($tuples.Count -eq 0) { return $false }
    foreach ($fields in $tuples.Values) {
        if ($fields.Count -ne $allowedFields.Count) { return $false }
        foreach ($requiredField in $allowedFields) {
            if (-not $fields.ContainsKey($requiredField)) { return $false }
        }
        $stable = $fields['stable_version']
        $defaultFormat = $fields['default_format']
        if (-not (Test-SafeIdentifier $stable) -or
            $defaultFormat -cnotin @('tar.xz', 'tar.gz', 'zip') -or
            -not (Test-LegacyList $fields['available_versions'] 'identifier' $stable) -or
            -not (Test-LegacyList $fields['formats'] 'format' $defaultFormat) -or
            -not (Test-LegacyUrlTemplate $fields['url_template'] $true) -or
            -not (Test-LegacyUrlTemplate $fields['checksum_url_template'] $false)) {
            return $false
        }
    }
    return $true
}

function Test-LegacyPolicy([string]$Path) {
    try { $records = Read-LegacyRecords $Path } catch { return $false }
    $seenFormat = $false
    $defaults = 0
    $profiles = 0
    $toolchains = 0
    foreach ($record in $records) {
        if ($record.Key -ceq 'format') {
            if ($seenFormat -or $record.Value -cne '1') { return $false }
            $seenFormat = $true
            continue
        }
        if (-not $seenFormat) { return $false }
        $parts = @($record.Key.Split('.'))
        if ($parts.Count -eq 4 -and $parts[0] -ceq 'default') {
            if (-not (Test-SupportedPlatform $parts[1]) -or
                -not (Test-SupportedPlatform $parts[2]) -or
                -not (Test-SupportedComponent $parts[3]) -or
                -not (Test-CanonicalName $record.Value) -or
                -not (Test-ToolComponent $parts[3] $record.Value)) {
                return $false
            }
            $defaults++
        } elseif ($parts.Count -eq 2 -and $parts[0] -ceq 'profile') {
            if (-not (Test-CanonicalName $parts[1]) -or
                -not (Test-LegacyList $record.Value 'component')) { return $false }
            $profiles++
        } elseif ($parts.Count -eq 2 -and $parts[0] -ceq 'toolchain') {
            if (-not (Test-CanonicalName $parts[1]) -or
                -not (Test-LegacyList $record.Value 'tool')) { return $false }
            $toolchains++
        } else {
            return $false
        }
    }
    return $seenFormat -and $defaults -gt 0 -and $profiles -gt 0 -and $toolchains -gt 0
}

function Test-LegacyState([string]$Path) {
    $item = Get-FileSystemItemOrNull $Path
    if ($null -eq $item) { return $true }
    try { $records = Read-LegacyRecords $Path } catch { return $false }
    $seenFormat = $false
    $installed = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($record in $records) {
        if ($record.Key -ceq 'format') {
            if ($seenFormat -or $record.Value -cne '1') { return $false }
            $seenFormat = $true
            continue
        }
        if (-not $seenFormat) { return $false }
        $parts = @($record.Key.Split('.'))
        if ($parts.Count -ne 4 -or $parts[0] -cnotin @('installed', 'default') -or
            -not (Test-SupportedComponent $parts[1]) -or
            -not (Test-SupportedPlatform $parts[2]) -or
            -not (Test-SupportedPlatform $parts[3])) {
            return $false
        }
        $selector = @($record.Value.Split('@'))
        if ($selector.Count -ne 2 -or -not (Test-CanonicalName $selector[0]) -or
            -not (Test-SafeIdentifier $selector[1]) -or $selector[1] -ceq 'stable' -or
            -not (Test-ToolComponent $parts[1] $selector[0])) {
            return $false
        }
        $scopeIdentity = "$($parts[1]).$($parts[2]).$($parts[3])=$($record.Value)"
        if ($parts[0] -ceq 'installed') {
            [void]$installed.Add($scopeIdentity)
        } elseif (-not $installed.Contains($scopeIdentity)) {
            return $false
        }
    }
    return $seenFormat
}

function Test-LegacyChecksumSet([string]$Path, [string[]]$ExpectedNames) {
    if (-not (Test-RealPathKind $Path $false)) {
        return $false
    }
    try {
        $entries = @(Read-ChecksumEntries $Path)
    } catch {
        return $false
    }
    if ($entries.Count -ne $ExpectedNames.Count) {
        return $false
    }
    foreach ($name in $ExpectedNames) {
        if (@($entries | Where-Object { $_.Name -ceq $name }).Count -ne 1) {
            return $false
        }
    }
    return $true
}

function Test-LegacyNamedChecksum(
    [string]$ChecksumFile,
    [string]$ExpectedName,
    [string]$ActualFile
) {
    if (-not (Test-RealPathKind $ActualFile $false)) {
        return $false
    }
    try {
        $entries = @(Read-ChecksumEntries $ChecksumFile)
        $matching = @($entries | Where-Object { $_.Name -ceq $ExpectedName })
        if ($matching.Count -ne 1) {
            return $false
        }
        $actual = (Get-FileHash -LiteralPath $ActualFile -Algorithm SHA256).Hash.ToLowerInvariant()
        return $actual -ceq $matching[0].Hash
    } catch {
        return $false
    }
}

function Test-CupRootTraces([string]$Candidate) {
    foreach ($relative in @(
        'bin\cup.exe',
        'helpers\cup-update-helper.exe',
        'helpers\uninstall.ps1',
        'config\SHA256SUMS.common',
        'state.txt'
    )) {
        if ($null -ne (Get-FileSystemItemOrNull (Join-Path $Candidate $relative))) {
            return $true
        }
    }
    return $false
}

function Test-CupRootBinary([string]$Candidate) {
    return $null -ne (Get-FileSystemItemOrNull (Join-Path $Candidate 'bin\cup.exe'))
}

function Test-LegacyCupRoot([string]$Candidate) {
    foreach ($relative in @('bin', 'components', 'staging', 'cache', 'config', 'helpers')) {
        if (-not (Test-RealPathKind (Join-Path $Candidate $relative) $true)) {
            return $false
        }
    }

    $binary = Join-Path $Candidate 'bin\cup.exe'
    $helper = Join-Path $Candidate 'helpers\cup-update-helper.exe'
    $uninstall = Join-Path $Candidate 'helpers\uninstall.ps1'
    $catalog = Join-Path $Candidate 'config\packages.cfg'
    $policy = Join-Path $Candidate 'config\install.cfg'
    $common = Join-Path $Candidate 'config\SHA256SUMS.common'
    $platformChecksums = Join-Path $Candidate 'config\SHA256SUMS.windows-x64'
    foreach ($path in @($binary, $helper, $uninstall, $catalog, $policy, $common, $platformChecksums)) {
        if (-not (Test-RealPathKind $path $false)) {
            return $false
        }
    }

    if (-not (Test-LegacyChecksumSet $common @(
            'packages.cfg', 'install.cfg', 'install.sh', 'install.ps1')) -or
        -not (Test-LegacyChecksumSet $platformChecksums @(
            'cup-windows-x64.exe', 'uninstall.ps1', 'release.txt')) -or
        -not (Test-LegacyNamedChecksum $platformChecksums 'cup-windows-x64.exe' $binary) -or
        -not (Test-LegacyNamedChecksum $platformChecksums 'uninstall.ps1' $uninstall) -or
        -not (Test-LegacyNamedChecksum $common 'packages.cfg' $catalog) -or
        -not (Test-LegacyNamedChecksum $common 'install.cfg' $policy)) {
        return $false
    }
    try {
        $binaryHash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
        $helperHash = (Get-FileHash -LiteralPath $helper -Algorithm SHA256).Hash
    } catch {
        return $false
    }
    if ($binaryHash -cne $helperHash -or
        -not (Test-LegacyCatalog $catalog) -or
        -not (Test-LegacyPolicy $policy) -or
        -not (Test-LegacyState (Join-Path $Candidate 'state.txt'))) {
        return $false
    }
    return $true
}

function Get-RootCandidateStatus([string]$Candidate) {
    $item = Get-FileSystemItemOrNull $Candidate
    if ($null -eq $item) {
        return 'missing'
    }
    if (-not $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        return 'foreign'
    }
    if (Test-RootMarker $Candidate) {
        return 'owned'
    }
    if ($null -ne (Get-FileSystemItemOrNull (Join-Path $Candidate 'root.txt'))) {
        if (Test-CupRootTraces $Candidate) {
            return 'invalid-marker'
        }
        return 'foreign'
    }
    if (Test-LegacyCupRoot $Candidate) {
        return 'legacy'
    }
    if (Test-CupRootBinary $Candidate) {
        return 'damaged'
    }
    return 'foreign'
}

function Select-CupRoot {
    $primary = Join-Path $script:UserProfilePath '.cup'
    $alternative = Join-Path $script:UserProfilePath '.coffee-cup'
    $primaryStatus = Get-RootCandidateStatus $primary
    $alternativeStatus = Get-RootCandidateStatus $alternative
    $primaryOwned = $primaryStatus -in @('owned', 'legacy')
    $alternativeOwned = $alternativeStatus -in @('owned', 'legacy')

    if ($primaryStatus -ceq 'damaged') {
        Fail (
            "a probable legacy cup root was found but its installed generation could not be " +
            "verified: $primary; the alternative root was preserved"
        )
    }
    if ($alternativeStatus -ceq 'damaged') {
        Fail (
            "a probable legacy cup root was found but its installed generation could not be " +
            "verified: $alternative; the primary root was preserved"
        )
    }
    if ($primaryStatus -ceq 'invalid-marker') {
        Fail (
            "cup root marker is invalid for the recognized root: $primary; " +
            'the alternative root was preserved'
        )
    }
    if ($alternativeStatus -ceq 'invalid-marker') {
        Fail (
            "cup root marker is invalid for the recognized root: $alternative; " +
            'the primary root was preserved'
        )
    }
    if ($primaryOwned -and $alternativeOwned) {
        Fail "both cup root candidates are recognized: $primary and $alternative"
    }
    if ($primaryOwned) { return $primary }
    if ($alternativeOwned) { return $alternative }
    if ($primaryStatus -ceq 'missing') { return $primary }
    if ($alternativeStatus -ceq 'missing') { return $alternative }
    Fail "neither existing cup root candidate is recognized: $primary or $alternative"
}
function Initialize-InstallationPaths {
    $script:CupRoot = Select-CupRoot
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
    $script:RootMarker = Join-Path $script:CupRoot "root.txt"
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
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "release metadata file is missing: $Path"
    }

    try {
        $lines = @(Get-Content -LiteralPath $Path -ErrorAction Stop)
    } catch {
        Fail "release metadata file is not readable: $Path"
    }

    $values = @{}
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $lineNumber = $index + 1
        $line = $lines[$index]
        $separator = $line.IndexOf('=')
        if ($separator -le 0 -or $separator -eq ($line.Length - 1) -or
            $line.IndexOf('=', $separator + 1) -ge 0) {
            Fail (
                "release metadata line $lineNumber must contain exactly one " +
                "non-empty 'key=value' assignment: $Path"
            )
        }

        $key = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        if ($key -cnotin @('format', 'version', 'commit')) {
            Fail "release metadata contains an unexpected field at line ${lineNumber}: $Path"
        }
        if ($values.ContainsKey($key)) {
            Fail "release metadata field '$key' is duplicated: $Path"
        }
        $values[$key] = $value
    }

    foreach ($required in @('format', 'version', 'commit')) {
        if (-not $values.ContainsKey($required)) {
            Fail "release metadata is missing required field '$required': $Path"
        }
    }
    if ($lines.Count -ne 3) {
        Fail "release metadata must contain exactly 3 lines; found $($lines.Count): $Path"
    }
    if ($values['format'] -cne '1') {
        Fail "release metadata format is unsupported; expected '1': $Path"
    }
    if ($values['version'] -notmatch
        '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$') {
        Fail "release metadata version is invalid; expected 'MAJOR.MINOR.PATCH': $Path"
    }
    if ($values['commit'] -notmatch '^[0-9a-f]{7,40}$') {
        Fail (
            "release metadata commit is invalid; expected 7 to 40 lowercase " +
            "hexadecimal characters: $Path"
        )
    }
    if ($ExpectedVersion -ne "" -and $values['version'] -cne $ExpectedVersion) {
        Fail (
            "release metadata version mismatch: expected '$ExpectedVersion', " +
            "received '$($values['version'])': $Path"
        )
    }
    if ($ExpectedCommit -ne "" -and $values['commit'] -cne $ExpectedCommit) {
        Fail (
            "release metadata commit mismatch: expected '$ExpectedCommit', " +
            "received '$($values['commit'])': $Path"
        )
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
            Key = "catalog"
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
            [System.IO.File]::Replace(
                $backup, $Asset.Destination, $NoFileReplaceBackup, $true)
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
        [System.IO.File]::Replace(
            $Asset.Source, $Asset.Destination, $NoFileReplaceBackup, $true)
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
            ($residue.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $residue.Name -notmatch '^\.cup-uninstall\.([A-Za-z0-9_-]+)$') {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        $token = $Matches[1]
        $binary = Join-Path $residue.FullName 'bin\cup.exe'
        $journal = Join-Path $residue.FullName 'transaction.txt'
        if (-not (Test-RootMarker $residue.FullName) -or
            -not (Test-RealPathKind $binary $false) -or
            -not (Test-RealPathKind $journal $false)) {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        try {
            $lines = @([IO.File]::ReadAllLines($journal))
        } catch {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        if ($lines.Count -ne 7 -or
            $lines[0] -cne 'format=1' -or
            $lines[1] -cne 'operation=uninstall' -or
            $lines[3] -cne "temporary_name=$($residue.Name)" -or
            $lines[4] -cne "token=$token") {
            Fail "unrecognized uninstall residue was preserved: $($residue.FullName)"
        }
        $isDetached = $lines[2] -ceq 'phase=detaching' -and
            $lines[5] -ceq 'stage=detach' -and $lines[6] -ceq 'error=0'
        $isFailedCleanup = $lines[2] -ceq 'phase=failed' -and
            $lines[5] -ceq 'stage=cleanup' -and
            $lines[6] -match '^error=([1-9][0-9]*)$'
        if (-not $isDetached -and -not $isFailedCleanup) {
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
    $requiredInheritance =
        [System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [System.Security.AccessControl.InheritanceFlags]::ObjectInherit
    $requiredRights = [System.Security.AccessControl.FileSystemRights]::FullControl
    if (-not $acl.AreAccessRulesProtected) {
        Fail "cup root ACL inherits permissions from its parent: $Path"
    }
    $ownerSid = ([System.Security.Principal.NTAccount]::new($acl.Owner)).Translate(
        [System.Security.Principal.SecurityIdentifier])
    if ($ownerSid.Value -cne $identity.User.Value) {
        Fail "cup root is not owned by the current user: $Path"
    }
    if (@($acl.Access).Count -ne 3) {
        Fail "cup root does not have the expected private ACL: $Path"
    }
    foreach ($rule in $acl.Access) {
        $sid = $rule.IdentityReference.Translate(
            [System.Security.Principal.SecurityIdentifier])
        if ($rule.IsInherited -or
            $rule.AccessControlType -ne [System.Security.AccessControl.AccessControlType]::Allow -or
            ($rule.InheritanceFlags -band $requiredInheritance) -ne $requiredInheritance -or
            ($rule.FileSystemRights -band $requiredRights) -ne $requiredRights -or
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
    if (-not (Test-RootMarker $CupRoot)) {
        if ($null -ne (Get-FileSystemItemOrNull $RootMarker)) {
            Fail "cup root marker is invalid: $RootMarker"
        }
        $temporaryMarker = Join-Path $CupRoot (
            ".root-marker." + [Guid]::NewGuid().ToString("N")
        )
        try {
            [System.IO.File]::WriteAllText(
                $temporaryMarker,
                "format=1`nproduct=coffee-clang/cup`nlayout=1`n",
                [System.Text.UTF8Encoding]::new($false)
            )
            [System.IO.File]::Move($temporaryMarker, $RootMarker)
        } finally {
            if (Test-Path -LiteralPath $temporaryMarker -PathType Leaf) {
                Remove-Item -LiteralPath $temporaryMarker -Force
            }
        }
    }

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
    try {
        Write-Info "Installing cup into $CupRoot"
        Receive-BootstrapAssets
        Assert-BootstrapAssets
        Backup-BootstrapAssets
        Commit-BootstrapAssets
        Set-BootstrapPermissions -RequireAssets $true

        New-Item -ItemType File -Path (Join-Path $Staging "committed") | Out-Null
    } catch {
        $installError = $_.Exception.Message
        if (Test-Path -LiteralPath $Staging -PathType Container) {
            try {
                Invoke-BootstrapRollback
            } catch {
                throw (
                    "$installError$([Environment]::NewLine)" +
                    $_.Exception.Message)
            }
        }
        throw
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
