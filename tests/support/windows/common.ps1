# Provides shared Windows integration assertions, isolated roots,
# and native fixture builders.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Script:CupTestScriptDir = $PSScriptRoot
$Script:CupTestProjectRoot = (Resolve-Path (Join-Path $Script:CupTestScriptDir "..\..\..")).Path
$Script:CupTestBuildRoot = $null
$Script:CupTestExecutable = $null
$Script:CupTestRoot = $null
$Script:CupTestHome = $null
$Script:CupTestDevRoot = $null
$Script:CupTestOriginalUserProfile = $null
$Script:CupTestOriginalEnvironment = @{}
$Script:CupTestOriginalEnvironmentCaptured = $false
$Script:CupTestCommandProcessor = $null

. (Join-Path $PSScriptRoot "configuration.ps1")
. (Join-Path $PSScriptRoot "build.ps1")
. (Join-Path $PSScriptRoot "process.ps1")
. (Join-Path $PSScriptRoot "hash.ps1")

function Fail-Test {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Message
    )
    throw "TEST FAILED: $Message"
}

function Assert-Contains {
    param(
        [AllowEmptyString()]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Expected
    )
    if (-not $Text.Contains($Expected)) {
        Fail-Test "expected output to contain: $Expected`nActual output:`n$Text"
    }
}

function Assert-ContainsPathText {
    param(
        [AllowEmptyString()]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Expected
    )

    $normalizedText = $Text.Replace('\', '/')
    $normalizedExpected = $Expected.Replace('\', '/')
    if (-not $normalizedText.Contains($normalizedExpected)) {
        Fail-Test (
            "expected output to contain path text: $Expected`n" +
            "Actual output:`n$Text")
    }
}

function Assert-NotContains {
    param(
        [AllowEmptyString()]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Unexpected
    )
    if ($Text.Contains($Unexpected)) {
        Fail-Test "expected output not to contain: $Unexpected`nActual output:`n$Text"
    }
}

function Assert-PathExists {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        Fail-Test "expected path: $Path"
    }
}

function Assert-PathMissing {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($null -ne $item) {
        Fail-Test "expected missing path: $Path"
    }
}

function Assert-Equals {
    param(
        [AllowEmptyString()]
        [string]$Actual,

        [AllowEmptyString()]
        [string]$Expected
    )
    if ($Actual -cne $Expected) {
        Fail-Test "expected '$Expected', got '$Actual'"
    }
}

# Isolated paths and native process invocation.
function New-RealTestDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Parent,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Name) -or
        $Name -in @(".", "..") -or
        $Name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
        $Name.Contains([string][IO.Path]::DirectorySeparatorChar) -or
        $Name.Contains([string][IO.Path]::AltDirectorySeparatorChar)) {
        Fail-Test "invalid test directory name: $Name"
    }
    $path = Join-Path $Parent $Name
    if (Test-Path -LiteralPath $path) {
        if (-not (Test-Path -LiteralPath $path -PathType Container)) {
            Fail-Test "test path is not a directory: $path"
        }
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail-Test "test directory is a reparse point: $path"
        }
        return $path
    }
    $item = New-Item -ItemType Directory -Path $path -ErrorAction Stop
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail-Test "created test directory is a reparse point: $path"
    }
    return $item.FullName
}

function New-IsolatedTestRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $configuration = Get-TestConfiguration
    if ($null -eq $Script:CupTestBuildRoot) {
        $Script:CupTestBuildRoot = Resolve-TestBuildRoot -ProjectRoot $Script:CupTestProjectRoot
    }
    $platformRoot = New-RealTestDirectory `
        -Parent $Script:CupTestBuildRoot -Name "windows-x64"
    $configurationRoot = New-RealTestDirectory `
        -Parent $platformRoot -Name $configuration
    $base = New-RealTestDirectory -Parent $configurationRoot -Name "tests"
    $rootName = "cup-$Name-tests-" + [guid]::NewGuid().ToString("N")
    return New-RealTestDirectory -Parent $base -Name $rootName
}

