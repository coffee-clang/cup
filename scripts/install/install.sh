#!/usr/bin/env sh

# Downloads and verifies one immutable release generation, then delegates installation
# to the verified cup executable. The installer owns transport; cup owns state and recovery.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG
umask 077

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
CUP_RELEASE_VERSION="@CUP_RELEASE_VERSION@"
CUP_RELEASE_TAG="@CUP_RELEASE_TAG@"
CUP_RELEASE_COMMIT="@CUP_RELEASE_COMMIT@"
DEFAULT_BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/${CUP_RELEASE_TAG}"
if [ -n "${CUP_INSTALL_BASE_URL:-}" ]; then
    BASE_URL=$CUP_INSTALL_BASE_URL
    BASE_URL_OVERRIDDEN=1
else
    BASE_URL=$DEFAULT_BASE_URL
    BASE_URL_OVERRIDDEN=0
fi
WAIT_ATTEMPTS=${CUP_INSTALL_WAIT_ATTEMPTS:-120}
MAX_BINARY_BYTES=268435456
MAX_TEXT_BYTES=16777216
WORK=

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    [ -z "$WORK" ] || rm -rf -- "$WORK"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

validate_identity() {
    case "$CUP_RELEASE_VERSION$CUP_RELEASE_TAG$CUP_RELEASE_COMMIT" in
        *'@CUP_RELEASE_'*) fail 'installer was not prepared for a concrete release' ;;
    esac
    case "$CUP_RELEASE_VERSION" in
        ''|*[!0-9.]*|.*|*.|*..*) fail 'installer has an invalid release version' ;;
    esac
    old_ifs=$IFS
    IFS=.
    set -- $CUP_RELEASE_VERSION
    IFS=$old_ifs
    [ "$#" -eq 3 ] || fail 'installer has an invalid release version'
    for component in "$@"; do
        case "$component" in
            0|[1-9]|[1-9][0-9]|[1-9][0-9][0-9]|[1-9][0-9][0-9][0-9]|\
                [1-9][0-9][0-9][0-9][0-9]|[1-9][0-9][0-9][0-9][0-9][0-9])
                ;;
            *)
                fail 'installer has an invalid release version'
                ;;
        esac
    done
    [ "$CUP_RELEASE_TAG" = "v$CUP_RELEASE_VERSION" ] ||
        fail 'installer release tag does not match its version'
    [ "${#CUP_RELEASE_COMMIT}" -eq 40 ] || fail 'installer has an invalid release commit'
    case "$CUP_RELEASE_COMMIT" in
        *[!0-9a-f]*)
            fail 'installer has an invalid release commit'
            ;;
    esac
}

