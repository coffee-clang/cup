#!/bin/sh

# Exercises native bootstrap from one private verified release subset into either a fresh
# private sibling root publication or a synchronous existing-root generation replacement.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin bootstrap
prepare_command_environment

prepare_source() {
    source_directory=$1
    binary_name="cup-$TEST_PLATFORM"
    mkdir -m 0700 "$source_directory"
    cp "$CUP" "$source_directory/$binary_name"
    cp "$PROJECT_ROOT/LICENSE" "$source_directory/LICENSE"
    cp "$PROJECT_ROOT/scripts/dependencies/THIRD_PARTY_NOTICES.txt" "$source_directory/THIRD_PARTY_NOTICES.txt"
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
        printf 'asset.1.sha256=%s\n' "$(hash_file "$source_directory/THIRD_PARTY_NOTICES.txt")"
        printf 'asset.2.name=catalog.cfg\n'
        printf 'asset.2.sha256=%s\n' "$(hash_file "$source_directory/catalog.cfg")"
        printf 'asset.3.name=%s\n' "$binary_name"
        printf 'asset.3.sha256=%s\n' "$(hash_file "$source_directory/$binary_name")"
    } > "$source_directory/release.txt"
    chmod 0600 "$source_directory/release.txt"
}

source_directory=$TMP_ROOT/source
prepare_source "$source_directory"
HOME="$TEST_HOME" "$CUP" --internal-bootstrap "$source_directory" "$TEST_HOME" \
    > "$TMP_ROOT/bootstrap.out"
assert_contains "$(cat "$TMP_ROOT/bootstrap.out")" 'Verified cup'
assert_contains "$(cat "$TMP_ROOT/bootstrap.out")" 'CUP_BOOTSTRAP_ROOT='

root=$TEST_HOME/.cup
assert_file "$root/root.txt"
assert_file "$root/cup.lock"
assert_file "$root/state.txt"
assert_file "$root/bin/cup"
assert_file "$root/release.txt"
assert_file "$root/LICENSE"
assert_file "$root/THIRD_PARTY_NOTICES.txt"
assert_file "$root/config/catalog.cfg"
assert_missing "$root/transaction.txt"
assert_missing "$root/helpers/update-helper"
[ "$(find "$root/staging" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')" -eq 0 ] ||
    fail 'successful fresh bootstrap left staging residue'
[ "$(find "$TEST_HOME" -maxdepth 1 -type d -name '.cup-install-*' | wc -l | tr -d '[:space:]')" -eq 0 ] ||
    fail 'successful fresh bootstrap left private-root residue'
cmp "$CUP" "$root/bin/cup" >/dev/null || fail 'bootstrap changed the binary bytes'
cmp "$source_directory/LICENSE" "$root/LICENSE" >/dev/null || fail 'bootstrap changed LICENSE bytes'
cmp "$source_directory/THIRD_PARTY_NOTICES.txt" "$root/THIRD_PARTY_NOTICES.txt" >/dev/null ||
    fail 'bootstrap changed notices bytes'
HOME="$TEST_HOME" "$root/bin/cup" --version >/dev/null

# Existing-root reinstall is synchronous and preserves non-generation runtime state.
HOME="$TEST_HOME" "$root/bin/cup" config set compiler clang >/dev/null
preference_before=$(hash_file "$root/config/preferences.txt")
state_before=$(hash_file "$root/state.txt")
catalog_before=$(hash_file "$root/config/catalog.cfg")
mkdir -m 0700 "$root/cache"
printf 'keep-me\n' > "$root/cache/preserved-object"
cache_before=$(hash_file "$root/cache/preserved-object")

second_source=$TMP_ROOT/second-source
prepare_source "$second_source"
HOME="$TEST_HOME" "$CUP" --internal-bootstrap "$second_source" "$TEST_HOME" \
    > "$TMP_ROOT/reinstall.out"
assert_contains "$(cat "$TMP_ROOT/reinstall.out")" 'Verified cup'
assert_missing "$root/transaction.txt"
[ "$(find "$root/staging" -mindepth 1 -maxdepth 1 | wc -l | tr -d '[:space:]')" -eq 0 ] ||
    fail 'successful existing-root reinstall left staging residue'
[ "$(hash_file "$root/config/preferences.txt")" = "$preference_before" ] ||
    fail 'existing-root reinstall changed preferences'
[ "$(hash_file "$root/state.txt")" = "$state_before" ] ||
    fail 'existing-root reinstall changed state'
[ "$(hash_file "$root/config/catalog.cfg")" = "$catalog_before" ] ||
    fail 'existing-root reinstall replaced a valid live catalog'
[ "$(hash_file "$root/cache/preserved-object")" = "$cache_before" ] ||
    fail 'existing-root reinstall changed cache state'

# Reinstall preserves an unsupported future catalog and refuses the operation.
future_base=$TMP_ROOT/future-base
mkdir -m 0700 "$future_base"
future_source=$TMP_ROOT/future-source
prepare_source "$future_source"
HOME="$future_base" "$CUP" --internal-bootstrap "$future_source" "$future_base" >/dev/null
future_root=$future_base/.cup
printf 'format=2\nrevision=9\nupdate_url=https://example.invalid/catalog.cfg\n' > "$future_root/config/catalog.cfg"
future_catalog_hash=$(hash_file "$future_root/config/catalog.cfg")
future_binary_hash=$(hash_file "$future_root/bin/cup")
if HOME="$future_base" "$CUP" --internal-bootstrap "$future_source" "$future_base" \
        > "$TMP_ROOT/future-reinstall.out" 2>&1; then
    fail 'existing-root bootstrap downgraded a future catalog'