function Get-TestHelperPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $configuration = Get-TestConfiguration
    if ($null -eq $Script:CupTestBuildRoot) {
        $Script:CupTestBuildRoot = Resolve-TestBuildRoot `
            -ProjectRoot $Script:CupTestProjectRoot
    }
    return Resolve-TestHelperPath `
        -BuildRoot $Script:CupTestBuildRoot `
        -Configuration $configuration `
        -Name $Name
}
function New-ZipPackageFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version,

        [string]$ExtraPath,

        [AllowEmptyString()]
        [string]$ExtraContent
    )

    $hasExtraPath = $PSBoundParameters.ContainsKey('ExtraPath')
    $hasExtraContent = $PSBoundParameters.ContainsKey('ExtraContent')
    if ($hasExtraPath -ne $hasExtraContent) {
        Fail-Test 'ZIP fixture extra path and content must be provided together'
    }
    if ($hasExtraPath -and [string]::IsNullOrWhiteSpace($ExtraPath)) {
        Fail-Test 'ZIP fixture extra path must not be empty'
    }

    $platform = 'windows-x64'
    $packageName = "clang-$Version-$platform-$platform"
    $cacheDir = Join-Path $Script:CupTestHome (
        ".cup\cache\compiler\clang\$platform\$platform\$Version")
    $archive = Join-Path $cacheDir "$packageName.zip"
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue

    $arguments = @($packageName, $Version, $platform, $platform, $archive)
    if ($hasExtraPath) {
        $arguments += @('extra-file', $ExtraPath, $ExtraContent)
    } else {
        $arguments += 'valid'
    }
    $result = Invoke-NativeProcess `
        -FilePath (Get-TestHelperPath -Name 'archive-fixture') `
        -Arguments $arguments `
        -WorkingDirectory $Script:CupTestRoot `
        -UseTestHelperCoverage
    if ($result.ExitCode -ne 0) {
        Fail-Test "archive fixture failed: $($result.Output)"
    }

    $hash = Get-Sha256Lower -Path $archive
    # MSYS2 GNU sha256sum marks archive records as binary with '*'.
    Write-Utf8NoBom -Path (Join-Path $cacheDir 'SHA256SUMS') -Lines @(
        "$hash *$(Split-Path -Leaf $archive)")
    return [pscustomobject]@{
        PackageName = $packageName
        Archive = $archive
    }
}

function Resolve-CommandProcessor {
    if (-not [string]::IsNullOrWhiteSpace($env:ComSpec) -and
        (Test-Path -LiteralPath $env:ComSpec -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $env:ComSpec).Path
    }

    $systemDirectory = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::System)
    if (-not [string]::IsNullOrWhiteSpace($systemDirectory)) {
        $candidate = Join-Path $systemDirectory "cmd.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $command = Get-Command cmd.exe -CommandType Application -ErrorAction SilentlyContinue
    if ($null -ne $command -and
        -not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    Fail-Test "could not locate cmd.exe"
}

function Get-CommandProcessor {
    if ($null -eq $Script:CupTestCommandProcessor) {
        $Script:CupTestCommandProcessor = Resolve-CommandProcessor
    }
    return $Script:CupTestCommandProcessor
}

function ConvertTo-NativeArgument {
    param(
        [AllowEmptyString()]
        [string]$Argument
    )

    if ($Argument.Length -eq 0) {
        return '""'
    }
    if ($Argument -notmatch '[\s"]') {
        return $Argument
    }

    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0

    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
            continue
        }

        if ($character -eq '"') {
            [void]$builder.Append([char]'\', ($backslashes * 2 + 1))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }

        if ($backslashes -gt 0) {
            [void]$builder.Append([char]'\', $backslashes)
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }

    if ($backslashes -gt 0) {
        [void]$builder.Append([char]'\', ($backslashes * 2))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Invoke-NativeProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [switch]$UseTestHelperCoverage,

        [ValidateRange(1, 86400)]
        [int]$TimeoutSeconds = 300
    )

    if ([string]::IsNullOrWhiteSpace($FilePath)) {
        Fail-Test "native process path is empty"
    }
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        Fail-Test "native process does not exist: $FilePath"
    }
    if ([string]::IsNullOrWhiteSpace($WorkingDirectory) -or
        -not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
        Fail-Test "invalid native process working directory: $WorkingDirectory"
    }

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($Arguments | ForEach-Object {
        ConvertTo-NativeArgument -Argument $_
    }) -join ' ')
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($UseTestHelperCoverage -and
        -not [string]::IsNullOrWhiteSpace($env:CUP_TEST_GCOV_HELPER_PREFIX)) {
        if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_GCOV_HELPER_STRIP)) {
            Fail-Test 'helper GCOV prefix is present without a strip count'
        }
        $startInfo.EnvironmentVariables['GCOV_PREFIX'] =
            $env:CUP_TEST_GCOV_HELPER_PREFIX
        $startInfo.EnvironmentVariables['GCOV_PREFIX_STRIP'] =
            $env:CUP_TEST_GCOV_HELPER_STRIP
    }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) {
            Fail-Test "failed to start native process: $FilePath"
        }

        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-TestProcessTree -Process $process
            Fail-Test "native process timed out after $TimeoutSeconds seconds: $FilePath"
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.Result.TrimEnd([char[]]"`r`n")
        $stderr = $stderrTask.Result.TrimEnd([char[]]"`r`n")

        $parts = [System.Collections.Generic.List[string]]::new()
        if ($stdout.Length -gt 0) {
            $parts.Add($stdout)
        }
        if ($stderr.Length -gt 0) {
            $parts.Add($stderr)
        }

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = ($parts -join "`n")
        }
    } finally {
        $process.Dispose()
    }
}

