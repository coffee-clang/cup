$ErrorActionPreference = "Stop"

$RepoOwner = "coffee-clang"
$RepoName = "cup"
$BaseUrl = "https://github.com/$RepoOwner/$RepoName/releases/latest/download"

$CupRoot = Join-Path $env:USERPROFILE ".cup"
$CupBinDir = Join-Path $CupRoot "bin"
$CupConfigDir = Join-Path $CupRoot "config"
$CupScriptsDir = Join-Path $CupRoot "scripts"
$CupExe = Join-Path $CupBinDir "cup.exe"
$PackagesCfg = Join-Path $CupConfigDir "packages.cfg"
$CommonChecksums = Join-Path $CupConfigDir "SHA256SUMS.common"
$Platform = "windows-x64"
$PlatformChecksums = Join-Path $CupConfigDir "SHA256SUMS.$Platform"
$UninstallScript = Join-Path $CupScriptsDir "uninstall.ps1"
$CupAsset = "cup-windows-x64.exe"
$Staging = Join-Path $CupRoot ".bootstrap"
$UninstallMarker = Join-Path $CupRoot "uninstall.pending"
$CupAvailableInPath = $false

function Fail([string]$Message) { throw "Error: $Message" }
function Write-Info([string]$Message) { Write-Host $Message }

function Test-WindowsX64 {
    if ($PSVersionTable.PSEdition -eq "Core" -and -not $IsWindows) {
        Fail "this installer supports Windows only"
    }
    if ([string]::IsNullOrWhiteSpace($env:USERPROFILE) -or
        -not [System.IO.Path]::IsPathRooted($env:USERPROFILE)) {
        Fail "the Windows user profile could not be determined as an absolute path"
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
    $file = Get-Item -LiteralPath $Output
    if ($file.Length -le 0) {
        Fail "downloaded file is empty: $Url"
    }
}

function Assert-Checksums(
    [string]$Directory,
    [string]$ChecksumFile,
    [string[]]$ExpectedNames
) {
    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $ChecksumFile) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^([0-9a-fA-F]{64})\s+\*?([^\s].*)$') {
            Fail "invalid checksum file: $ChecksumFile"
        }
        $entries.Add([pscustomobject]@{
            Hash = $Matches[1].ToLowerInvariant()
            Name = $Matches[2].Trim()
        })
    }

    if ($entries.Count -ne $ExpectedNames.Count) {
        Fail "checksum file contains an unexpected number of entries: $ChecksumFile"
    }

    foreach ($expectedName in $ExpectedNames) {
        $matching = @($entries | Where-Object { $_.Name -ceq $expectedName })
        if ($matching.Count -ne 1) {
            Fail "checksum entry is missing or duplicated: $expectedName"
        }
        $path = Join-Path $Directory $expectedName
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Fail "checksum asset is missing: $expectedName"
        }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $matching[0].Hash) {
            Fail "checksum verification failed for $expectedName"
        }
    }
}

function Assert-ReleaseMetadata([string]$Path) {
    $values = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch '^([^=]+)=(.+)$' -or $values.ContainsKey($Matches[1])) {
            Fail "invalid release metadata: $Path"
        }
        $values[$Matches[1]] = $Matches[2]
    }
    if ($values.Count -ne 3 -or $values["format"] -cne "1" -or
        $values["version"] -notmatch '^\d+\.\d+\.\d+$' -or
        [string]::IsNullOrWhiteSpace($values["commit"])) {
        Fail "invalid release metadata: $Path"
    }
}

