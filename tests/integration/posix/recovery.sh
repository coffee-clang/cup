#!/bin/sh

# Exercises interrupted package and cup-generation recovery at their durable commit boundaries.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin recovery
prepare_command_environment

prepare_bootstrap_source() {
    source_directory=$1
    binary_name="cup-$TEST_PLATFORM"
    mkdir -m 0700 "$source_directory"
    cp "$CUP" "$source_directory/$binary_name"
    cp "$PROJECT_ROOT/LICENSE" "$source_directory/LICENSE"
    cp "$PROJECT_ROOT/scripts/dependencies/THIRD_PARTY_NOTICES.txt" \
        "$source_directory/THIRD_PARTY_NOTICES.txt"
    cp "$PROJECT_ROOT/tests/fixtures/catalog.cfg" "$source_directory/catalog.cfg"
    chmod 0700 "$source_directory/$binary_name"
    chmod 0600 "$source_directory/LICENSE" \
        "$source_directory/THIRD_PARTY_NOTICES.txt" "$source_directory/catalog.cfg"

    version=$(cat "$PROJECT_ROOT/VERSION")
    commit=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
    {
        printf 'format=2\n'
        printf 'version=%s\n' "$version"
        printf 'commit=%s\n' "$commit"
        printf 'root_layout=2\n'
        printf 'catalog_format=1\n'
        printf 'asset_count=4\n'
        printf 'asset.0.name=LICENSE\n'
        printf 'asset.0.sha256=%s\n' "$(hash_file "$source_directory/LICENSE")"
        printf 'asset.1.name=THIRD_PARTY_NOTICES.txt\n'
        printf 'asset.1.sha256=%s\n' \
            "$(hash_file "$source_directory/THIRD_PARTY_NOTICES.txt")"
        printf 'asset.2.name=catalog.cfg\n'
        printf 'asset.2.sha256=%s\n' "$(hash_file "$source_directory/catalog.cfg")"
        printf 'asset.3.name=%s\n' "$binary_name"
        printf 'asset.3.sha256=%s\n' "$(hash_file "$source_directory/$binary_name")"
    } > "$source_directory/release.txt"
    chmod 0600 "$source_directory/release.txt"
}

bootstrap_source=$TMP_ROOT/bootstrap-source
prepare_bootstrap_source "$bootstrap_source"
HOME="$TEST_HOME" "$CUP" --internal-bootstrap "$bootstrap_source" "$TEST_HOME" >/dev/null
assert_cup_healthy

write_package_journal() {
    operation=$1
    component=$2
    tool=$3
    version=$4
    temporary_name=$5

    cat > "$TEST_HOME/.cup/transaction.txt" <<JOURNAL
format=2
operation=$operation
component=$component
tool=$tool
target_platform=$TEST_PLATFORM
package_version=$version
temporary_name=$temporary_name
JOURNAL
}

# If state already committed an installation, repair completes the package move from staging.
make_package compiler clang 23.1.0 "$TEST_PLATFORM" clang
run_cup install compiler clang@23.1.0 >/dev/null
install_path=$TEST_HOME/.cup/components/compiler/clang/$TEST_PLATFORM/23.1.0
install_staging_name=install-compiler-clang-$TEST_PLATFORM-23.1.0-recovery
install_staging=$TEST_HOME/.cup/staging/$install_staging_name
mv "$install_path" "$install_staging"
write_package_journal install compiler clang 23.1.0 "$install_staging_name"
run_cup help >/dev/null
run_cup --version >/dev/null
run_cup_expect_failure "$TMP_ROOT/pending-package-list.out" list
assert_contains "$(cat "$TMP_ROOT/pending-package-list.out")" \
    'a package transaction is active or requires recovery'
run_cup_expect_failure "$TMP_ROOT/pending-package-doctor.out" doctor
assert_contains "$(cat "$TMP_ROOT/pending-package-doctor.out")" \
    'interrupted install transaction detected'
output=$(run_cup repair)
assert_contains "$output" 'Recovered interrupted install transaction for clang@23.1.0.'
assert_file "$install_path/info.txt"
assert_missing "$install_staging"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