function Start-TestHelperProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string]$RedirectStandardOutput,

        [Parameter(Mandatory = $true)]
        [string]$RedirectStandardError,

        [switch]$Hidden
    )

    $savedPrefix = Get-Item -LiteralPath Env:GCOV_PREFIX -ErrorAction SilentlyContinue
    $savedStrip = Get-Item -LiteralPath Env:GCOV_PREFIX_STRIP -ErrorAction SilentlyContinue
    try {
        if (-not [string]::IsNullOrWhiteSpace($env:CUP_TEST_GCOV_HELPER_PREFIX)) {
            if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_GCOV_HELPER_STRIP)) {
                Fail-Test 'helper GCOV prefix is present without a strip count'
            }
            $env:GCOV_PREFIX = $env:CUP_TEST_GCOV_HELPER_PREFIX
            $env:GCOV_PREFIX_STRIP = $env:CUP_TEST_GCOV_HELPER_STRIP
        }
        $nativeArguments = (($ArgumentList | ForEach-Object {
            ConvertTo-NativeArgument -Argument $_
        }) -join ' ')
        $parameters = @{
            FilePath = $FilePath
            ArgumentList = $nativeArguments
            RedirectStandardOutput = $RedirectStandardOutput
            RedirectStandardError = $RedirectStandardError
            PassThru = $true
        }
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            $parameters.WorkingDirectory = $WorkingDirectory
        }
        if ($Hidden) {
            $parameters.WindowStyle = 'Hidden'
        } else {
            $parameters.NoNewWindow = $true
        }
        return Start-Process @parameters
    } finally {
        if ($null -eq $savedPrefix) {
            Remove-Item -LiteralPath Env:GCOV_PREFIX -ErrorAction SilentlyContinue
        } else {
            $env:GCOV_PREFIX = $savedPrefix.Value
        }
        if ($null -eq $savedStrip) {
            Remove-Item -LiteralPath Env:GCOV_PREFIX_STRIP -ErrorAction SilentlyContinue
        } else {
            $env:GCOV_PREFIX_STRIP = $savedStrip.Value
        }
    }
}

# Isolated runtime setup and teardown.
# CUP persistent fixture text is canonical LF regardless of the Windows host newline.
function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [AllowEmptyCollection()]
        [string[]]$Lines
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    $text = if ($Lines.Count -eq 0) { "" } else { ($Lines -join "`n") + "`n" }
    [System.IO.File]::WriteAllText($Path, $text, $encoding)
}

