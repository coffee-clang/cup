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
$Script:CupTestCommandProcessor = $null

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
function Resolve-TestBuildRoot {
    $candidate = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_BUILD_ROOT)) {
        Join-Path $Script:CupTestProjectRoot "build"
    } else {
        $env:CUP_TEST_BUILD_ROOT
    }
    $fullPath = [IO.Path]::GetFullPath($candidate)
    $pathRoot = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot) -or $fullPath -ceq $pathRoot) {
        Fail-Test "unsafe test build root: $fullPath"
    }

    $current = $pathRoot
    $relative = $fullPath.Substring($pathRoot.Length)
    $separators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $components = $relative.Split(
        $separators,
        [StringSplitOptions]::RemoveEmptyEntries)
    foreach ($component in $components) {
        if ($component -in @(".", "..")) {
            Fail-Test "unsafe test build root component: $fullPath"
        }
        $current = Join-Path $current $component
        if (-not (Test-Path -LiteralPath $current -PathType Container)) {
            Fail-Test "test build root is not a real directory: $current"
        }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail-Test "test build root contains a reparse point: $current"
        }
    }

    $marker = Join-Path $fullPath ".cup-build-root"
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        Fail-Test "test build root marker is missing: $marker"
    }
    $markerItem = Get-Item -LiteralPath $marker -Force
    if (($markerItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail-Test "test build root marker is a reparse point: $marker"
    }
    $expected = @(
        "format=1",
        "product=coffee-clang/cup",
        "kind=build-root",
        "layout=1"
    )
    $actual = @(Get-Content -LiteralPath $marker)
    if ($actual.Count -ne $expected.Count) {
        Fail-Test "test build root marker is invalid: $marker"
    }
    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($actual[$index] -cne $expected[$index]) {
            Fail-Test "test build root marker is invalid: $marker"
        }
    }
    return $fullPath
}

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

    $configuration = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_CONFIGURATION)) {
        "development"
    } else {
        $env:CUP_TEST_CONFIGURATION
    }
    if ($configuration -notin @("development", "debug", "coverage", "sanitizers", "release")) {
        Fail-Test "unsupported CUP_TEST_CONFIGURATION: $configuration"
    }
    if ($null -eq $Script:CupTestBuildRoot) {
        $Script:CupTestBuildRoot = Resolve-TestBuildRoot
    }
    $platformRoot = New-RealTestDirectory `
        -Parent $Script:CupTestBuildRoot -Name "windows-x64"
    $configurationRoot = New-RealTestDirectory `
        -Parent $platformRoot -Name $configuration
    $base = New-RealTestDirectory -Parent $configurationRoot -Name "tests"
    $rootName = "cup-$Name-tests-" + [guid]::NewGuid().ToString("N")
    return New-RealTestDirectory -Parent $base -Name $rootName
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

function Stop-TestProcessTree {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,

        [ValidateRange(1, 60000)]
        [int]$WaitMilliseconds = 10000
    )

    if ($Process.HasExited) {
        return
    }

    $treeStopped = $false
    try {
        & taskkill.exe /PID $Process.Id /T /F 2>&1 | Out-Null
        $treeStopped = ($LASTEXITCODE -eq 0)
    } catch {
        $treeStopped = $false
    }

    if (-not $treeStopped -and -not $Process.HasExited) {
        try {
            $Process.Kill()
        } catch {
            # Cleanup is best effort.
        }
    }
    if (-not $Process.WaitForExit($WaitMilliseconds) -and -not $Process.HasExited) {
        try {
            $Process.Kill()
        } catch {
            # Cleanup is best effort.
        }
        [void]$Process.WaitForExit($WaitMilliseconds)
    }
}

function Invoke-NativeProcess {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$Arguments = @(),

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

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

    if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
        Fail-Test "cup executable path is empty"
    }
    $Script:CupTestExecutable = (Resolve-Path -LiteralPath $ExecutablePath).Path
    $Script:CupTestRoot = New-IsolatedTestRoot -Name $Name
    $Script:CupTestHome = Join-Path $Script:CupTestRoot "home"
    $Script:CupTestDevRoot = Join-Path $Script:CupTestRoot "development-root"
    $Script:CupTestOriginalUserProfile = $env:USERPROFILE
    $Script:CupTestOriginalEnvironment = @{}
    foreach ($variable in @(
        'CUP_INSTALL_BASE_URL', 'CUP_INSTALL_ALLOW_INSECURE',
        'HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY')) {
        $item = Get-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
        $Script:CupTestOriginalEnvironment[$variable] = if ($null -eq $item) {
            $null
        } else {
            $item.Value
        }
        Remove-Item -LiteralPath "Env:$variable" -ErrorAction SilentlyContinue
    }

    New-Item -ItemType Directory -Force -Path $Script:CupTestHome | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Script:CupTestDevRoot "config") | Out-Null
    $installScripts = Join-Path $Script:CupTestDevRoot "scripts\install"
    New-Item -ItemType Directory -Force -Path $installScripts | Out-Null
    Copy-Item (Join-Path $Script:CupTestProjectRoot "config\packages.cfg") (
        Join-Path $Script:CupTestDevRoot "config\packages.cfg")
    Copy-Item (Join-Path $Script:CupTestProjectRoot "config\install.cfg") (
        Join-Path $Script:CupTestDevRoot "config\install.cfg")
    Copy-Item (Join-Path $Script:CupTestProjectRoot "scripts\install\uninstall-cup-windows.ps1") (
        Join-Path $Script:CupTestDevRoot "scripts\install\uninstall-cup-windows.ps1")

    $env:USERPROFILE = $Script:CupTestHome
}

function Remove-TestEnvironment {
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
    $output = Invoke-Cup -CommandArgs @("doctor")
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
        Fail-Test "command failed: cup $($CommandArgs -join ' ')`n$($result.Output)"
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
        $message = "cup $commandText returned status $($result.ExitCode), " +
            "expected $ExpectedStatus`n$($result.Output)"
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
    $info.Add("platform.host=$HostPlatform")
    $info.Add("platform.target=$TargetPlatform")
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

    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive

    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path $cacheDir "SHA256SUMS") -Lines @(
        "$hash  $(Split-Path -Leaf $archive)"
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

        [string]$TargetPlatform = "windows-x64",

        [switch]$ProtectMetadata
    )

    $packageRoot = Join-Path $Script:CupTestHome (
        ".cup\components\$Component\$Tool\$HostPlatform\$TargetPlatform\$Version")
    $bin = Join-Path $packageRoot "bin"
    New-Item -ItemType Directory -Force -Path $bin | Out-Null

    $info = [System.Collections.Generic.List[string]]::new()
    $info.Add("package.component=$Component")
    $info.Add("package.tool=$Tool")
    $info.Add("package.version=$Version")
    $info.Add("platform.host=$HostPlatform")
    $info.Add("platform.target=$TargetPlatform")
    foreach ($entry in $Entries) {
        $info.Add("entry.$entry=bin/$entry.cmd")
        $body = "@echo off`r`necho $Tool-$Version-${TargetPlatform}:$entry`r`n"
        Set-Content -LiteralPath (Join-Path $bin "$entry.cmd") `
            -Value $body -Encoding ascii -NoNewline
    }

    $infoPath = Join-Path $packageRoot "info.txt"
    Write-Utf8NoBom -Path $infoPath -Lines $info
    if ($ProtectMetadata) {
        (Get-Item -LiteralPath $infoPath).IsReadOnly = $true
    }
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
