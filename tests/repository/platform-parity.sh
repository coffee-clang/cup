#!/bin/sh
# Purpose: Prevents shared release and test contracts from diverging by host.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"
fail() { printf 'Platform parity test failed: %s\n' "$*" >&2; exit 1; }

for asset in packages.cfg install.cfg install.sh install.ps1; do
    grep -Fq "$asset" include/constants.h || fail "C checksum contract misses $asset"
    grep -Fq "$asset" scripts/release/common-assets.sh || fail "release generator misses $asset"
    grep -Fq "$asset" scripts/release/publish.sh || fail "publisher misses $asset"
    grep -Fq "$asset" tests/release/posix.sh || fail "POSIX release test misses $asset"
    grep -Fq "$asset" tests/release/windows.ps1 || fail "Windows release test misses $asset"
done

for source in src/cup_assets.c src/cup_update.c src/command_repair.c; do
    grep -Fq CUP_COMMON_CHECKSUM_ASSETS "$source" || fail "$source bypasses common checksum contract"
    grep -Fq CUP_COMMON_CHECKSUM_ASSET_COUNT "$source" || fail "$source bypasses common checksum count"
done

portable='test_command_queries test_package_transaction test_cup_update_journal test_runtime_journal test_wrappers test_command_repair'
windows_guard=$(grep -n '^    compile_test test_interrupt ' tests/build/unit.sh | head -n 1 | cut -d: -f1)
[ -n "$windows_guard" ] || fail 'platform-specific unit-test section is missing'
for suite in $portable; do
    line=$(grep -n "^compile_test $suite " tests/build/unit.sh | head -n 1 | cut -d: -f1)
    [ -n "$line" ] || fail "$suite is not compiled"
    [ "$line" -lt "$windows_guard" ] || fail "$suite is still excluded from Windows"
done

for suite in harness.ps1 release-metadata.ps1 commands.ps1 filesystem-archives.ps1 state.ps1 wrappers.ps1 recovery.ps1 doctor.ps1 repair.ps1; do
    [ -f "tests/integration/windows/$suite" ] || fail "missing Windows suite $suite"
    grep -Fq "\"$suite\"" tests/integration/windows/run.ps1 || fail "Windows runner does not execute $suite"
done

printf '%s\n' 'Platform parity tests passed.'