# If removal had only staged the package and state still references it, repair rolls it back.
make_package debugger lldb 23.1.0 "$TEST_PLATFORM" lldb
run_cup install debugger lldb@23.1.0 >/dev/null
remove_path=$TEST_HOME/.cup/components/debugger/lldb/$TEST_PLATFORM/23.1.0
remove_staging_name=remove-debugger-lldb-$TEST_PLATFORM-23.1.0-recovery
remove_staging=$TEST_HOME/.cup/staging/$remove_staging_name
mv "$remove_path" "$remove_staging"
write_package_journal remove debugger lldb 23.1.0 "$remove_staging_name"
output=$(run_cup repair)
assert_contains "$output" 'Recovered interrupted remove transaction for lldb@23.1.0.'
assert_file "$remove_path/info.txt"
assert_missing "$remove_staging"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

# A conflicting invalid canonical path is preserved before the valid staged package is restored.
conflict_staging_name=remove-debugger-lldb-$TEST_PLATFORM-23.1.0-conflict
conflict_staging=$TEST_HOME/.cup/staging/$conflict_staging_name
mv "$remove_path" "$conflict_staging"
mkdir -p "$remove_path"
printf 'corrupted package\n' > "$remove_path/info.txt"
write_package_journal remove debugger lldb 23.1.0 "$conflict_staging_name"
output=$(run_cup repair)
assert_contains "$output" 'Preserved invalid package path as'
assert_contains "$output" 'Recovered interrupted remove transaction for lldb@23.1.0.'
assert_file "$remove_path/info.txt"
assert_contains "$(cat "$remove_path/info.txt")" 'package.component=debugger'
assert_missing "$conflict_staging"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

binary_release_name="cup-$TEST_PLATFORM"
root=$TEST_HOME/.cup

write_generation_release() {
    directory=$1
    license_text=$2
    notices_text=$3
    binary_source=$4
    mkdir -p "$directory"
    printf '%s\n' "$license_text" > "$directory/LICENSE"
    printf '%s\n' "$notices_text" > "$directory/THIRD_PARTY_NOTICES.txt"
    cp "$binary_source" "$directory/$binary_release_name"
    chmod 0700 "$directory/$binary_release_name"
    version=$(cat "$PROJECT_ROOT/VERSION")
    commit=$(git -C "$PROJECT_ROOT" rev-parse HEAD)
    {
        printf 'format=2\n'
        printf 'version=%s\n' "$version"
        printf 'commit=%s\n' "$commit"
        printf 'root_layout=2\n'
        printf 'catalog_format=1\n'
        printf 'asset_count=3\n'
        printf 'asset.0.name=LICENSE\n'
        printf 'asset.0.sha256=%s\n' "$(hash_file "$directory/LICENSE")"
        printf 'asset.1.name=THIRD_PARTY_NOTICES.txt\n'
        printf 'asset.1.sha256=%s\n' \
            "$(hash_file "$directory/THIRD_PARTY_NOTICES.txt")"
        printf 'asset.2.name=%s\n' "$binary_release_name"
        printf 'asset.2.sha256=%s\n' "$(hash_file "$directory/$binary_release_name")"
    } > "$directory/release.txt"
}

snapshot_generation() {
    destination=$1
    mkdir -p "$destination"
    cp "$root/release.txt" "$destination/release.txt"
    cp "$root/LICENSE" "$destination/LICENSE"
    cp "$root/THIRD_PARTY_NOTICES.txt" "$destination/THIRD_PARTY_NOTICES.txt"
    cp "$root/bin/cup" "$destination/$binary_release_name"
}

prepare_generation_transaction() {
    name=$1
    license_text=$2
    notices_text=$3
    staging=$root/staging/$name
    new_dir=$staging/new
    old_dir=$staging/old
    rm -rf "$staging"
    mkdir -p "$new_dir"
    write_generation_release "$new_dir" "$license_text" "$notices_text" "$root/bin/cup"
    snapshot_generation "$old_dir"
    target_release_sha=$(hash_file "$new_dir/release.txt")
    cat > "$root/transaction.txt" <<JOURNAL
format=2
operation=cup-generation
target_release_sha256=$target_release_sha
temporary_name=$name
JOURNAL
}

install_generation_asset() {
    new_dir=$1
    name=$2
    case "$name" in
        "$binary_release_name")
            cp "$new_dir/$name" "$root/bin/cup"
            chmod 0755 "$root/bin/cup"
            ;;
        release.txt|LICENSE|THIRD_PARTY_NOTICES.txt)
            chmod u+w "$root/$name" 2>/dev/null || true
            cp "$new_dir/$name" "$root/$name"
            chmod 0444 "$root/$name"
            ;;
        *) fail "unknown generation asset: $name" ;;
    esac
}

