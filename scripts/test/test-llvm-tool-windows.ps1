param(
    [Parameter(Mandatory = $true)]
    [string] $Tool
)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [string[]] $ArgumentList = @()
    )

    Write-Host "==> $FilePath $($ArgumentList -join ' ')"
    & $FilePath @ArgumentList

    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [string[]] $ArgumentList = @()
    )

    Write-Host "==> $FilePath $($ArgumentList -join ' ')"
    $output = & $FilePath @ArgumentList 2>&1

    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }

    return $output
}

function Assert-FileExists {
    param([Parameter(Mandatory = $true)][string] $Path)

    if (-not (Test-Path $Path)) {
        throw "Expected file was not created: $Path"
    }
}

function Assert-OutputContains {
    param(
        [Parameter(Mandatory = $true)]
        [object[]] $Output,

        [Parameter(Mandatory = $true)]
        [string] $Pattern
    )

    $matched = $Output | Select-String -Pattern $Pattern
    if (-not $matched) {
        Write-Host 'Captured output:'
        $Output | ForEach-Object { Write-Host $_ }
        throw "Expected output to contain pattern: $Pattern"
    }
}
$releaseEnv = Get-Content dist/release.env
$packageBase = ($releaseEnv | Where-Object { $_ -like 'package_base=*' }) -replace '^package_base=', ''
if (-not $packageBase) { throw 'package_base not found in dist/release.env' }

Remove-Item -Recurse -Force dist/package-test -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force dist/package-test | Out-Null
Expand-Archive -Force "dist/$packageBase.zip" dist/package-test

$root = Join-Path (Resolve-Path dist/package-test) $packageBase
Get-Content "$root\info.txt"

$env:Path = "$env:SystemRoot\System32;$env:SystemRoot"

switch ($Tool) {
    'clang' { $testExe = 'clang.exe' }
    'lld' { $testExe = 'lld.exe' }
    'lldb' { $testExe = 'lldb.exe' }
    'clangd' { $testExe = 'clangd.exe' }
    'clang-format' { $testExe = 'clang-format.exe' }
    'clang-tidy' { $testExe = 'clang-tidy.exe' }
    default { throw "unsupported LLVM tool: $Tool" }
}

Invoke-Native -FilePath "$root\bin\$testExe" -ArgumentList @('--version')

if ($Tool -eq 'clang') {
    Invoke-Native -FilePath "$root\bin\clang++.exe" -ArgumentList @('--version')
    Invoke-Native -FilePath "$root\bin\ld.lld.exe" -ArgumentList @('--version')
    Invoke-Native -FilePath "$root\bin\clang.exe" -ArgumentList @('-print-resource-dir')

    'int add(int a, int b) { return a + b; } int main(void) { return add(20, 22) == 42 ? 0 : 1; }' | Set-Content "$env:TEMP\cup-clang-test.c"
    Invoke-Native -FilePath "$root\bin\clang.exe" -ArgumentList @(
        '-fsyntax-only',
        "$env:TEMP\cup-clang-test.c"
    )
    Invoke-Native -FilePath "$root\bin\clang.exe" -ArgumentList @(
        '-c',
        "$env:TEMP\cup-clang-test.c",
        '-o',
        "$env:TEMP\cup-clang-test.o"
    )
    Assert-FileExists "$env:TEMP\cup-clang-test.o"

    'int add(int a, int b) { return a + b; } int main() { return add(20, 22) == 42 ? 0 : 1; }' | Set-Content "$env:TEMP\cup-clang-cpp-test.cpp"
    Invoke-Native -FilePath "$root\bin\clang++.exe" -ArgumentList @(
        '-fsyntax-only',
        "$env:TEMP\cup-clang-cpp-test.cpp"
    )
    Invoke-Native -FilePath "$root\bin\clang++.exe" -ArgumentList @(
        '-c',
        "$env:TEMP\cup-clang-cpp-test.cpp",
        '-o',
        "$env:TEMP\cup-clang-cpp-test.o"
    )
    Assert-FileExists "$env:TEMP\cup-clang-cpp-test.o"
}

if ($Tool -eq 'lld') {
    Invoke-Native -FilePath "$root\bin\ld.lld.exe" -ArgumentList @('--version')
    Invoke-Native -FilePath "$root\bin\lld-link.exe" -ArgumentList @('--version')
}

if ($Tool -eq 'lldb') {
    Invoke-Native -FilePath "$root\bin\lldb.exe" -ArgumentList @(
        '-b',
        '-o', 'help',
        '-o', 'quit'
    )
}

if ($Tool -eq 'clang-format') {
    'int main( void ){return 0;}' | Set-Content "$env:TEMP\cup-format-test.c"
    $formatOutput = Invoke-NativeCapture -FilePath "$root\bin\clang-format.exe" -ArgumentList @(
        "$env:TEMP\cup-format-test.c"
    )
    $formatOutput | Tee-Object -FilePath "$env:TEMP\cup-format-output.c"
    Assert-OutputContains -Output $formatOutput -Pattern 'int main\(void\)'
}

if ($Tool -eq 'clangd') {
    $projectDir = Join-Path $env:TEMP 'cup-clangd-project'
    Remove-Item -Recurse -Force $projectDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $projectDir | Out-Null
    $sourcePath = Join-Path $projectDir 'main.c'
    'int main(void) { return 0; }' | Set-Content $sourcePath
    $sourcePathForJson = $sourcePath.Replace('\', '/')
    $projectDirForJson = $projectDir.Replace('\', '/')
    @"
[
  {
    "directory": "$projectDirForJson",
    "command": "clang -std=c11 main.c",
    "file": "$sourcePathForJson"
  }
]
"@ | Set-Content (Join-Path $projectDir 'compile_commands.json')
    Invoke-Native -FilePath "$root\bin\clangd.exe" -ArgumentList @(
        "--check=$sourcePathForJson"
    )
}

if ($Tool -eq 'clang-tidy') {
    Invoke-Native -FilePath "$root\bin\clang-tidy.exe" -ArgumentList @(
        '--list-checks',
        '-checks=clang-analyzer-*'
    )
    'int main(void) { return 0; }' | Set-Content "$env:TEMP\cup-tidy-test.c"
    Invoke-Native -FilePath "$root\bin\clang-tidy.exe" -ArgumentList @(
        "$env:TEMP\cup-tidy-test.c",
        '--',
        '-std=c11'
    )
}
