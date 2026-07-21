# Purpose: Exercises ZIP path safety, private ACLs, reparse points, and long paths on native Windows.

param(
    [Parameter(Mandatory = $true)]
    [string]$CupExecutablePath
)
. (Join-Path $PSScriptRoot "common.ps1")

# Build controlled ZIP fixtures without relying on external archive tools.
function New-CustomZipPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [hashtable]$ExtraEntries
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $platform = "windows-x64"
    $packageName = "clang-$Version-$platform-$platform"
    $cacheDir = Join-Path $Script:CupTestHome ".cup\cache\compiler\clang\$platform\$platform\$Version"
    $archive = Join-Path $cacheDir "$packageName.zip"
    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue

    $stream = [System.IO.File]::Open(
        $archive,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
    try {
        $zip = [System.IO.Compression.ZipArchive]::new(
            $stream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
        try {
            [void]$zip.CreateEntry("$packageName/")
            [void]$zip.CreateEntry("$packageName/bin/")
            $entries = [ordered]@{
                "$packageName/info.txt" = (
                    "package.component=compiler`n" +
                    "package.tool=clang`n" +
                    "package.version=$Version`n" +
                    "platform.host=$platform`n" +
                    "platform.target=$platform`n" +
                    "entry.clang=bin/clang.cmd`n")
                "$packageName/bin/clang.cmd" = "@echo off`r`necho clang-$Version-$platform`:clang`r`n"
            }
            foreach ($name in $ExtraEntries.Keys) {
                $entries[$name] = [string]$ExtraEntries[$name]
            }
            foreach ($name in $entries.Keys) {
                $entry = $zip.CreateEntry($name, [System.IO.Compression.CompressionLevel]::Optimal)
                $entryStream = $entry.Open()
                try {
                    $bytes = [System.Text.Encoding]::UTF8.GetBytes([string]$entries[$name])
                    $entryStream.Write($bytes, 0, $bytes.Length)
                } finally {
                    $entryStream.Dispose()
                }
            }
        } finally {
            $zip.Dispose()
        }
    } finally {
        $stream.Dispose()
    }

    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path $cacheDir "SHA256SUMS") -Lines @(
        "$hash  $(Split-Path -Leaf $archive)")
    return [pscustomobject]@{
        PackageName = $packageName
        Archive = $archive
    }
}

# Rejecting one fixture must not register a partial package.
function Assert-InstallRejected([string]$Version) {
    $output = Invoke-Cup -CommandArgs @("install", "compiler", "clang@$Version") -ExpectFailure
    Assert-NotContains (Invoke-Cup -CommandArgs @("list", "compiler")) "compiler:clang@$Version"
    return $output
}

