#!/usr/bin/env bash

# Purpose: Verifies that generated POSIX installers explain release metadata failures.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cup-installer-metadata.XXXXXX")"
VERSION=0.2.0
TAG=v0.2.0
SHA=0123456789abcdef0123456789abcdef01234567

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

# Prepare the same generated installer shape used by an official release.
. "$ROOT/scripts/release/common.sh"
prepare_installer "$ROOT/scripts/install/install-cup.sh" "$WORK/install.sh"
chmod +x "$WORK/install.sh"

mkdir -p "$WORK/mock-bin"
cat > "$WORK/mock-bin/curl" <<'MOCK'
#!/usr/bin/env sh
set -eu
output=
url=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o)
            output=$2
            shift 2
            ;;
        http://*|https://*)
            url=$1
            shift
            ;;
        *)
            shift
            ;;
    esac
done
[ -n "$output" ] && [ -n "$url" ]
cp "$CUP_FIXTURE/${url##*/}" "$output"
MOCK
chmod +x "$WORK/mock-bin/curl"

# Execute the generated installer with optional host text utilities unavailable.
# A successful installation below demonstrates the actual public behavior.
for blocked_command in awk find grep sed wc tr cat basename; do
    cat > "$WORK/mock-bin/$blocked_command" <<'MOCK_BLOCKED'
#!/usr/bin/env sh
printf 'unexpected installer command: %s\n' "${0##*/}" >&2
exit 97
MOCK_BLOCKED
    chmod +x "$WORK/mock-bin/$blocked_command"
done

hash_file_local() {
    sha256sum "$1" | awk '{print $1}'
}

prepare_fixture() {
    directory=$1
    metadata_file=$2
    mkdir -p "$directory"
    printf 'test binary\n' > "$directory/cup-linux-x64"
    printf '#!/usr/bin/env sh\nexit 0\n' > "$directory/uninstall.sh"
    printf 'format=1\n' > "$directory/packages.cfg"
    printf 'format=1\n' > "$directory/install.cfg"
    cp "$metadata_file" "$directory/release.txt"

    {
        printf '%s  cup-linux-x64\n' "$(hash_file_local "$directory/cup-linux-x64")"
        printf '%s  uninstall.sh\n' "$(hash_file_local "$directory/uninstall.sh")"
        printf '%s  release.txt\n' "$(hash_file_local "$directory/release.txt")"
    } > "$directory/SHA256SUMS.linux-x64"

    package_hash=$(hash_file_local "$directory/packages.cfg")
    install_hash=$(hash_file_local "$directory/install.cfg")
    {
        printf '%s  packages.cfg\n' "$package_hash"
        printf '%s  install.cfg\n' "$install_hash"
        printf '%s  install.sh\n' "$package_hash"
        printf '%s  install.ps1\n' "$install_hash"
    } > "$directory/SHA256SUMS.common"
}

run_success_case() {
    label=$1
    shift
    metadata="$WORK/valid-release-$label.txt"
    fixture="$WORK/valid-release-$label-fixture"
    home="$WORK/valid-release-$label-home"

    cat > "$metadata" <<EOF_METADATA
format=1
version=$VERSION
commit=$SHA
EOF_METADATA
    prepare_fixture "$fixture" "$metadata"
    mkdir -p "$home"

    output=$(
        unset SHELL
        HOME="$home" \
        PATH="$WORK/mock-bin:$PATH" \
        CUP_FIXTURE="$fixture" \
        CUP_INSTALL_BASE_URL=https://example.invalid \
        CUP_INSTALL_NO_PATH_PROMPT=1 \
            "$@" "$WORK/install.sh" 2>&1
    )

    printf '%s\n' "$output" | grep -F 'cup installed successfully.' >/dev/null || {
        printf '%s\n' "$output" >&2
        fail "valid release fixture did not complete installation with $label"
    }

    root="$home/.cup"
    marker="$WORK/expected-installed-marker-$label.txt"
    cat > "$marker" <<'MARKER'
format=1
product=coffee-clang/cup
layout=1
MARKER
    cmp "$marker" "$root/root.txt" >/dev/null ||
        fail "successful $label installation did not create the exact root marker"
    cmp "$root/bin/cup" "$root/helpers/cup-update-helper" >/dev/null ||
        fail "successful $label installation produced a different update helper"
    cmp "$fixture/packages.cfg" "$root/config/packages.cfg" >/dev/null ||
        fail "successful $label installation changed the package catalog"
    cmp "$fixture/install.cfg" "$root/config/install.cfg" >/dev/null ||
        fail "successful $label installation changed the install policy"
    [ -x "$root/bin/cup" ] || fail "successful $label installation left cup non-executable"
    [ -x "$root/helpers/cup-update-helper" ] ||
        fail "successful $label installation left the update helper non-executable"
    [ ! -e "$root/.bootstrap" ] || fail "successful $label installation left bootstrap staging"
    [ ! -e "$home/.coffee-cup" ] || fail "successful $label installation selected the fallback root"
}