validate_base_url() {
    while [ "${BASE_URL%/}" != "$BASE_URL" ]; do
        BASE_URL=${BASE_URL%/}
    done

    if [ "$BASE_URL_OVERRIDDEN" -eq 0 ]; then
        [ "$BASE_URL" = "$DEFAULT_BASE_URL" ] || fail 'installer official release base URL is invalid'
        TRANSPORT_PROTOCOL=https
        MAX_REDIRECTS=10
        export BASE_URL TRANSPORT_PROTOCOL MAX_REDIRECTS
        return 0
    fi

    [ "${CUP_INSTALL_ALLOW_INSECURE:-0}" = 1 ] ||
        fail 'release base URL override is test-only and requires CUP_INSTALL_ALLOW_INSECURE=1'

    case "$BASE_URL" in
        *[![:print:]]*|*[[:space:]]*|*'@'*|*'?'*|*'#'*|*'\'*)
            fail 'installer test release base URL is invalid'
            ;;
        http://*) ;;
        *) fail 'installer release base URL override must use loopback HTTP' ;;
    esac

    remainder=${BASE_URL#http://}
    authority=${remainder%%/*}
    case "$authority" in
        127.0.0.1:*) port=${authority#127.0.0.1:} ;;
        localhost:*) port=${authority#localhost:} ;;
        \[::1\]:*) port=${authority#\[::1\]:} ;;
        *) fail 'installer release base URL override must use an allowed loopback host and explicit port' ;;
    esac
    case "$port" in
        ''|*[!0-9]*) fail 'installer release base URL override has an invalid port' ;;
    esac
    [ "$port" -ge 1 ] 2>/dev/null && [ "$port" -le 65535 ] 2>/dev/null ||
        fail 'installer release base URL override has an invalid port'

    TRANSPORT_PROTOCOL=http
    MAX_REDIRECTS=0
    export BASE_URL TRANSPORT_PROTOCOL MAX_REDIRECTS
}

validate_wait_attempts() {
    case "$WAIT_ATTEMPTS" in
        ''|*[!0-9]*|0)
            fail 'CUP_INSTALL_WAIT_ATTEMPTS is invalid'
            ;;
    esac
    [ "$WAIT_ATTEMPTS" -le 3600 ] || fail 'CUP_INSTALL_WAIT_ATTEMPTS is too large'
}

detect_platform() {
    os=$(uname -s) || fail 'could not detect the operating system'
    arch=$(uname -m) || fail 'could not detect the architecture'
    case "$os" in
        Linux) os=linux ;;
        Darwin) os=macos ;;
        MSYS*|MINGW*|CYGWIN*)
            run_windows_installer
            ;;
        *) fail "unsupported operating system: $os" ;;
    esac
    case "$arch" in
        x86_64|amd64) arch=x64 ;;
        arm64|aarch64) arch=arm64 ;;
        *) fail "unsupported architecture: $arch" ;;
    esac
    PLATFORM=$os-$arch
    case "$PLATFORM" in
        linux-x64|linux-arm64|macos-x64|macos-arm64) ;;
        *) fail "unsupported platform: $PLATFORM" ;;
    esac
    BINARY_ASSET=cup-$PLATFORM
    PLATFORM_SUMS=SHA256SUMS.$PLATFORM
    export PLATFORM BINARY_ASSET PLATFORM_SUMS
}

select_hash_command() {
    if command -v sha256sum >/dev/null 2>&1; then
        HASH_COMMAND=sha256sum
    elif command -v shasum >/dev/null 2>&1; then
        HASH_COMMAND='shasum -a 256'
    else
        fail 'sha256sum or shasum is required'
    fi
    export HASH_COMMAND
}

require_commands() {
    for command_name in chmod cmp curl mktemp rm sleep uname wc; do
        command -v "$command_name" >/dev/null 2>&1 || fail "required command is unavailable: $command_name"
    done
    select_hash_command
}

create_work_directory() {
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/cup-install.XXXXXX") ||
        fail 'could not create the private transport directory'
    chmod 0700 "$WORK" || fail 'could not protect the transport directory'
    case "$WORK" in
        /*)
            ;;
        *)
            fail 'transport directory is not absolute'
            ;;
    esac
}

download_asset() {
    asset=$1
    case "$asset" in
        ''|*/*|*\*|.|..)
            fail "unsafe release asset name: $asset"
            ;;
    esac

    case "$asset" in
        cup-*) maximum=$MAX_BINARY_BYTES ;;
        *) maximum=$MAX_TEXT_BYTES ;;
    esac

    destination=$WORK/$asset
    url=$BASE_URL/$asset
    curl --fail --location --silent --show-error \
        --proto "=$TRANSPORT_PROTOCOL" --proto-redir "=$TRANSPORT_PROTOCOL" \
        --max-redirs "$MAX_REDIRECTS" \
        --connect-timeout 15 --max-time 180 --speed-time 30 --speed-limit 1024 \
        --max-filesize "$maximum" --output "$destination" "$url" ||
        fail "could not download $asset"

    [ -f "$destination" ] && [ ! -L "$destination" ] && [ -s "$destination" ] ||
        fail "downloaded asset is not a non-empty regular file: $asset"
    size=$(wc -c < "$destination") || fail "could not measure $asset"
    case "$size" in
        ''|*[!0-9]*) fail "could not measure $asset" ;;
    esac
    [ "$size" -le "$maximum" ] || fail "downloaded asset is too large: $asset"
}

