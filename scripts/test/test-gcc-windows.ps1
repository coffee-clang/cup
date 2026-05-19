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

# Test the package without relying on MSYS2 runtime PATH.
$env:Path = "$env:SystemRoot\System32;$env:SystemRoot"

Invoke-Native "$root\bin\gcc.exe" --version
Invoke-Native "$root\bin\g++.exe" --version
Invoke-Native "$root\bin\x86_64-w64-mingw32-gcc.exe" --version
Invoke-Native "$root\bin\x86_64-w64-mingw32-g++.exe" --version
Invoke-Native "$root\bin\as.exe" --version
Invoke-Native "$root\bin\ld.exe" --version

@'
#include <stdio.h>

int main(void) {
    printf("hello gcc windows c\n");
    return 0;
}
'@ | Set-Content "$env:TEMP\cup-gcc-windows-c-test.c"
Invoke-Native "$root\bin\gcc.exe" -static "$env:TEMP\cup-gcc-windows-c-test.c" -o "$env:TEMP\cup-gcc-windows-c-test.exe"
Assert-FileExists "$env:TEMP\cup-gcc-windows-c-test.exe"
Invoke-Native "$env:TEMP\cup-gcc-windows-c-test.exe" | Select-String 'hello gcc windows c'

@'
#include <iostream>
#include <vector>

int main() {
    std::vector<int> values = {20, 22};
    std::cout << (values[0] + values[1]) << "\n";
    return 0;
}
'@ | Set-Content "$env:TEMP\cup-gcc-windows-cpp-test.cpp"
Invoke-Native "$root\bin\g++.exe" -static "$env:TEMP\cup-gcc-windows-cpp-test.cpp" -o "$env:TEMP\cup-gcc-windows-cpp-test.exe"
Assert-FileExists "$env:TEMP\cup-gcc-windows-cpp-test.exe"
Invoke-Native "$env:TEMP\cup-gcc-windows-cpp-test.exe" | Select-String '42'

@'
#include <pthread.h>
#include <stdio.h>

static void *worker(void *arg) {
    return arg;
}

int main(void) {
    pthread_t thread;
    void *result = 0;

    if (pthread_create(&thread, 0, worker, (void *)42) != 0) {
        return 1;
    }

    if (pthread_join(thread, &result) != 0) {
        return 1;
    }

    printf("pthread %ld\n", (long)result);
    return result == (void *)42 ? 0 : 1;
}
'@ | Set-Content "$env:TEMP\cup-gcc-windows-pthread-test.c"
Invoke-Native "$root\bin\gcc.exe" -static "$env:TEMP\cup-gcc-windows-pthread-test.c" -o "$env:TEMP\cup-gcc-windows-pthread-test.exe" -pthread
Assert-FileExists "$env:TEMP\cup-gcc-windows-pthread-test.exe"
Invoke-Native "$env:TEMP\cup-gcc-windows-pthread-test.exe" | Select-String 'pthread 42'

@'
static int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(20, 22) == 42 ? 0 : 1;
}
'@ | Set-Content "$env:TEMP\cup-gcc-windows-lto-test.c"
Invoke-Native "$root\bin\gcc.exe" -static -flto "$env:TEMP\cup-gcc-windows-lto-test.c" -o "$env:TEMP\cup-gcc-windows-lto-test.exe"
Assert-FileExists "$env:TEMP\cup-gcc-windows-lto-test.exe"
Invoke-Native "$env:TEMP\cup-gcc-windows-lto-test.exe"
