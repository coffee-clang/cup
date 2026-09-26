# Downloads and verifies one immutable Windows release generation, then delegates
# installation to the verified cup executable. The installer owns transport; cup owns state and recovery.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoOwner = 'coffee-clang'
$RepoName = 'cup'
$ReleaseVersion = "@CUP_RELEASE_VERSION@"
$ReleaseTag = "@CUP_RELEASE_TAG@"
$ReleaseCommit = "@CUP_RELEASE_COMMIT@"
$DefaultBaseUrl = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag"
$BaseUrlOverridden = -not [string]::IsNullOrWhiteSpace($env:CUP_INSTALL_BASE_URL)
$BaseUrl = if ($BaseUrlOverridden) {
    $env:CUP_INSTALL_BASE_URL.TrimEnd('/')
} else {
    $DefaultBaseUrl
}
$Work = $null
$MaxBinaryBytes = 268435456L
$MaxTextBytes = 16777216L
$DownloadTimeoutSeconds = 180
$LowSpeedSeconds = 30
$LowSpeedBytesPerSecond = 1024
$MaxRedirects = if ($BaseUrlOverridden) { 0 } else { 10 }

Add-Type -AssemblyName System.Net.Http

function Fail([string]$Message) {
    throw "Error: $Message"
}

function Write-Phase([string]$Message) {
    [Console]::Error.WriteLine("==> $Message")
}

function Assert-InstallerIdentity {
    if ($ReleaseVersion.Contains('@CUP_RELEASE_') -or
        $ReleaseTag.Contains('@CUP_RELEASE_') -or
        $ReleaseCommit.Contains('@CUP_RELEASE_')) {
        Fail 'installer was not prepared for a concrete release'
    }
    if ($ReleaseVersion -cnotmatch '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$') {
        Fail 'installer has an invalid release version'
    }
    if ($ReleaseTag -cne "v$ReleaseVersion") {
        Fail 'installer release tag does not match its version'
    }
    if ($ReleaseCommit -cnotmatch '^[0-9a-f]{40}$') {
        Fail 'installer has an invalid release commit'
    }
}