function Initialize-TestEnvironment {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $originalEnvironment = @{}
    foreach ($variable in @(
        'CUP_INSTALL_BASE_URL', 'CUP_INSTALL_ALLOW_INSECURE',
        'HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY')) {
        $item = Get-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
        $originalEnvironment[$variable] = if ($null -eq $item) {
            $null
        } else {
            $item.Value
        }
    }
    $Script:CupTestOriginalUserProfile = $env:USERPROFILE
    $Script:CupTestOriginalEnvironment = $originalEnvironment
    $Script:CupTestOriginalEnvironmentCaptured = $true

    if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
        Fail-Test "cup executable path is empty"
    }
    $Script:CupTestExecutable = (Resolve-Path -LiteralPath $ExecutablePath).Path
    $Script:CupTestRoot = New-IsolatedTestRoot -Name $Name
    $Script:CupTestHome = Join-Path $Script:CupTestRoot "home"
    $Script:CupTestDevRoot = Join-Path $Script:CupTestRoot "development-root"
    foreach ($variable in $Script:CupTestOriginalEnvironment.Keys) {
        Remove-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
    }

    New-Item -ItemType Directory -Force -Path $Script:CupTestHome | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Script:CupTestDevRoot "config") | Out-Null
    Copy-Item (Join-Path $Script:CupTestProjectRoot "config\packages.cfg") (
        Join-Path $Script:CupTestDevRoot "config\packages.cfg")
    Copy-Item (Join-Path $Script:CupTestProjectRoot "config\install.cfg") (
        Join-Path $Script:CupTestDevRoot "config\install.cfg")

    $env:USERPROFILE = $Script:CupTestHome
}

function Remove-TestEnvironment {
    if ($Script:CupTestOriginalEnvironmentCaptured) {
        foreach ($entry in $Script:CupTestOriginalEnvironment.GetEnumerator()) {
            if ($null -eq $entry.Value) {
                Remove-Item -LiteralPath "Env:$($entry.Key)" -ErrorAction SilentlyContinue
            } else {
                Set-Item -LiteralPath "Env:$($entry.Key)" -Value $entry.Value
            }
        }
        $Script:CupTestOriginalEnvironment = @{}

        if ($null -eq $Script:CupTestOriginalUserProfile) {
            Remove-Item Env:USERPROFILE -ErrorAction SilentlyContinue
        } else {
            $env:USERPROFILE = $Script:CupTestOriginalUserProfile
        }
        $Script:CupTestOriginalEnvironmentCaptured = $false
    }

    if ($null -ne $Script:CupTestRoot -and (Test-Path -LiteralPath $Script:CupTestRoot)) {
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while ($true) {
            try {
                Remove-Item -LiteralPath $Script:CupTestRoot -Recurse -Force `
                    -ErrorAction Stop
                break
            } catch {
                if ([DateTime]::UtcNow -ge $deadline) {
                    throw
                }
                Start-Sleep -Milliseconds 100
            }
        }
    }
}

function Assert-CupHealthy {
    $savedPath = $env:Path
    try {
        $env:Path = "$(Join-Path $Script:CupTestHome '.cup\bin');$savedPath"
        $output = Invoke-Cup -CommandArgs @("doctor")
    } finally {
        $env:Path = $savedPath
    }
    Assert-Contains $output "Doctor found no issues."
    Assert-NotContains $output "Error:"
    Assert-NotContains $output "Issue:"
    Assert-NotContains $output "Warning:"
    Assert-NotContains $output "Incomplete:"
}

function Invoke-Cup {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$CommandArgs,

        [switch]$ExpectFailure
    )

    $result = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments $CommandArgs -WorkingDirectory $Script:CupTestDevRoot

    if ($ExpectFailure) {
        if ($result.ExitCode -eq 0) {
            Fail-Test "command unexpectedly succeeded: cup $($CommandArgs -join ' ')"
        }
    } elseif ($result.ExitCode -ne 0) {
        $profile = $env:USERPROFILE
        if ($null -eq $profile) {
            $profile = "<unset>"
        }
        $message = "command failed: cup $($CommandArgs -join ' ') " +
            "[status $($result.ExitCode), USERPROFILE='$profile']`n$($result.Output)"
        Fail-Test $message
    }
    return $result.Output
}

function Assert-CupStatus {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$CommandArgs,

        [Parameter(Mandatory = $true)]
        [int]$ExpectedStatus,

        [string]$ExpectedText = ""
    )

    $result = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments $CommandArgs -WorkingDirectory $Script:CupTestDevRoot
    if ($result.ExitCode -ne $ExpectedStatus) {
        $commandText = $CommandArgs -join ' '
        $profile = $env:USERPROFILE
        if ($null -eq $profile) {
            $profile = "<unset>"
        }
        $message = "cup $commandText returned status $($result.ExitCode), " +
            "expected $ExpectedStatus [USERPROFILE='$profile']`n$($result.Output)"
        Fail-Test $message
    }
    if (-not [string]::IsNullOrEmpty($ExpectedText)) {
        Assert-Contains $result.Output $ExpectedText
    }
    return $result.Output
}

# Catalog and package fixtures used by command-level suites.
function Set-PackageCatalogField {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Component,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$Field,

        [Parameter(Mandatory = $true)]
        [string]$Value,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Prepend", "Replace")]
        [string]$Mode,

        [string]$HostPlatform = "windows-x64",

        [string]$TargetPlatform = "windows-x64"
    )

    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $key = "$Component.$Tool.$HostPlatform.$TargetPlatform.$Field="
    $content = Get-Content -LiteralPath $catalog
    $found = $false
    $updated = foreach ($line in $content) {
        if ($line.StartsWith($key, [System.StringComparison]::Ordinal)) {
            $found = $true
            if ($Mode -eq "Prepend") {
                $key + $Value + "," + $line.Substring($key.Length)
            } else {
                $key + $Value
            }
        } else {
            $line
        }
    }
    if (-not $found) {
        Fail-Test "catalog entry not found: $key"
    }
    Write-Utf8NoBom -Path $catalog -Lines $updated
}


