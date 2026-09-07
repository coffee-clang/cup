# Exercises the hidden initial-install bridge from one private verified source generation
# into the canonical Windows root lock, journal, staging and detached update-helper protocol.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function New-PrivateBootstrapDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

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
    [IO.Directory]::CreateDirectory($Path, $security) | Out-Null
    return $Path
}

function New-BootstrapSource {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    New-PrivateBootstrapDirectory -Path $Path | Out-Null
    $configuration = Get-TestConfiguration
    $releaseMetadata = Join-Path $Script:CupTestBuildRoot `
        "windows-x64\$configuration\generated\release.txt"
    Assert-PathExists $releaseMetadata

    $binary = Join-Path $Path "cup-windows-x64.exe"
    $packages = Join-Path $Path "packages.cfg"
    $installPolicy = Join-Path $Path "install.cfg"
    $installSh = Join-Path $Path "install.sh"
    $installPs1 = Join-Path $Path "install.ps1"
    $release = Join-Path $Path "release.txt"
    $commonChecksums = Join-Path $Path "SHA256SUMS.common"
    $platformChecksums = Join-Path $Path "SHA256SUMS.windows-x64"

    Copy-Item -LiteralPath $Script:CupTestExecutable -Destination $binary
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "config\packages.cfg") `
        -Destination $packages
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "config\install.cfg") `
        -Destination $installPolicy
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "scripts\install\install.sh") `
        -Destination $installSh
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot "scripts\install\install.ps1") `
        -Destination $installPs1
    Copy-Item -LiteralPath $releaseMetadata -Destination $release

    Write-Utf8NoBom -Path $commonChecksums -Lines @(
        "$(Get-Sha256Lower -Path $packages)  packages.cfg",
        "$(Get-Sha256Lower -Path $installPolicy)  install.cfg",
        "$(Get-Sha256Lower -Path $installSh)  install.sh",
        "$(Get-Sha256Lower -Path $installPs1)  install.ps1"
    )
    Write-Utf8NoBom -Path $platformChecksums -Lines @(
        "$(Get-Sha256Lower -Path $binary)  cup-windows-x64.exe",
        "$(Get-Sha256Lower -Path $release)  release.txt",
        "$(Get-Sha256Lower -Path $commonChecksums)  SHA256SUMS.common"
    )
}

function Test-BootstrapStagingEmpty {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $false
    }
    return @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue).Count -eq 0
}

function Wait-ForBootstrapCommit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root
    )

    $binary = Join-Path $Root "bin\cup.exe"
    $transaction = Join-Path $Root "transaction.txt"
    $staging = Join-Path $Root "staging"
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Test-Path -LiteralPath $binary -PathType Leaf) -and
            -not (Test-Path -LiteralPath $transaction) -and
            (Test-BootstrapStagingEmpty -Path $staging)) {
            $ready = Invoke-NativeProcess -FilePath $binary `
                -Arguments @("--internal-runtime-ready") `
                -WorkingDirectory $Script:CupTestDevRoot
            if ($ready.ExitCode -eq 0) {
                return
            }
        }
        Start-Sleep -Milliseconds 100
    }
    Fail-Test "canonical bootstrap helper did not finish"
}

function Invoke-Bootstrap {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Source,

        [Parameter(Mandatory = $true)]
        [string]$Base,

        [switch]$ExpectFailure
    )

    $result = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @("--internal-bootstrap", $Source, $Base) `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($ExpectFailure) {
        if ($result.ExitCode -eq 0) {
            Fail-Test "bootstrap unexpectedly accepted source: $Source"
        }
    } elseif ($result.ExitCode -ne 0) {
        Fail-Test "bootstrap failed [$($result.ExitCode)]`n$($result.Output)"
    }
    return $result
}