function Assert-BaseUrl {
    try {
        $uri = [Uri]$BaseUrl
    } catch {
        Fail 'installer base URL is invalid'
    }
    if (-not $uri.IsAbsoluteUri -or -not [string]::IsNullOrEmpty($uri.UserInfo) -or
        -not [string]::IsNullOrEmpty($uri.Query) -or
        -not [string]::IsNullOrEmpty($uri.Fragment) -or $BaseUrl.Contains('\')) {
        Fail 'installer base URL is invalid'
    }

    if (-not $BaseUrlOverridden) {
        if ($BaseUrl -cne $DefaultBaseUrl -or $uri.Scheme -cne 'https') {
            Fail 'installer official release base URL is invalid'
        }
        return
    }

    if ($env:CUP_INSTALL_ALLOW_INSECURE -cne '1') {
        Fail 'release base URL override is test-only and requires CUP_INSTALL_ALLOW_INSECURE=1'
    }
    if ($uri.Scheme -cne 'http') {
        Fail 'installer release base URL override must use loopback HTTP'
    }
    if ($uri.Host -cnotin @('127.0.0.1', 'localhost', '[::1]', '::1')) {
        Fail 'installer release base URL override must use an allowed loopback host and explicit port'
    }
    if ($BaseUrl -cnotmatch '^http://(?:127\.0\.0\.1|localhost|\[::1\]):([0-9]+)(?:/|$)') {
        Fail 'installer release base URL override must use an allowed loopback host and explicit port'
    }
    $port = 0
    if (-not [int]::TryParse($Matches[1], [ref]$port) -or $port -lt 1 -or $port -gt 65535) {
        Fail 'installer release base URL override has an invalid port'
    }
}

function New-PrivateDirectory {
    $path = Join-Path ([IO.Path]::GetTempPath()) ("cup-install-" + [Guid]::NewGuid().ToString('N'))
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $security = New-Object Security.AccessControl.DirectorySecurity
    $security.SetOwner($identity.User)
    $security.SetAccessRuleProtection($true, $false)
    $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
        [Security.AccessControl.InheritanceFlags]::ObjectInherit
    $propagation = [Security.AccessControl.PropagationFlags]::None
    $principals = @(
        $identity.User,
        [Security.Principal.SecurityIdentifier]::new(
            [Security.Principal.WellKnownSidType]::LocalSystemSid, $null),
        [Security.Principal.SecurityIdentifier]::new(
            [Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid, $null)
    )
    foreach ($principal in $principals) {
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $principal,
            [Security.AccessControl.FileSystemRights]::FullControl,
            $inheritance,
            $propagation,
            [Security.AccessControl.AccessControlType]::Allow)
        [void]$security.AddAccessRule($rule)
    }
    [IO.Directory]::CreateDirectory($path, $security) | Out-Null
    return $path
}

function Get-CanonicalLines([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.Length -le 0 -or $item.Length -gt $MaxTextBytes) {
        Fail "release text asset is invalid: $($item.Name)"
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ne $item.Length -or $bytes[$bytes.Length - 1] -ne 10) {
        Fail "release text asset is not canonical: $($item.Name)"
    }
    foreach ($byte in $bytes) {
        if ($byte -ne 10 -and ($byte -lt 32 -or $byte -gt 126)) {
            Fail "release text asset contains non-canonical bytes: $($item.Name)"
        }
    }
    $parts = [Text.Encoding]::ASCII.GetString($bytes).Split([char]10)
    if ($parts[$parts.Length - 1].Length -ne 0) {
        Fail "release text asset is not canonical: $($item.Name)"
    }
    return @($parts[0..($parts.Length - 2)])
}

function Assert-TransportUri([Uri]$Uri) {
    if (-not $Uri.IsAbsoluteUri -or -not [string]::IsNullOrEmpty($Uri.UserInfo)) {
        Fail 'release asset redirect URI is invalid'
    }
    $baseScheme = ([Uri]$BaseUrl).Scheme
    if ($baseScheme -ceq 'https') {
        if ($Uri.Scheme -cne 'https') {
            Fail 'release asset redirect left HTTPS'
        }
        return
    }
    if ($Uri.Scheme -cne 'http' -or -not $Uri.IsLoopback) {
        Fail 'release asset redirect left the allowed loopback transport'
    }
}

function Get-AssetLimit([string]$Name) {
    if ($Name.StartsWith('cup-', [StringComparison]::Ordinal)) {
        return $MaxBinaryBytes
    }
    return $MaxTextBytes
}

function Receive-Asset([string]$Name) {
    if ([string]::IsNullOrWhiteSpace($Name) -or $Name.Contains('/') -or $Name.Contains('\') -or
        $Name -eq '.' -or $Name -eq '..') {
        Fail "unsafe release asset name: $Name"
    }

    $destination = Join-Path $Work $Name
    $current = [Uri]("$BaseUrl/$Name")
    if ($current.AbsoluteUri -cne "$BaseUrl/$Name") {
        Fail "unsafe release asset URI: $Name"
    }
    Assert-TransportUri $current

    $maximum = Get-AssetLimit $Name
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [Threading.Timeout]::InfiniteTimeSpan
    $client.DefaultRequestHeaders.UserAgent.ParseAdd("cup-installer/$ReleaseVersion")
    $requestCancellation = [Threading.CancellationTokenSource]::new()
    $requestCancellation.CancelAfter([TimeSpan]::FromSeconds($DownloadTimeoutSeconds))
    $completed = $false
    $showProgress = (
        $Name -ceq 'cup-windows-x64.exe' -and
        [Environment]::UserInteractive -and
        -not [Console]::IsErrorRedirected
    )

    try {
        for ($redirect = 0; $redirect -le $MaxRedirects; $redirect++) {
            $response = $null
            try {
                $response = $client.GetAsync(
                    $current,
                    [Net.Http.HttpCompletionOption]::ResponseHeadersRead,
                    $requestCancellation.Token
                ).GetAwaiter().GetResult()
                $status = [int]$response.StatusCode
                if ($status -ge 300 -and $status -lt 400) {
                    $location = $response.Headers.Location
                    if ($null -eq $location -or $redirect -eq $MaxRedirects) {
                        Fail "could not follow a release redirect for $Name"
                    }
                    $current = [Uri]::new($current, $location)
                    Assert-TransportUri $current
                    continue
                }
                if (-not $response.IsSuccessStatusCode) {
                    Fail "could not download $Name"
                }
                $contentLength = $response.Content.Headers.ContentLength
                if ($null -ne $contentLength -and $contentLength -gt $maximum) {
                    Fail "downloaded asset is too large: $Name"
                }

                $input = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $output = [IO.FileStream]::new(
                    $destination,
                    [IO.FileMode]::CreateNew,
                    [IO.FileAccess]::Write,
                    [IO.FileShare]::None
                )
                try {
                    $buffer = [byte[]]::new(65536)
                    [Int64]$total = 0
                    [Int64]$windowBytes = 0
                    $window = [Diagnostics.Stopwatch]::StartNew()
                    while ($true) {
                        $read = $input.ReadAsync(
                            $buffer,
                            0,
                            $buffer.Length,
                            $requestCancellation.Token
                        )
                        if (-not $read.Wait([TimeSpan]::FromSeconds($LowSpeedSeconds))) {
                            Fail "downloaded asset remained below the minimum transfer speed: $Name"
                        }
                        $count = $read.GetAwaiter().GetResult()
                        if ($count -le 0) {
                            if ($window.Elapsed.TotalSeconds -ge $LowSpeedSeconds) {
                                $minimum = $LowSpeedBytesPerSecond * $window.Elapsed.TotalSeconds
                                if ($windowBytes -lt $minimum) {
                                    Fail "downloaded asset remained below the minimum transfer speed: $Name"
                                }
                            }
                            break
                        }

                        $total += $count
                        $windowBytes += $count
                        if ($total -gt $maximum) {
                            Fail "downloaded asset is too large: $Name"
                        }
                        $output.Write($buffer, 0, $count)
                        if ($showProgress) {
                            if ($null -ne $contentLength -and $contentLength -gt 0) {
                                $percent = [Math]::Min(100, [int](100 * $total / $contentLength))
                                Write-Progress -Activity "Downloading cup $ReleaseVersion" `
                                    -Status "$total / $contentLength bytes" -PercentComplete $percent
                            } else {
                                Write-Progress -Activity "Downloading cup $ReleaseVersion" `
                                    -Status "$total bytes"
                            }
                        }

                        if ($window.Elapsed.TotalSeconds -ge $LowSpeedSeconds) {
                            $minimum = $LowSpeedBytesPerSecond * $window.Elapsed.TotalSeconds
                            if ($windowBytes -lt $minimum) {
                                Fail "downloaded asset remained below the minimum transfer speed: $Name"
                            }
                            $window.Restart()
                            $windowBytes = 0
                        }
                    }
                    $output.Flush($true)
                } finally {
                    $output.Dispose()
                    $input.Dispose()
                }
                if ($total -le 0) {
                    Fail "downloaded asset is empty: $Name"
                }
                $completed = $true
                break
            } finally {
                if ($null -ne $response) {
                    $response.Dispose()
                }
            }
        }
    } catch {
        $message = $_.Exception.Message
        if ($message.StartsWith('Error: ', [StringComparison]::Ordinal)) {
            throw
        }
        Fail "could not download $Name`: $message"
    } finally {
        if ($showProgress) {
            Write-Progress -Activity "Downloading cup $ReleaseVersion" -Completed
        }
        $requestCancellation.Dispose()
        $client.Dispose()
        $handler.Dispose()
        if (-not $completed -and [IO.File]::Exists($destination)) {
            [IO.File]::Delete($destination)
        }
    }

    if (-not $completed) {
        Fail "could not download $Name"
    }
    $item = Get-Item -LiteralPath $destination -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.Length -le 0 -or $item.Length -gt $maximum) {
        Fail "downloaded asset is not a valid regular file: $Name"
    }
}

function Assert-ReleaseManifest {
    $lines = Get-CanonicalLines (Join-Path $Work 'release.txt')
    if ($lines.Count -lt 6) { Fail 'release metadata is incomplete' }
    if ($lines[0] -cne 'format=2') { Fail 'release metadata has an unsupported format' }
    if ($lines[1] -cne "version=$ReleaseVersion") { Fail 'release metadata version does not match the installer' }
    if ($lines[2] -cne "commit=$ReleaseCommit") { Fail 'release metadata commit does not match the installer' }
    if ($lines[3] -cne 'root_layout=2') { Fail 'release metadata root layout is incompatible' }
    if ($lines[4] -cne 'catalog_format=1') { Fail 'release metadata catalog format is incompatible' }
    if ($lines[5] -cnotmatch '^asset_count=([0-9]+)$') { Fail 'release metadata asset count is invalid' }
    $assetCount = [int]$Matches[1]
    if ($assetCount -lt 1 -or $assetCount -gt 256 -or $lines.Count -ne 6 + (2 * $assetCount)) {
        Fail 'release metadata asset count is invalid'
    }
    $script:ManifestHashes = @{}
    $previous = $null
    for ($i = 0; $i -lt $assetCount; $i++) {
        $nameLine = $lines[6 + (2 * $i)]
        $shaLine = $lines[7 + (2 * $i)]
        $namePrefix = "asset.$i.name="
        $shaPrefix = "asset.$i.sha256="
        if (-not $nameLine.StartsWith($namePrefix, [StringComparison]::Ordinal) -or
            -not $shaLine.StartsWith($shaPrefix, [StringComparison]::Ordinal)) {
            Fail 'release metadata asset record is not contiguous'
        }
        $name = $nameLine.Substring($namePrefix.Length)
        $sha = $shaLine.Substring($shaPrefix.Length)
        if ($name -cnotmatch '^[A-Za-z0-9._-]+$' -or $name -ceq 'release.txt') {
            Fail 'release metadata contains an unsafe asset name'
        }
        if ($sha -cnotmatch '^[0-9a-f]{64}$') { Fail "release metadata has an invalid digest for $name" }
        if ($null -ne $previous -and [string]::CompareOrdinal($previous, $name) -ge 0) {
            Fail 'release metadata assets are not strictly ordered'
        }
        if ($script:ManifestHashes.ContainsKey($name)) { Fail "release metadata duplicates asset $name" }
        $script:ManifestHashes[$name] = $sha
        $previous = $name
    }
}

function Assert-ManifestAsset([string]$Name) {
    if (-not $script:ManifestHashes.ContainsKey($Name)) { Fail "release manifest does not authenticate $Name" }
    $path = Join-Path $Work $Name
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $script:ManifestHashes[$Name]) { Fail "release manifest digest mismatch for $Name" }
}