run_failure_case() {
    name=$1
    expected=$2
    metadata="$WORK/$name.txt"
    fixture="$WORK/$name-fixture"
    home="$WORK/$name-home"
    shift 2

    printf '%s\n' "$@" > "$metadata"
    prepare_fixture "$fixture" "$metadata"
    mkdir -p "$home"

    set +e
    output=$(
        HOME="$home" \
        PATH="$WORK/mock-bin:$PATH" \
        CUP_FIXTURE="$fixture" \
        CUP_INSTALL_BASE_URL=https://example.invalid \
        CUP_INSTALL_NO_PATH_PROMPT=1 \
            sh "$WORK/install.sh" 2>&1
    )
    status=$?
    set -e

    if [ "$status" -eq 0 ]; then
        printf '%s\n' "$output" >&2
        fail "metadata diagnostic case unexpectedly succeeded: $name"
    fi
    printf '%s\n' "$output" | grep -F "$expected" >/dev/null || {
        printf '%s\n' "$output" >&2
        fail "metadata diagnostic case did not explain '$name'"
    }
}

run_success_case default-sh sh
if command -v dash >/dev/null 2>&1; then
    run_success_case dash dash
fi
if command -v busybox >/dev/null 2>&1; then
    run_success_case busybox-sh busybox sh
fi

# Root selection must stop before downloading when a recognizable CUP root has a corrupt marker.
corrupt_home="$WORK/corrupt-root-home"
mkdir -p "$corrupt_home/.cup/components" "$corrupt_home/.cup/staging" \
    "$corrupt_home/.cup/cache"
printf 'format=1\n' > "$corrupt_home/.cup/state.txt"
printf 'corrupt\n' > "$corrupt_home/.cup/root.txt"
corrupt_state_hash=$(hash_file_local "$corrupt_home/.cup/state.txt")
corrupt_marker_hash=$(hash_file_local "$corrupt_home/.cup/root.txt")
set +e
corrupt_output=$(HOME="$corrupt_home" PATH="$WORK/mock-bin:$PATH" \
    CUP_FIXTURE="$WORK/missing-fixture" \
    CUP_INSTALL_BASE_URL=https://example.invalid \
    CUP_INSTALL_NO_PATH_PROMPT=1 sh "$WORK/install.sh" 2>&1)
corrupt_status=$?
set -e
[ "$corrupt_status" -ne 0 ] || fail 'installer accepted a recognized root with corrupt marker'
printf '%s\n' "$corrupt_output" | grep -F \
    'cup root marker is invalid for the recognized root' >/dev/null
[ "$(hash_file_local "$corrupt_home/.cup/state.txt")" = "$corrupt_state_hash" ] ||
    fail 'installer modified state below the corrupt root'
[ "$(hash_file_local "$corrupt_home/.cup/root.txt")" = "$corrupt_marker_hash" ] ||
    fail 'installer modified the corrupt root marker'
[ ! -e "$corrupt_home/.coffee-cup" ] ||
    fail 'installer created the alternative root after a corrupt marker'

# A complete markerless generation is adopted even when its catalog and policy contain comments
# and blank lines. Public installers must ignore those lines without invoking optional text tools.
legacy_home="$WORK/verified-legacy-home"
legacy_root="$legacy_home/.cup"
mkdir -p "$legacy_root/bin" "$legacy_root/components" "$legacy_root/staging" \
    "$legacy_root/cache" "$legacy_root/config" "$legacy_root/helpers"
