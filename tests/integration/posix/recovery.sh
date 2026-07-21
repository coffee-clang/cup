#!/bin/sh

# Purpose: Exercises interrupted install, remove, and cup-update recovery at
# their persistent commit boundaries.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin recovery
prepare_command_environment
run_cup repair >/dev/null

install_cup_assets_fixture() {
    mkdir -p "$TEST_HOME/.cup/bin" "$TEST_HOME/.cup/config" \
        "$TEST_HOME/.cup/helpers"
    cp "$CUP" "$TEST_HOME/.cup/bin/cup"
    cp "$CUP" "$TEST_HOME/.cup/helpers/cup-update-helper"
    cp "$DEV_ROOT/config/packages.cfg" "$TEST_HOME/.cup/config/packages.cfg"
    cp "$DEV_ROOT/config/install.cfg" "$TEST_HOME/.cup/config/install.cfg"
    cp "$DEV_ROOT/scripts/install/uninstall-cup.sh" \
        "$TEST_HOME/.cup/helpers/uninstall.sh"
    chmod 0755 "$TEST_HOME/.cup/bin/cup" \
        "$TEST_HOME/.cup/helpers/cup-update-helper"
    chmod 0555 "$TEST_HOME/.cup/helpers/uninstall.sh"

    binary_hash=$(hash_file "$TEST_HOME/.cup/bin/cup")
    package_catalog_hash=$(hash_file "$TEST_HOME/.cup/config/packages.cfg")
    install_policy_hash=$(hash_file "$TEST_HOME/.cup/config/install.cfg")
    uninstall_hash=$(hash_file "$TEST_HOME/.cup/helpers/uninstall.sh")
    base_version=$(sed -n '1p' "$PROJECT_ROOT/VERSION" | tr -d '\r')
    metadata="format=1
version=$base_version
commit=abcdef0
"
    metadata_hash=$(hash_text "$metadata")

    {
        printf '%s  packages.cfg\n' "$package_catalog_hash"
        printf '%s  install.cfg\n' "$install_policy_hash"
    } > "$TEST_HOME/.cup/config/SHA256SUMS.common"
    {
        printf '%s  cup-%s\n' "$binary_hash" "$TEST_PLATFORM"
        printf '%s  uninstall.sh\n' "$uninstall_hash"
        printf '%s  release.txt\n' "$metadata_hash"
    } > "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
    chmod 0444 "$TEST_HOME/.cup/config/packages.cfg" \
        "$TEST_HOME/.cup/config/install.cfg" \
        "$TEST_HOME/.cup/config/SHA256SUMS.common" \
        "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
}

install_cup_assets_fixture

write_package_journal() {
    operation=$1
    component=$2
    tool=$3
    version=$4
    temporary_name=$5

    cat > "$TEST_HOME/.cup/transaction.txt" <<JOURNAL
format=1
operation=$operation
component=$component
tool=$tool
host_platform=$TEST_PLATFORM
target_platform=$TEST_PLATFORM
package_version=$version
temporary_name=$temporary_name
JOURNAL
}

# If state already committed an installation, repair must complete the package
# move rather than discarding the valid staged directory.
make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang
run_cup install compiler clang@stable >/dev/null
install_path=$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5
install_staging_name=install-compiler-clang-$TEST_PLATFORM-$TEST_PLATFORM-22.1.5-recovery
install_staging=$TEST_HOME/.cup/staging/$install_staging_name
mv "$install_path" "$install_staging"
write_package_journal install compiler clang 22.1.5 "$install_staging_name"
run_cup help >/dev/null
run_cup --version >/dev/null
run_cup_expect_failure "$TMP_ROOT/pending-package-list.out" list
assert_contains "$(cat "$TMP_ROOT/pending-package-list.out")" \
    'interrupted package transaction must be repaired first'
run_cup_expect_failure "$TMP_ROOT/pending-package-doctor.out" doctor
assert_contains "$(cat "$TMP_ROOT/pending-package-doctor.out")" \
    'interrupted install transaction detected'
output=$(run_cup repair)
assert_contains "$output" 'Recovered interrupted install transaction for clang@22.1.5.'
assert_file "$install_path/info.txt"
assert_missing "$install_staging"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

# If removal had only staged the package and state still references it, repair
# must roll the package back into its installed location.
make_package debugger lldb 22.1.5 "$TEST_PLATFORM" lldb
run_cup install debugger lldb@stable >/dev/null
remove_path=$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5
remove_staging_name=remove-debugger-lldb-$TEST_PLATFORM-$TEST_PLATFORM-22.1.5-recovery
remove_staging=$TEST_HOME/.cup/staging/$remove_staging_name
mv "$remove_path" "$remove_staging"
write_package_journal remove debugger lldb 22.1.5 "$remove_staging_name"
output=$(run_cup repair)
assert_contains "$output" 'Recovered interrupted remove transaction for lldb@22.1.5.'
assert_file "$remove_path/info.txt"
assert_missing "$remove_staging"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

