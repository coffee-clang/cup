# Purpose: Provides Windows ZIP fixtures shared by filesystem and archive tests.

# Build controlled ZIP fixtures without relying on external archive tools.
function New-CustomZipPackage {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$ExtraEntries
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $platform = "windows-x64"
    $packageName = "clang-$Version-$platform-$platform"
    $cacheDir = Join-Path $Script:CupTestHome (
        ".cup\cache\compiler\clang\$platform\$platform\$Version")
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
            # Keep entries as a sequence rather than a hashtable. PowerShell
            # hashtables compare string keys case-insensitively, which would
            # collapse clang.cmd and CLANG.cmd before the ZIP is created.
            $entries = [System.Collections.Generic.List[object]]::new()
            $entries.Add([pscustomobject]@{
                Name = "$packageName/info.txt"
                Content = (
                    "package.component=compiler`n" +
                    "package.tool=clang`n" +
                    "package.version=$Version`n" +
                    "platform.host=$platform`n" +
                    "platform.target=$platform`n" +
                    "entry.clang=bin/clang.cmd`n")
            })
            $entries.Add([pscustomobject]@{
                Name = "$packageName/bin/clang.cmd"
                Content = "@echo off`r`necho clang-$Version-$platform`:clang`r`n"
            })
            foreach ($name in $ExtraEntries.Keys) {
                $entries.Add([pscustomobject]@{
                    Name = [string]$name
                    Content = [string]$ExtraEntries[$name]
                })
            }
            foreach ($entrySpec in $entries) {
                $entry = $zip.CreateEntry(
                    $entrySpec.Name,
                    [System.IO.Compression.CompressionLevel]::Optimal)
                $entryStream = $entry.Open()
                try {
                    $bytes = [System.Text.Encoding]::UTF8.GetBytes($entrySpec.Content)
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

# Validate generated ZIP entry names with ordinal comparison before cup consumes
# the fixture. This prevents a broken fixture from being reported as a cup bug.
function Assert-ZipContainsExactEntries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Archive,

        [Parameter(Mandatory = $true)]
        [string[]]$Names
    )

    $stream = [System.IO.File]::OpenRead($Archive)
    try {
        $zip = [System.IO.Compression.ZipArchive]::new(
            $stream, [System.IO.Compression.ZipArchiveMode]::Read, $false)
        try {
            $actual = @($zip.Entries | ForEach-Object { $_.FullName })
            foreach ($name in $Names) {
                if (-not ($actual -ccontains $name)) {
                    Fail-Test "ZIP fixture is missing exact entry: $name"
                }
            }
        } finally {
            $zip.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

# Rejecting one fixture must not register a partial package.
function Assert-InstallRejected([string]$Version) {
    $output = Invoke-Cup -CommandArgs @("install", "compiler", "clang@$Version") -ExpectFailure
    Assert-NotContains (Invoke-Cup -CommandArgs @("list", "compiler")) "compiler:clang@$Version"
    return $output
}