function Get-TestPlatformTriple([string]$Platform) {
    switch ($Platform) {
        'linux-x64' { return 'x86_64-linux-gnu' }
        'linux-arm64' { return 'aarch64-linux-gnu' }
        'windows-x64' { return 'x86_64-w64-mingw32' }
        'macos-x64' { return 'x86_64-apple-darwin' }
        'macos-arm64' { return 'arm64-apple-darwin' }
        default { Fail-Test "unsupported fixture platform: $Platform" }
    }
}

function Get-TestPlatformFamily([string]$Platform) {
    if ($Platform.StartsWith('macos-', [StringComparison]::Ordinal)) { return 'darwin' }
    if ($Platform.StartsWith('linux-', [StringComparison]::Ordinal) -or
        $Platform.StartsWith('windows-', [StringComparison]::Ordinal)) { return 'gnu' }
    Fail-Test "unsupported fixture platform: $Platform"
}

function Get-TestPlatformRuntime([string]$Platform) {
    if ($Platform.StartsWith('linux-', [StringComparison]::Ordinal)) { return 'glibc' }
    if ($Platform.StartsWith('windows-', [StringComparison]::Ordinal)) { return 'ucrt' }
    if ($Platform.StartsWith('macos-', [StringComparison]::Ordinal)) { return 'libSystem' }
    Fail-Test "unsupported fixture platform: $Platform"
}

function Get-TestPrimarySourceName([string]$Tool) {
    switch ($Tool) {
        'gcc' { return 'gcc' }
        'gdb' { return 'gdb' }
        'ld' { return 'binutils' }
        { $_ -in @('clang', 'lld', 'lldb', 'clangd', 'clang-format', 'clang-tidy') } {
            return 'llvm-project'
        }
        'valgrind' { return 'valgrind' }
        default { Fail-Test "unsupported fixture tool: $Tool" }
    }
}

function Get-TestPrimarySourceVersion([string]$Tool, [string]$Version) {
    if ($Tool -eq 'gcc') {
        if ($Version -notmatch '^(.+)-rev[1-9][0-9]*$') {
            Fail-Test "invalid GCC fixture revision: $Version"
        }
        return $Matches[1]
    }
    return $Version
}

