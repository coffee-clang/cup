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