# If the canonical path is present but corrupted while staging still contains
# the valid package referenced by state, repair must preserve the bad path and
# restore the valid copy instead of discarding it.
conflict_staging_name=remove-debugger-lldb-$TEST_PLATFORM-$TEST_PLATFORM-22.1.5-conflict
conflict_staging=$TEST_HOME/.cup/staging/$conflict_staging_name
mv "$remove_path" "$conflict_staging"
mkdir -p "$remove_path"
printf 'corrupted package\n' > "$remove_path/info.txt"
write_package_journal remove debugger lldb 22.1.5 "$conflict_staging_name"
output=$(run_cup repair)
assert_contains "$output" 'Preserved invalid package path as'
assert_contains "$output" 'Recovered interrupted remove transaction for lldb@22.1.5.'
assert_file "$remove_path/info.txt"
assert_contains "$(cat "$remove_path/info.txt")" 'package.component=debugger'
assert_missing "$conflict_staging"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

staging=$TEST_HOME/.cup/staging/cup-update-rollback-test
mkdir -p "$staging"
cp "$TEST_HOME/.cup/bin/cup" "$staging/binary.old"
cp "$TEST_HOME/.cup/helpers/uninstall.sh" "$staging/uninstall.old"
cp "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM" "$staging/platform-checksums.old"
expected_hash=$(hash_file "$staging/binary.old")

chmod u+w "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM" \
    "$TEST_HOME/.cup/helpers/uninstall.sh"
printf 'broken binary\n' > "$TEST_HOME/.cup/bin/cup"
printf 'broken uninstall\n' > "$TEST_HOME/.cup/helpers/uninstall.sh"
printf 'broken checksums\n' > "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM"
cat > "$TEST_HOME/.cup/transaction.txt" <<'JOURNAL'
format=1
operation=cup-update
phase=scheduled
temporary_name=cup-update-rollback-test
token=recovery-rollback
version=0.0.0
error=0
JOURNAL

output=$(run_cup repair)
assert_contains "$output" 'Rolled back interrupted cup update transaction.'
assert_equals "$(hash_file "$TEST_HOME/.cup/bin/cup")" "$expected_hash"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_missing "$staging"
assert_cup_healthy

staging=$TEST_HOME/.cup/staging/cup-update-committed-test
mkdir -p "$staging"
cp "$TEST_HOME/.cup/bin/cup" "$staging/binary.old"
cp "$TEST_HOME/.cup/helpers/uninstall.sh" "$staging/uninstall.old"
cp "$TEST_HOME/.cup/config/SHA256SUMS.$TEST_PLATFORM" "$staging/platform-checksums.old"
: > "$staging/committed"
cat > "$TEST_HOME/.cup/transaction.txt" <<'JOURNAL'
format=1
operation=cup-update
phase=committing
temporary_name=cup-update-committed-test
token=recovery-committed
version=0.0.0
error=0
JOURNAL

output=$(run_cup repair)
assert_contains "$output" 'Completed interrupted cup update transaction.'
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_missing "$staging"
assert_cup_healthy

# Exercise the private native helper with an inherited descriptor that is
# already at EOF. This proves the handoff is based on a pipe lifetime rather
# than on a reusable parent PID.
staging=$TEST_HOME/.cup/staging/cup-update-helper-test
mkdir -p "$staging"
cp "$TEST_HOME/.cup/bin/cup" "$staging/binary.new"
cp "$TEST_HOME/.cup/helpers/uninstall.sh" "$staging/uninstall.new"
cp "$TEST_HOME/.cup/config/packages.cfg" "$staging/manifest.new"
cp "$TEST_HOME/.cup/config/install.cfg" "$staging/install-config.new"
chmod 0755 "$staging/binary.new" "$staging/uninstall.new"

binary_hash=$(hash_file "$staging/binary.new")
uninstall_hash=$(hash_file "$staging/uninstall.new")
package_catalog_hash=$(hash_file "$staging/manifest.new")
install_policy_hash=$(hash_file "$staging/install-config.new")
{
    printf '%s  packages.cfg\n' "$package_catalog_hash"
    printf '%s  install.cfg\n' "$install_policy_hash"
} > "$staging/common-checksums.new"
{
    printf '%s  cup-%s\n' "$binary_hash" "$TEST_PLATFORM"
    printf '%s  uninstall.sh\n' "$uninstall_hash"
    printf '%s  release.txt\n' "$metadata_hash"
} > "$staging/platform-checksums.new"
cat > "$TEST_HOME/.cup/transaction.txt" <<JOURNAL
format=1
operation=cup-update
phase=scheduled
temporary_name=cup-update-helper-test
token=helper-token
version=$base_version
error=0
JOURNAL

if (cd "$DEV_ROOT" && HOME="$TEST_HOME" "$CUP" \
        --internal-cup-update-helper wrong-token 0 </dev/null \
        >"$TMP_ROOT/helper-wrong-token.out" 2>&1); then
    fail 'native helper accepted the wrong handoff token'
fi
assert_file "$TEST_HOME/.cup/transaction.txt"
(cd "$DEV_ROOT" && HOME="$TEST_HOME" "$CUP" \
    --internal-cup-update-helper helper-token 0 </dev/null)
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_missing "$staging"
assert_contains "$(cat "$TEST_HOME/.cup/cup-update-result.txt")" 'status=success'
assert_contains "$(cat "$TEST_HOME/.cup/cup-update-result.txt")" "version=$base_version"
assert_cup_healthy

printf 'Recovery tests passed for %s.\n' "$TEST_PLATFORM"