function Write-TestPackageManifest([string]$PackageRoot) {
    $root = [IO.Path]::GetFullPath($PackageRoot).TrimEnd([char[]]'\\/')
    [string[]]$relativePaths = @(
        Get-ChildItem -LiteralPath $PackageRoot -Recurse -Force | ForEach-Object {
            $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        } | Where-Object { $_ -cne 'manifest.txt' -and $_ -cne '.manifest.paths' }
    )
    [Array]::Sort($relativePaths, [StringComparer]::Ordinal)

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('format=2')
    foreach ($relative in $relativePaths) {
        $native = Join-Path $PackageRoot ($relative.Replace('/', '\'))
        $item = Get-Item -LiteralPath $native -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail-Test "Windows fixture package contains a reparse point: $relative"
        }
        if ($item.PSIsContainer) {
            $lines.Add("d`t0755`t-`t$relative")
            continue
        }

        $extension = [IO.Path]::GetExtension($item.Name).ToLowerInvariant()
        $executable = @('.exe', '.com', '.bat', '.cmd', '.dll', '.pyd') -contains $extension
        if (-not $executable) {
            $stream = [IO.File]::OpenRead($item.FullName)
            try {
                $first = $stream.ReadByte()
                $second = $stream.ReadByte()
                $executable = $first -eq [int][char]'#' -and $second -eq [int][char]'!'
            } finally {
                $stream.Dispose()
            }
        }
        $mode = if ($executable) { '0755' } else { '0644' }
        $digest = Get-Sha256Lower -Path $item.FullName
        $lines.Add("f`t$mode`t$digest`t$relative")
    }
    Write-Utf8NoBom -Path (Join-Path $PackageRoot 'manifest.txt') -Lines $lines
}

function New-TestPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Component,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [string[]]$Entries,

        [string]$HostPlatform = "windows-x64",

        [string]$TargetPlatform = "windows-x64"
    )

    $packageName = "$Tool-$Version-$HostPlatform-$TargetPlatform"
    $packageRoot = Join-Path $Script:CupTestRoot "packages\$packageName"
    $cacheDir = Join-Path $Script:CupTestHome (
        ".cup\cache\$Component\$Tool\$HostPlatform\$TargetPlatform\$Version")
    $archive = Join-Path $cacheDir "$packageName.zip"

    Remove-Item -LiteralPath $packageRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $packageRoot "bin") | Out-Null
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null

    $info = [System.Collections.Generic.List[string]]::new()
    $info.Add("package.component=$Component")
    $info.Add("package.tool=$Tool")
    $info.Add("package.version=$Version")
    if ($Tool -eq "gcc") {
        if ($Version -notmatch "-rev([1-9][0-9]*)$") {
            Fail-Test "invalid GCC fixture revision: $Version"
        }
        $info.Add("package.revision=$($Matches[1])")
    }
    $info.Add("package.mode=self-contained")
    $info.Add("package.formats=zip,tar.xz,tar.gz")
    $info.Add("platform.host=$HostPlatform")
    $info.Add("platform.target=$TargetPlatform")
    $info.Add("platform.host_triple=$(Get-TestPlatformTriple $HostPlatform)")
    $info.Add("platform.target_triple=$(Get-TestPlatformTriple $TargetPlatform)")
    $info.Add("platform.family=$(Get-TestPlatformFamily $TargetPlatform)")
    $info.Add("platform.runtime=$(Get-TestPlatformRuntime $TargetPlatform)")
    $info.Add("platform.thread_model=posix")
    $info.Add("build.environment=test")
    $info.Add("build.source_policy=fixture")
    $info.Add("source.primary.name=$(Get-TestPrimarySourceName $Tool)")
    $info.Add("source.primary.version=$(Get-TestPrimarySourceVersion $Tool $Version)")
    $info.Add("source.primary.url=https://example.invalid/$Tool-$Version.tar.xz")
    $info.Add("source.primary.sha256=$('0' * 64)")
    foreach ($entry in $Entries) {
        $info.Add("entry.$entry=bin/$entry.cmd")
        $body = "@echo off`r`necho $Tool-$Version-${TargetPlatform}:$entry`r`n"
        $entryPath = Join-Path $packageRoot "bin\$entry.cmd"
        Set-Content `
            -LiteralPath $entryPath `
            -Value $body `
            -Encoding ascii `
            -NoNewline
    }
    Write-Utf8NoBom -Path (Join-Path $packageRoot "info.txt") -Lines $info
    Write-TestPackageManifest -PackageRoot $packageRoot

    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive

    $hash = Get-Sha256Lower -Path $archive
    # MSYS2 GNU sha256sum marks archive records as binary with '*'.
    Write-Utf8NoBom -Path (Join-Path $cacheDir "SHA256SUMS") -Lines @(
        "$hash *$(Split-Path -Leaf $archive)"
    )
}

