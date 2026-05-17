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
$CupScriptsDir = Join-Path $CupHome "scripts"

$CupExe = Join-Path $CupBinDir "cup.exe"
$PackagesCfg = Join-Path $CupConfigDir "packages.cfg"
$UninstallScript = Join-Path $CupScriptsDir "uninstall.ps1"

$CupAsset = "cup-windows-x64.exe"
$PackagesAsset = "packages.cfg"
$UninstallAsset = "uninstall.ps1"

$CupAvailableInPath = $false

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

function Test-CupBinInUserPath {
    $currentUserPath = [Environment]::GetEnvironmentVariable("Path", "User")

    if ($null -eq $currentUserPath) {
        return $false
    }

    $pathEntries = $currentUserPath -split ";"

    foreach ($entry in $pathEntries) {
        if ($entry.TrimEnd("\") -ieq $CupBinDir.TrimEnd("\")) {
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

    $currentUserPath = [Environment]::GetEnvironmentVariable("Path", "User")

    if ($null -eq $currentUserPath) {
        $currentUserPath = ""
    }

    $answer = Read-Host "Add $CupBinDir to your user PATH? [y/N]"

    if ($answer -eq "y" -or $answer -eq "Y" -or $answer -eq "yes" -or $answer -eq "YES") {
        if ([string]::IsNullOrWhiteSpace($currentUserPath)) {
            $newPath = $CupBinDir
        } else {
            $newPath = "$currentUserPath;$CupBinDir"
        }

        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")

        $script:CupAvailableInPath = $true

        Write-Info "User PATH updated."
        Write-Info "Open a new terminal to use cup from PATH."
    } else {
        $script:CupAvailableInPath = $false

        Write-Info "PATH not modified."
        Write-Info "To use cup without the full path, add this directory to your user PATH:"
        Write-Info "  $CupBinDir"
    }
}

function Write-InstallTestHint {
    Write-Info ""
    Write-Info "You can test the installation with:"

    if ($CupAvailableInPath) {
        Write-Info "  cup help"
        Write-Info ""
        Write-Info "If 'cup' is not found yet, open a new terminal."
    } else {
        Write-Info "  & `"$CupExe`" help"
    }
}

function Main {
    Test-WindowsX64

    Write-Info "Installing cup into $CupHome"

    New-Item -ItemType Directory -Force -Path $CupBinDir | Out-Null
    New-Item -ItemType Directory -Force -Path $CupConfigDir | Out-Null
    New-Item -ItemType Directory -Force -Path $CupScriptsDir | Out-Null

    Write-Info "Downloading cup binary..."
    Download-File "$BaseUrl/$CupAsset" $CupExe

    Write-Info "Downloading package manifest..."
    Download-File "$BaseUrl/$PackagesAsset" $PackagesCfg

    Write-Info "Downloading uninstall script..."
    Download-File "$BaseUrl/$UninstallAsset" $UninstallScript

    Write-Info ""
    Write-Info "cup installed successfully."
    Write-Info "Binary:    $CupExe"
    Write-Info "Manifest:  $PackagesCfg"
    Write-Info "Uninstall: $UninstallScript"
    Write-Info ""

    Add-CupToUserPath
    Write-InstallTestHint
}

Main