# End-to-end filesystem, ACL, long-path and archive-safety scenarios.
try {
    Initialize-TestEnvironment -Name "filesystem archives" -ExecutablePath $CupExecutablePath
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
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version $longVersion
    $segments = 1..18 | ForEach-Object { "segment$($_.ToString('00'))abcdef" }
    $relativeLongPath = "share\" + (($segments -join '\') + "\payload.txt")
    $zipLongPath = "clang-$longVersion-windows-x64-windows-x64/" + $relativeLongPath.Replace('\', '/')
    [void](New-CustomZipPackage -Version $longVersion -ExtraEntries @{
        $zipLongPath = "long path payload`n"
    })
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@$longVersion") | Out-Null
    $installedLongPath = Join-Path (
        Join-Path $cupRoot "components\compiler\clang\windows-x64\windows-x64\$longVersion") $relativeLongPath
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
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version $fallbackVersion
    New-TestPackage -Component "compiler" -Tool "clang" -Version $fallbackVersion -Entries @("clang")
    Invoke-Cup -CommandArgs @("install", "compiler", "clang@$fallbackVersion") | Out-Null
    Invoke-Cup -CommandArgs @("default", "compiler", "clang@$fallbackVersion") | Out-Null

    $external = Join-Path $Script:CupTestRoot "external-target"
    New-Item -ItemType Directory -Force -Path $external | Out-Null
    $sentinel = Join-Path $external "sentinel.txt"
    Set-Content -LiteralPath $sentinel -Encoding ascii -Value "preserve"
    $packageRoot = Join-Path $cupRoot "components\compiler\clang\windows-x64\windows-x64\$longVersion"
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

    $caseVersion = "30.1.1"
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version $caseVersion
    $casePackage = "clang-$caseVersion-windows-x64-windows-x64"
    [void](New-CustomZipPackage -Version $caseVersion -ExtraEntries @{
        "$casePackage/bin/CLANG.cmd" = "collision`n"
    })
    [void](Assert-InstallRejected $caseVersion)

    $traversalVersion = "30.1.2"
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version $traversalVersion
    $traversalPackage = "clang-$traversalVersion-windows-x64-windows-x64"
    [void](New-CustomZipPackage -Version $traversalVersion -ExtraEntries @{
        "$traversalPackage/../escape.txt" = "escape`n"
    })
    [void](Assert-InstallRejected $traversalVersion)
    Assert-PathMissing (Join-Path $cupRoot "components\compiler\clang\windows-x64\windows-x64\escape.txt")

    # Invalid cached formats use a bounded loopback refresh target so this native
    # integration never contacts the public release service.
    $catalog = Join-Path $Script:CupTestDevRoot "config\packages.cfg"
    $updatedCatalog = foreach ($line in (Get-Content -LiteralPath $catalog)) {
        if ($line.StartsWith(
                "compiler.clang.windows-x64.windows-x64.url_template=",
                [System.StringComparison]::Ordinal)) {
            "compiler.clang.windows-x64.windows-x64.url_template=" +
                "https://127.0.0.1:1/{version}-{host_platform}-{target_platform}/" +
                "clang-{version}-{host_platform}-{target_platform}.{format}"
        } elseif ($line.StartsWith(
                "compiler.clang.windows-x64.windows-x64.checksum_url_template=",
                [System.StringComparison]::Ordinal)) {
            "compiler.clang.windows-x64.windows-x64.checksum_url_template=" +
                "https://127.0.0.1:1/{version}-{host_platform}-{target_platform}/SHA256SUMS"
        } else {
            $line
        }
    }
    Write-Utf8NoBom -Path $catalog -Lines $updatedCatalog

    $mismatchVersion = "30.1.3"
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version $mismatchVersion
    Set-ManifestFormat -Component "compiler" -Tool "clang" -Format "tar.gz"
    $mismatchFixture = New-CustomZipPackage -Version $mismatchVersion -ExtraEntries @{}
    $mismatchArchive = Join-Path (Split-Path -Parent $mismatchFixture.Archive) `
        "$($mismatchFixture.PackageName).tar.gz"
    Move-Item -LiteralPath $mismatchFixture.Archive -Destination $mismatchArchive
    $mismatchHash = (Get-FileHash -LiteralPath $mismatchArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path (Split-Path -Parent $mismatchArchive) "SHA256SUMS") -Lines @(
        "$mismatchHash  $(Split-Path -Leaf $mismatchArchive)")
    $mismatchOutput = Assert-InstallRejected $mismatchVersion
    Assert-Contains $mismatchOutput "failed to download"

    Set-ManifestFormat -Component "compiler" -Tool "clang" -Format "zip"
    $invalidVersion = "30.1.4"
    Add-ManifestVersion -Component "compiler" -Tool "clang" -Version $invalidVersion
    $invalidPackage = "clang-$invalidVersion-windows-x64-windows-x64"
    $invalidCache = Join-Path $cupRoot "cache\compiler\clang\windows-x64\windows-x64\$invalidVersion"
    New-Item -ItemType Directory -Force -Path $invalidCache | Out-Null
    $invalidArchive = Join-Path $invalidCache "$invalidPackage.zip"
    Set-Content -LiteralPath $invalidArchive -Encoding ascii -Value "not a zip archive"
    $invalidHash = (Get-FileHash -LiteralPath $invalidArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Utf8NoBom -Path (Join-Path $invalidCache "SHA256SUMS") -Lines @(
        "$invalidHash  $(Split-Path -Leaf $invalidArchive)")
    $invalidOutput = Assert-InstallRejected $invalidVersion
    Assert-Contains $invalidOutput "failed to download"

    Assert-CupHealthy
    Write-Host "Windows filesystem and archive tests passed."
} finally {
    Remove-TestEnvironment
}
