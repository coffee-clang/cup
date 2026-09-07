# Owns Windows test build-root and helper resolution.

function Resolve-TestBuildRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProjectRoot
    )

    $candidate = if ([string]::IsNullOrWhiteSpace($env:CUP_TEST_BUILD_ROOT)) {
        Join-Path $ProjectRoot "build"
    } else {
        $env:CUP_TEST_BUILD_ROOT
    }
    $fullPath = [IO.Path]::GetFullPath($candidate)
    $pathRoot = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot) -or $fullPath -ceq $pathRoot) {
        throw "TEST FAILED: unsafe test build root: $fullPath"
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
        throw "TEST FAILED: test build root is not a directory: $fullPath"
    }

    $marker = Join-Path $fullPath ".cup-build-root"
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) {
        throw "TEST FAILED: test build root marker is missing: $marker"
    }
    $expected = @(
        "format=1",
        "product=coffee-clang/cup",
        "kind=build-root",
        "layout=1"
    )
    $actual = @(Get-Content -LiteralPath $marker)
    if ($actual.Count -ne $expected.Count) {
        throw "TEST FAILED: test build root marker is invalid: $marker"
    }
    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($actual[$index] -cne $expected[$index]) {
            throw "TEST FAILED: test build root marker is invalid: $marker"
        }
    }
    return $fullPath
}

function Resolve-TestHelperPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildRoot,
        [Parameter(Mandatory = $true)]
        [string]$Configuration,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $path = Join-Path $BuildRoot (
        "windows-x64\$Configuration\tests\helpers\$Name.exe")
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "TEST FAILED: test helper is unavailable: $path"
    }
    return (Get-Item -LiteralPath $path -Force).FullName
}