fi
[ "$(hash_file "$future_root/config/catalog.cfg")" = "$future_catalog_hash" ] ||
    fail 'existing-root bootstrap changed a future catalog'
[ "$(hash_file "$future_root/bin/cup")" = "$future_binary_hash" ] ||
    fail 'future-catalog refusal changed the installed binary'
assert_missing "$future_root/config/catalog.cfg.invalid"

# Reinstall preserves malformed catalog evidence and restores the release snapshot.
malformed_base=$TMP_ROOT/malformed-base
mkdir -m 0700 "$malformed_base"
malformed_source=$TMP_ROOT/malformed-source
prepare_source "$malformed_source"
HOME="$malformed_base" "$CUP" --internal-bootstrap "$malformed_source" "$malformed_base" >/dev/null
malformed_root=$malformed_base/.cup
printf 'invalid=1\n' > "$malformed_root/config/catalog.cfg"
HOME="$malformed_base" "$CUP" --internal-bootstrap "$malformed_source" "$malformed_base" \
    > "$TMP_ROOT/malformed-reinstall.out"
cmp "$malformed_source/catalog.cfg" "$malformed_root/config/catalog.cfg" >/dev/null ||
    fail 'existing-root bootstrap did not restore the authenticated catalog seed'
assert_file "$malformed_root/config/catalog.cfg.invalid"
grep -Fx 'invalid=1' "$malformed_root/config/catalog.cfg.invalid" >/dev/null ||
    fail 'existing-root bootstrap did not preserve malformed catalog evidence'

# Reinstall restores a missing managed binary.
repair_base=$TMP_ROOT/binary-repair-base
mkdir -m 0700 "$repair_base"
repair_source=$TMP_ROOT/binary-repair-source
prepare_source "$repair_source"
HOME="$repair_base" "$CUP" --internal-bootstrap "$repair_source" "$repair_base" >/dev/null
repair_root=$repair_base/.cup
rm -f "$repair_root/bin/cup"
HOME="$repair_base" "$CUP" --internal-bootstrap "$repair_source" "$repair_base" >/dev/null
cmp "$repair_source/cup-$TEST_PLATFORM" "$repair_root/bin/cup" >/dev/null ||
    fail 'existing-root bootstrap did not restore a missing managed binary'

# Reinstall refuses a newer installed generation.
downgrade_base=$TMP_ROOT/downgrade-base
mkdir -m 0700 "$downgrade_base"
downgrade_source=$TMP_ROOT/downgrade-source
prepare_source "$downgrade_source"
HOME="$downgrade_base" "$CUP" --internal-bootstrap "$downgrade_source" "$downgrade_base" >/dev/null
downgrade_root=$downgrade_base/.cup
sed 's/^version=.*/version=999.0.0/' "$downgrade_root/release.txt" > "$downgrade_root/release.txt.tmp"
mv "$downgrade_root/release.txt.tmp" "$downgrade_root/release.txt"
downgrade_binary_hash=$(hash_file "$downgrade_root/bin/cup")
if HOME="$downgrade_base" "$CUP" --internal-bootstrap "$downgrade_source" "$downgrade_base" \
        > "$TMP_ROOT/downgrade.out" 2>&1; then
    fail 'existing-root bootstrap accepted a healthy generation downgrade'
fi
grep -F 'downgrade refused' "$TMP_ROOT/downgrade.out" >/dev/null ||
    fail 'existing-root bootstrap did not explain downgrade refusal'
[ "$(hash_file "$downgrade_root/bin/cup")" = "$downgrade_binary_hash" ] ||
    fail 'downgrade refusal changed the installed binary'

# A quiescent complete root remains relocatable. Existing-root bootstrap uses the selected
# relocated root and does not recreate the old HOME path.
relocated_base=$TMP_ROOT/relocated-base
relocated_root=$relocated_base/.cup
mkdir -m 0700 "$relocated_base"
mv "$root" "$relocated_root"
HOME="$TEST_HOME" "$relocated_root/bin/cup" --version >/dev/null
assert_missing "$TEST_HOME/.cup"
third_source=$TMP_ROOT/third-source
prepare_source "$third_source"
HOME="$TEST_HOME" "$CUP" --internal-bootstrap "$third_source" "$relocated_base" \
    > "$TMP_ROOT/relocated-reinstall.out"
assert_missing "$TEST_HOME/.cup"
assert_file "$relocated_root/bin/cup"
assert_missing "$relocated_root/transaction.txt"

# Exact-set and authenticated-byte failures occur before any root mutation.
invalid_home=$TMP_ROOT/invalid-home
mkdir -m 0700 "$invalid_home"
invalid_source=$TMP_ROOT/invalid-source
prepare_source "$invalid_source"
printf 'extra\n' > "$invalid_source/extra.txt"
if HOME="$invalid_home" "$CUP" --internal-bootstrap "$invalid_source" "$invalid_home" \
        > "$TMP_ROOT/extra.out" 2>&1; then
    fail 'bootstrap accepted an extra transport source entry'
fi
assert_missing "$invalid_home/.cup"
assert_missing "$invalid_home/.coffee-cup"

rm -f "$invalid_source/extra.txt"
printf 'tampered\n' >> "$invalid_source/catalog.cfg"
if HOME="$invalid_home" "$CUP" --internal-bootstrap "$invalid_source" "$invalid_home" \
        > "$TMP_ROOT/tampered.out" 2>&1; then
    fail 'bootstrap accepted a tampered authenticated source asset'
fi
assert_missing "$invalid_home/.cup"
assert_missing "$invalid_home/.coffee-cup"

printf 'Bootstrap integration tests passed.\n'