# Before binary-last commit, repair may roll non-binary assets back because the canonical binary
# still proves the old generation.
rollback_name=cup-update-recovery-rollback
prepare_generation_transaction "$rollback_name" 'target-license-a' 'target-notices-a'
rollback_staging=$root/staging/$rollback_name
rollback_new=$rollback_staging/new
old_release_hash=$(hash_file "$rollback_staging/old/release.txt")
old_license_hash=$(hash_file "$rollback_staging/old/LICENSE")
old_notices_hash=$(hash_file "$rollback_staging/old/THIRD_PARTY_NOTICES.txt")
old_binary_hash=$(hash_file "$rollback_staging/old/$binary_release_name")
install_generation_asset "$rollback_new" LICENSE
install_generation_asset "$rollback_new" release.txt
output=$(run_cup repair)
assert_contains "$output" 'Rolled back interrupted cup generation transaction.'
assert_equals "$(hash_file "$root/release.txt")" "$old_release_hash"
assert_equals "$(hash_file "$root/LICENSE")" "$old_license_hash"
assert_equals "$(hash_file "$root/THIRD_PARTY_NOTICES.txt")" "$old_notices_hash"
assert_equals "$(hash_file "$root/bin/cup")" "$old_binary_hash"
assert_missing "$root/transaction.txt"
assert_missing "$rollback_staging"
assert_cup_healthy

# Once every canonical generation byte equals the frozen target, repair finalizes by clearing the
# journal/workspace rather than rolling anything back.
finalize_name=cup-update-recovery-finalize
prepare_generation_transaction "$finalize_name" 'target-license-b' 'target-notices-b'
finalize_staging=$root/staging/$finalize_name
finalize_new=$finalize_staging/new
for generation_asset in LICENSE THIRD_PARTY_NOTICES.txt release.txt "$binary_release_name"; do
    install_generation_asset "$finalize_new" "$generation_asset"
done
target_release_hash=$(hash_file "$root/release.txt")
target_license_hash=$(hash_file "$root/LICENSE")
target_notices_hash=$(hash_file "$root/THIRD_PARTY_NOTICES.txt")
target_binary_hash=$(hash_file "$root/bin/cup")
output=$(run_cup repair)
assert_contains "$output" 'Completed interrupted cup generation transaction.'
assert_equals "$(hash_file "$root/release.txt")" "$target_release_hash"
assert_equals "$(hash_file "$root/LICENSE")" "$target_license_hash"
assert_equals "$(hash_file "$root/THIRD_PARTY_NOTICES.txt")" "$target_notices_hash"
assert_equals "$(hash_file "$root/bin/cup")" "$target_binary_hash"
assert_missing "$root/transaction.txt"
assert_missing "$finalize_staging"
assert_cup_healthy

# A third binary is ambiguous evidence. Repair must not guess, mutate the generation, or discard
# the journal/workspace. Restore the exact old snapshot manually only after proving preservation.
ambiguous_name=cup-update-recovery-ambiguous
prepare_generation_transaction "$ambiguous_name" 'target-license-c' 'target-notices-c'
ambiguous_staging=$root/staging/$ambiguous_name
ambiguous_old=$ambiguous_staging/old
printf 'third-binary\n' > "$root/bin/cup"
chmod 0755 "$root/bin/cup"
third_binary_hash=$(hash_file "$root/bin/cup")
run_cup_expect_failure "$TMP_ROOT/ambiguous-generation-repair.out" repair
output=$(cat "$TMP_ROOT/ambiguous-generation-repair.out")
assert_contains "$output" 'interrupted operation cannot be repaired safely'
assert_equals "$(hash_file "$root/bin/cup")" "$third_binary_hash"
assert_file "$root/transaction.txt"
assert_file "$ambiguous_staging/new/release.txt"
assert_file "$ambiguous_old/$binary_release_name"

# Reset the isolated fixture after verifying evidence preservation.
cp "$ambiguous_old/release.txt" "$root/release.txt"
cp "$ambiguous_old/LICENSE" "$root/LICENSE"
cp "$ambiguous_old/THIRD_PARTY_NOTICES.txt" "$root/THIRD_PARTY_NOTICES.txt"
cp "$ambiguous_old/$binary_release_name" "$root/bin/cup"
chmod 0444 "$root/release.txt" "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.txt"
chmod 0755 "$root/bin/cup"
rm -f "$root/transaction.txt"
rm -rf "$ambiguous_staging"
assert_cup_healthy

printf 'Recovery tests passed for %s.\n' "$TEST_PLATFORM"