function Test-CupBinInUserPath {
    $path = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($null -eq $path) { return $false }
    foreach ($entry in ($path -split ';')) {
        if ($entry.TrimEnd('\') -ieq $CupBinDir.TrimEnd('\')) { return $true }
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
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        return
    }
    Set-ItemProperty -LiteralPath $Path -Name IsReadOnly -Value $false
}

function Assert-DirectoryIsNotReparsePoint([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Fail "bootstrap staging path is a reparse point: $Path"
    }
    if (-not $item.PSIsContainer) {
        Fail "bootstrap staging path is not a directory: $Path"
    }
}

function Set-BootstrapPermissions {
    foreach ($path in @($PackagesCfg, $CommonChecksums, $PlatformChecksums, $UninstallScript)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
            Set-ItemProperty -LiteralPath $path -Name IsReadOnly -Value $true
        }
    }
}

function Get-Assets {
    return @(
        @{ Key = "binary"; Source = Join-Path $Staging $CupAsset; Destination = $CupExe },
        @{ Key = "manifest"; Source = Join-Path $Staging "packages.cfg"; Destination = $PackagesCfg },
        @{ Key = "common-checksums"; Source = Join-Path $Staging "SHA256SUMS.common"; Destination = $CommonChecksums },
        @{ Key = "platform-checksums"; Source = Join-Path $Staging "SHA256SUMS.$Platform"; Destination = $PlatformChecksums },
        @{ Key = "uninstall"; Source = Join-Path $Staging "uninstall.ps1"; Destination = $UninstallScript }
    )
}

function Restore-Asset([hashtable]$Asset) {
    $backup = Join-Path (Join-Path $Staging "backup") $Asset.Key
    $absent = "$backup.absent"
    $installed = Join-Path (Join-Path $Staging "installed") $Asset.Key

    Clear-ReadOnly $Asset.Destination
    if (Test-Path -LiteralPath $installed -PathType Leaf) {
        if (Test-Path -LiteralPath $Asset.Destination) {
            Remove-Item -LiteralPath $Asset.Destination -Force
        }
    }
    if (Test-Path -LiteralPath $backup) {
        Move-Item -LiteralPath $backup -Destination $Asset.Destination -Force
    } elseif (Test-Path -LiteralPath $absent -PathType Leaf) {
        if (Test-Path -LiteralPath $Asset.Destination) {
            Remove-Item -LiteralPath $Asset.Destination -Force
        }
    }
}

function Recover-Staging {
    if (-not (Test-Path -LiteralPath $Staging -PathType Container)) { return }
    Write-Info "Recovering an interrupted cup bootstrap installation."
    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($asset in (Get-Assets)) {
        try { Restore-Asset $asset } catch { $errors.Add($_.Exception.Message) }
    }
    try { Set-BootstrapPermissions } catch { $errors.Add($_.Exception.Message) }

    if ($errors.Count -gt 0) {
        Fail "the previous bootstrap installation could not be recovered; staging was preserved at $Staging"
    }
    Remove-Item -LiteralPath $Staging -Recurse -Force
}

function Backup-Asset([hashtable]$Asset) {
    $backup = Join-Path (Join-Path $Staging "backup") $Asset.Key
    Clear-ReadOnly $Asset.Destination
    if (Test-Path -LiteralPath $Asset.Destination) {
        Move-Item -LiteralPath $Asset.Destination -Destination $backup -Force
    } else {
        New-Item -ItemType File -Path "$backup.absent" | Out-Null
    }
}

function Commit-Asset([hashtable]$Asset) {
    Move-Item -LiteralPath $Asset.Source -Destination $Asset.Destination -Force
    New-Item -ItemType File -Path (Join-Path (Join-Path $Staging "installed") $Asset.Key) | Out-Null
}

function Main {
    Test-WindowsX64
    foreach ($directory in @($CupRoot, $CupBinDir, $CupConfigDir, $CupScriptsDir)) {
        Assert-DirectoryIsNotReparsePoint $directory
        New-Item -ItemType Directory -Force -Path $directory | Out-Null
        Assert-DirectoryIsNotReparsePoint $directory
    }

    Assert-DirectoryIsNotReparsePoint $Staging
    Recover-Staging
    New-Item -ItemType Directory -Path $Staging | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Staging "backup") | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Staging "installed") | Out-Null
    $committed = $false

    try {
        Write-Info "Installing cup into $CupRoot"
        Download-File "$BaseUrl/$CupAsset" (Join-Path $Staging $CupAsset)
        Download-File "$BaseUrl/packages.cfg" (Join-Path $Staging "packages.cfg")
        Download-File "$BaseUrl/uninstall.ps1" (Join-Path $Staging "uninstall.ps1")
        Download-File "$BaseUrl/release.txt" (Join-Path $Staging "release.txt")
        Download-File "$BaseUrl/SHA256SUMS.$Platform" (Join-Path $Staging "SHA256SUMS.$Platform")
        Download-File "$BaseUrl/SHA256SUMS.common" (Join-Path $Staging "SHA256SUMS.common")

        Assert-Checksums -Directory $Staging `
            -ChecksumFile (Join-Path $Staging "SHA256SUMS.$Platform") `
            -ExpectedNames @($CupAsset, "uninstall.ps1", "release.txt")
        Assert-Checksums -Directory $Staging `
            -ChecksumFile (Join-Path $Staging "SHA256SUMS.common") `
            -ExpectedNames @("packages.cfg")
        Assert-ReleaseMetadata (Join-Path $Staging "release.txt")

        $assets = Get-Assets
        foreach ($asset in $assets) { Backup-Asset $asset }
        foreach ($asset in $assets) { Commit-Asset $asset }
        Set-BootstrapPermissions
        if (Test-Path -LiteralPath $UninstallMarker) {
            Remove-Item -LiteralPath $UninstallMarker -Force
        }
        $committed = $true
    } finally {
        if (-not $committed -and (Test-Path -LiteralPath $Staging -PathType Container)) {
            $rollbackErrors = [System.Collections.Generic.List[string]]::new()
            foreach ($asset in (Get-Assets)) {
                try { Restore-Asset $asset } catch { $rollbackErrors.Add($_.Exception.Message) }
            }
            try { Set-BootstrapPermissions } catch { $rollbackErrors.Add($_.Exception.Message) }
            if ($rollbackErrors.Count -eq 0) {
                Remove-Item -LiteralPath $Staging -Recurse -Force
            } else {
                throw "rollback was incomplete; staging was preserved at $Staging"
            }
        }
    }

    Remove-Item -LiteralPath $Staging -Recurse -Force
    Write-Info "cup installed successfully."
    Write-Info "Binary:    $CupExe"
    Write-Info "Manifest:  $PackagesCfg"
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
