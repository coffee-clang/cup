#!/usr/bin/env sh
set -eu

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/latest/download"

CUP_ROOT=""
CUP_BIN_DIR=""
CUP_CONFIG_DIR=""
CUP_SCRIPTS_DIR=""
PACKAGES_CFG=""
COMMON_CHECKSUMS=""
PLATFORM_CHECKSUMS=""
UNINSTALL_SCRIPT=""
UNINSTALL_MARKER=""
CUP_AVAILABLE_IN_PATH=0
WINDOWS_SHELL_INSTALL=0

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }
need_command() { command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"; }
has_tty() { ( : </dev/tty && : >/dev/tty ) 2>/dev/null; }

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

configure_paths() {
    CUP_ROOT="$1"
    uninstall_name="$2"
    platform="$3"
    CUP_BIN_DIR="$CUP_ROOT/bin"
    CUP_CONFIG_DIR="$CUP_ROOT/config"
    CUP_SCRIPTS_DIR="$CUP_ROOT/scripts"
    PACKAGES_CFG="$CUP_CONFIG_DIR/packages.cfg"
    COMMON_CHECKSUMS="$CUP_CONFIG_DIR/SHA256SUMS.common"
    PLATFORM_CHECKSUMS="$CUP_CONFIG_DIR/SHA256SUMS.$platform"
    UNINSTALL_SCRIPT="$CUP_SCRIPTS_DIR/$uninstall_name"
    UNINSTALL_MARKER="$CUP_ROOT/uninstall.pending"
}

download_file() {
    url="$1"
    output="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$output" || fail "failed to download $url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$url" -O "$output" || fail "failed to download $url"
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
    valid_count="$(awk '/^[0-9a-fA-F]{64}[[:space:]]+\*?[^[:space:]].*$/ { count++ } END { print count + 0 }' "$checksum_file")"
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
    awk -F= '
        NF != 2 || $1 == "" || $2 == "" { exit 1 }
        seen[$1]++ > 0 { exit 1 }
        { count++ }
        $1 == "format" { format=$2; next }
        $1 == "version" { version=$2; next }
        $1 == "commit" { commit=$2; next }
        { exit 1 }
        END {
            if (format != "1" ||
                version !~ /^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/ ||
                commit == "" || count != 3) {
                exit 1
            }
        }
    ' "$metadata" || fail "invalid release metadata: $metadata"
}

detect_shell_profile() {
    case "$(basename "${SHELL:-}")" in
        zsh) printf '%s\n' "$HOME/.zshrc" ;;
        bash) printf '%s\n' "$HOME/.bashrc" ;;
        *)
            if [ -f "$HOME/.bashrc" ]; then
                printf '%s\n' "$HOME/.bashrc"
            else
                printf '%s\n' "$HOME/.profile"
            fi
            ;;
    esac
}

cup_bin_in_current_path() { printf '%s' ":$PATH:" | grep -F ":$CUP_BIN_DIR:" >/dev/null 2>&1; }

offer_path_update() {
    profile="$(detect_shell_profile)"
    path_line="export PATH=\"$CUP_BIN_DIR:\$PATH\""
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
    answer="$(prompt_tty "Add $CUP_BIN_DIR to PATH in $profile? [y/N] " "")"
    case "$answer" in
        y|Y|yes|YES)
            { printf '\n# cup\n'; printf '%s\n' "$path_line"; } >> "$profile"
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
    is_regular_asset "$PACKAGES_CFG" && chmod 0444 "$PACKAGES_CFG"
    is_regular_asset "$COMMON_CHECKSUMS" && chmod 0444 "$COMMON_CHECKSUMS"
    is_regular_asset "$PLATFORM_CHECKSUMS" && chmod 0444 "$PLATFORM_CHECKSUMS"
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        is_regular_asset "$PACKAGES_CFG" && set_windows_read_only "$PACKAGES_CFG"
        is_regular_asset "$COMMON_CHECKSUMS" && set_windows_read_only "$COMMON_CHECKSUMS"
        is_regular_asset "$PLATFORM_CHECKSUMS" && set_windows_read_only "$PLATFORM_CHECKSUMS"
        is_regular_asset "$UNINSTALL_SCRIPT" && set_windows_read_only "$UNINSTALL_SCRIPT"
    else
        is_regular_asset "$UNINSTALL_SCRIPT" && chmod 0555 "$UNINSTALL_SCRIPT"
    fi
    return 0
}

