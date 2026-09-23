#!/bin/sh

# Exercises deterministic repair, state reconstruction, quarantine and evidence preservation.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin repair
prepare_command_environment
ensure_fixture_runtime_root

state_file=$TEST_HOME/.cup/state.txt
transaction_file=$TEST_HOME/.cup/transaction.txt

# A valid package found on disk is adopted without rewriting producer-owned metadata.
make_installed_package compiler clang 23.1.0 "$TEST_PLATFORM" clang
package_root="$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/23.1.0"
package_metadata=$package_root/info.txt
package_manifest=$package_root/manifest.txt
metadata_hash=$(hash_file "$package_metadata")
manifest_hash=$(hash_file "$package_manifest")
output=$(run_cup repair)
assert_contains "$output" "Prepared state repair: adopt valid package 'compiler:clang@23.1.0'"
assert_contains "$(cat "$state_file")" "installed.compiler.$TEST_PLATFORM=clang@23.1.0"
assert_equals "$(hash_file "$package_metadata")" "$metadata_hash"
assert_equals "$(hash_file "$package_manifest")" "$manifest_hash"

# State entries with no package are removed together with their defaults.
chmod u+w "$state_file"
cat >> "$state_file" <<STATE
installed.debugger.$TEST_PLATFORM=lldb@23.1.0
default.debugger.$TEST_PLATFORM=lldb@23.1.0
STATE
output=$(run_cup repair)
assert_contains "$output" "Removed stale state record 'debugger:lldb@23.1.0'."
assert_not_contains "$(cat "$state_file")" 'lldb@23.1.0'

# Package-shaped invalid data is quarantined outside components/. A malformed path above the
# package leaf remains diagnostic evidence because its identity is ambiguous.
invalid_package=$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/23.1.0
mkdir -p "$invalid_package"
mkdir -p "$TEST_HOME/.cup/components/unknown-component"
output=$(run_cup repair)
assert_contains "$output" "Quarantined invalid package '$invalid_package'"
assert_missing "$invalid_package"
find "$TEST_HOME/.cup/recovery" -type d -name package | grep . >/dev/null ||
    fail 'quarantined package was not preserved under recovery'
assert_contains "$output" 'unknown component'
rm -rf "$TEST_HOME/.cup/components/unknown-component"
assert_file "$package_root/info.txt"

# Invalid state is preserved and rebuilt from complete valid package evidence.
chmod u+w "$state_file"
printf 'unexpected.key=value\n' > "$state_file"
output=$(run_cup repair)
assert_contains "$output" 'Preserved invalid state as'
assert_file "$state_file.invalid"
assert_contains "$(cat "$state_file")" 'clang@23.1.0'

# Invalid state must not be guessed while a package transaction is pending: transaction recovery
# happens before package scanning or state preservation.
cp "$state_file" "$TMP_ROOT/state.valid"
chmod u+w "$state_file"
printf 'unexpected.key=value\n' > "$state_file"
cat > "$transaction_file" <<JOURNAL
format=2
operation=install
component=compiler
tool=clang
target_platform=$TEST_PLATFORM
package_version=23.1.0
temporary_name=install-compiler-clang-$TEST_PLATFORM-23.1.0-test
JOURNAL
invalid_state_hash=$(hash_file "$state_file")
run_cup_expect_failure "$TMP_ROOT/ambiguous-state.out" repair
assert_contains "$(cat "$TMP_ROOT/ambiguous-state.out")" \
    'state.txt is missing or invalid while a package transaction is pending'
assert_equals "$(hash_file "$state_file")" "$invalid_state_hash"
assert_file "$transaction_file"
rm -f "$transaction_file"
cp "$TMP_ROOT/state.valid" "$state_file"
rm -f "$state_file.invalid"

# A malformed generation journal is likewise the first blocker; invalid state and staging evidence
# remain byte-identical until the journal can be interpreted.
chmod u+w "$state_file"
printf 'unexpected.key=value\n' > "$state_file"
cat > "$transaction_file" <<'JOURNAL'
format=2
operation=cup-generation
target_release_sha256=not-a-digest
temporary_name=cup-update-malformed
JOURNAL
mkdir -p "$TEST_HOME/.cup/staging/cup-update-malformed"
invalid_state_hash=$(hash_file "$state_file")
invalid_generation_hash=$(hash_file "$transaction_file")
run_cup_expect_failure "$TMP_ROOT/malformed-generation-invalid-state.out" repair
output=$(cat "$TMP_ROOT/malformed-generation-invalid-state.out")
assert_contains "$output" 'cup generation transaction journal is invalid'
assert_equals "$(hash_file "$state_file")" "$invalid_state_hash"
assert_equals "$(hash_file "$transaction_file")" "$invalid_generation_hash"
assert_missing "$state_file.invalid"
[ -d "$TEST_HOME/.cup/staging/cup-update-malformed" ] ||
    fail 'repair removed staging evidence after an invalid generation journal'
rm -rf "$TEST_HOME/.cup/staging/cup-update-malformed" "$transaction_file"
cp "$TMP_ROOT/state.valid" "$state_file"

