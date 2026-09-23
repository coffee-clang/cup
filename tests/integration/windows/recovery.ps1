# Exercises interrupted package and CUP-generation recovery on Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function New-PrivateBootstrapDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

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
}

function Get-FixtureIdentity {
    $version = (Get-Content -LiteralPath (Join-Path $Script:CupTestProjectRoot 'VERSION') -Raw).Trim()
    $git = Invoke-NativeProcess -FilePath 'git' `
        -Arguments @('-C', $Script:CupTestProjectRoot, 'rev-parse', 'HEAD') `
        -WorkingDirectory $Script:CupTestProjectRoot
    if ($git.ExitCode -ne 0) { Fail-Test 'could not resolve recovery fixture commit' }
    return [pscustomobject]@{ Version = $version; Commit = $git.Output.Trim() }
}

function Write-ReleaseManifest {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string[]]$AssetNames
    )

    $identity = Get-FixtureIdentity
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('format=2')
    $lines.Add("version=$($identity.Version)")
    $lines.Add("commit=$($identity.Commit)")
    $lines.Add('root_layout=2')
    $lines.Add('catalog_format=1')
    $lines.Add("asset_count=$($AssetNames.Count)")
    for ($i = 0; $i -lt $AssetNames.Count; $i++) {
        $name = $AssetNames[$i]
        $lines.Add("asset.$i.name=$name")
        $lines.Add("asset.$i.sha256=$(Get-Sha256Lower -Path (Join-Path $Directory $name))")
    }
    Write-Utf8NoBom -Path (Join-Path $Directory 'release.txt') -Lines $lines
}

function New-BootstrapSource {
    param([Parameter(Mandatory = $true)][string]$Path)

    New-PrivateBootstrapDirectory -Path $Path
    Copy-Item -LiteralPath $Script:CupTestExecutable `
        -Destination (Join-Path $Path 'cup-windows-x64.exe')
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot 'LICENSE') `
        -Destination (Join-Path $Path 'LICENSE')
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot `
        'scripts\dependencies\THIRD_PARTY_NOTICES.txt') `
        -Destination (Join-Path $Path 'THIRD_PARTY_NOTICES.txt')
    Copy-Item -LiteralPath (Join-Path $Script:CupTestProjectRoot 'tests\fixtures\catalog.cfg') `
        -Destination (Join-Path $Path 'catalog.cfg')
    Write-ReleaseManifest -Directory $Path `
        -AssetNames @('LICENSE', 'THIRD_PARTY_NOTICES.txt', 'catalog.cfg', 'cup-windows-x64.exe')
}

function Invoke-Bootstrap {
    param([Parameter(Mandatory = $true)][string]$Source)
    $result = Invoke-NativeProcess -FilePath $Script:CupTestExecutable `
        -Arguments @('--internal-bootstrap', $Source, $Script:CupTestHome) `
        -WorkingDirectory $Script:CupTestDevRoot
    if ($result.ExitCode -ne 0) {
        Fail-Test "recovery bootstrap failed [$($result.ExitCode)]`n$($result.Output)"
    }
}

function Write-PackageJournal {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('install', 'remove')][string]$Operation,
        [Parameter(Mandatory = $true)][string]$Component,
        [Parameter(Mandatory = $true)][string]$Tool,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$TemporaryName
    )

    Write-Utf8NoBom -Path (Join-Path $Script:CupTestHome '.cup\transaction.txt') -Lines @(
        'format=2',
        "operation=$Operation",
        "component=$Component",
        "tool=$Tool",
        'target_platform=windows-x64',
        "package_version=$Version",
        "temporary_name=$TemporaryName"
    )
}

