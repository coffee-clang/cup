# Exercises private ACLs, reparse points and long paths on native Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

try {
    Initialize-TestEnvironment -Name "filesystem" -ExecutablePath $CupExecutablePath
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    $cupRoot = Join-Path $Script:CupTestHome ".cup"
    $acl = Get-Acl -LiteralPath $cupRoot
    if (-not $acl.AreAccessRulesProtected) {
        Fail-Test "cup root inherits ACL entries"
    }
    $currentSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $ownerSid = ([System.Security.Principal.NTAccount]::new($acl.Owner)).Translate(
        [System.Security.Principal.SecurityIdentifier]).Value
    Assert-Equals $ownerSid $currentSid

    $longVersion = "30.0.1"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $longVersion `
        -Mode "Prepend"
    $segments = 1..18 | ForEach-Object { "segment$($_.ToString('00'))abcdef" }
    $relativeLongPath = "share\" + (($segments -join '\') + "\payload.txt")
    $zipLongPath = (
        "clang-$longVersion-windows-x64-windows-x64/" +
        $relativeLongPath.Replace('\', '/'))
    [void](New-ZipPackageFixture `
        -Version $longVersion `
        -ExtraPath $zipLongPath `
        -ExtraContent "long path payload`n")
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@$longVersion") | Out-Null
    $longPackageRoot = Join-Path $cupRoot (
        "components\compiler\clang\windows-x64\windows-x64\$longVersion")
    $installedLongPath = Join-Path $longPackageRoot $relativeLongPath
    if ($installedLongPath.Length -le 260) {
        Fail-Test "long-path fixture did not exceed MAX_PATH"
    }
    $extendedLongPath = if ($installedLongPath.StartsWith('\\')) {
        '\\?\UNC\' + $installedLongPath.Substring(2)
    } else {
        '\\?\' + $installedLongPath
    }
    if (-not [System.IO.File]::Exists($extendedLongPath)) {
        Fail-Test "long archive path was not extracted: $installedLongPath"
    }

    $fallbackVersion = "30.0.2"
    Set-PackageCatalogField `
        -Component "compiler" `
        -Tool "clang" `
        -Field "available_versions" `
        -Value $fallbackVersion `
        -Mode "Prepend"
    New-TestPackage `
        -Component "compiler" `
        -Tool "clang" `
        -Version $fallbackVersion `
        -Entries @("clang")
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@$fallbackVersion") | Out-Null
    Invoke-Cup -CommandArgs @("default", "compiler", "clang@$fallbackVersion") | Out-Null

    $external = Join-Path $Script:CupTestRoot "external-target"
    New-Item -ItemType Directory -Force -Path $external | Out-Null
    $sentinel = Join-Path $external "sentinel.txt"
    Set-Content -LiteralPath $sentinel -Encoding ascii -Value "preserve"
    $packageRoot = Join-Path $cupRoot (
        "components\compiler\clang\windows-x64\windows-x64\$longVersion")
    $junction = Join-Path $packageRoot "external-junction"
    $mklink = Invoke-NativeProcess -FilePath (Get-CommandProcessor) `
        -Arguments @('/d', '/c', 'mklink', '/J', $junction, $external) `
        -WorkingDirectory $Script:CupTestRoot
    if ($mklink.ExitCode -ne 0) {
        Fail-Test "failed to create reparse-point fixture: $($mklink.Output)"
    }
    Invoke-Cup -CommandArgs @("remove", "compiler", "clang@$longVersion") | Out-Null
    Assert-PathExists $sentinel
    Assert-PathMissing $junction

    Assert-CupHealthy
    Write-Host "Windows filesystem tests passed."
} finally {
    Remove-TestEnvironment
}