printf 'legacy cup binary\n' > "$legacy_root/bin/cup"
cp "$legacy_root/bin/cup" "$legacy_root/helpers/cup-update-helper"
printf '#!/usr/bin/env sh\nexit 0\n' > "$legacy_root/helpers/uninstall.sh"
chmod 0755 "$legacy_root/bin/cup" "$legacy_root/helpers/cup-update-helper"
chmod 0555 "$legacy_root/helpers/uninstall.sh"
cat > "$legacy_root/config/packages.cfg" <<'CATALOG'
# A comment before the first tuple is valid.

compiler.clang.linux-x64.linux-x64.stable_version=22.1.5
compiler.clang.linux-x64.linux-x64.available_versions=22.1.5
compiler.clang.linux-x64.linux-x64.default_format=tar.gz
compiler.clang.linux-x64.linux-x64.formats=tar.xz,tar.gz,zip
compiler.clang.linux-x64.linux-x64.url_template=https://example.invalid/clang-{version}-{host_platform}-{target_platform}.{format}
compiler.clang.linux-x64.linux-x64.checksum_url_template=https://example.invalid/clang-{version}-{host_platform}-{target_platform}/SHA256SUMS

# A trailing comment is also ignored.
CATALOG
cat > "$legacy_root/config/install.cfg" <<'POLICY'
# Official policy comments and spacing are not records.
format=1

default.linux-x64.linux-x64.compiler=clang

profile.minimal=compiler
# Toolchains remain explicit.
toolchain.llvm=clang
POLICY
legacy_binary_hash=$(hash_file_local "$legacy_root/bin/cup")
legacy_uninstall_hash=$(hash_file_local "$legacy_root/helpers/uninstall.sh")
legacy_catalog_hash=$(hash_file_local "$legacy_root/config/packages.cfg")
legacy_policy_hash=$(hash_file_local "$legacy_root/config/install.cfg")
zeros=0000000000000000000000000000000000000000000000000000000000000000
cat > "$legacy_root/config/SHA256SUMS.linux-x64" <<EOF_PLATFORM_SUMS
$legacy_binary_hash  cup-linux-x64
$legacy_uninstall_hash  uninstall.sh
$zeros  release.txt
EOF_PLATFORM_SUMS
cat > "$legacy_root/config/SHA256SUMS.common" <<EOF_COMMON_SUMS
$legacy_catalog_hash  packages.cfg
$legacy_policy_hash  install.cfg
$zeros  install.sh
$zeros  install.ps1
EOF_COMMON_SUMS
set +e
legacy_output=$(HOME="$legacy_home" PATH="$WORK/mock-bin:$PATH" \
    CUP_FIXTURE="$WORK/missing-fixture" \
    CUP_INSTALL_BASE_URL=https://example.invalid \
    CUP_INSTALL_NO_PATH_PROMPT=1 sh "$WORK/install.sh" 2>&1)
legacy_status=$?
set -e
[ "$legacy_status" -ne 0 ] || fail 'missing fixture unexpectedly completed legacy installation'
printf '%s\n' "$legacy_output" | grep -F 'failed to download' >/dev/null
cat > "$WORK/expected-root-marker.txt" <<'MARKER'
format=1
product=coffee-clang/cup
layout=1
MARKER
cmp "$WORK/expected-root-marker.txt" "$legacy_root/root.txt" >/dev/null ||
    fail 'verified legacy root was not adopted with the exact marker'
[ ! -e "$legacy_home/.coffee-cup" ] || fail 'verified legacy root selected the fallback root'
[ "$(hash_file_local "$legacy_root/config/packages.cfg")" = "$legacy_catalog_hash" ] ||
    fail 'legacy catalog was modified during recognition'
[ "$(hash_file_local "$legacy_root/config/install.cfg")" = "$legacy_policy_hash" ] ||
    fail 'legacy install policy was modified during recognition'

