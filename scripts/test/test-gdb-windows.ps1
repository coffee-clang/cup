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
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
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

$testSource = @(
  '#include <stdio.h>',
  '',
  'static int add(int a, int b) {',
  '    return a + b;',
  '}',
  '',
  'int main(void) {',
  '    int x = add(20, 22);',
  '    printf("x = %d\n", x);',
  '    return 0;',
  '}'
)
$testSource | Set-Content "$env:TEMP\cup-gdb-test.c"

$gcc = (Get-Command gcc.exe -ErrorAction Stop).Source
Invoke-Native $gcc -g -O0 -static "$env:TEMP\cup-gdb-test.c" -o "$env:TEMP\cup-gdb-test.exe"
Assert-FileExists "$env:TEMP\cup-gdb-test.exe"

$gdbTestExe = "$env:TEMP\cup-gdb-test.exe".Replace('\', '/')

# From here on, test the package without relying on MSYS2 runtime PATH.
$env:Path = "$env:SystemRoot\System32;$env:SystemRoot"

Invoke-Native "$root\bin\gdb.exe" --version
Invoke-Native "$root\bin\gdb.exe" --configuration

Get-Content "$root\info.txt" | Select-String 'config.python=true'
Get-Content "$root\info.txt" | Select-String 'config.readline=system'
Get-Content "$root\info.txt" | Select-String 'config.expat=true'
Get-Content "$root\info.txt" | Select-String 'config.zlib=true'
Get-Content "$root\info.txt" | Select-String 'config.lzma=true'
Get-Content "$root\info.txt" | Select-String 'config.zstd=true'

& "$root\bin\gdb.exe" -q -batch `
  -ex 'python import sys, gdb; print("python-ok", sys.version_info[0], sys.version_info[1])' |
  Tee-Object -FilePath "$env:TEMP\cup-gdb-python-output.txt"
if ($LASTEXITCODE -ne 0) { throw "GDB Python test failed with exit code $LASTEXITCODE" }
Select-String -Path "$env:TEMP\cup-gdb-python-output.txt" -Pattern 'python-ok'

& "$root\bin\gdb.exe" -q -batch `
  -ex "file $gdbTestExe" `
  -ex "break add" `
  -ex "run" `
  -ex "print a" `
  -ex "print b" `
  -ex "backtrace" | Tee-Object -FilePath "$env:TEMP\cup-gdb-output.txt"
if ($LASTEXITCODE -ne 0) { throw "GDB batch test failed with exit code $LASTEXITCODE" }

Select-String -Path "$env:TEMP\cup-gdb-output.txt" -Pattern '\$1 = 20'
Select-String -Path "$env:TEMP\cup-gdb-output.txt" -Pattern '\$2 = 22'