rollback_asset() {
    key="$1"
    destination="$2"
    staging="$3"

    clear_read_only "$destination" || return 1
    if [ -f "$staging/installed/$key" ]; then
        rm -f -- "$destination" || return 1
    fi
    if [ -e "$staging/backup/$key" ] || [ -L "$staging/backup/$key" ]; then
        mv "$staging/backup/$key" "$destination" || return 1
    elif [ -f "$staging/backup/$key.absent" ]; then
        rm -f -- "$destination" || return 1
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

    info "Recovering an interrupted cup bootstrap installation."
    recovery_failed=0
    rollback_asset binary "$cup_bin" "$staging" || recovery_failed=1
    rollback_asset manifest "$PACKAGES_CFG" "$staging" || recovery_failed=1
    rollback_asset common-checksums "$COMMON_CHECKSUMS" "$staging" || recovery_failed=1
    rollback_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging" || recovery_failed=1
    rollback_asset uninstall "$UNINSTALL_SCRIPT" "$staging" || recovery_failed=1
    restore_permissions || recovery_failed=1

    if [ "$recovery_failed" -ne 0 ]; then
        fail "the previous bootstrap installation could not be recovered; staging was preserved at $staging"
    fi
    rm -rf -- "$staging" || fail "could not remove recovered staging directory"
}

backup_asset() {
    key="$1"
    destination="$2"
    staging="$3"
    clear_read_only "$destination"
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        mv "$destination" "$staging/backup/$key"
    else
        : > "$staging/backup/$key.absent"
    fi
}

commit_asset() {
    key="$1"
    source="$2"
    destination="$3"
    staging="$4"
    mv "$source" "$destination"
    : > "$staging/installed/$key"
}

install_assets() {
    cup_asset="$1"
    installed_name="$2"
    platform="$3"
    uninstall_asset="$4"
    cup_bin="$CUP_BIN_DIR/$installed_name"
    staging="$CUP_ROOT/.bootstrap"

    need_command chmod
    need_command mkdir
    need_command mv
    need_command rm

    assert_real_directory "$CUP_ROOT"
    assert_real_directory "$CUP_BIN_DIR"
    assert_real_directory "$CUP_CONFIG_DIR"
    assert_real_directory "$CUP_SCRIPTS_DIR"
    mkdir -p "$CUP_BIN_DIR" "$CUP_CONFIG_DIR" "$CUP_SCRIPTS_DIR"
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
            rollback_asset binary "$cup_bin" "$staging" || recovery_failed=1
            rollback_asset manifest "$PACKAGES_CFG" "$staging" || recovery_failed=1
            rollback_asset common-checksums "$COMMON_CHECKSUMS" "$staging" || recovery_failed=1
            rollback_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging" || recovery_failed=1
            rollback_asset uninstall "$UNINSTALL_SCRIPT" "$staging" || recovery_failed=1
            restore_permissions || recovery_failed=1
            if [ "$recovery_failed" -eq 0 ]; then
                rm -rf -- "$staging"
            else
                printf 'Error: rollback was incomplete; staging was preserved at %s\n' "$staging" >&2
            fi
        fi
    }
    trap rollback EXIT HUP INT TERM

    info "Installing cup into $CUP_ROOT"
    download_file "$BASE_URL/$cup_asset" "$staging/$cup_asset"
    download_file "$BASE_URL/packages.cfg" "$staging/packages.cfg"
    download_file "$BASE_URL/$uninstall_asset" "$staging/$uninstall_asset"
    download_file "$BASE_URL/release.txt" "$staging/release.txt"
    download_file "$BASE_URL/SHA256SUMS.$platform" "$staging/SHA256SUMS.$platform"
    download_file "$BASE_URL/SHA256SUMS.common" "$staging/SHA256SUMS.common"
    verify_checksum_file "$staging" "SHA256SUMS.$platform" "$cup_asset" "$uninstall_asset" "release.txt"
    verify_checksum_file "$staging" "SHA256SUMS.common" "packages.cfg"
    validate_release_metadata "$staging/release.txt"

    backup_asset binary "$cup_bin" "$staging"
    backup_asset manifest "$PACKAGES_CFG" "$staging"
    backup_asset common-checksums "$COMMON_CHECKSUMS" "$staging"
    backup_asset platform-checksums "$PLATFORM_CHECKSUMS" "$staging"
    backup_asset uninstall "$UNINSTALL_SCRIPT" "$staging"

    commit_asset binary "$staging/$cup_asset" "$cup_bin" "$staging"
    commit_asset manifest "$staging/packages.cfg" "$PACKAGES_CFG" "$staging"
    commit_asset common-checksums "$staging/SHA256SUMS.common" "$COMMON_CHECKSUMS" "$staging"
    commit_asset platform-checksums "$staging/SHA256SUMS.$platform" "$PLATFORM_CHECKSUMS" "$staging"
    commit_asset uninstall "$staging/$uninstall_asset" "$UNINSTALL_SCRIPT" "$staging"

    chmod 0755 "$cup_bin"
    restore_permissions
    if [ -e "$UNINSTALL_MARKER" ] || [ -L "$UNINSTALL_MARKER" ]; then
        rm -f -- "$UNINSTALL_MARKER" ||
            fail "failed to remove stale uninstall marker: $UNINSTALL_MARKER"
    fi

    committed=1
    trap - EXIT HUP INT TERM
    rm -rf -- "$staging"

    info "cup installed successfully."
    info "Binary:    $cup_bin"
    info "Manifest:  $PACKAGES_CFG"
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

