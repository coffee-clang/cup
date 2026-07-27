#!/usr/bin/env sh

# Purpose: Installs one immutable official cup bootstrap on POSIX, or delegates
# Windows shells to PowerShell.
# The generated release version, tag and commit select all downloaded assets.
set -eu
umask 077

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
CUP_RELEASE_VERSION="@CUP_RELEASE_VERSION@"
CUP_RELEASE_TAG="@CUP_RELEASE_TAG@"
CUP_RELEASE_COMMIT="@CUP_RELEASE_COMMIT@"
DEFAULT_BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/\
${CUP_RELEASE_TAG}"
BASE_URL="${CUP_INSTALL_BASE_URL:-$DEFAULT_BASE_URL}"
BASE_URL="${BASE_URL%/}"

CUP_ROOT=""
CUP_BIN_DIR=""
CUP_CONFIG_DIR=""
CUP_HELPERS_DIR=""
PACKAGES_CFG=""
INSTALL_CONFIG=""
COMMON_CHECKSUMS=""
PLATFORM_CHECKSUMS=""
UNINSTALL_SCRIPT=""
UPDATE_HELPER=""
UNINSTALL_MARKER=""
CUP_AVAILABLE_IN_PATH=0
WINDOWS_SHELL_INSTALL=0

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}
info() {
    printf '%s\n' "$*"
}

# Validate the generated installer identity before any network request.
validate_installer_identity() {
    placeholder_marker='@''CUP_RELEASE_'
    case "$CUP_RELEASE_VERSION:$CUP_RELEASE_TAG:$CUP_RELEASE_COMMIT" in
        *"$placeholder_marker"*)
            fail "installer was not prepared for a concrete release"
            ;;
    esac
    printf '%s\n' "$CUP_RELEASE_VERSION" |
        awk 'BEGIN { ok=0 }
            /^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$/ { ok=1 }
            END { exit ok ? 0 : 1 }' ||
        fail "installer has an invalid release version"
    [ "$CUP_RELEASE_TAG" = "v$CUP_RELEASE_VERSION" ] ||
        fail "installer release tag does not match its version"
    printf '%s\n' "$CUP_RELEASE_COMMIT" |
        awk '/^[0-9a-f]{40}$/ { ok=1 } END { exit ok ? 0 : 1 }' ||
        fail "installer has an invalid release commit"
}
validate_base_url() {
    case "$BASE_URL" in
        *://*@*)
            fail "installer base URL must not contain credentials"
            ;;
    esac
    case "$BASE_URL" in
        https://*) ;;
        http://127.0.0.1:*|http://localhost:*)
            [ "${CUP_INSTALL_ALLOW_INSECURE:-0}" = 1 ] ||
                fail "HTTP is allowed only for an explicit loopback test"
            ;;
        *)
            fail "installer base URL must use HTTPS"
            ;;
    esac
}

need_command() {
    command -v "$1" >/dev/null 2>&1 ||
        fail "required command not found: $1"
}
has_tty() {
    ( : </dev/tty && : >/dev/tty ) 2>/dev/null
}

prompt_tty() {
    prompt="$1"
    default_value="$2"
    if has_tty; then
        printf '%s' "$prompt" > /dev/tty
        IFS= read -r answer < /dev/tty || answer="$default_value"
        printf '%s\n' "$answer"
    else
        printf 'No interactive terminal available; using default: %s\n' "$default_value" >&2
        printf '%s\n' "$default_value"
    fi
}

# Canonical root and bootstrap paths.
configure_paths() {
    CUP_ROOT="$1"
    uninstall_name="$2"
    platform="$3"
    CUP_BIN_DIR="$CUP_ROOT/bin"
    CUP_CONFIG_DIR="$CUP_ROOT/config"
    CUP_HELPERS_DIR="$CUP_ROOT/helpers"
    PACKAGES_CFG="$CUP_CONFIG_DIR/packages.cfg"
    INSTALL_CONFIG="$CUP_CONFIG_DIR/install.cfg"
    COMMON_CHECKSUMS="$CUP_CONFIG_DIR/SHA256SUMS.common"
    PLATFORM_CHECKSUMS="$CUP_CONFIG_DIR/SHA256SUMS.$platform"
    UNINSTALL_SCRIPT="$CUP_HELPERS_DIR/$uninstall_name"
    case "$platform" in
        windows-*)
            UPDATE_HELPER="$CUP_HELPERS_DIR/cup-update-helper.exe"
            ;;
        *)
            UPDATE_HELPER="$CUP_HELPERS_DIR/cup-update-helper"
            ;;
    esac
    UNINSTALL_MARKER="$CUP_ROOT/uninstall.pending"
}