function New-InstalledPackageFixture {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Component,

        [Parameter(Mandatory = $true)]
        [string]$Tool,

        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [string[]]$Entries,

        [string]$HostPlatform = "windows-x64",

        [string]$TargetPlatform = "windows-x64"
    )

    $packageRoot = Join-Path $Script:CupTestHome (
        ".cup\components\$Component\$Tool\$HostPlatform\$TargetPlatform\$Version")
    $bin = Join-Path $packageRoot "bin"
    New-Item -ItemType Directory -Force -Path $bin | Out-Null

    $info = [System.Collections.Generic.List[string]]::new()
    $info.Add("package.component=$Component")
    $info.Add("package.tool=$Tool")
    $info.Add("package.version=$Version")
    if ($Tool -eq "gcc") {
        if ($Version -notmatch "-rev([1-9][0-9]*)$") {
            Fail-Test "invalid GCC fixture revision: $Version"
        }
        $info.Add("package.revision=$($Matches[1])")
    }
    $info.Add("package.mode=self-contained")
    $info.Add("package.formats=zip,tar.xz,tar.gz")
    $info.Add("platform.host=$HostPlatform")
    $info.Add("platform.target=$TargetPlatform")
    $info.Add("platform.host_triple=$(Get-TestPlatformTriple $HostPlatform)")
    $info.Add("platform.target_triple=$(Get-TestPlatformTriple $TargetPlatform)")
    $info.Add("platform.family=$(Get-TestPlatformFamily $TargetPlatform)")
    $info.Add("platform.runtime=$(Get-TestPlatformRuntime $TargetPlatform)")
    $info.Add("platform.thread_model=posix")
    $info.Add("build.environment=test")
    $info.Add("build.source_policy=fixture")
    $info.Add("source.primary.name=$(Get-TestPrimarySourceName $Tool)")
    $info.Add("source.primary.version=$(Get-TestPrimarySourceVersion $Tool $Version)")
    $info.Add("source.primary.url=https://example.invalid/$Tool-$Version.tar.xz")
    $info.Add("source.primary.sha256=$('0' * 64)")
    foreach ($entry in $Entries) {
        $info.Add("entry.$entry=bin/$entry.cmd")
        $body = "@echo off`r`necho $Tool-$Version-${TargetPlatform}:$entry`r`n"
        Set-Content -LiteralPath (Join-Path $bin "$entry.cmd") `
            -Value $body -Encoding ascii -NoNewline
    }

    $infoPath = Join-Path $packageRoot "info.txt"
    Write-Utf8NoBom -Path $infoPath -Lines $info
    Write-TestPackageManifest -PackageRoot $packageRoot
    return $packageRoot
}


# Execute one wrapper generated by cup and capture its output.
function Invoke-ManagedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $path = Join-Path $Script:CupTestHome ".cup\bin\$Name.cmd"
    Assert-PathExists $path
    $result = Invoke-NativeProcess -FilePath (Get-CommandProcessor) `
        -Arguments @('/d', '/c', 'call', $path) `
        -WorkingDirectory $Script:CupTestHome
    if ($result.ExitCode -ne 0) {
        Fail-Test "managed command failed: $Name`n$($result.Output)"
    }
    return $result.Output
}
