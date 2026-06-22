Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Script:TestScriptDir = $PSScriptRoot
$Script:ProjectRoot = (Resolve-Path (Join-Path $Script:TestScriptDir "..\..\..")).Path

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


function Write-Utf8NoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Lines
    )
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($Path, $Lines, $encoding)
}

function Initialize-TestEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$CupPath
    )

    $Script:CupPath = (Resolve-Path $CupPath).Path
    $Script:TestRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
        "cup-$Name-tests-" + [guid]::NewGuid().ToString("N"))
    $Script:TestHome = Join-Path $Script:TestRoot "home"
    $Script:DevRoot = Join-Path $Script:TestRoot "development-root"

    New-Item -ItemType Directory -Force -Path $Script:TestHome | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Script:DevRoot "config") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Script:DevRoot "scripts\install") | Out-Null
    Copy-Item (Join-Path $Script:ProjectRoot "config\packages.cfg") (
        Join-Path $Script:DevRoot "config\packages.cfg")
    Copy-Item (Join-Path $Script:ProjectRoot "scripts\install\uninstall-cup-windows.ps1") (
        Join-Path $Script:DevRoot "scripts\install\uninstall-cup-windows.ps1")

    $Script:OriginalUserProfile = $env:USERPROFILE
    $env:USERPROFILE = $Script:TestHome
}

function Remove-TestEnvironment {
    if ($null -ne $Script:OriginalUserProfile) {
        $env:USERPROFILE = $Script:OriginalUserProfile
    }
    if ($null -ne $Script:TestRoot -and (Test-Path -LiteralPath $Script:TestRoot)) {
        Remove-Item -LiteralPath $Script:TestRoot -Recurse -Force
    }
}

function Invoke-Cup {
    param(
        [Parameter(Mandatory = $true)][string[]]$CommandArgs,
        [switch]$ExpectFailure
    )

    Push-Location $Script:DevRoot
    try {
        $lines = & $Script:CupPath @CommandArgs 2>&1 | ForEach-Object { $_.ToString() }
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    $text = ($lines -join "`n").TrimEnd()
    if ($ExpectFailure) {
        if ($exitCode -eq 0) {
            Fail-Test "command unexpectedly succeeded: cup $($CommandArgs -join ' ')"
        }
    } elseif ($exitCode -ne 0) {
        Fail-Test "command failed: cup $($CommandArgs -join ' ')`n$text"
    }
    return $text
}

function Add-ManifestVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Component,
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $manifest = Join-Path $Script:DevRoot "config\packages.cfg"
    $key = "$Component.$Tool.windows-x64.windows-x64.available_versions="
    $content = Get-Content -LiteralPath $manifest
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
        Fail-Test "manifest entry not found: $key"
    }
    Write-Utf8NoBom -Path $manifest -Lines $updated
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
    $packageRoot = Join-Path $Script:TestRoot "packages\$packageName"
    $cacheDir = Join-Path $Script:TestHome ".cup\cache\$Component\$Tool\$platform\$platform\$Version"
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
}

function Invoke-ManagedCommand {
    param([Parameter(Mandatory = $true)][string]$Name)
    $path = Join-Path $Script:TestHome ".cup\bin\$Name.cmd"
    Assert-PathExists $path
    $lines = & cmd.exe /d /c $path 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) {
        Fail-Test "managed command failed: $Name"
    }
    return ($lines -join "`n").TrimEnd()
}