# A pending uninstall residue without a CUP root marker is never owned strongly enough to delete.
residue_home="$WORK/unowned-residue-home"
residue="$residue_home/.cup-uninstall.fixture"
mkdir -p "$residue/bin"
printf 'binary\n' > "$residue/bin/cup"
cat > "$residue/transaction.txt" <<'JOURNAL'
format=1
operation=uninstall
phase=failed
temporary_name=.cup-uninstall.fixture
token=fixture
stage=cleanup
error=1
JOURNAL
set +e
residue_output=$(HOME="$residue_home" PATH="$WORK/mock-bin:$PATH" \
    CUP_FIXTURE="$WORK/missing-fixture" \
    CUP_INSTALL_BASE_URL=https://example.invalid \
    CUP_INSTALL_NO_PATH_PROMPT=1 sh "$WORK/install.sh" 2>&1)
residue_status=$?
set -e
[ "$residue_status" -ne 0 ] || fail 'installer deleted an unowned uninstall residue'
printf '%s\n' "$residue_output" | grep -F \
    'unrecognized uninstall residue was preserved' >/dev/null
[ -f "$residue/bin/cup" ] || fail 'installer removed the unowned residue binary'
[ -f "$residue/transaction.txt" ] || fail 'installer removed the unowned residue journal'

# A direct sibling residue with a valid root marker, canonical binary and coherent uninstall
# journal is owned strongly enough for the installer to remove before beginning a new install.
owned_home="$WORK/owned-residue-home"
owned_residue="$owned_home/.cup-uninstall.fixture"
mkdir -p "$owned_residue/bin"
printf 'binary\n' > "$owned_residue/bin/cup"
cat > "$owned_residue/root.txt" <<'MARKER'
format=1
product=coffee-clang/cup
layout=1
MARKER
cat > "$owned_residue/transaction.txt" <<'JOURNAL'
format=1
operation=uninstall
phase=failed
temporary_name=.cup-uninstall.fixture
token=fixture
stage=cleanup
error=1
JOURNAL
set +e
HOME="$owned_home" PATH="$WORK/mock-bin:$PATH" \
    CUP_FIXTURE="$WORK/missing-fixture" \
    CUP_INSTALL_BASE_URL=https://example.invalid \
    CUP_INSTALL_NO_PATH_PROMPT=1 sh "$WORK/install.sh" >/dev/null 2>&1
owned_status=$?
set -e
[ "$owned_status" -ne 0 ] || fail 'missing fixture unexpectedly completed installation'
[ ! -e "$owned_residue" ] || fail 'installer preserved a validated uninstall residue'

run_failure_case malformed-line \
    "release metadata line 2 must contain exactly one non-empty 'key=value' assignment" \
    'format=1' 'version' "commit=$SHA"
run_failure_case unexpected-field \
    'release metadata contains an unexpected field at line 1' \
    'channel=stable' "version=$VERSION" "commit=$SHA"
run_failure_case duplicate-field \
    "release metadata field 'version' is duplicated" \
    'format=1' "version=$VERSION" "version=$VERSION"
run_failure_case missing-field \
    "release metadata is missing required field 'commit'" \
    'format=1' "version=$VERSION"
run_failure_case unsupported-format \
    "release metadata format is unsupported; expected '1'" \
    'format=2' "version=$VERSION" "commit=$SHA"
run_failure_case invalid-version \
    "release metadata version is invalid; expected 'MAJOR.MINOR.PATCH'" \
    'format=1' 'version=0.2' "commit=$SHA"
run_failure_case invalid-commit \
    'release metadata commit is invalid; expected 7 to 40 lowercase hexadecimal characters' \
    'format=1' "version=$VERSION" 'commit=ARCHIVE'
run_failure_case version-mismatch \
    "release metadata version mismatch: expected '$VERSION', received '0.2.1'" \
    'format=1' 'version=0.2.1' "commit=$SHA"
run_failure_case commit-mismatch \
    "release metadata commit mismatch: expected '$SHA', received 'fedcba9876543210fedcba9876543210fedcba98'" \
    'format=1' "version=$VERSION" 'commit=fedcba9876543210fedcba9876543210fedcba98'

printf 'Installer release metadata diagnostic tests passed.\n'