# A generic malformed journal also preserves all unrelated state/staging evidence.
printf 'not-a-valid-journal\n' > "$transaction_file"
mkdir -p "$TEST_HOME/.cup/staging/ambiguous-data"
state_hash=$(hash_file "$state_file")
run_cup_expect_failure "$TMP_ROOT/invalid-journal.out" repair
output=$(cat "$TMP_ROOT/invalid-journal.out")
assert_contains "$output" 'transaction.txt is invalid'
assert_file "$transaction_file"
assert_equals "$(hash_file "$state_file")" "$state_hash"
[ -d "$TEST_HOME/.cup/staging/ambiguous-data" ] ||
    fail 'repair removed staging data after an invalid journal'
run_cup_expect_failure "$TMP_ROOT/blocked-by-invalid-journal.out" list
assert_contains "$(cat "$TMP_ROOT/blocked-by-invalid-journal.out")" \
    'transaction journal is invalid'
rm -rf "$TEST_HOME/.cup/staging/ambiguous-data" "$transaction_file"

# Without ambiguous transaction evidence, stale staging is deterministic garbage.
mkdir -p "$TEST_HOME/.cup/staging/stale-data"
run_cup repair >/dev/null
assert_missing "$TEST_HOME/.cup/staging/stale-data"

# A package in the correct physical path but declaring a foreign host in info.txt is invalid content
# for this authenticated root and is quarantined rather than adopted as a second host dimension.
make_installed_package debugger lldb 23.2.0 "$TEST_PLATFORM" lldb
foreign_metadata_root=$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/23.2.0
sed "s/^platform.host=$TEST_PLATFORM$/platform.host=windows-x64/" \
    "$foreign_metadata_root/info.txt" > "$foreign_metadata_root/info.txt.tmp"
mv "$foreign_metadata_root/info.txt.tmp" "$foreign_metadata_root/info.txt"
write_package_manifest "$foreign_metadata_root"
output=$(run_cup repair)
assert_contains "$output" "Quarantined invalid package '$foreign_metadata_root'"
assert_missing "$foreign_metadata_root"
assert_not_contains "$(cat "$state_file")" 'lldb@23.2.0'

# Invalid preferences are preserved as evidence; absence afterwards means no user preference.
run_cup config set compiler clang >/dev/null
preferences=$TEST_HOME/.cup/config/preferences.txt
printf 'unexpected.key=value\n' > "$preferences"
output=$(run_cup repair)
assert_contains "$output" 'Preserved invalid preferences as'
assert_missing "$preferences"
assert_file "$preferences.invalid"

# Development repair preserves a malformed catalog without synthesizing a replacement.
catalog=$TEST_HOME/.cup/config/catalog.cfg
printf 'broken catalog\n' > "$catalog"
run_cup_expect_failure "$TMP_ROOT/repair-catalog.out" repair
output=$(cat "$TMP_ROOT/repair-catalog.out")
assert_contains "$output" 'Preserved invalid catalog as'
assert_contains "$output" 'development catalog is unavailable'
assert_file "$catalog.invalid"
assert_missing "$catalog"

# A structurally well-formed future catalog format is not corruption and must not be backed up or
# downgraded to the current seed.
rm -f "$catalog.invalid"
cat > "$catalog" <<'CATALOG'
format=2
revision=999
update_url=https://example.invalid/catalog.cfg
CATALOG
future_catalog_hash=$(hash_file "$catalog")
run_cup_expect_failure "$TMP_ROOT/future-catalog.out" repair
assert_contains "$(cat "$TMP_ROOT/future-catalog.out")" 'newer unsupported format'
assert_equals "$(hash_file "$catalog")" "$future_catalog_hash"
assert_missing "$catalog.invalid"
cp "$DEV_ROOT/config/catalog.cfg" "$catalog"

# A stale pre-detach uninstall journal can be cancelled while repair owns the canonical lock and no
# detached sibling exists. The proven uninstall journal format remains unchanged.
cat > "$transaction_file" <<'JOURNAL'
format=2
operation=uninstall
phase=scheduled
temporary_name=.cup-uninstall-fixture
token=fixture
error=0
JOURNAL
stale_uninstall_helper="$TEST_HOME/.cup-uninstall-helper-fixture"
printf 'stale helper\n' > "$stale_uninstall_helper"
output=$(run_cup repair)
assert_contains "$output" "Cancelled interrupted cup uninstall in phase 'scheduled'."
assert_missing "$transaction_file"
assert_missing "$stale_uninstall_helper"

# A failed detach whose detached sibling is absent can also be explicitly acknowledged.
cat > "$transaction_file" <<'JOURNAL'
format=2
operation=uninstall
phase=failed
temporary_name=.cup-uninstall-fixture
token=fixture
error=6
JOURNAL
output=$(run_cup repair)
assert_contains "$output" 'Acknowledged failed cup uninstall (error 6).'
assert_missing "$transaction_file"
assert_cup_healthy

printf 'Repair integration tests passed for %s.\n' "$TEST_PLATFORM"