function Get-CupCanonicalBase([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { Fail 'cup base directory is not available' }
    $windowsPath = $Path.Replace('/', '\')
    if ($windowsPath.StartsWith('\\.\', [StringComparison]::Ordinal)) {
        Fail 'cup base contains an unsupported Windows device path'
    }
    $driveAbsolute = $windowsPath -match '^[A-Za-z]:\\'
    $uncAbsolute = $windowsPath.StartsWith('\\', [StringComparison]::Ordinal)
    if (-not $driveAbsolute -and -not $uncAbsolute) { Fail 'cup base must contain an absolute path' }
    try {
        $absolute = [IO.Path]::GetFullPath($windowsPath)
        $item = Get-Item -LiteralPath $absolute -Force
    } catch { Fail 'cup base must name an existing directory' }
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail 'cup base must name an existing real directory'
    }
    $trimmed = $absolute.TrimEnd([char[]]'\/')
    $volume = [IO.Path]::GetPathRoot($absolute).TrimEnd([char[]]'\/')
    if ($trimmed.Equals($volume, [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'cup base must be a directory below the volume root'
    }
    if ($trimmed.StartsWith('\\?\UNC\', [StringComparison]::OrdinalIgnoreCase)) {
        $trimmed = '//' + $trimmed.Substring(8)
    } elseif ($trimmed.StartsWith('\\?\', [StringComparison]::Ordinal)) {
        $trimmed = $trimmed.Substring(4)
    } elseif ($trimmed.StartsWith('\\', [StringComparison]::Ordinal)) {
        $trimmed = '//' + $trimmed.Substring(2)
    }
    return $trimmed.Replace('\', '/')
}

function Invoke-NativeRootSelect([string]$Bootstrap, [string]$Base) {
    $output = @(& $Bootstrap --internal-select-root $Base 2>$null)
    if ($LASTEXITCODE -ne 0 -or $output.Count -ne 1) { Fail "could not select a canonical cup root below $Base" }
    $root = $output[0]
    if (-not ($root.Equals("$Base/.cup", [StringComparison]::OrdinalIgnoreCase) -or
              $root.Equals("$Base/.coffee-cup", [StringComparison]::OrdinalIgnoreCase))) {
        Fail 'native root selection returned an unexpected path'
    }
    return $root
}

function Test-NativeRootProbe([string]$Bootstrap, [string]$Root) {
    try { & $Bootstrap --internal-root-probe $Root *> $null; return $LASTEXITCODE -eq 0 } catch { return $false }
}

function Get-InstalledVersion([string]$Binary) {
    $output = @(& $Binary --version 2>$null)
    if ($LASTEXITCODE -ne 0 -or $output.Count -ne 1 -or
        -not $output[0].StartsWith('cup ', [StringComparison]::Ordinal)) {
        Fail 'existing cup version response is invalid'
    }
    return $output[0].Substring(4)
}

function Compare-CupVersion([string]$Left, [string]$Right) {
    $l = $Left.Split('.') | ForEach-Object { [int]$_ }
    $r = $Right.Split('.') | ForEach-Object { [int]$_ }
    for ($i = 0; $i -lt 3; $i++) {
        if ($l[$i] -lt $r[$i]) { return -1 }
        if ($l[$i] -gt $r[$i]) { return 1 }
    }
    return 0
}

function Find-PathInstallation([string]$Bootstrap) {
    $command = Get-Command cup.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) { $command = Get-Command cup -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if ($null -eq $command) { return $null }
    try { $item = Get-Item -LiteralPath $command.Source -Force } catch { return $null }
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return $null }
    $bin = [IO.Path]::GetDirectoryName($item.FullName)
    if ([IO.Path]::GetFileName($bin) -ine 'bin') { return $null }
    $rootNative = [IO.Path]::GetDirectoryName($bin)
    $leaf = [IO.Path]::GetFileName($rootNative)
    if ($leaf -ine '.cup' -and $leaf -ine '.coffee-cup') { return $null }
    $root = (Get-CupCanonicalBase ([IO.Path]::GetDirectoryName($rootNative))) + "/$leaf"
    if (-not (Test-NativeRootProbe $Bootstrap $root)) { return $null }
    return [PSCustomObject]@{ Root = $root; Binary = $item.FullName }
}

function Select-Installation([string]$Bootstrap) {
    if (-not [string]::IsNullOrWhiteSpace($env:CUP_INSTALL_BASE_DIR)) {
        $base = Get-CupCanonicalBase $env:CUP_INSTALL_BASE_DIR
        return [PSCustomObject]@{ Base = $base; Root = (Invoke-NativeRootSelect $Bootstrap $base) }
    }
    $pathInstallation = Find-PathInstallation $Bootstrap
    if ($null -ne $pathInstallation -and [Environment]::UserInteractive) {
        $version = Get-InstalledVersion $pathInstallation.Binary
        $answer = Read-Host "Found cup $version at $($pathInstallation.Root). Use this installation? [Y/n]"
        if ([string]::IsNullOrWhiteSpace($answer) -or $answer -match '^(?i:y|yes)$') {
            $base = Get-CupCanonicalBase ([IO.Path]::GetDirectoryName($pathInstallation.Root.Replace('/', '\')))
            return [PSCustomObject]@{ Base = $base; Root = $pathInstallation.Root }
        }
    }
    $baseInput = $env:USERPROFILE
    if ([Environment]::UserInteractive) {
        $answer = Read-Host "Choose the parent/base directory for cup [$baseInput]"
        if (-not [string]::IsNullOrWhiteSpace($answer)) { $baseInput = $answer }
    }
    $base = Get-CupCanonicalBase $baseInput
    return [PSCustomObject]@{ Base = $base; Root = (Invoke-NativeRootSelect $Bootstrap $base) }
}

function Assert-NoImplicitDowngrade([string]$Bootstrap, [string]$Root) {
    if (-not (Test-NativeRootProbe $Bootstrap $Root)) { return }
    $binary = Join-Path ($Root.Replace('/', '\')) 'bin\cup.exe'
    $existing = Get-InstalledVersion $binary
    if ((Compare-CupVersion $existing $ReleaseVersion) -gt 0) {
        Fail "refusing to replace newer cup $existing at $Root with older $ReleaseVersion; choose another base directory"
    }
}

function Get-BootstrapRoot([string[]]$Output, [string]$ExpectedRoot) {
    $records = @($Output | Where-Object { $_.StartsWith('CUP_BOOTSTRAP_ROOT=', [StringComparison]::Ordinal) })
    if ($records.Count -ne 1) { Fail 'bootstrap did not report one canonical root' }
    $root = $records[0].Substring('CUP_BOOTSTRAP_ROOT='.Length)
    if (-not $root.Equals($ExpectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'bootstrap changed the selected canonical root'
    }
    foreach ($line in $Output) {
        if (-not $line.StartsWith('CUP_BOOTSTRAP_ROOT=', [StringComparison]::Ordinal)) { Write-Host $line }
    }
    return $root.Replace('/', '\')
}

function Normalize-PathEntry([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { return '' }
    return $Value.Trim().TrimEnd([char[]]'\/')
}

function Test-PathContains([string]$PathValue, [string]$Directory) {
    $wanted = Normalize-PathEntry $Directory
    foreach ($entry in ($PathValue -split ';')) {
        if ((Normalize-PathEntry $entry).Equals($wanted, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Offer-PathIntegration([string]$Root) {
    $bin = Join-Path ($Root.Replace('/', '\')) 'bin'
    $hasControl = $false
    foreach ($character in $bin.ToCharArray()) {
        if ([char]::IsControl($character)) { $hasControl = $true; break }
    }
    if ($bin.Contains(';') -or $hasControl) {
        Write-Host (
            "Automatic User PATH integration is unavailable for $bin because the path " +
            "contains unsafe separator/control content. Configure PATH manually or use the full path to cup.exe.")
        return
    }
    if (Test-PathContains $env:Path $bin) { return }
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (Test-PathContains $userPath $bin) {
        Write-Host "cup is already in your User PATH. Open a new shell to use $bin."
        return
    }
    if ($env:CUP_INSTALL_NO_PATH_PROMPT -ceq '1' -or -not [Environment]::UserInteractive) { return }
    $answer = Read-Host "Add $bin to your User PATH? [y/N]"
    if ([string]::IsNullOrWhiteSpace($answer) -or $answer -notmatch '^(?i:y|yes)$') { return }
    $newPath = if ([string]::IsNullOrWhiteSpace($userPath)) { $bin } else { "$userPath;$bin" }
    try {
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    } catch {
        Write-Warning (
            "Could not update your User PATH automatically. cup remains installed at $bin. " +
            "Configure PATH manually to use cup from a new shell.")
        return
    }
    Write-Host "Added $bin to your User PATH. Open a new shell to use it."
}

try {
    Assert-InstallerIdentity
    Assert-BaseUrl
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) { Fail 'USERPROFILE is not available' }
    $Work = New-PrivateDirectory
    $binaryAsset = 'cup-windows-x64.exe'

    Write-Phase "Downloading cup $ReleaseVersion..."
    Receive-Asset 'release.txt'
    Assert-ReleaseManifest
    foreach ($asset in @($binaryAsset, 'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'catalog.cfg')) {
        Receive-Asset $asset
    }
    Write-Phase 'Verifying release...'
    foreach ($asset in @($binaryAsset, 'LICENSE', 'THIRD_PARTY_NOTICES.txt', 'catalog.cfg')) {
        Assert-ManifestAsset $asset
    }

    $bootstrap = Join-Path $Work $binaryAsset
    $selection = Select-Installation $bootstrap
    $freshInstall = -not (Test-NativeRootProbe $bootstrap $selection.Root)
    Write-Host "cup will be installed in $($selection.Root)"
    Write-Phase 'Installing cup...'
    $bootstrapOutput = @(& $bootstrap --internal-bootstrap $Work $selection.Base)
    if ($LASTEXITCODE -ne 0) { Fail 'the verified cup bootstrap transaction was rejected' }
    $bootstrapRoot = Get-BootstrapRoot $bootstrapOutput $selection.Root
    $installed = Join-Path $bootstrapRoot 'bin\cup.exe'
    if (-not (Test-NativeRootProbe $bootstrap $selection.Root) -or
        -not [IO.File]::Exists($installed) -or (Get-InstalledVersion $installed) -cne $ReleaseVersion) {
        Fail 'installed cup generation did not validate after bootstrap'
    }

    if ($freshInstall) {
        & $installed install coffee
        if ($LASTEXITCODE -eq 0) {
            Write-Host 'Coffee installed successfully.'
        } else {
            $transaction = Join-Path $bootstrapRoot 'transaction.txt'
            if ([IO.File]::Exists($transaction) -or [IO.Directory]::Exists($transaction)) {
                Fail 'optional Coffee installation left an unresolved cup transaction; run cup repair'
            }
            $coffeeState = @(& $installed list package-manager 2>$null)
            if ($LASTEXITCODE -eq 0 -and ($coffeeState -join "`n") -match 'package-manager: coffee@') {
                Write-Warning 'Coffee was installed, but derived commands need repair; run cup repair.'
            } else {
                Write-Warning 'Coffee was not installed; the cup core installation is ready.'
            }
        }
    }

    Write-Host "cup $ReleaseVersion installed successfully."
    Write-Host "Binary: $installed"
    Offer-PathIntegration $selection.Root
} catch {
    Write-Error $_.Exception.Message
    exit 1
} finally {
    if ($null -ne $Work -and [IO.Directory]::Exists($Work)) {
        [IO.Directory]::Delete($Work, $true)
    }
}
