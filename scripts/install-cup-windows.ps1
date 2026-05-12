$ErrorActionPreference = "Stop"

$RepoOwner = "coffee-clang"
$RepoName = "cup"
$ReleaseTag = "cup-bootstrap"

$BaseUrl = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag"

$CupHome = if ($env:CUP_HOME) {
    $env:CUP_HOME
} else {
    Join-Path $env:USERPROFILE ".cup"
}

$CupBinDir = Join-Path $CupHome "bin"
$CupConfigDir = Join-Path $CupHome "config"

$CupExe = Join-Path $CupBinDir "cup.exe"
$PackagesCfg = Join-Path $CupConfigDir "packages.cfg"

$CupAsset = "cup-windows-x64.exe"
$PackagesAsset = "packages.cfg"

function Write-Info {
    param([string]$Message)
    Write-Host $Message
}

function Fail {
    param([string]$Message)
    Write-Error "Error: $Message"
    exit 1
}

function Test-WindowsX64 {
    if ($PSVersionTable.PSEdition -eq "Core") {
        if (-not $IsWindows) {
            Fail "this installer currently supports Windows only."
        }
    }

    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture

    if ($arch.ToString() -ne "X64") {
        Fail "unsupported architecture: $arch. This installer currently supports x64 only."
    }
}

function Download-File {
    param(
        [string]$Url,
        [string]$Output
    )

    $tmp = "$Output.tmp"

    if (Test-Path $tmp) {
        Remove-Item -Force $tmp
    }

    try {
        Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
        Move-Item -Force $tmp $Output
    } catch {
        if (Test-Path $tmp) {
            Remove-Item -Force $tmp
        }

        Fail "failed to download $Url"
    }
}

function Add-CupToUserPath {
    $currentUserPath = [Environment]::GetEnvironmentVariable("Path", "User")

    if ($null -eq $currentUserPath) {
        $currentUserPath = ""
    }

    $pathEntries = $currentUserPath -split ";"

    foreach ($entry in $pathEntries) {
        if ($entry.TrimEnd("\") -ieq $CupBinDir.TrimEnd("\")) {
            Write-Info "cup bin directory is already in the user PATH."
            return
        }
    }

    $answer = Read-Host "Add $CupBinDir to your user PATH? [y/N]"

    if ($answer -eq "y" -or $answer -eq "Y" -or $answer -eq "yes" -or $answer -eq "YES") {
        if ([string]::IsNullOrWhiteSpace($currentUserPath)) {
            $newPath = $CupBinDir
        } else {
            $newPath = "$currentUserPath;$CupBinDir"
        }

        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")

        Write-Info "User PATH updated."
        Write-Info "Open a new terminal to use cup from PATH."
    } else {
        Write-Info "PATH not modified."
        Write-Info "To use cup without the full path, add this directory to your user PATH:"
        Write-Info "  $CupBinDir"
    }
}

function Main {
    Test-WindowsX64

    Write-Info "Installing cup into $CupHome"

    New-Item -ItemType Directory -Force -Path $CupBinDir | Out-Null
    New-Item -ItemType Directory -Force -Path $CupConfigDir | Out-Null

    Write-Info "Downloading cup binary..."
    Download-File "$BaseUrl/$CupAsset" $CupExe

    Write-Info "Downloading package manifest..."
    Download-File "$BaseUrl/$PackagesAsset" $PackagesCfg

    Write-Info ""
    Write-Info "cup installed successfully."
    Write-Info "Binary:   $CupExe"
    Write-Info "Manifest: $PackagesCfg"
    Write-Info ""

    Add-CupToUserPath

    Write-Info ""
    Write-Info "You can test the installation with:"
    Write-Info "  `"$CupExe`" --help"
}

Main