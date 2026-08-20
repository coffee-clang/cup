#!/bin/sh

# Exercises the hidden initial-install bridge from one private verified source generation
# into the canonical root lock, journal, staging and detached update-helper protocol.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin bootstrap
prepare_command_environment

prepare_source() {
    source_directory=$1
    mkdir -m 0700 "$source_directory"
    cp "$CUP" "$source_directory/cup-$TEST_PLATFORM"
    cp "$PROJECT_ROOT/scripts/install/uninstall-cup.sh" "$source_directory/uninstall.sh"
    cp "$PROJECT_ROOT/config/packages.cfg" "$source_directory/packages.cfg"
    cp "$PROJECT_ROOT/config/install.cfg" "$source_directory/install.cfg"
    cp "$PROJECT_ROOT/scripts/install/install-cup.sh" "$source_directory/install.sh"
    cp "$PROJECT_ROOT/scripts/install/install-cup-windows.ps1" "$source_directory/install.ps1"
    cp "$TEST_BUILD_ROOT/$TEST_PLATFORM/$TEST_CONFIGURATION/generated/release.txt" \
        "$source_directory/release.txt"
    (
        cd "$source_directory"
        {
            for asset in packages.cfg install.cfg install.sh install.ps1; do
                printf '%s  %s\n' "$(hash_file "$asset")" "$asset"
            done
        } > SHA256SUMS.common
        {
            for asset in "cup-$TEST_PLATFORM" uninstall.sh release.txt SHA256SUMS.common; do
                printf '%s  %s\n' "$(hash_file "$asset")" "$asset"
            done
        } > "SHA256SUMS.$TEST_PLATFORM"
        chmod 0700 "cup-$TEST_PLATFORM" uninstall.sh
        chmod 0600 release.txt packages.cfg install.cfg install.sh install.ps1 \
            SHA256SUMS.common "SHA256SUMS.$TEST_PLATFORM"
    )
}

wait_for_install() {
    attempt=0
    while [ "$attempt" -lt 100 ]; do
        if [ -x "$TEST_HOME/.cup/bin/cup" ] && [ ! -e "$TEST_HOME/.cup/transaction.txt" ] &&
                [ -d "$TEST_HOME/.cup/staging" ] &&
                [ "$(find "$TEST_HOME/.cup/staging" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')" -eq 0 ]; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 0.1
    done
    fail 'canonical bootstrap helper did not finish'
}

source_directory=$TMP_ROOT/source
prepare_source "$source_directory"
HOME="$TEST_HOME" "$CUP" --internal-bootstrap "$source_directory" > "$TMP_ROOT/bootstrap.out"
assert_contains "$(cat "$TMP_ROOT/bootstrap.out")" 'installation scheduled'
wait_for_install

assert_file "$TEST_HOME/.cup/root.txt"
assert_file "$TEST_HOME/.cup/cup.lock"
assert_file "$TEST_HOME/.cup/state.txt"
assert_file "$TEST_HOME/.cup/bin/cup"
assert_file "$TEST_HOME/.cup/helpers/cup-update-helper"
assert_file "$TEST_HOME/.cup/helpers/uninstall.sh"
assert_file "$TEST_HOME/.cup/config/packages.cfg"
assert_file "$TEST_HOME/.cup/config/install.cfg"
assert_file "$TEST_HOME/.cup/config/SHA256SUMS.common"
assert_file "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_missing "$TEST_HOME/.cup/.bootstrap"
[ "$(find "$TEST_HOME/.cup/staging" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')" -eq 0 ] ||
    fail 'successful bootstrap left staging residue'
cmp "$CUP" "$TEST_HOME/.cup/bin/cup" >/dev/null || fail 'bootstrap changed the binary bytes'
cmp "$TEST_HOME/.cup/bin/cup" "$TEST_HOME/.cup/helpers/cup-update-helper" >/dev/null ||
    fail 'bootstrap helper is not the verified source binary'
HOME="$TEST_HOME" "$TEST_HOME/.cup/bin/cup" --version >/dev/null
HOME="$TEST_HOME" "$TEST_HOME/.cup/bin/cup" doctor > "$TMP_ROOT/doctor.out"
assert_contains "$(cat "$TMP_ROOT/doctor.out")" 'Doctor found no issues.'

# A second verified generation uses the same canonical update protocol rather
# than a bootstrap-specific replacement path.
second_source=$TMP_ROOT/second-source
prepare_source "$second_source"
HOME="$TEST_HOME" "$CUP" --internal-bootstrap "$second_source" > "$TMP_ROOT/reinstall.out"
wait_for_install
assert_missing "$TEST_HOME/.cup/transaction.txt"
[ "$(find "$TEST_HOME/.cup/staging" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')" -eq 0 ] ||
    fail 'successful reinstall left staging residue'

# Exact-set and digest failures occur before any root mutation.
invalid_home=$TMP_ROOT/invalid-home
mkdir -m 0700 "$invalid_home"
invalid_source=$TMP_ROOT/invalid-source
prepare_source "$invalid_source"
printf 'extra\n' > "$invalid_source/extra.txt"
if HOME="$invalid_home" "$CUP" --internal-bootstrap "$invalid_source" \
        > "$TMP_ROOT/extra.out" 2>&1; then
    fail 'bootstrap accepted an extra transport source entry'
fi
assert_missing "$invalid_home/.cup"
assert_missing "$invalid_home/.coffee-cup"

rm -f "$invalid_source/extra.txt"
printf 'tampered\n' >> "$invalid_source/packages.cfg"
if HOME="$invalid_home" "$CUP" --internal-bootstrap "$invalid_source" \
        > "$TMP_ROOT/tampered.out" 2>&1; then
    fail 'bootstrap accepted a tampered authenticated source asset'
fi
assert_missing "$invalid_home/.cup"
assert_missing "$invalid_home/.coffee-cup"

printf 'Bootstrap integration tests passed.\n'