run_windows_installer() {
    for command_name in chmod cmp curl cygpath grep mktemp powershell.exe rm wc; do
        command -v "$command_name" >/dev/null 2>&1 ||
            fail "required Windows handoff command is unavailable: $command_name"
    done
    select_hash_command
    create_work_directory
    for asset in install.ps1 release.txt SHA256SUMS.windows-x64 \
            SHA256SUMS.common packages.cfg install.cfg install.sh; do
        download_asset "$asset"
    done
    verify_windows_handoff_generation
    validate_release_metadata
    verify_checksum_document "$WORK/SHA256SUMS.common" \
        packages.cfg install.cfg install.sh install.ps1
    grep -F "\$ReleaseVersion = \"$CUP_RELEASE_VERSION\"" "$WORK/install.ps1" >/dev/null ||
        fail 'Windows installer release version does not match the shell installer'
    grep -F "\$ReleaseTag = \"$CUP_RELEASE_TAG\"" "$WORK/install.ps1" >/dev/null ||
        fail 'Windows installer release tag does not match the shell installer'
    grep -F "\$ReleaseCommit = \"$CUP_RELEASE_COMMIT\"" "$WORK/install.ps1" >/dev/null ||
        fail 'Windows installer release commit does not match the shell installer'
    windows_installer=$(cygpath -w "$WORK/install.ps1") ||
        fail 'could not translate the Windows installer path'
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_installer" ||
        fail 'Windows installer failed'
    exit 0
}

hash_file() {
    file=$1
    if [ "$HASH_COMMAND" = sha256sum ]; then
        result=$(sha256sum "$file") || return 1
    else
        result=$(shasum -a 256 "$file") || return 1
    fi
    printf '%s\n' "${result%% *}"
}

validate_hash() {
    [ "${#1}" -eq 64 ] || return 1
    case "$1" in
        *[!0-9a-f]*)
            return 1
            ;;
    esac
}