function Write-GenerationRelease {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$LicenseText,
        [Parameter(Mandatory = $true)][string]$NoticesText,
        [Parameter(Mandatory = $true)][string]$BinarySource
    )

    New-Item -ItemType Directory -Force -Path $Directory | Out-Null
    Write-Utf8NoBom -Path (Join-Path $Directory 'LICENSE') -Lines @($LicenseText)
    Write-Utf8NoBom -Path (Join-Path $Directory 'THIRD_PARTY_NOTICES.txt') -Lines @($NoticesText)
    Copy-Item -LiteralPath $BinarySource -Destination (Join-Path $Directory 'cup-windows-x64.exe') -Force
    Write-ReleaseManifest -Directory $Directory `
        -AssetNames @('LICENSE', 'THIRD_PARTY_NOTICES.txt', 'cup-windows-x64.exe')
}

function Snapshot-Generation {
    param(
        [Parameter(Mandatory = $true)][string]$CupRoot,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -LiteralPath (Join-Path $CupRoot 'release.txt') -Destination $Destination -Force
    Copy-Item -LiteralPath (Join-Path $CupRoot 'LICENSE') -Destination $Destination -Force
    Copy-Item -LiteralPath (Join-Path $CupRoot 'THIRD_PARTY_NOTICES.txt') -Destination $Destination -Force
    Copy-Item -LiteralPath (Join-Path $CupRoot 'bin\cup.exe') `
        -Destination (Join-Path $Destination 'cup-windows-x64.exe') -Force
}

function Prepare-GenerationTransaction {
    param(
        [Parameter(Mandatory = $true)][string]$CupRoot,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$LicenseText,
        [Parameter(Mandatory = $true)][string]$NoticesText
    )

    $staging = Join-Path $CupRoot "staging\$Name"
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    $newDirectory = Join-Path $staging 'new'
    $oldDirectory = Join-Path $staging 'old'
    Write-GenerationRelease -Directory $newDirectory -LicenseText $LicenseText `
        -NoticesText $NoticesText -BinarySource (Join-Path $CupRoot 'bin\cup.exe')
    Snapshot-Generation -CupRoot $CupRoot -Destination $oldDirectory
    $targetReleaseSha = Get-Sha256Lower -Path (Join-Path $newDirectory 'release.txt')
    Write-Utf8NoBom -Path (Join-Path $CupRoot 'transaction.txt') -Lines @(
        'format=2',
        'operation=cup-generation',
        "target_release_sha256=$targetReleaseSha",
        "temporary_name=$Name"
    )
    return [pscustomobject]@{ Staging = $staging; New = $newDirectory; Old = $oldDirectory }
}

function Set-FileWritable {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $item = Get-Item -LiteralPath $Path -Force
        $item.IsReadOnly = $false
    }
}

function Install-GenerationAsset {
    param(
        [Parameter(Mandatory = $true)][string]$CupRoot,
        [Parameter(Mandatory = $true)][string]$NewDirectory,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($Name -ceq 'cup-windows-x64.exe') {
        Copy-Item -LiteralPath (Join-Path $NewDirectory $Name) `
            -Destination (Join-Path $CupRoot 'bin\cup.exe') -Force
        return
    }
    if ($Name -notin @('release.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.txt')) {
        Fail-Test "unknown generation asset: $Name"
    }
    $destination = Join-Path $CupRoot $Name
    Set-FileWritable -Path $destination
    Copy-Item -LiteralPath (Join-Path $NewDirectory $Name) -Destination $destination -Force
}

