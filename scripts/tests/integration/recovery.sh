#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

test_begin recovery
prepare_command_environment
run_cup repair >/dev/null

install_official_bootstrap_fixture() {
    mkdir -p "$TEST_HOME/.cup/bin" "$TEST_HOME/.cup/config" \
        "$TEST_HOME/.cup/scripts"
    cp "$CUP" "$TEST_HOME/.cup/bin/cup"
    cp "$DEV_ROOT/config/packages.cfg" "$TEST_HOME/.cup/config/packages.cfg"
    cp "$DEV_ROOT/scripts/install/uninstall-cup.sh" \
        "$TEST_HOME/.cup/scripts/uninstall.sh"
    chmod 0755 "$TEST_HOME/.cup/bin/cup"
    chmod 0555 "$TEST_HOME/.cup/scripts/uninstall.sh"

    binary_hash=$(hash_file "$TEST_HOME/.cup/bin/cup")
    manifest_hash=$(hash_file "$TEST_HOME/.cup/config/packages.cfg")
    uninstall_hash=$(hash_file "$TEST_HOME/.cup/scripts/uninstall.sh")
    metadata='format=1
version=0.2.0
commit=abcdef0
'
    metadata_hash=$(hash_text "$metadata")

    {
        printf '%s  packages.cfg\n' "$manifest_hash"
    } > "$TEST_HOME/.cup/config/SHA256SUMS.common"
    {
        printf '%s  cup-%s\n' "$binary_hash" "$TEST_PLATFORM"
        printf '%s  uninstall.sh\n' "$uninstall_hash"
        printf '%s  release.txt\n' "$metadata_hash"
    } > "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
    chmod 0444 "$TEST_HOME/.cup/config/packages.cfg" \
        "$TEST_HOME/.cup/config/SHA256SUMS.common" \
        "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
}

install_official_bootstrap_fixture
staging=$TEST_HOME/.cup/tmp/self-update-test
mkdir -p "$staging"
cp "$TEST_HOME/.cup/bin/cup" "$staging/binary.old"
cp "$TEST_HOME/.cup/scripts/uninstall.sh" "$staging/uninstall.old"
cp "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM" "$staging/checksums.old"
expected_hash=$(hash_file "$staging/binary.old")

chmod u+w "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM" \
    "$TEST_HOME/.cup/scripts/uninstall.sh"
printf 'broken binary\n' > "$TEST_HOME/.cup/bin/cup"
printf 'broken uninstall\n' > "$TEST_HOME/.cup/scripts/uninstall.sh"
printf 'broken checksums\n' > "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
cat > "$TEST_HOME/.cup/tmp/transaction.txt" <<'JOURNAL'
journal_version=2
operation=self-update
temporary_name=self-update-test
JOURNAL

output=$(run_cup repair)
assert_contains "$output" 'Rolled back interrupted self-update transaction.'
assert_equals "$(hash_file "$TEST_HOME/.cup/bin/cup")" "$expected_hash"
assert_missing "$TEST_HOME/.cup/tmp/transaction.txt"
assert_missing "$staging"
run_cup doctor >/dev/null

staging=$TEST_HOME/.cup/tmp/self-update-committed-test
mkdir -p "$staging"
cp "$TEST_HOME/.cup/bin/cup" "$staging/binary.old"
cp "$TEST_HOME/.cup/scripts/uninstall.sh" "$staging/uninstall.old"
cp "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM" "$staging/checksums.old"
: > "$staging/committed"
cat > "$TEST_HOME/.cup/tmp/transaction.txt" <<'JOURNAL'
journal_version=2
operation=self-update
temporary_name=self-update-committed-test
JOURNAL

output=$(run_cup repair)
assert_contains "$output" 'Completed interrupted self-update transaction.'
assert_missing "$TEST_HOME/.cup/tmp/transaction.txt"
assert_missing "$staging"
run_cup doctor >/dev/null

printf 'Recovery tests passed for %s.\n' "$TEST_PLATFORM"
