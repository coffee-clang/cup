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
$WaitAttempts = if ([string]::IsNullOrWhiteSpace($env:CUP_INSTALL_WAIT_ATTEMPTS)) {
    120
} else {
    0
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

function Resolve-WaitAttempts {
    if (-not [string]::IsNullOrWhiteSpace($env:CUP_INSTALL_WAIT_ATTEMPTS)) {
        $parsed = 0
        if (-not [int]::TryParse($env:CUP_INSTALL_WAIT_ATTEMPTS, [ref]$parsed) -or
            $parsed -lt 1 -or $parsed -gt 3600) {
            Fail 'CUP_INSTALL_WAIT_ATTEMPTS is invalid'
        }
        $script:WaitAttempts = $parsed
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

function Assert-ChecksumDocument([string]$DocumentName, [string[]]$ExpectedNames) {
    $lines = @(Get-CanonicalLines (Join-Path $Work $DocumentName))
    if ($lines.Count -ne $ExpectedNames.Count) {
        Fail "checksum document has an unexpected entry count: $DocumentName"
    }
    for ($index = 0; $index -lt $ExpectedNames.Count; $index++) {
        $expectedName = $ExpectedNames[$index]
        $match = [regex]::Match($lines[$index], '^([0-9a-f]{64})  ([^\s]+)$')
        if (-not $match.Success -or $match.Groups[2].Value -cne $expectedName) {
            Fail "checksum entry is not canonical: $expectedName"
        }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Work $expectedName)).Hash.ToLowerInvariant()
        if ($actual -cne $match.Groups[1].Value) {
            Fail "checksum mismatch for $expectedName"
        }
    }
}

function Assert-ReleaseMetadata {
    $lines = @(Get-CanonicalLines (Join-Path $Work 'release.txt'))
    if ($lines.Count -ne 3 -or $lines[0] -cne 'format=1') {
        Fail 'release metadata has an unsupported format'
    }
    if ($lines[1] -cne "version=$ReleaseVersion") {
        Fail 'release metadata version does not match the installer'
    }
    if ($lines[2] -cne "commit=$ReleaseCommit") {
        Fail 'release metadata commit does not match the installer'
    }
}

function Test-DirectoryEmpty([string]$Path) {
    if (-not [IO.Directory]::Exists($Path)) {
        return $false
    }
    $enumerator = [IO.Directory]::EnumerateFileSystemEntries($Path).GetEnumerator()
    try {
        return -not $enumerator.MoveNext()
    } finally {
        $enumerator.Dispose()
    }
}

function Test-CupRootMarker([string]$Root) {
    try {
        $lines = @(Get-CanonicalLines (Join-Path $Root 'root.txt'))
    } catch {
        return $false
    }
    return $lines.Count -eq 3 -and
        $lines[0] -ceq 'format=1' -and
        $lines[1] -ceq 'product=coffee-clang/cup' -and
        $lines[2] -ceq 'layout=1'
}

function Test-ExpectedCupBinary([string]$Path) {
    try {
        $item = Get-Item -LiteralPath $Path -Force
        if ($item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $false
        }
        $output = @(& $Path --version 2>$null)
        return $LASTEXITCODE -eq 0 -and $output.Count -eq 1 -and
            $output[0] -ceq "cup $ReleaseVersion"
    } catch {
        return $false
    }
}

function Test-CupReady([string]$Path) {
    try {
        & $Path --internal-runtime-ready *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

function Get-BootstrapRoot([string[]]$Output) {
    $records = @($Output | Where-Object {
        $_.StartsWith('CUP_BOOTSTRAP_ROOT=', [StringComparison]::Ordinal)
    })
    if ($records.Count -ne 1) {
        Fail 'bootstrap did not report one canonical root'
    }
    $root = $records[0].Substring('CUP_BOOTSTRAP_ROOT='.Length)
    $primary = Join-Path $env:USERPROFILE '.cup'
    $fallback = Join-Path $env:USERPROFILE '.coffee-cup'
    if (-not $root.Equals($primary, [StringComparison]::OrdinalIgnoreCase) -and
        -not $root.Equals($fallback, [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'bootstrap reported an unsupported canonical root'
    }
    foreach ($line in $Output) {
        if (-not $line.StartsWith('CUP_BOOTSTRAP_ROOT=', [StringComparison]::Ordinal)) {
            Write-Host $line
        }
    }
    return $root
}

function Wait-ForCommit([string]$Root) {
    $binary = Join-Path $Root 'bin\cup.exe'
    $transaction = Join-Path $Root 'transaction.txt'
    $staging = Join-Path $Root 'staging'

    for ($attempt = 0; $attempt -lt $WaitAttempts; $attempt++) {
        if ((Test-CupRootMarker $Root) -and
            -not [IO.File]::Exists($transaction) -and
            -not [IO.Directory]::Exists($transaction) -and
            (Test-DirectoryEmpty $staging) -and
            (Test-ExpectedCupBinary $binary) -and
            (Test-CupReady $binary)) {
            return $binary
        }
        Start-Sleep -Seconds 1
    }
    Fail 'timed out while waiting for the installed cup to become ready'
}

try {
    Assert-InstallerIdentity
    Assert-BaseUrl
    Resolve-WaitAttempts
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        Fail 'USERPROFILE is not available'
    }
    $Work = New-PrivateDirectory
    $binaryAsset = 'cup-windows-x64.exe'
    $platformSums = 'SHA256SUMS.windows-x64'
    $assets = @(
        $binaryAsset, 'release.txt', $platformSums,
        'SHA256SUMS.common', 'packages.cfg', 'install.cfg', 'install.sh', 'install.ps1')
    foreach ($asset in $assets) {
        Receive-Asset $asset
    }
    Assert-ChecksumDocument 'SHA256SUMS.common' @(
        'packages.cfg', 'install.cfg', 'install.sh', 'install.ps1')
    Assert-ChecksumDocument $platformSums @(
        $binaryAsset, 'release.txt', 'SHA256SUMS.common')
    Assert-ReleaseMetadata

    $bootstrap = Join-Path $Work $binaryAsset
    $bootstrapOutput = @(& $bootstrap --internal-bootstrap $Work)
    if ($LASTEXITCODE -ne 0) {
        Fail 'the verified cup bootstrap transaction was rejected'
    }
    $bootstrapRoot = Get-BootstrapRoot $bootstrapOutput
    $installed = Wait-ForCommit $bootstrapRoot
    Write-Host "cup $ReleaseVersion installed successfully."
    Write-Host "Binary: $installed"
    Write-Host "Add $([IO.Path]::GetDirectoryName($installed)) to PATH if it is not already available."
} catch {
    Write-Error $_.Exception.Message
    exit 1
} finally {
    if ($null -ne $Work -and [IO.Directory]::Exists($Work)) {
        [IO.Directory]::Delete($Work, $true)
    }
}
