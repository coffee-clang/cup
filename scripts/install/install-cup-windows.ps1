$ErrorActionPreference = "Stop"

$RepoOwner = "coffee-clang"
$RepoName = "cup"
$ReleaseTag = "cup-bootstrap"
$BaseUrl = "https://github.com/$RepoOwner/$RepoName/releases/download/$ReleaseTag"

$CupRoot = Join-Path $env:USERPROFILE ".cup"
$CupBinDir = Join-Path $CupRoot "bin"
$CupConfigDir = Join-Path $CupRoot "config"
$CupScriptsDir = Join-Path $CupRoot "scripts"
$CupExe = Join-Path $CupBinDir "cup.exe"
$PackagesCfg = Join-Path $CupConfigDir "packages.cfg"
$UninstallScript = Join-Path $CupScriptsDir "uninstall.ps1"
$Platform = "windows-x64"
$CupAsset = "cup-windows-x64.exe"
$CupAvailableInPath = $false

function Fail([string]$Message) {
    throw "Error: $Message"
}

function Write-Info([string]$Message) {
    Write-Host $Message
}

function Test-WindowsX64 {
    if ($PSVersionTable.PSEdition -eq "Core" -and -not $IsWindows) {
        Fail "this installer supports Windows only"
    }

    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        Fail "the Windows user profile could not be determined"
    }

    $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
    if ($arch.ToString() -ne "X64") {
        Fail "unsupported architecture: $arch. This installer supports x64 only"
    }
}

function Download-File([string]$Url, [string]$Output) {
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Output -UseBasicParsing
    } catch {
        Fail "failed to download $Url"
    }
}

function Assert-Checksums([string]$Directory, [string]$ChecksumFile) {
    foreach ($line in Get-Content -LiteralPath $ChecksumFile) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            Fail "invalid checksum file"
        }

        $expected = $Matches[1].ToLowerInvariant()
        $name = $Matches[2].Trim()
        $path = Join-Path $Directory $name

        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Fail "checksum asset is missing: $name"
        }

        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            Fail "checksum verification failed for $name"
        }
    }
}

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

function Clear-ReadOnly([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        Set-ItemProperty -LiteralPath $Path -Name IsReadOnly -Value $false
    }
}

function Set-BootstrapPermissions {
    Set-ItemProperty -LiteralPath $PackagesCfg -Name IsReadOnly -Value $true
    Set-ItemProperty -LiteralPath $UninstallScript -Name IsReadOnly -Value $true
}

function Main {
    Test-WindowsX64

    foreach ($directory in @($CupRoot, $CupBinDir, $CupConfigDir, $CupScriptsDir)) {
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
    }

    $staging = Join-Path $CupRoot (".bootstrap-" + [Guid]::NewGuid().ToString("N"))
    $backup = Join-Path $staging "backup"
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    $committed = $false
    $backedUp = @{}
    $installedDestinations = [System.Collections.Generic.List[string]]::new()

    try {
        Write-Info "Installing cup into $CupRoot"
        Download-File "$BaseUrl/$CupAsset" (Join-Path $staging $CupAsset)
        Download-File "$BaseUrl/packages.cfg" (Join-Path $staging "packages.cfg")
        Download-File "$BaseUrl/uninstall.ps1" (Join-Path $staging "uninstall.ps1")
        Download-File "$BaseUrl/SHA256SUMS.$Platform" (Join-Path $staging "SHA256SUMS.$Platform")
        Download-File "$BaseUrl/SHA256SUMS.common" (Join-Path $staging "SHA256SUMS.common")
        Assert-Checksums $staging (Join-Path $staging "SHA256SUMS.$Platform")
        Assert-Checksums $staging (Join-Path $staging "SHA256SUMS.common")

        $assets = @(
            @{
                Source = Join-Path $staging $CupAsset
                Destination = $CupExe
                Backup = Join-Path $backup "cup.exe"
            },
            @{
                Source = Join-Path $staging "packages.cfg"
                Destination = $PackagesCfg
                Backup = Join-Path $backup "packages.cfg"
            },
            @{
                Source = Join-Path $staging "uninstall.ps1"
                Destination = $UninstallScript
                Backup = Join-Path $backup "uninstall.ps1"
            }
        )

        Clear-ReadOnly $PackagesCfg
        Clear-ReadOnly $UninstallScript

        foreach ($asset in $assets) {
            if (Test-Path -LiteralPath $asset.Destination) {
                Move-Item -LiteralPath $asset.Destination -Destination $asset.Backup -Force
                $backedUp[$asset.Destination] = $asset.Backup
            }

            Move-Item -LiteralPath $asset.Source -Destination $asset.Destination -Force
            $installedDestinations.Add($asset.Destination)
        }

        Set-BootstrapPermissions
        $committed = $true
    } finally {
        if (-not $committed) {
            Clear-ReadOnly $PackagesCfg
            Clear-ReadOnly $UninstallScript

            foreach ($destination in $installedDestinations) {
                if (Test-Path -LiteralPath $destination) {
                    Remove-Item -LiteralPath $destination -Force
                }
            }

            foreach ($destination in $backedUp.Keys) {
                Move-Item -LiteralPath $backedUp[$destination] -Destination $destination -Force
            }

            if ((Test-Path -LiteralPath $PackagesCfg -PathType Leaf) -and
                (Test-Path -LiteralPath $UninstallScript -PathType Leaf)) {
                Set-BootstrapPermissions
            } else {
                if (Test-Path -LiteralPath $PackagesCfg -PathType Leaf) {
                    Set-ItemProperty -LiteralPath $PackagesCfg -Name IsReadOnly -Value $true
                }
                if (Test-Path -LiteralPath $UninstallScript -PathType Leaf) {
                    Set-ItemProperty -LiteralPath $UninstallScript -Name IsReadOnly -Value $true
                }
            }
        }

        if (Test-Path -LiteralPath $staging) {
            Remove-Item -LiteralPath $staging -Recurse -Force
        }
    }

    Write-Info "cup installed successfully."
    Write-Info "Binary:    $CupExe"
    Write-Info "Manifest:  $PackagesCfg"
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
    Write-Error $_
    exit 1
}
