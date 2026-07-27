# Purpose: Exercises the native Windows install, update, default, inspect, and remove lifecycle.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "..\..\support\windows\common.ps1")

function Initialize-LifecycleFixture {
    Set-PackageCatalogField -Component "compiler" -Tool "clang" `
        -Field "available_versions" -Value "21.1.5" -Mode "Prepend"
    Set-PackageCatalogField -Component "debugger" -Tool "gdb" `
        -Field "available_versions" -Value "16.1" -Mode "Prepend"
    Invoke-Cup -CommandArgs @("repair") | Out-Null

    New-TestPackage -Component "compiler" -Tool "clang" -Version "21.1.5" `
        -Entries @("clang", "clang++")
    New-TestPackage -Component "compiler" -Tool "clang" -Version "22.1.5" `
        -Entries @("clang", "clang++")
    New-TestPackage -Component "debugger" -Tool "gdb" -Version "16.1" -Entries @("gdb")
    New-TestPackage -Component "debugger" -Tool "gdb" -Version "17.1" -Entries @("gdb")
}

function Test-InstallDefaults {
    $installed = Invoke-Cup -CommandArgs @("install", "compiler", "clang@21.1.5")
    Assert-Contains $installed "set it as the first default"

    $statePath = Join-Path $Script:CupTestHome ".cup\state.txt"
    $wrapperPath = Join-Path $Script:CupTestHome ".cup\bin\clang.cmd"
    $stateHash = (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash
    $wrapperHash = (Get-FileHash -LiteralPath $wrapperPath -Algorithm SHA256).Hash
    $reinstall = Invoke-Cup -CommandArgs @("install", "compiler", "clang@21.1.5")
    Assert-Contains $reinstall "Package 'compiler:clang@21.1.5' is already installed"
    Assert-Contains $reinstall "no changes were made."
    Assert-NotContains $reinstall "Error:"
    Assert-Equals ((Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash) $stateHash
    Assert-Equals ((Get-FileHash -LiteralPath $wrapperPath -Algorithm SHA256).Hash) $wrapperHash
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\transaction.txt")

    $staging = @(Get-ChildItem (Join-Path $Script:CupTestHome ".cup\staging") `
        -Force -ErrorAction SilentlyContinue)
    if ($staging.Count -ne 0) {
        Fail-Test "idempotent reinstall created staging content"
    }

    $second = Invoke-Cup -CommandArgs @("install", "compiler", "clang@22.1.5")
    Assert-NotContains $second "set it as the first default"
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) `
        "compiler [windows-x64]: clang@21.1.5"
    Assert-Equals (Invoke-ManagedCommand -Name "clang") `
        "clang-21.1.5-windows-x64:clang"

    Invoke-Cup -CommandArgs @("install", "debugger", "gdb@16.1") | Out-Null
}

function Test-DevelopmentCupUpdate {
    $embeddedVersion = Invoke-Cup -CommandArgs @("--version")
    if ($embeddedVersion -like "*-dev*") {
        $failure = Invoke-Cup -CommandArgs @("update", "cup") -ExpectFailure
        Assert-Contains $failure "available only from an official cup release"
    }
}

function Test-CatalogViews {
    $infoOutput = Invoke-Cup -CommandArgs @("info")
    Assert-Contains $infoOutput "compiler [windows-x64]: clang@21.1.5"
    Assert-Contains $infoOutput "debugger [windows-x64]: gdb@16.1"
    Assert-Contains $infoOutput "status: default"

    $catalog = Invoke-Cup -CommandArgs @("search", "compiler")
    Assert-Contains $catalog "Available tools for component 'compiler'"
    Assert-Contains $catalog "clang"

    $installed = Invoke-Cup -CommandArgs @("list", "compiler")
    Assert-Contains $installed "compiler:clang@21.1.5"
    Assert-Contains $installed "compiler:clang@22.1.5"
    Assert-NotContains $installed "debugger:gdb@16.1"
}

function Test-Updates {
    $componentUpdate = Invoke-Cup -CommandArgs @("update", "compiler")
    Assert-Contains $componentUpdate "0 stable package(s) installed, 1 default(s) moved"
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) `
        "compiler [windows-x64]: clang@22.1.5 (stable)"
    Assert-Equals (Invoke-ManagedCommand -Name "clang") `
        "clang-22.1.5-windows-x64:clang"

    $globalUpdate = Invoke-Cup -CommandArgs @("update")
    Assert-Contains $globalUpdate "1 stable package(s) installed, 1 default(s) moved"
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "debugger")) `
        "debugger [windows-x64]: gdb@17.1 (stable)"
    Assert-Equals (Invoke-ManagedCommand -Name "gdb") "gdb-17.1-windows-x64:gdb"
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\cup-update-result.txt")
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\transaction.txt")

    $packageInfo = Invoke-Cup -CommandArgs @("inspect", "compiler", "clang@stable")
    Assert-Contains $packageInfo "Package information for compiler clang@stable -> clang@22.1.5"
    Assert-Contains $packageInfo "component          compiler"
    Assert-Contains $packageInfo "version            22.1.5"

    Invoke-Cup -CommandArgs @("default", "compiler", "clang@21.1.5") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("info", "compiler")) `
        "compiler [windows-x64]: clang@21.1.5"
    Invoke-Cup -CommandArgs @("default", "compiler", "clang@stable") | Out-Null
    Assert-Contains (Invoke-Cup -CommandArgs @("update", "clang")) `
        "0 stable package(s) installed, 0 default(s) moved"
}

function Test-RemoveDefaultWithoutPromotion {
    Invoke-Cup -CommandArgs @("remove", "compiler", "clang@stable") | Out-Null
    Assert-PathMissing (Join-Path $Script:CupTestHome `
        ".cup\components\compiler\clang\windows-x64\windows-x64\22.1.5")
    Assert-PathExists (Join-Path $Script:CupTestHome `
        ".cup\components\compiler\clang\windows-x64\windows-x64\21.1.5\info.txt")
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\bin\clang.cmd")
    Assert-PathMissing (Join-Path $Script:CupTestHome ".cup\bin\clang++.cmd")
    Assert-Contains (Invoke-Cup -CommandArgs @(
        "info", "compiler", "--target", "windows-x64")) `
        "No default for component 'compiler' on host 'windows-x64', target 'windows-x64'."
    Assert-Contains (Invoke-Cup -CommandArgs @("list", "compiler")) "compiler:clang@21.1.5"
    Assert-NotContains (Invoke-Cup -CommandArgs @("list", "compiler")) "compiler:clang@22.1.5"
    Assert-CupHealthy

    Invoke-Cup -CommandArgs @("remove", "compiler", "clang@21.1.5") | Out-Null
    Assert-NotContains (Invoke-Cup -CommandArgs @("list", "compiler")) "compiler:clang@"
    Assert-CupHealthy
}

try {
    Initialize-TestEnvironment -Name "package-lifecycle" -ExecutablePath $CupExecutablePath
    Initialize-LifecycleFixture
    Test-InstallDefaults
    Test-DevelopmentCupUpdate
    Test-CatalogViews
    Test-Updates
    Test-RemoveDefaultWithoutPromotion
    Write-Host "Windows package lifecycle tests passed."
} finally {
    Remove-TestEnvironment
}