# Download and checksum validation.
download_file() {
    url="$1"
    output="$2"
    if command -v curl >/dev/null 2>&1; then
        case "$BASE_URL" in
            https://*)
                curl -fsSL --proto '=https' --proto-redir '=https' \
                    "$url" -o "$output" || fail "failed to download $url"
                ;;
            *)
                curl -fsSL "$url" -o "$output" || fail "failed to download $url"
                ;;
        esac
    elif command -v wget >/dev/null 2>&1; then
        case "$BASE_URL" in
            https://*)
                wget -q --https-only "$url" -O "$output" || fail "failed to download $url"
                ;;
            *)
                wget -q "$url" -O "$output" || fail "failed to download $url"
                ;;
        esac
    else
        fail "neither curl nor wget is available"
    fi
    [ -s "$output" ] || fail "downloaded file is empty: $url"
}

assert_checksum_entries() {
    checksum_file="$1"
    shift
    [ -s "$checksum_file" ] || fail "checksum file is empty"

    entry_count="$(awk 'NF > 0 { count++ } END { print count + 0 }' "$checksum_file")"
    valid_count="$(
        awk '/^[0-9a-fA-F]{64}[[:space:]]+\*?[^[:space:]].*$/ {
                count++
            }
            END { print count + 0 }' "$checksum_file"
    )"
    [ "$entry_count" -eq "$#" ] && [ "$valid_count" -eq "$#" ] ||
        fail "checksum file contains invalid or unexpected entries"

    for expected in "$@"; do
        matches="$(awk -v name="$expected" '
            /^[0-9a-fA-F]{64}[[:space:]]+\*?[^[:space:]].*$/ {
                file=$0
                sub(/^[0-9a-fA-F]{64}[[:space:]]+\*?/, "", file)
                if (file == name) count++
            }
            END { print count + 0 }
        ' "$checksum_file")"
        [ "$matches" -eq 1 ] || fail "checksum entry is missing or duplicated: $expected"
    done
}

verify_checksum_file() {
    directory="$1"
    checksum_file="$2"
    shift 2
    assert_checksum_entries "$directory/$checksum_file" "$@"

    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$directory" && sha256sum -c "$checksum_file") >/dev/null ||
            fail "checksum verification failed"
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$directory" && shasum -a 256 -c "$checksum_file") >/dev/null ||
            fail "checksum verification failed"
    else
        fail "neither sha256sum nor shasum is available"
    fi
}