try {
    Initialize-TestEnvironment -Name "bootstrap" -ExecutablePath $CupExecutablePath

    $source = Join-Path $Script:CupTestRoot "source"
    New-BootstrapSource -Path $source
    $result = Invoke-Bootstrap -Source $source -Base $Script:CupTestHome
    Assert-Contains $result.Output "installation scheduled"

    $root = Join-Path $Script:CupTestHome ".cup"
    Wait-ForBootstrapCommit -Root $root
    foreach ($relative in @(
        "root.txt",
        "cup.lock",
        "state.txt",
        "bin\cup.exe",
        "helpers\update-helper.exe",
        "config\packages.cfg",
        "config\install.cfg",
        "config\SHA256SUMS.common",
        "config\SHA256SUMS.windows-x64"
    )) {
        Assert-PathExists (Join-Path $root $relative)
    }
    Assert-PathMissing (Join-Path $root "transaction.txt")
    Assert-PathMissing (Join-Path $root ".bootstrap")
    if (-not (Test-BootstrapStagingEmpty -Path (Join-Path $root "staging"))) {
        Fail-Test "successful bootstrap left staging residue"
    }

    $installed = Join-Path $root "bin\cup.exe"
    $helper = Join-Path $root "helpers\update-helper.exe"
    Assert-Equals (Get-Sha256Lower -Path $installed) `
        (Get-Sha256Lower -Path $Script:CupTestExecutable)
    Assert-Equals (Get-Sha256Lower -Path $helper) (Get-Sha256Lower -Path $installed)

    $version = Invoke-NativeProcess -FilePath $installed -Arguments @("--version") `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($version.ExitCode -ne 0) {
        Fail-Test "installed bootstrap binary failed --version"
    }
    $savedPath = $env:Path
    try {
        $env:Path = "$(Join-Path $root 'bin');$savedPath"
        $doctor = Invoke-NativeProcess -FilePath $installed -Arguments @("doctor") `
            -WorkingDirectory $Script:CupTestDevRoot
    } finally {
        $env:Path = $savedPath
    }
    if ($doctor.ExitCode -ne 0) {
        Fail-Test "installed bootstrap binary failed doctor`n$($doctor.Output)"
    }
    Assert-Contains $doctor.Output "Doctor found no issues."

    # Moving the complete canonical root preserves identity independently of USERPROFILE.
    $relocatedBase = Join-Path $Script:CupTestRoot "relocated base"
    New-Item -ItemType Directory -Path $relocatedBase | Out-Null
    $relocatedRoot = Join-Path $relocatedBase ".cup"
    Move-Item -LiteralPath $root -Destination $relocatedRoot
    $installed = Join-Path $relocatedRoot "bin\cup.exe"
    $version = Invoke-NativeProcess -FilePath $installed -Arguments @("--version") `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($version.ExitCode -ne 0) {
        Fail-Test "relocated bootstrap binary failed --version"
    }
    $savedPath = $env:Path
    try {
        $env:Path = "$(Join-Path $relocatedRoot 'bin');$savedPath"
        $doctor = Invoke-NativeProcess -FilePath $installed -Arguments @("doctor") `
            -WorkingDirectory $Script:CupTestDevRoot
    } finally {
        $env:Path = $savedPath
    }
    if ($doctor.ExitCode -ne 0) {
        Fail-Test "relocated bootstrap binary failed doctor`n$($doctor.Output)"
    }
    Assert-Contains $doctor.Output "Doctor found no issues."
    Assert-PathMissing $root

    # A second verified generation must reuse the relocated canonical root.
    $secondSource = Join-Path $Script:CupTestRoot "second-source"
    New-BootstrapSource -Path $secondSource
    $second = Invoke-Bootstrap -Source $secondSource -Base $relocatedBase
    Assert-Contains $second.Output "installation scheduled"
    Wait-ForBootstrapCommit -Root $relocatedRoot
    Assert-PathMissing (Join-Path $relocatedRoot "transaction.txt")
    Assert-PathMissing $root
    if (-not (Test-BootstrapStagingEmpty -Path (Join-Path $relocatedRoot "staging"))) {
        Fail-Test "successful relocated bootstrap reinstall left staging residue"
    }

    # Exact-set and digest failures must occur before any root mutation.
    $primaryHome = $Script:CupTestHome
    $invalidHome = Join-Path $Script:CupTestRoot "invalid-home"
    New-Item -ItemType Directory -Path $invalidHome | Out-Null
    $env:USERPROFILE = $invalidHome
    try {
        $invalidSource = Join-Path $Script:CupTestRoot "invalid-source"
        New-BootstrapSource -Path $invalidSource
        Write-Utf8NoBom -Path (Join-Path $invalidSource "extra.txt") -Lines @("extra")
        Invoke-Bootstrap -Source $invalidSource -Base $invalidHome -ExpectFailure | Out-Null
        Assert-PathMissing (Join-Path $invalidHome ".cup")
        Assert-PathMissing (Join-Path $invalidHome ".coffee-cup")

        Remove-Item -LiteralPath (Join-Path $invalidSource "extra.txt") -Force
        Add-Content -LiteralPath (Join-Path $invalidSource "packages.cfg") `
            -Value "tampered" -Encoding ascii
        Invoke-Bootstrap -Source $invalidSource -Base $invalidHome -ExpectFailure | Out-Null
        Assert-PathMissing (Join-Path $invalidHome ".cup")
        Assert-PathMissing (Join-Path $invalidHome ".coffee-cup")
    } finally {
        $env:USERPROFILE = $primaryHome
    }

    Write-Host "Windows bootstrap integration tests passed."
} finally {
    Remove-TestEnvironment
}
