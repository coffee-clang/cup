# Purpose: Runs Windows source tests, the development build, repository
# contracts and the native integration suite against the canonical UCRT64
# dependency prefix prepared by `make deps`.

$ErrorActionPreference = "Stop"

& bash.exe -lc "PLATFORM=windows-x64 CUP_TEST_PLATFORM=windows-x64 make test-unit"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

make PLATFORM=windows-x64
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

make PLATFORM=windows-x64 check-binary
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& bash.exe -lc "PLATFORM=windows-x64 CUP_TEST_PLATFORM=windows-x64 make test-repository"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

make test-windows
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