validate_release_metadata() {
    metadata="$1"
    expected_version="${2:-}"
    expected_commit="${3:-}"

    [ -f "$metadata" ] || fail "release metadata file is missing: $metadata"
    [ -r "$metadata" ] || fail "release metadata file is not readable: $metadata"

    diagnostic="$(awk -v expected_version="$expected_version" \
        -v expected_commit="$expected_commit" '
        function set_error(code, detail) {
            if (error_code == "") {
                error_code=code
                error_detail=detail
            }
        }
        function valid_part(value) {
            return value ~ /^(0|[1-9][0-9]*)$/ &&
                length(value) <= 6 && (value + 0) <= 999999
        }
        function valid_version(value, parts, count) {
            count=split(value, parts, ".")
            return count == 3 && valid_part(parts[1]) &&
                valid_part(parts[2]) && valid_part(parts[3])
        }
        function valid_commit(value) {
            return value ~ /^[0-9a-f]{7,40}$/
        }
        {
            line=$0
            sub(/\r$/, "", line)
            separator=index(line, "=")
            if (separator <= 1 || separator == length(line) ||
                    index(substr(line, separator + 1), "=") != 0) {
                set_error("malformed-line", NR)
                next
            }

            key=substr(line, 1, separator - 1)
            value=substr(line, separator + 1)
            if (key == "format") {
                if (seen_format) set_error("duplicate-field", "format")
                else { seen_format=1; format=value }
            } else if (key == "version") {
                if (seen_version) set_error("duplicate-field", "version")
                else { seen_version=1; version=value }
            } else if (key == "commit") {
                if (seen_commit) set_error("duplicate-field", "commit")
                else { seen_commit=1; commit=value }
            } else {
                set_error("unexpected-field", NR)
            }
        }
        END {
            if (error_code == "" && !seen_format)
                set_error("missing-field", "format")
            if (error_code == "" && !seen_version)
                set_error("missing-field", "version")
            if (error_code == "" && !seen_commit)
                set_error("missing-field", "commit")
            if (error_code == "" && NR != 3)
                set_error("line-count", NR)
            if (error_code == "" && format != "1")
                set_error("unsupported-format", format)
            if (error_code == "" && !valid_version(version))
                set_error("invalid-version", version)
            if (error_code == "" && !valid_commit(commit))
                set_error("invalid-commit", commit)
            if (error_code == "" && expected_version != "" &&
                    version != expected_version)
                set_error("version-mismatch", version)
            if (error_code == "" && expected_commit != "" &&
                    commit != expected_commit)
                set_error("commit-mismatch", commit)

            if (error_code == "") print "ok:"
            else print error_code ":" error_detail
        }
    ' "$metadata")"

    diagnostic_code=${diagnostic%%:*}
    diagnostic_detail=${diagnostic#*:}
    case "$diagnostic_code" in
        ok)
            return 0
            ;;
        malformed-line)
            fail "release metadata line $diagnostic_detail must contain exactly one non-empty 'key=value' assignment: $metadata"
            ;;
        unexpected-field)
            fail "release metadata contains an unexpected field at line $diagnostic_detail: $metadata"
            ;;
        duplicate-field)
            fail "release metadata field '$diagnostic_detail' is duplicated: $metadata"
            ;;
        missing-field)
            fail "release metadata is missing required field '$diagnostic_detail': $metadata"
            ;;
        line-count)
            fail "release metadata must contain exactly 3 lines; found $diagnostic_detail: $metadata"
            ;;
        unsupported-format)
            fail "release metadata format is unsupported; expected '1': $metadata"
            ;;
        invalid-version)
            fail "release metadata version is invalid; expected 'MAJOR.MINOR.PATCH': $metadata"
            ;;
        invalid-commit)
            fail "release metadata commit is invalid; expected 7 to 40 lowercase hexadecimal characters: $metadata"
            ;;
        version-mismatch)
            fail "release metadata version mismatch: expected '$expected_version', received '$diagnostic_detail': $metadata"
            ;;
        commit-mismatch)
            fail "release metadata commit mismatch: expected '$expected_commit', received '$diagnostic_detail': $metadata"
            ;;
        *)
            fail "release metadata validation failed for an unknown reason: $metadata"
            ;;
    esac
}
verify_named_checksum() {
    directory="$1"
    checksum_file="$2"
    expected="$3"
    selected="$directory/.cup-selected-checksum"

    matches="$(awk -v name="$expected" '
        /^[0-9a-fA-F]{64}[[:space:]]+\*?[^[:space:]].*$/ {
            file=$0
            sub(/^[0-9a-fA-F]{64}[[:space:]]+\*?/, "", file)
            if (file == name) { print; count++ }
        }
        END { if (count != 1) exit 1 }
    ' "$checksum_file")" || fail "checksum entry is missing or duplicated: $expected"
    printf '%s\n' "$matches" > "$selected"
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$directory" && sha256sum -c "${selected##*/}") >/dev/null ||
            fail "checksum verification failed for $expected"
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$directory" && shasum -a 256 -c "${selected##*/}") >/dev/null ||
            fail "checksum verification failed for $expected"
    else
        fail "neither sha256sum nor shasum is available"
    fi
    rm -f "$selected"
}

validate_profile_path() {
    profile_path="$1"
    profile_parent="${profile_path%/*}"
    [ "$profile_parent" != "$profile_path" ] || profile_parent="."

    [ ! -L "$profile_path" ] ||
        fail "shell profile is a symbolic link and was not modified: $profile_path"
    if [ -e "$profile_path" ] && [ ! -f "$profile_path" ]; then
        fail "shell profile is not a regular file and was not modified: $profile_path"
    fi
    case "$profile_path" in
        "$HOME"/*) ;;
        *)
            fail "shell profile is outside HOME and was not modified: $profile_path"
            ;;
    esac
    current_parent="$profile_parent"
    while [ "$current_parent" != "$HOME" ]; do
        [ ! -L "$current_parent" ] ||
            fail "shell profile directory is a symbolic link and was not modified: $current_parent"
        if [ -e "$current_parent" ] && [ ! -d "$current_parent" ]; then
            fail "shell profile parent is not a directory: $current_parent"
        fi
        next_parent="${current_parent%/*}"
        [ -n "$next_parent" ] || next_parent="/"
        [ "$next_parent" != "$current_parent" ] ||
            fail "shell profile parent could not be validated: $profile_parent"
        current_parent="$next_parent"
    done
}

append_profile_line() {
    profile_path="$1"
    line="$2"
    profile_parent="${profile_path%/*}"
    [ "$profile_parent" != "$profile_path" ] || profile_parent="."

    validate_profile_path "$profile_path"
    mkdir -p "$profile_parent"
    validate_profile_path "$profile_path"
    need_command mktemp
    profile_temp="$(mktemp "$profile_parent/.cup-profile.XXXXXX")" ||
        fail "could not create a private shell-profile temporary file"
    if [ -f "$profile_path" ]; then
        cp -p "$profile_path" "$profile_temp" || {
            rm -f "$profile_temp"
            fail "could not copy shell profile safely"
        }
        printf '\n%s\n' "$line" >> "$profile_temp" || {
            rm -f "$profile_temp"
            fail "could not prepare shell profile update"
        }
    else
        chmod 0600 "$profile_temp"
        printf '%s\n' "$line" > "$profile_temp" || {
            rm -f "$profile_temp"
            fail "could not prepare shell profile update"
        }
    fi
    validate_profile_path "$profile_path"
    mv "$profile_temp" "$profile_path" || {
        rm -f "$profile_temp"
        fail "could not replace shell profile atomically"
    }
}

# Optional user PATH integration.
detect_shell_profile() {
    case "$(basename "${SHELL:-}")" in
        fish)
            printf '%s\n' "$HOME/.config/fish/conf.d/cup.fish"
            ;;
        zsh)
            printf '%s\n' "$HOME/.zshrc"
            ;;
        bash)
            printf '%s\n' "$HOME/.bashrc"
            ;;
        *)
            if [ -f "$HOME/.bashrc" ]; then
                printf '%s\n' "$HOME/.bashrc"
            else
                printf '%s\n' "$HOME/.profile"
            fi
            ;;
    esac
}

shell_quote() {
    printf "'"
    printf '%s' "$1" | sed "s/'/'\\\\''/g"
    printf "'"
}

cup_bin_in_current_path() {
    printf '%s' ":$PATH:" |
        grep -F ":$CUP_BIN_DIR:" >/dev/null 2>&1
}

offer_path_update() {
    shell_name="$(basename "${SHELL:-}")"
    profile="$(detect_shell_profile)"
    if [ "$shell_name" = fish ]; then
        path_line='fish_add_path "$HOME/.cup/bin"'
    else
        need_command sed
        quoted_bin="$(shell_quote "$CUP_BIN_DIR")"
        path_line="export PATH=$quoted_bin:\"\$PATH\""
    fi
    if cup_bin_in_current_path; then
        CUP_AVAILABLE_IN_PATH=1
        info "cup bin directory is already available in PATH for this shell."
        return
    fi
    if [ -f "$profile" ] && grep -F "$path_line" "$profile" >/dev/null 2>&1; then
        CUP_AVAILABLE_IN_PATH=1
        info "PATH entry already exists in $profile."
        info "Restart the shell or run:"
        info "  $path_line"
        return
    fi
    if [ "${CUP_INSTALL_NO_PATH_PROMPT:-0}" = 1 ]; then
        info "PATH not modified. Add this line manually when needed:"
        info "  $path_line"
        return
    fi
    answer="$(prompt_tty "Add $CUP_BIN_DIR to PATH in $profile? [y/N] " "")"
    case "$answer" in
        y|Y|yes|YES)
            append_profile_line "$profile" "$path_line"
            CUP_AVAILABLE_IN_PATH=1
            info "PATH updated in $profile. Restart the shell or run:"
            info "  $path_line"
            ;;
        *)
            info "PATH not modified. Add this line manually when needed:"
            info "  $path_line"
            ;;
    esac
}

# Recoverable bootstrap replacement.
assert_real_directory() {
    path="$1"
    if [ -L "$path" ]; then
        fail "managed directory is a symbolic link: $path"
    fi
    if [ -e "$path" ] && [ ! -d "$path" ]; then
        fail "managed path is not a directory: $path"
    fi
}

set_windows_read_only() {
    need_command cygpath
    need_command attrib.exe
    attrib.exe +R "$(cygpath -w "$1")" >/dev/null ||
        fail "failed to set read-only attribute on $1"
}

clear_read_only() {
    path="$1"
    [ -e "$path" ] || [ -L "$path" ] || return 0
    [ -L "$path" ] && return 0
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        attrib.exe -R "$(cygpath -w "$path")" >/dev/null
    else
        chmod u+w "$path"
    fi
}

is_regular_asset() {
    [ -f "$1" ] && [ ! -L "$1" ]
}

restore_permissions() {
    require_assets="${1:-0}"
    if [ "$require_assets" -eq 1 ]; then
        for path in "$PACKAGES_CFG" "$INSTALL_CONFIG" "$COMMON_CHECKSUMS" \
            "$PLATFORM_CHECKSUMS" "$UNINSTALL_SCRIPT" "$UPDATE_HELPER"; do
            is_regular_asset "$path" || return 1
        done
    fi

    if is_regular_asset "$PACKAGES_CFG"; then chmod 0444 "$PACKAGES_CFG" || return 1; fi
    if is_regular_asset "$INSTALL_CONFIG"; then chmod 0444 "$INSTALL_CONFIG" || return 1; fi
    if is_regular_asset "$COMMON_CHECKSUMS"; then chmod 0444 "$COMMON_CHECKSUMS" || return 1; fi
    if is_regular_asset "$PLATFORM_CHECKSUMS"; then chmod 0444 "$PLATFORM_CHECKSUMS" || return 1; fi
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        if is_regular_asset "$PACKAGES_CFG"; then set_windows_read_only "$PACKAGES_CFG" || return 1; fi
        if is_regular_asset "$INSTALL_CONFIG"; then set_windows_read_only "$INSTALL_CONFIG" || return 1; fi
        if is_regular_asset "$COMMON_CHECKSUMS"; then set_windows_read_only "$COMMON_CHECKSUMS" || return 1; fi
        if is_regular_asset "$PLATFORM_CHECKSUMS"; then set_windows_read_only "$PLATFORM_CHECKSUMS" || return 1; fi
        if is_regular_asset "$UNINSTALL_SCRIPT"; then set_windows_read_only "$UNINSTALL_SCRIPT" || return 1; fi
    elif is_regular_asset "$UNINSTALL_SCRIPT"; then
        chmod 0555 "$UNINSTALL_SCRIPT" || return 1
    fi
    if is_regular_asset "$UPDATE_HELPER"; then chmod 0755 "$UPDATE_HELPER" || return 1; fi
    return 0
}

rollback_asset() {
    key="$1"
    destination="$2"
    staging="$3"
    backup="$staging/backup/$key"
    absent="$backup.absent"
    installed="$staging/installed/$key"

    clear_read_only "$destination" || return 1
    if { [ -e "$destination" ] || [ -L "$destination" ]; } &&
        ! is_regular_asset "$destination"; then
        return 1
    fi
    for evidence in "$backup" "$absent" "$installed"; do
        if { [ -e "$evidence" ] || [ -L "$evidence" ]; } &&
            ! is_regular_asset "$evidence"; then
            return 1
        fi
    done
    if is_regular_asset "$backup" && is_regular_asset "$absent"; then
        return 1
    fi
    if is_regular_asset "$installed" &&
        ! is_regular_asset "$backup" && ! is_regular_asset "$absent"; then
        return 1
    fi

    if is_regular_asset "$backup"; then
        mv -f -- "$backup" "$destination" || return 1
    elif is_regular_asset "$absent"; then
        if is_regular_asset "$installed"; then
            rm -f -- "$destination" || return 1
        elif [ -e "$destination" ] || [ -L "$destination" ]; then
            return 1
        fi
    fi
    return 0
}

recover_staging() {
    staging="$1"
    cup_bin="$2"
    if [ -L "$staging" ]; then
        fail "bootstrap staging path is a symbolic link: $staging"
    fi
    [ -d "$staging" ] || return 0

    if [ -f "$staging/committed" ] && [ ! -L "$staging/committed" ]; then
        is_regular_asset "$cup_bin" && restore_permissions 1 ||
            fail "completed bootstrap staging does not match a complete installed generation"
        info "Finishing cleanup from a completed cup bootstrap installation."
        rm -rf -- "$staging" ||
            fail "could not remove completed bootstrap staging directory"
        return 0
    fi
    if [ -e "$staging/committed" ] || [ -L "$staging/committed" ]; then
        fail "bootstrap commit marker is not a regular file: $staging/committed"
    fi

    info "Recovering an interrupted cup bootstrap installation."
    recovery_failed=0
    rollback_asset catalog "$PACKAGES_CFG" "$staging" || recovery_failed=1
    rollback_asset install-config "$INSTALL_CONFIG" "$staging" || recovery_failed=1
    rollback_asset common-checksums "$COMMON_CHECKSUMS" "$staging" || recovery_failed=1
    rollback_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging" || recovery_failed=1
    rollback_asset uninstall "$UNINSTALL_SCRIPT" "$staging" || recovery_failed=1
    rollback_asset update-helper "$UPDATE_HELPER" "$staging" || recovery_failed=1
    rollback_asset binary "$cup_bin" "$staging" || recovery_failed=1
    restore_permissions || recovery_failed=1

    if [ "$recovery_failed" -ne 0 ]; then
        fail "the previous bootstrap installation could not be recovered; \
staging was preserved at $staging"
    fi
    rm -rf -- "$staging" || fail "could not remove recovered staging directory"
}

backup_asset() {
    key="$1"
    destination="$2"
    staging="$3"
    clear_read_only "$destination"
    if is_regular_asset "$destination"; then
        cp -p "$destination" "$staging/backup/$key" ||
            fail "could not back up $destination"
    elif [ -e "$destination" ] || [ -L "$destination" ]; then
        fail "existing bootstrap asset is not a regular file: $destination"
    else
        : > "$staging/backup/$key.absent" ||
            fail "could not record absent asset: $destination"
    fi
}

commit_asset() {
    key="$1"
    source="$2"
    destination="$3"
    staging="$4"

    is_regular_asset "$source" ||
        fail "bootstrap staged asset is not a regular file: $source"
    if { [ -e "$destination" ] || [ -L "$destination" ]; } &&
        ! is_regular_asset "$destination"; then
        fail "installed bootstrap asset is not a regular file: $destination"
    fi
    : > "$staging/installed/$key" ||
        fail "could not record bootstrap replacement: $destination"
    mv -f -- "$source" "$destination" ||
        fail "could not install bootstrap asset: $destination"
}

cleanup_uninstall_residues() {
    installed_name="$1"
    need_command find

    for residue in "$HOME"/.cup-uninstall.*; do
        [ -e "$residue" ] || [ -L "$residue" ] || continue
        if [ -L "$residue" ] || [ ! -d "$residue" ]; then
            fail "unrecognized uninstall residue was preserved: $residue"
        fi
        staged_root="$residue/root"
        child_count="$(find "$residue" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d ' ')"
        if [ "$child_count" -ne 1 ] || [ -L "$staged_root" ] ||
            [ ! -d "$staged_root" ] ||
            [ ! -f "$staged_root/uninstall.pending" ] ||
            [ -L "$staged_root/uninstall.pending" ] ||
            [ ! -f "$staged_root/bin/$installed_name" ] ||
            [ -L "$staged_root/bin/$installed_name" ]; then
            fail "unrecognized uninstall residue was preserved: $residue"
        fi
        marker_line=$(cat "$staged_root/uninstall.pending" 2>/dev/null) ||
            fail "unrecognized uninstall residue was preserved: $residue"
        case "$marker_line" in
            parent_pid=*)
                marker_pid=${marker_line#parent_pid=}
                case "$marker_pid" in
                    ''|*[!0-9]*|0)
                        fail "unrecognized uninstall residue was preserved: $residue"
                        ;;
                esac
                ;;
            *) fail "unrecognized uninstall residue was preserved: $residue" ;;
        esac
        [ "$(wc -l < "$staged_root/uninstall.pending" | tr -d '[:space:]')" -eq 1 ] ||
            fail "unrecognized uninstall residue was preserved: $residue"
        info "Removing validated uninstall residue: $residue"
        rm -rf -- "$residue" ||
            fail "could not remove validated uninstall residue: $residue"
    done
}


# Bootstrap transfer and transaction phases are kept separate so the main
# installer reads as an ordered recovery-safe pipeline.
download_bootstrap_assets() (
    staging=$1
    cup_asset=$2
    platform=$3
    uninstall_asset=$4

    download_file "$BASE_URL/$cup_asset" "$staging/$cup_asset"
    download_file "$BASE_URL/packages.cfg" "$staging/packages.cfg"
    download_file "$BASE_URL/install.cfg" "$staging/install.cfg"
    download_file "$BASE_URL/$uninstall_asset" "$staging/$uninstall_asset"
    download_file "$BASE_URL/release.txt" "$staging/release.txt"
    download_file "$BASE_URL/SHA256SUMS.$platform" "$staging/SHA256SUMS.$platform"
    download_file "$BASE_URL/SHA256SUMS.common" "$staging/SHA256SUMS.common"
)

verify_bootstrap_assets() (
    staging=$1
    cup_asset=$2
    platform=$3
    uninstall_asset=$4

    verify_checksum_file "$staging" "SHA256SUMS.$platform" \
        "$cup_asset" "$uninstall_asset" "release.txt"
    assert_checksum_entries "$staging/SHA256SUMS.common" \
        "packages.cfg" "install.cfg" "install.sh" "install.ps1"
    verify_named_checksum "$staging" "$staging/SHA256SUMS.common" "packages.cfg"
    verify_named_checksum "$staging" "$staging/SHA256SUMS.common" "install.cfg"
    validate_release_metadata \
        "$staging/release.txt" "$CUP_RELEASE_VERSION" "$CUP_RELEASE_COMMIT"
    cp "$staging/$cup_asset" "$staging/cup-update-helper"
    chmod 0755 "$staging/$cup_asset" "$staging/cup-update-helper"
    if [ "$WINDOWS_SHELL_INSTALL" -eq 0 ]; then
        chmod 0555 "$staging/$uninstall_asset"
    fi
)

backup_bootstrap_assets() (
    staging=$1
    cup_bin=$2

    backup_asset catalog "$PACKAGES_CFG" "$staging"
    backup_asset install-config "$INSTALL_CONFIG" "$staging"
    backup_asset common-checksums "$COMMON_CHECKSUMS" "$staging"
    backup_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging"
    backup_asset uninstall "$UNINSTALL_SCRIPT" "$staging"
    backup_asset update-helper "$UPDATE_HELPER" "$staging"
    backup_asset binary "$cup_bin" "$staging"
)

commit_bootstrap_assets() (
    staging=$1
    cup_asset=$2
    cup_bin=$3
    platform=$4
    uninstall_asset=$5

    commit_asset catalog "$staging/packages.cfg" "$PACKAGES_CFG" "$staging"
    commit_asset install-config "$staging/install.cfg" "$INSTALL_CONFIG" "$staging"
    commit_asset common-checksums \
        "$staging/SHA256SUMS.common" "$COMMON_CHECKSUMS" "$staging"
    commit_asset platform-checksums \
        "$staging/SHA256SUMS.$platform" "$PLATFORM_CHECKSUMS" "$staging"
    commit_asset uninstall "$staging/$uninstall_asset" "$UNINSTALL_SCRIPT" "$staging"
    commit_asset update-helper "$staging/cup-update-helper" "$UPDATE_HELPER" "$staging"
    commit_asset binary "$staging/$cup_asset" "$cup_bin" "$staging"
)

install_assets() {
    cup_asset="$1"
    installed_name="$2"
    platform="$3"
    uninstall_asset="$4"
    cup_bin="$CUP_BIN_DIR/$installed_name"
    staging="$CUP_ROOT/.bootstrap"

    need_command chmod
    need_command cp
    need_command mkdir
    need_command mv
    need_command rm

    cleanup_uninstall_residues "$installed_name"
    assert_real_directory "$CUP_ROOT"
    assert_real_directory "$CUP_BIN_DIR"
    assert_real_directory "$CUP_CONFIG_DIR"
    assert_real_directory "$CUP_HELPERS_DIR"
    mkdir -p "$CUP_BIN_DIR" "$CUP_CONFIG_DIR" "$CUP_HELPERS_DIR"
    chmod 0700 "$CUP_ROOT" "$CUP_BIN_DIR" "$CUP_CONFIG_DIR" "$CUP_HELPERS_DIR" ||
        fail "could not make the cup root private"
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        need_command cygpath
        need_command attrib.exe
    fi
    recover_staging "$staging" "$cup_bin"
    mkdir "$staging"
    mkdir "$staging/backup" "$staging/installed"
    committed=0

    rollback() {
        if [ "$committed" -eq 0 ] && [ -d "$staging" ]; then
            recovery_failed=0
            rollback_asset catalog "$PACKAGES_CFG" "$staging" || recovery_failed=1
            rollback_asset install-config "$INSTALL_CONFIG" "$staging" || recovery_failed=1
            rollback_asset common-checksums "$COMMON_CHECKSUMS" "$staging" || recovery_failed=1
            rollback_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging" || recovery_failed=1
            rollback_asset uninstall "$UNINSTALL_SCRIPT" "$staging" || recovery_failed=1
            rollback_asset update-helper "$UPDATE_HELPER" "$staging" || recovery_failed=1
            rollback_asset binary "$cup_bin" "$staging" || recovery_failed=1
            restore_permissions || recovery_failed=1
            if [ "$recovery_failed" -eq 0 ]; then
                rm -rf -- "$staging"
            else
                printf '%s\n' \
                    "Error: rollback was incomplete; staging was preserved at $staging" >&2
            fi
        fi
    }
    trap rollback EXIT HUP INT TERM

    info "Installing cup into $CUP_ROOT"
    download_bootstrap_assets "$staging" "$cup_asset" "$platform" "$uninstall_asset"
    verify_bootstrap_assets "$staging" "$cup_asset" "$platform" "$uninstall_asset"
    backup_bootstrap_assets "$staging" "$cup_bin"
    commit_bootstrap_assets \
        "$staging" "$cup_asset" "$cup_bin" "$platform" "$uninstall_asset"

    chmod 0755 "$cup_bin"
    restore_permissions 1 || fail "installed bootstrap assets are incomplete or unsafe"
    if [ -e "$UNINSTALL_MARKER" ] || [ -L "$UNINSTALL_MARKER" ]; then
        rm -f -- "$UNINSTALL_MARKER" ||
            fail "failed to remove stale uninstall marker: $UNINSTALL_MARKER"
    fi

    : > "$staging/committed" ||
        fail "could not record completed bootstrap installation"
    committed=1
    trap - EXIT HUP INT TERM
    if ! rm -rf -- "$staging"; then
        printf '%s\n' \
            "Warning: cup was installed, but completed bootstrap staging could not be removed." \
            >&2
    fi

    info "cup installed successfully."
    info "Binary:    $cup_bin"
    info "Package catalog: $PACKAGES_CFG"
    info "Install configuration: $INSTALL_CONFIG"
    info "Checksums: $COMMON_CHECKSUMS"
    info "           $PLATFORM_CHECKSUMS"
    info "Uninstall: $UNINSTALL_SCRIPT"
    offer_path_update
    if [ "$CUP_AVAILABLE_IN_PATH" -eq 1 ]; then
        info "Test with: cup help"
    else
        info "Test with: $cup_bin help"
    fi
}

# Native POSIX installation.
install_unix() {
    [ -n "${HOME:-}" ] || fail "HOME is not set"
    case "$HOME" in
        /)
            fail "HOME must not be the filesystem root"
            ;;
        /*) ;;
        *)
            fail "HOME must contain an absolute path"
            ;;
    esac

    os="$(uname -s 2>/dev/null || true)"
    arch="$(uname -m 2>/dev/null || true)"
    case "$os:$arch" in
        Linux:x86_64|Linux:amd64)
            asset=cup-linux-x64
            platform=linux-x64
            ;;
        Linux:aarch64|Linux:arm64)
            asset=cup-linux-arm64
            platform=linux-arm64
            ;;
        Darwin:x86_64|Darwin:amd64)
            asset=cup-macos-x64
            platform=macos-x64
            ;;
        Darwin:arm64|Darwin:aarch64)
            asset=cup-macos-arm64
            platform=macos-arm64
            ;;
        *)
            fail "unsupported platform: $os $arch"
            ;;
    esac
    WINDOWS_SHELL_INSTALL=0
    configure_paths "$HOME/.cup" "uninstall.sh" "$platform"
    install_assets "$asset" "cup" "$platform" "uninstall.sh"
}

# Windows shell delegation and native fallback.
run_powershell_installer() {
    need_command mktemp
    delegate_dir="$(mktemp -d "${TMPDIR:-/tmp}/cup-install.XXXXXX")" ||
        fail "could not create a private installer directory"
    delegate_cleanup() {
        rm -rf -- "$delegate_dir"
    }
    trap delegate_cleanup EXIT HUP INT TERM

    download_file "$BASE_URL/SHA256SUMS.common" "$delegate_dir/SHA256SUMS.common"
    download_file "$BASE_URL/install.ps1" "$delegate_dir/install.ps1"
    assert_checksum_entries "$delegate_dir/SHA256SUMS.common" \
        "packages.cfg" "install.cfg" "install.sh" "install.ps1"
    verify_named_checksum "$delegate_dir" "$delegate_dir/SHA256SUMS.common" "install.ps1"

    need_command cygpath
    installer_windows="$(cygpath -w "$delegate_dir/install.ps1")"
    if command -v powershell.exe >/dev/null 2>&1; then
        if powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$installer_windows"; then
            status=0
        else
            status=$?
        fi
    elif command -v pwsh.exe >/dev/null 2>&1; then
        if pwsh.exe -NoProfile -File "$installer_windows"; then
            status=0
        else
            status=$?
        fi
    else
        fail "PowerShell was not found. Run the verified Windows installer manually."
    fi
    trap - EXIT HUP INT TERM
    delegate_cleanup
    exit "$status"
}

get_windows_profile_root() {
    windows_profile="${USERPROFILE:-}"
    need_command cygpath
    if [ -z "$windows_profile" ] && command -v cmd.exe >/dev/null 2>&1; then
        windows_profile="$(cmd.exe /d /c echo %USERPROFILE% 2>/dev/null | tr -d '\r')"
    fi
    [ -n "$windows_profile" ] || fail "Windows user profile could not be determined"
    cygpath -u "$windows_profile"
}

install_windows_from_shell_directly() {
    arch="$(uname -m 2>/dev/null || true)"
    case "$arch" in
        x86_64|amd64) ;;
        *)
            fail "unsupported Windows architecture: $arch. This installer supports x64 only"
            ;;
    esac
    WINDOWS_SHELL_INSTALL=1
    configure_paths "$(get_windows_profile_root)/.cup" "uninstall.ps1" "windows-x64"
    install_assets "cup-windows-x64.exe" "cup.exe" "windows-x64" "uninstall.ps1"
}

install_windows_from_shell() {
    info "Windows Unix-like shell detected."
    info ""
    info "Choose installation mode:"
    info "  1) Native Windows installation via PowerShell"
    info "  2) Installation from the current shell"
    choice="$(prompt_tty "Choice [1/2, default: 1]: " "1")"
    case "$choice" in
        ""|1)
            run_powershell_installer
            ;;
        2)
            install_windows_from_shell_directly
            ;;
        *)
            fail "invalid choice"
            ;;
    esac
}

# Platform dispatch.
main() {
    validate_installer_identity
    validate_base_url
    os="$(uname -s 2>/dev/null || true)"
    case "$os" in
        Linux|Darwin)
            install_unix
            ;;
        MINGW*|MSYS*|CYGWIN*)
            install_windows_from_shell
            ;;
        *)
            fail "unsupported operating system: $os"
            ;;
    esac
}

main "$@"