verify_checksum_document() {
    document=$1
    shift
    exec 3< "$document" || fail "could not read ${document##*/}"
    for expected_name in "$@"; do
        IFS= read -r line <&3 || fail "checksum entry is missing: $expected_name"
        expected_hash=${line%%  *}
        actual_name=${line#*  }
        [ "$line" = "$expected_hash  $actual_name" ] ||
            fail "checksum entry is not canonical: $expected_name"
        validate_hash "$expected_hash" || fail "checksum is invalid: $expected_name"
        [ "$actual_name" = "$expected_name" ] ||
            fail "checksum document has an unexpected asset: $actual_name"
        actual_hash=$(hash_file "$WORK/$actual_name") || fail "could not hash $actual_name"
        [ "$actual_hash" = "$expected_hash" ] || fail "checksum mismatch for $actual_name"
    done
    extra=
    if IFS= read -r extra <&3 || [ -n "$extra" ]; then
        fail "checksum document has unexpected entries: ${document##*/}"
    fi
    exec 3<&-
}

verify_windows_handoff_generation() {
    document=$WORK/SHA256SUMS.windows-x64
    exec 3< "$document" || fail 'could not read SHA256SUMS.windows-x64'
    for expected_name in cup-windows-x64.exe release.txt SHA256SUMS.common; do
        IFS= read -r line <&3 || fail "checksum entry is missing: $expected_name"
        expected_hash=${line%%  *}
        actual_name=${line#*  }
        [ "$line" = "$expected_hash  $actual_name" ] ||
            fail "checksum entry is not canonical: $expected_name"
        validate_hash "$expected_hash" || fail "checksum is invalid: $expected_name"
        [ "$actual_name" = "$expected_name" ] ||
            fail "checksum document has an unexpected asset: $actual_name"
        case "$actual_name" in
            release.txt|SHA256SUMS.common)
                actual_hash=$(hash_file "$WORK/$actual_name") || fail "could not hash $actual_name"
                [ "$actual_hash" = "$expected_hash" ] || fail "checksum mismatch for $actual_name"
                ;;
        esac
    done
    extra=
    if IFS= read -r extra <&3 || [ -n "$extra" ]; then
        fail 'checksum document has unexpected entries: SHA256SUMS.windows-x64'
    fi
    exec 3<&-
}

validate_release_metadata() {
    metadata=$WORK/release.txt
    exec 3< "$metadata" || fail 'could not read release metadata'
    IFS= read -r line1 <&3 || fail 'release metadata is incomplete'
    IFS= read -r line2 <&3 || fail 'release metadata is incomplete'
    IFS= read -r line3 <&3 || fail 'release metadata is incomplete'
    if IFS= read -r extra <&3; then
        fail 'release metadata has unexpected records'
    fi
    exec 3<&-
    printf '%s\n%s\n%s\n' "$line1" "$line2" "$line3" | cmp -s - "$metadata" ||
        fail 'release metadata contains non-canonical bytes or incomplete records'
    [ "$line1" = format=1 ] || fail 'release metadata has an unsupported format'
    [ "$line2" = "version=$CUP_RELEASE_VERSION" ] ||
        fail 'release metadata version does not match the installer'
    [ "$line3" = "commit=$CUP_RELEASE_COMMIT" ] ||
        fail 'release metadata commit does not match the installer'
}

root_marker_is_valid() {
    root=$1
    marker=$root/root.txt
    [ -f "$marker" ] && [ ! -L "$marker" ] || return 1
    printf 'format=1\nproduct=coffee-clang/cup\nlayout=1\n' | cmp -s - "$marker"
}

installed_version_is_expected() {
    binary=$1
    output=$("$binary" --version 2>/dev/null) || return 1
    [ "$output" = "cup $CUP_RELEASE_VERSION" ]
}

directory_is_empty() {
    directory=$1
    for entry in "$directory"/* "$directory"/.[!.]* "$directory"/..?*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        return 1
    done
    return 0
}

parse_bootstrap_root() {
    output=$1
    bootstrap_root=
    root_records=0

    while IFS= read -r line; do
        case "$line" in
            CUP_BOOTSTRAP_ROOT=*)
                bootstrap_root=${line#CUP_BOOTSTRAP_ROOT=}
                root_records=$((root_records + 1))
                ;;
            *)
                printf '%s\n' "$line"
                ;;
        esac
    done <<EOF_BOOTSTRAP
$output
EOF_BOOTSTRAP

    [ "$root_records" -eq 1 ] || fail 'bootstrap did not report one canonical root'
    case "$bootstrap_root" in
        "$HOME/.cup"|"$HOME/.coffee-cup") ;;
        *) fail 'bootstrap reported an unsupported canonical root' ;;
    esac
    BOOTSTRAP_ROOT=$bootstrap_root
    export BOOTSTRAP_ROOT
}

wait_for_commit() {
    candidate=$1
    binary=$candidate/bin/cup
    attempts=0

    while [ "$attempts" -lt "$WAIT_ATTEMPTS" ]; do
        if root_marker_is_valid "$candidate" &&
                [ -f "$binary" ] && [ ! -L "$binary" ] && [ -x "$binary" ] &&
                [ ! -e "$candidate/transaction.txt" ] && [ ! -L "$candidate/transaction.txt" ] &&
                [ -d "$candidate/staging" ] && [ ! -L "$candidate/staging" ] &&
                directory_is_empty "$candidate/staging" &&
                installed_version_is_expected "$binary"; then
            INSTALLED_BINARY=$binary
            export INSTALLED_BINARY
            return 0
        fi
        attempts=$((attempts + 1))
        sleep 1
    done
    fail 'timed out while the canonical update helper committed the installation'
}

validate_identity
validate_base_url
validate_wait_attempts
detect_platform
require_commands
create_work_directory

for asset in "$BINARY_ASSET" release.txt "$PLATFORM_SUMS" \
        SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1; do
    download_asset "$asset"
done

verify_checksum_document "$WORK/SHA256SUMS.common" \
    packages.cfg install.cfg install.sh install.ps1
verify_checksum_document "$WORK/$PLATFORM_SUMS" \
    "$BINARY_ASSET" release.txt SHA256SUMS.common
validate_release_metadata
chmod 0700 "$WORK/$BINARY_ASSET" || fail 'could not make the verified bootstrap executable'

bootstrap_output=$("$WORK/$BINARY_ASSET" --internal-bootstrap "$WORK") ||
    fail 'the verified cup bootstrap transaction was rejected'
parse_bootstrap_root "$bootstrap_output"
wait_for_commit "$BOOTSTRAP_ROOT"

printf 'cup %s installed successfully.\n' "$CUP_RELEASE_VERSION"
printf 'Binary: %s\n' "$INSTALLED_BINARY"
printf 'Add %s to PATH if it is not already available.\n' "${INSTALLED_BINARY%/*}"
