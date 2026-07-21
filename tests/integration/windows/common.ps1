# Purpose: Provides shared Windows integration assertions, isolated roots,
# and native fixture builders.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Script:CupTestScriptDir = $PSScriptRoot
$Script:CupTestProjectRoot = (Resolve-Path (Join-Path $Script:CupTestScriptDir "..\..\..")).Path
$Script:CupTestExecutable = $null
$Script:CupTestRoot = $null
$Script:CupTestHome = $null
$Script:CupTestDevRoot = $null
$Script:CupTestOriginalUserProfile = $null
$Script:CupTestCommandProcessor = $null

function Fail-Test {
    param([Parameter(Mandatory = $true)][string]$Message)
    throw "TEST FAILED: $Message"
}

function Assert-Contains {
    param(
        [AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)][string]$Expected
    )
    if (-not $Text.Contains($Expected)) {
        Fail-Test "expected output to contain: $Expected`nActual output:`n$Text"
    }
}

function Assert-NotContains {
    param(
        [AllowEmptyString()][string]$Text,
        [Parameter(Mandatory = $true)][string]$Unexpected
    )
    if ($Text.Contains($Unexpected)) {
        Fail-Test "expected output not to contain: $Unexpected`nActual output:`n$Text"
    }
}

function Assert-PathExists {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        Fail-Test "expected path: $Path"
    }
}

function Assert-PathMissing {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Fail-Test "expected missing path: $Path"
    }
}

function Assert-Equals {
    param(
        [AllowEmptyString()][string]$Actual,
        [AllowEmptyString()][string]$Expected
    )
    if ($Actual -cne $Expected) {
        Fail-Test "expected '$Expected', got '$Actual'"
    }
}


function New-IsolatedTestRoot {
    param([Parameter(Mandatory = $true)][string]$Name)

    $base = Join-Path $Script:CupTestProjectRoot "build\windows-x64\dynamic\tests"
    New-Item -ItemType Directory -Force -Path $base | Out-Null

    $root = Join-Path $base ("cup-$Name-tests-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    return (Resolve-Path -LiteralPath $root).Path
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
    param([AllowEmptyString()][string]$Argument)

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
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)][string]$WorkingDirectory
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
        $process.WaitForExit()
        $stdout = $stdoutTask.Result.TrimEnd([char[]]"`r`n")
        $stderr = $stderrTask.Result.TrimEnd([char[]]"`r`n")

        $parts = [System.Collections.Generic.List[string]]::new()
        if ($stdout.Length -gt 0) { $parts.Add($stdout) }
        if ($stderr.Length -gt 0) { $parts.Add($stderr) }

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = ($parts -join "`n")
        }
    } finally {
        $process.Dispose()
    }
}

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
    [System.IO.File]::WriteAllLines($Path, $Lines, $encoding)
}

function Initialize-TestEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$ExecutablePath
    )

    if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
        Fail-Test "cup executable path is empty"
    }
    $Script:CupTestExecutable = (Resolve-Path -LiteralPath $ExecutablePath).Path
    $Script:CupTestRoot = New-IsolatedTestRoot -Name $Name
    $Script:CupTestHome = Join-Path $Script:CupTestRoot "home"
    $Script:CupTestDevRoot = Join-Path $Script:CupTestRoot "development-root"
    $Script:CupTestOriginalUserProfile = $env:USERPROFILE

    New-Item -ItemType Directory -Force -Path $Script:CupTestHome | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Script:CupTestDevRoot "config") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Script:CupTestDevRoot "scripts\install") | Out-Null
    Copy-Item (Join-Path $Script:CupTestProjectRoot "config\packages.cfg") (
        Join-Path $Script:CupTestDevRoot "config\packages.cfg")
    Copy-Item (Join-Path $Script:CupTestProjectRoot "config\install.cfg") (
        Join-Path $Script:CupTestDevRoot "config\install.cfg")
    Copy-Item (Join-Path $Script:CupTestProjectRoot "scripts\install\uninstall-cup-windows.ps1") (
        Join-Path $Script:CupTestDevRoot "scripts\install\uninstall-cup-windows.ps1")

    $env:USERPROFILE = $Script:CupTestHome
}

function Remove-TestEnvironment {
    if ($null -eq $Script:CupTestOriginalUserProfile) {
        Remove-Item Env:USERPROFILE -ErrorAction SilentlyContinue
    } else {
        $env:USERPROFILE = $Script:CupTestOriginalUserProfile
    }

    if ($null -ne $Script:CupTestRoot -and (Test-Path -LiteralPath $Script:CupTestRoot)) {
        Remove-Item -LiteralPath $Script:CupTestRoot -Recurse -Force
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
        [Parameter(Mandatory = $true)][string[]]$CommandArgs,
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
        [Parameter(Mandatory = $true)][string[]]$CommandArgs,
        [Parameter(Mandatory = $true)][int]$ExpectedStatus,
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

function Add-ManifestVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Component,
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $key = "$Component.$Tool.windows-x64.windows-x64.available_versions="
    $content = Get-Content -LiteralPath $catalog
    $found = $false
    $updated = foreach ($line in $content) {
        if ($line.StartsWith($key, [System.StringComparison]::Ordinal)) {
            $found = $true
            $key + $Version + "," + $line.Substring($key.Length)
        } else {
            $line
        }
    }
    if (-not $found) {
        Fail-Test "catalog entry not found: $key"
    }
    Write-Utf8NoBom -Path $catalog -Lines $updated
}


function Set-ManifestFormat {
    param(
        [Parameter(Mandatory = $true)][string]$Component,
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string]$Format
    )

    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $key = "$Component.$Tool.windows-x64.windows-x64.default_format="
    $content = Get-Content -LiteralPath $catalog
    $found = $false
    $updated = foreach ($line in $content) {
        if ($line.StartsWith($key, [System.StringComparison]::Ordinal)) {
            $found = $true
            $key + $Format
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
        [Parameter(Mandatory = $true)][string]$Component,
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string[]]$Entries
    )

    $platform = "windows-x64"
    $packageName = "$Tool-$Version-$platform-$platform"
    $packageRoot = Join-Path $Script:CupTestRoot "packages\$packageName"
    $cacheDir = Join-Path $Script:CupTestHome ".cup\cache\$Component\$Tool\$platform\$platform\$Version"
    $archive = Join-Path $cacheDir "$packageName.zip"

    Remove-Item -LiteralPath $packageRoot -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $packageRoot "bin") | Out-Null
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null

    $info = [System.Collections.Generic.List[string]]::new()
    $info.Add("package.component=$Component")
    $info.Add("package.tool=$Tool")
    $info.Add("package.version=$Version")
    $info.Add("platform.host=$platform")
    $info.Add("platform.target=$platform")
    foreach ($entry in $Entries) {
        $info.Add("entry.$entry=bin/$entry.cmd")
        $body = "@echo off`r`necho $Tool-$Version-${platform}:$entry`r`n"
        Set-Content -LiteralPath (Join-Path $packageRoot "bin\$entry.cmd") -Value $body -Encoding ascii -NoNewline
    }
    Write-Utf8NoBom -Path (Join-Path $packageRoot "info.txt") -Lines $info

    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive

    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path $cacheDir "SHA256SUMS") -Lines @(
        "$hash  $(Split-Path -Leaf $archive)"
    )
}

function Invoke-ManagedCommand {
    param([Parameter(Mandatory = $true)][string]$Name)

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
