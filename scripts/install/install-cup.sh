#!/usr/bin/env sh
set -eu

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
RELEASE_TAG="cup-bootstrap"
BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/${RELEASE_TAG}"

CUP_ROOT=""
CUP_BIN_DIR=""
CUP_CONFIG_DIR=""
CUP_SCRIPTS_DIR=""
PACKAGES_CFG=""
UNINSTALL_SCRIPT=""
CUP_AVAILABLE_IN_PATH=0
WINDOWS_SHELL_INSTALL=0

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

info() {
    printf '%s\n' "$*"
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

has_tty() {
    [ -r /dev/tty ] && [ -w /dev/tty ]
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

configure_paths() {
    CUP_ROOT="$1"
    uninstall_name="$2"
    CUP_BIN_DIR="$CUP_ROOT/bin"
    CUP_CONFIG_DIR="$CUP_ROOT/config"
    CUP_SCRIPTS_DIR="$CUP_ROOT/scripts"
    PACKAGES_CFG="$CUP_CONFIG_DIR/packages.cfg"
    UNINSTALL_SCRIPT="$CUP_SCRIPTS_DIR/$uninstall_name"
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
}

verify_checksum_file() {
    directory="$1"
    checksum_file="$2"

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

detect_shell_profile() {
    case "$(basename "${SHELL:-}")" in
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

cup_bin_in_current_path() {
    printf '%s' ":$PATH:" | grep -F ":$CUP_BIN_DIR:" >/dev/null 2>&1
}

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
            {
                printf '\n# cup\n'
                printf '%s\n' "$path_line"
            } >> "$profile"
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

set_windows_read_only() {
    path="$1"

    need_command cygpath
    need_command attrib.exe
    attrib.exe +R "$(cygpath -w "$path")" >/dev/null ||
        fail "failed to set read-only attribute on $path"
}

clear_windows_read_only() {
    path="$1"

    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ] && [ -e "$path" ]; then
        attrib.exe -R "$(cygpath -w "$path")" >/dev/null 2>&1 || true
    fi
}

install_assets() {
    cup_asset="$1"
    installed_name="$2"
    platform="$3"
    uninstall_asset="$4"
    cup_bin="$CUP_BIN_DIR/$installed_name"

    need_command chmod
    need_command mkdir
    need_command mktemp
    need_command mv
    need_command rm

    mkdir -p "$CUP_BIN_DIR" "$CUP_CONFIG_DIR" "$CUP_SCRIPTS_DIR"
    staging="$(mktemp -d "$CUP_ROOT/.bootstrap.XXXXXX")" ||
        fail "failed to create bootstrap staging directory"
    backup="$staging/backup"
    committed=0
    had_bin=0
    had_manifest=0
    had_uninstall=0
    installed_bin=0
    installed_manifest=0
    installed_uninstall=0

    mkdir -p "$backup"

    rollback() {
        if [ "$committed" -eq 0 ]; then
            clear_windows_read_only "$PACKAGES_CFG"
            clear_windows_read_only "$UNINSTALL_SCRIPT"
            [ "$installed_bin" -eq 1 ] && rm -f "$cup_bin" || true
            [ "$installed_manifest" -eq 1 ] && rm -f "$PACKAGES_CFG" || true
            [ "$installed_uninstall" -eq 1 ] && rm -f "$UNINSTALL_SCRIPT" || true
            [ "$had_bin" -eq 1 ] && mv "$backup/bin" "$cup_bin" || true
            [ "$had_manifest" -eq 1 ] && mv "$backup/packages.cfg" "$PACKAGES_CFG" || true
            [ "$had_uninstall" -eq 1 ] && mv "$backup/uninstall" "$UNINSTALL_SCRIPT" || true

            if [ "$had_manifest" -eq 1 ] || [ "$had_uninstall" -eq 1 ]; then
                if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
                    if [ "$had_manifest" -eq 1 ]; then
                        attrib.exe +R "$(cygpath -w "$PACKAGES_CFG")" >/dev/null 2>&1 || true
                    fi
                    if [ "$had_uninstall" -eq 1 ]; then
                        attrib.exe +R "$(cygpath -w "$UNINSTALL_SCRIPT")" >/dev/null 2>&1 || true
                    fi
                else
                    [ "$had_manifest" -eq 1 ] && chmod 0444 "$PACKAGES_CFG" || true
                    [ "$had_uninstall" -eq 1 ] && chmod 0555 "$UNINSTALL_SCRIPT" || true
                fi
            fi
        fi
        rm -rf "$staging"
    }
    trap rollback EXIT HUP INT TERM

    info "Installing cup into $CUP_ROOT"
    download_file "$BASE_URL/$cup_asset" "$staging/$cup_asset"
    download_file "$BASE_URL/packages.cfg" "$staging/packages.cfg"
    download_file "$BASE_URL/$uninstall_asset" "$staging/$uninstall_asset"
    download_file "$BASE_URL/SHA256SUMS.$platform" "$staging/SHA256SUMS.$platform"
    download_file "$BASE_URL/SHA256SUMS.common" "$staging/SHA256SUMS.common"
    verify_checksum_file "$staging" "SHA256SUMS.$platform"
    verify_checksum_file "$staging" "SHA256SUMS.common"

    clear_windows_read_only "$PACKAGES_CFG"
    clear_windows_read_only "$UNINSTALL_SCRIPT"

    if [ -e "$cup_bin" ]; then
        mv "$cup_bin" "$backup/bin"
        had_bin=1
    fi
    if [ -e "$PACKAGES_CFG" ]; then
        mv "$PACKAGES_CFG" "$backup/packages.cfg"
        had_manifest=1
    fi
    if [ -e "$UNINSTALL_SCRIPT" ]; then
        mv "$UNINSTALL_SCRIPT" "$backup/uninstall"
        had_uninstall=1
    fi

    mv "$staging/$cup_asset" "$cup_bin"
    installed_bin=1
    mv "$staging/packages.cfg" "$PACKAGES_CFG"
    installed_manifest=1
    mv "$staging/$uninstall_asset" "$UNINSTALL_SCRIPT"
    installed_uninstall=1

    chmod 0755 "$cup_bin"
    chmod 0444 "$PACKAGES_CFG"
    if [ "$WINDOWS_SHELL_INSTALL" -eq 1 ]; then
        chmod 0444 "$UNINSTALL_SCRIPT" 2>/dev/null || true
        set_windows_read_only "$PACKAGES_CFG"
        set_windows_read_only "$UNINSTALL_SCRIPT"
    else
        chmod 0555 "$UNINSTALL_SCRIPT"
    fi

    committed=1
    trap - EXIT HUP INT TERM
    rm -rf "$staging"

    info "cup installed successfully."
    info "Binary:    $cup_bin"
    info "Manifest:  $PACKAGES_CFG"
    info "Uninstall: $UNINSTALL_SCRIPT"
    offer_path_update

    if [ "$CUP_AVAILABLE_IN_PATH" -eq 1 ]; then
        info "Test with: cup help"
    else
        info "Test with: $cup_bin help"
    fi
}

install_unix() {
    os="$(uname -s 2>/dev/null || true)"
    arch="$(uname -m 2>/dev/null || true)"

    case "$os:$arch" in
        Linux:x86_64|Linux:amd64)
            asset="cup-linux-x64"
            platform="linux-x64"
            ;;
        Linux:aarch64|Linux:arm64)
            asset="cup-linux-arm64"
            platform="linux-arm64"
            ;;
        Darwin:x86_64|Darwin:amd64)
            asset="cup-macos-x64"
            platform="macos-x64"
            ;;
        Darwin:arm64|Darwin:aarch64)
            asset="cup-macos-arm64"
            platform="macos-arm64"
            ;;
        *)
            fail "unsupported platform: $os $arch"
            ;;
    esac

    WINDOWS_SHELL_INSTALL=0
    configure_paths "$HOME/.cup" "uninstall.sh"
    install_assets "$asset" "cup" "$platform" "uninstall.sh"
}

run_powershell_installer() {
    ps_command="iwr $BASE_URL/install.ps1 -OutFile \$env:TEMP\\install-cup.ps1; & \$env:TEMP\\install-cup.ps1"

    if command -v powershell.exe >/dev/null 2>&1; then
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ps_command"
        exit $?
    fi

    if command -v pwsh.exe >/dev/null 2>&1; then
        pwsh.exe -NoProfile -Command "$ps_command"
        exit $?
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

    case "$arch" in
        x86_64|amd64)
            ;;
        *)
            fail "unsupported Windows architecture: $arch. This installer supports x64 only"
            ;;
    esac

    WINDOWS_SHELL_INSTALL=1
    configure_paths "$(get_windows_profile_root)/.cup" "uninstall.ps1"
    install_assets "cup-windows-x64.exe" "cup.exe" "windows-x64" "uninstall.ps1"
}

install_windows_from_shell() {
    info "Windows Unix-like shell detected."
    info ""
    info "Choose installation mode:"
    info "  1) Native Windows installation via PowerShell"
    info "     Updates the Windows user PATH."
    info ""
    info "  2) Installation from the current shell"
    info "     Uses the same %USERPROFILE%\\.cup root, but updates this shell profile."
    info ""

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

main() {
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