try {
    Initialize-TestEnvironment -Name 'recovery' -ExecutablePath $CupExecutablePath
    $bootstrapSource = Join-Path $Script:CupTestRoot 'bootstrap-source'
    New-BootstrapSource -Path $bootstrapSource
    Invoke-Bootstrap -Source $bootstrapSource
    Assert-CupHealthy

    $cupRoot = Join-Path $Script:CupTestHome '.cup'
    $transactionPath = Join-Path $cupRoot 'transaction.txt'

    # If state already committed an installation, repair completes the package move from staging.
    New-TestPackage -Component 'compiler' -Tool 'clang' -Version '23.1.0' -Entries @('clang')
    Invoke-Cup -CommandArgs @('install', 'compiler', 'clang@23.1.0') | Out-Null
    $installPath = Join-Path $cupRoot 'components\compiler\clang\windows-x64\23.1.0'
    $installStagingName = 'install-compiler-clang-windows-x64-23.1.0-recovery'
    $installStaging = Join-Path $cupRoot "staging\$installStagingName"
    Move-Item -LiteralPath $installPath -Destination $installStaging
    Write-PackageJournal -Operation install -Component compiler -Tool clang `
        -Version '23.1.0' -TemporaryName $installStagingName

    Invoke-Cup -CommandArgs @('help') | Out-Null
    Invoke-Cup -CommandArgs @('--version') | Out-Null
    $blocked = Invoke-Cup -CommandArgs @('list') -ExpectFailure
    Assert-Contains $blocked 'a package transaction is active or requires recovery'
    $diagnosis = Invoke-Cup -CommandArgs @('doctor') -ExpectFailure
    Assert-Contains $diagnosis 'interrupted install transaction detected'
    $installRepair = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $installRepair 'Recovered interrupted install transaction for clang@23.1.0.'
    Assert-PathExists (Join-Path $installPath 'info.txt')
    Assert-PathMissing $installStaging
    Assert-PathMissing $transactionPath
    Assert-CupHealthy

    # If remove only staged a package while state still references it, repair rolls it back.
    New-TestPackage -Component 'debugger' -Tool 'lldb' -Version '23.1.0' -Entries @('lldb')
    Invoke-Cup -CommandArgs @('install', 'debugger', 'lldb@23.1.0') | Out-Null
    $removePath = Join-Path $cupRoot 'components\debugger\lldb\windows-x64\23.1.0'
    $removeStagingName = 'remove-debugger-lldb-windows-x64-23.1.0-recovery'
    $removeStaging = Join-Path $cupRoot "staging\$removeStagingName"
    Move-Item -LiteralPath $removePath -Destination $removeStaging
    Write-PackageJournal -Operation remove -Component debugger -Tool lldb `
        -Version '23.1.0' -TemporaryName $removeStagingName

    $removeRepair = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $removeRepair 'Recovered interrupted remove transaction for lldb@23.1.0.'
    Assert-PathExists (Join-Path $removePath 'info.txt')
    Assert-PathMissing $removeStaging
    Assert-PathMissing $transactionPath
    Assert-CupHealthy

    # A conflicting invalid canonical path is preserved before restoring the state-owned package.
    $conflictStagingName = 'remove-debugger-lldb-windows-x64-23.1.0-conflict'
    $conflictStaging = Join-Path $cupRoot "staging\$conflictStagingName"
    Move-Item -LiteralPath $removePath -Destination $conflictStaging
    New-Item -ItemType Directory -Force -Path $removePath | Out-Null
    Write-Utf8NoBom -Path (Join-Path $removePath 'info.txt') -Lines @('corrupted package')
    Write-PackageJournal -Operation remove -Component debugger -Tool lldb `
        -Version '23.1.0' -TemporaryName $conflictStagingName

    $conflictRepair = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $conflictRepair 'Preserved invalid package path as'
    Assert-Contains $conflictRepair 'Recovered interrupted remove transaction for lldb@23.1.0.'
    Assert-Contains (Get-Content -LiteralPath (Join-Path $removePath 'info.txt') -Raw) `
        'package.component=debugger'
    Assert-PathMissing $conflictStaging
    Assert-PathMissing $transactionPath
    Assert-CupHealthy

    # Before binary-last commit, repair rolls non-binary assets back because the canonical binary
    # still proves the old generation.
    $rollback = Prepare-GenerationTransaction -CupRoot $cupRoot `
        -Name 'cup-update-recovery-rollback' `
        -LicenseText 'target-license-a' -NoticesText 'target-notices-a'
    $oldReleaseHash = Get-Sha256Lower -Path (Join-Path $rollback.Old 'release.txt')
    $oldLicenseHash = Get-Sha256Lower -Path (Join-Path $rollback.Old 'LICENSE')
    $oldNoticesHash = Get-Sha256Lower -Path (Join-Path $rollback.Old 'THIRD_PARTY_NOTICES.txt')
    $oldBinaryHash = Get-Sha256Lower -Path (Join-Path $rollback.Old 'cup-windows-x64.exe')
    Install-GenerationAsset -CupRoot $cupRoot -NewDirectory $rollback.New -Name 'LICENSE'
    Install-GenerationAsset -CupRoot $cupRoot -NewDirectory $rollback.New -Name 'release.txt'
    $rollbackOutput = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $rollbackOutput 'Rolled back interrupted CUP generation transaction.'
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'release.txt')) $oldReleaseHash
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'LICENSE')) $oldLicenseHash
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'THIRD_PARTY_NOTICES.txt')) $oldNoticesHash
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'bin\cup.exe')) $oldBinaryHash
    Assert-PathMissing $transactionPath
    Assert-PathMissing $rollback.Staging
    Assert-CupHealthy

    # Once every canonical generation byte equals the target, repair only finalizes cleanup.
    $finalize = Prepare-GenerationTransaction -CupRoot $cupRoot `
        -Name 'cup-update-recovery-finalize' `
        -LicenseText 'target-license-b' -NoticesText 'target-notices-b'
    foreach ($name in @('LICENSE', 'THIRD_PARTY_NOTICES.txt', 'release.txt', 'cup-windows-x64.exe')) {
        Install-GenerationAsset -CupRoot $cupRoot -NewDirectory $finalize.New -Name $name
    }
    $targetReleaseHash = Get-Sha256Lower -Path (Join-Path $cupRoot 'release.txt')
    $targetLicenseHash = Get-Sha256Lower -Path (Join-Path $cupRoot 'LICENSE')
    $targetNoticesHash = Get-Sha256Lower -Path (Join-Path $cupRoot 'THIRD_PARTY_NOTICES.txt')
    $targetBinaryHash = Get-Sha256Lower -Path (Join-Path $cupRoot 'bin\cup.exe')
    $finalizeOutput = Invoke-Cup -CommandArgs @('repair')
    Assert-Contains $finalizeOutput 'Completed interrupted CUP generation transaction.'
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'release.txt')) $targetReleaseHash
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'LICENSE')) $targetLicenseHash
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'THIRD_PARTY_NOTICES.txt')) $targetNoticesHash
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'bin\cup.exe')) $targetBinaryHash
    Assert-PathMissing $transactionPath
    Assert-PathMissing $finalize.Staging
    Assert-CupHealthy

    # A third binary is ambiguous evidence and must leave journal/workspace untouched.
    $ambiguous = Prepare-GenerationTransaction -CupRoot $cupRoot `
        -Name 'cup-update-recovery-ambiguous' `
        -LicenseText 'target-license-c' -NoticesText 'target-notices-c'
    [IO.File]::WriteAllText((Join-Path $cupRoot 'bin\cup.exe'), 'third-binary', [Text.Encoding]::ASCII)
    $thirdBinaryHash = Get-Sha256Lower -Path (Join-Path $cupRoot 'bin\cup.exe')
    $ambiguousOutput = Invoke-Cup -CommandArgs @('repair') -ExpectFailure
    Assert-Contains $ambiguousOutput 'interrupted operation cannot be repaired safely'
    Assert-Equals (Get-Sha256Lower -Path (Join-Path $cupRoot 'bin\cup.exe')) $thirdBinaryHash
    Assert-PathExists $transactionPath
    Assert-PathExists (Join-Path $ambiguous.New 'release.txt')
    Assert-PathExists (Join-Path $ambiguous.Old 'cup-windows-x64.exe')

    # Restore the exact old snapshot only after proving ambiguous evidence preservation.
    foreach ($name in @('release.txt', 'LICENSE', 'THIRD_PARTY_NOTICES.txt')) {
        $destination = Join-Path $cupRoot $name
        Set-FileWritable -Path $destination
        Copy-Item -LiteralPath (Join-Path $ambiguous.Old $name) -Destination $destination -Force
    }
    Copy-Item -LiteralPath (Join-Path $ambiguous.Old 'cup-windows-x64.exe') `
        -Destination (Join-Path $cupRoot 'bin\cup.exe') -Force
    Remove-Item -LiteralPath $transactionPath -Force
    Remove-Item -LiteralPath $ambiguous.Staging -Recurse -Force
    Assert-CupHealthy

    Write-Host 'Windows recovery tests passed.'
} finally {
    Remove-TestEnvironment
}
