param(
    [Parameter(Mandatory = $true)]
    [string]$Tool
)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE: $FilePath $($Arguments -join ' ')"
    }
}

function Assert-FileExists {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Expected file was not created: $Path"
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

# Test the package without relying on the CLANG64 runtime PATH.
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

Invoke-Native "$root\bin\$testExe" --version

if ($Tool -eq 'clang') {
    Invoke-Native "$root\bin\clang++.exe" --version
    Invoke-Native "$root\bin\ld.lld.exe" --version
    Invoke-Native "$root\bin\clang.exe" -print-resource-dir

    'int add(int a, int b) { return a + b; } int main(void) { return add(20, 22) == 42 ? 0 : 1; }' | Set-Content "$env:TEMP\cup-clang-test.c"
    Invoke-Native "$root\bin\clang.exe" -fsyntax-only "$env:TEMP\cup-clang-test.c"
    Invoke-Native "$root\bin\clang.exe" -c "$env:TEMP\cup-clang-test.c" -o "$env:TEMP\cup-clang-test.o"
    Assert-FileExists "$env:TEMP\cup-clang-test.o"

    'int add(int a, int b) { return a + b; } int main() { return add(20, 22) == 42 ? 0 : 1; }' | Set-Content "$env:TEMP\cup-clang-cpp-test.cpp"
    Invoke-Native "$root\bin\clang++.exe" -fsyntax-only "$env:TEMP\cup-clang-cpp-test.cpp"
    Invoke-Native "$root\bin\clang++.exe" -c "$env:TEMP\cup-clang-cpp-test.cpp" -o "$env:TEMP\cup-clang-cpp-test.o"
    Assert-FileExists "$env:TEMP\cup-clang-cpp-test.o"
}

if ($Tool -eq 'lld') {
    Invoke-Native "$root\bin\ld.lld.exe" --version
    Invoke-Native "$root\bin\lld-link.exe" --version
}

if ($Tool -eq 'lldb') {
    Invoke-Native "$root\bin\lldb.exe" -b -o "help" -o "quit"
}

if ($Tool -eq 'clang-format') {
    'int main( void ){return 0;}' | Set-Content "$env:TEMP\cup-format-test.c"
    & "$root\bin\clang-format.exe" "$env:TEMP\cup-format-test.c" | Tee-Object -FilePath "$env:TEMP\cup-format-output.c"
    if ($LASTEXITCODE -ne 0) { throw "clang-format failed with exit code $LASTEXITCODE" }
    Select-String -Path "$env:TEMP\cup-format-output.c" -Pattern 'int main\(void\)'
}

if ($Tool -eq 'clangd') {
    $projectDir = Join-Path $env:TEMP 'cup-clangd-project'
    Remove-Item -Recurse -Force $projectDir -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $projectDir | Out-Null
    $sourcePath = Join-Path $projectDir 'main.c'
    'int main(void) { return 0; }' | Set-Content $sourcePath
    $sourcePathForJson = $sourcePath.Replace('\\', '/')
    $projectDirForJson = $projectDir.Replace('\\', '/')
    @"
[
  {
    "directory": "$projectDirForJson",
    "command": "clang -std=c11 main.c",
    "file": "$sourcePathForJson"
  }
]
"@ | Set-Content (Join-Path $projectDir 'compile_commands.json')
    Invoke-Native "$root\bin\clangd.exe" "--check=$sourcePathForJson"
}

if ($Tool -eq 'clang-tidy') {
    Invoke-Native "$root\bin\clang-tidy.exe" --list-checks -checks=clang-analyzer-*
    'int main(void) { return 0; }' | Set-Content "$env:TEMP\cup-tidy-test.c"
    Invoke-Native "$root\bin\clang-tidy.exe" "$env:TEMP\cup-tidy-test.c" -- -std=c11
}