install_unix() {
    [ -n "${HOME:-}" ] || fail "HOME is not set"
    case "$HOME" in
        /) fail "HOME must not be the filesystem root" ;;
        /*) ;;
        *) fail "HOME must contain an absolute path" ;;
    esac

    os="$(uname -s 2>/dev/null || true)"
    arch="$(uname -m 2>/dev/null || true)"
    case "$os:$arch" in
        Linux:x86_64|Linux:amd64) asset="cup-linux-x64"; platform="linux-x64" ;;
        Linux:aarch64|Linux:arm64) asset="cup-linux-arm64"; platform="linux-arm64" ;;
        Darwin:x86_64|Darwin:amd64) asset="cup-macos-x64"; platform="macos-x64" ;;
        Darwin:arm64|Darwin:aarch64) asset="cup-macos-arm64"; platform="macos-arm64" ;;
        *) fail "unsupported platform: $os $arch" ;;
    esac
    WINDOWS_SHELL_INSTALL=0
    configure_paths "$HOME/.cup" "uninstall.sh" "$platform"
    install_assets "$asset" "cup" "$platform" "uninstall.sh"
}

run_powershell_installer() {
    ps_command="iwr $BASE_URL/install.ps1 -OutFile \$env:TEMP\\install-cup.ps1; & \$env:TEMP\\install-cup.ps1"
    if command -v powershell.exe >/dev/null 2>&1; then
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ps_command"; exit $?
    fi
    if command -v pwsh.exe >/dev/null 2>&1; then
        pwsh.exe -NoProfile -Command "$ps_command"; exit $?
    fi
    fail "PowerShell was not found. Run the Windows installer manually from PowerShell."
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
    case "$arch" in x86_64|amd64) ;; *) fail "unsupported Windows architecture: $arch. This installer supports x64 only" ;; esac
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
        ""|1) run_powershell_installer ;;
        2) install_windows_from_shell_directly ;;
        *) fail "invalid choice" ;;
    esac
}

main() {
    os="$(uname -s 2>/dev/null || true)"
    case "$os" in
        Linux|Darwin) install_unix ;;
        MINGW*|MSYS*|CYGWIN*) install_windows_from_shell ;;
        *) fail "unsupported operating system: $os" ;;
    esac
}

main "$@"
