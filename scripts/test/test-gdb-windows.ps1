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
Invoke-Native -FilePath $gcc -ArgumentList @(
    '-g',
    '-O0',
    '-static',
    "$env:TEMP\cup-gdb-test.c",
    '-o',
    "$env:TEMP\cup-gdb-test.exe"
)
Assert-FileExists "$env:TEMP\cup-gdb-test.exe"

$gdbTestExe = "$env:TEMP\cup-gdb-test.exe".Replace('\', '/')

$env:Path = "$env:SystemRoot\System32;$env:SystemRoot"

Invoke-Native -FilePath "$root\bin\gdb.exe" -ArgumentList @('--version')
Invoke-Native -FilePath "$root\bin\gdb.exe" -ArgumentList @('--configuration')

Get-Content "$root\info.txt" | Select-String 'config.python=true'
Get-Content "$root\info.txt" | Select-String 'config.readline=system'
Get-Content "$root\info.txt" | Select-String 'config.expat=true'
Get-Content "$root\info.txt" | Select-String 'config.zlib=true'
Get-Content "$root\info.txt" | Select-String 'config.lzma=true'
Get-Content "$root\info.txt" | Select-String 'config.zstd=true'

$gdbPythonOutput = Invoke-NativeCapture -FilePath "$root\bin\gdb.exe" -ArgumentList @(
    '-q',
    '-batch',
    '-ex',
    'python import sys, gdb; print("python-ok", sys.version_info[0], sys.version_info[1])'
)
$gdbPythonOutput | Tee-Object -FilePath "$env:TEMP\cup-gdb-python-output.txt"
Assert-OutputContains -Output $gdbPythonOutput -Pattern 'python-ok'

$gdbOutput = Invoke-NativeCapture -FilePath "$root\bin\gdb.exe" -ArgumentList @(
    '-q',
    '-batch',
    '-ex', "file $gdbTestExe",
    '-ex', 'break add',
    '-ex', 'run',
    '-ex', 'print a',
    '-ex', 'print b',
    '-ex', 'backtrace'
)
$gdbOutput | Tee-Object -FilePath "$env:TEMP\cup-gdb-output.txt"
Assert-OutputContains -Output $gdbOutput -Pattern '\$1 = 20'
Assert-OutputContains -Output $gdbOutput -Pattern '\$2 = 22'
