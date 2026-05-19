#!/usr/bin/env sh
set -eu

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
RELEASE_TAG="cup-bootstrap"

BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/${RELEASE_TAG}"

CUP_HOME="${CUP_HOME:-"$HOME/.cup"}"
CUP_BIN_DIR="$CUP_HOME/bin"
CUP_CONFIG_DIR="$CUP_HOME/config"
CUP_SCRIPTS_DIR="$CUP_HOME/scripts"

PACKAGES_CFG="$CUP_CONFIG_DIR/packages.cfg"
UNINSTALL_SCRIPT="$CUP_SCRIPTS_DIR/uninstall.sh"

PACKAGES_ASSET="packages.cfg"
UNINSTALL_ASSET="uninstall.sh"

CUP_AVAILABLE_IN_PATH=0

die() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

info() {
    printf '%s\n' "$*"
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
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

download_file() {
    url="$1"
    output="$2"

    tmp="${output}.tmp"

    rm -f "$tmp"

    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$tmp" || {
            rm -f "$tmp"
            die "failed to download $url"
        }
    elif command -v wget >/dev/null 2>&1; then
        wget -q "$url" -O "$tmp" || {
            rm -f "$tmp"
            die "failed to download $url"
        }
    else
        die "neither curl nor wget is available"
    fi

    mv "$tmp" "$output"
}

detect_shell_profile() {
    shell_name="$(basename "${SHELL:-}")"

    case "$shell_name" in
        zsh)
            printf '%s\n' "$HOME/.zshrc"
            ;;
        bash)
            printf '%s\n' "$HOME/.bashrc"
            ;;
        *)
            if [ -f "$HOME/.bashrc" ]; then
                printf '%s\n' "$HOME/.bashrc"
            elif [ -f "$HOME/.profile" ]; then
                printf '%s\n' "$HOME/.profile"
            else
                printf '%s\n' "$HOME/.profile"
            fi
            ;;
    esac
}

path_line_exists() {
    profile="$1"

    [ -f "$profile" ] || return 1

    grep -F 'export PATH="$HOME/.cup/bin:$PATH"' "$profile" >/dev/null 2>&1
}

cup_bin_in_current_path() {
    printf '%s' ":$PATH:" | grep -F ":$CUP_BIN_DIR:" >/dev/null 2>&1
}

offer_path_update_unix_shell() {
    profile="$(detect_shell_profile)"

    if cup_bin_in_current_path; then
        CUP_AVAILABLE_IN_PATH=1
        info "cup bin directory is already available in PATH for this shell."
        return 0
    fi

    if path_line_exists "$profile"; then
        CUP_AVAILABLE_IN_PATH=1
        info "PATH entry already exists in $profile."
        info "Restart your shell or run:"
        info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
        return 0
    fi

    answer="$(prompt_tty "Add $CUP_BIN_DIR to PATH in $profile? [y/N] " "")"

    case "$answer" in
        y|Y|yes|YES)
            {
                printf '\n# cup\n'
                printf 'export PATH="$HOME/.cup/bin:$PATH"\n'
            } >> "$profile"

            CUP_AVAILABLE_IN_PATH=1
            info "PATH updated in $profile."
            info "Restart your shell or run:"
            info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
            ;;
        *)
            CUP_AVAILABLE_IN_PATH=0
            info "PATH not modified."
            info "To use cup without the full path, add this line to your shell profile:"
            info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
            ;;
    esac
}

detect_arch_x64() {
    arch="$(uname -m 2>/dev/null || true)"

    case "$arch" in
        x86_64|amd64)
            return 0
            ;;
        *)
            die "unsupported architecture: $arch. This installer currently supports x86_64 only."
            ;;
    esac
}

print_install_test_hint() {
    cup_bin="$1"

    info ""
    info "You can test the installation with:"

    if [ "$CUP_AVAILABLE_IN_PATH" = "1" ]; then
        info "  cup help"
        info ""
        info "If 'cup' is not found yet, restart your shell or run:"
        info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
    else
        info "  $cup_bin help"
    fi
}

install_unix_like() {
    cup_asset="$1"
    installed_name="$2"

    cup_bin="$CUP_BIN_DIR/$installed_name"

    need_command chmod
    need_command mkdir
    need_command mv
    need_command rm
    need_command uname

    detect_arch_x64

    info "Installing cup into $CUP_HOME"

    mkdir -p "$CUP_BIN_DIR"
    mkdir -p "$CUP_CONFIG_DIR"
    mkdir -p "$CUP_SCRIPTS_DIR"

    info "Downloading cup binary..."
    download_file "$BASE_URL/$cup_asset" "$cup_bin"
    chmod +x "$cup_bin"

    info "Downloading package manifest..."
    download_file "$BASE_URL/$PACKAGES_ASSET" "$PACKAGES_CFG"

    info "Downloading uninstall script..."
    download_file "$BASE_URL/$UNINSTALL_ASSET" "$UNINSTALL_SCRIPT"
    chmod +x "$UNINSTALL_SCRIPT"

    info ""
    info "cup installed successfully."
    info "Binary:    $cup_bin"
    info "Manifest:  $PACKAGES_CFG"
    info "Uninstall: $UNINSTALL_SCRIPT"
    info ""

    offer_path_update_unix_shell
    print_install_test_hint "$cup_bin"
}

run_powershell_installer() {
    ps_command="irm https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/${RELEASE_TAG}/install.ps1 | iex"

    if command -v powershell.exe >/dev/null 2>&1; then
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ps_command"
        exit $?
    fi

    if command -v pwsh.exe >/dev/null 2>&1; then
        pwsh.exe -NoProfile -Command "$ps_command"
        exit $?
    fi

    die "PowerShell was not found. Run the Windows installer manually from PowerShell: irm $BASE_URL/install.ps1 | iex"
}

install_windows_from_shell() {
    info "Windows shell detected."
    info ""
    info "Choose installation mode:"
    info "  1) Native Windows installation via PowerShell"
    info "     Installs to: C:\\Users\\<user>\\.cup"
    info "     Recommended if you want cup available from normal Windows terminals."
    info ""
    info "  2) Current Unix-like shell environment"
    info "     Installs to: $HOME/.cup"
    info "     Recommended only if you mainly use cup inside MSYS2/Git Bash/Cygwin."
    info ""

    choice="$(prompt_tty "Choice [1/2, default: 1]: " "1")"

    case "$choice" in
        ""|1)
            run_powershell_installer
            ;;
        2)
            install_unix_like "cup-windows-x64.exe" "cup.exe"
            ;;
        *)
            die "invalid choice"
            ;;
    esac
}

main() {
    os="$(uname -s 2>/dev/null || true)"

    case "$os" in
        Linux)
            install_unix_like "cup-linux-x64" "cup"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            install_windows_from_shell
            ;;
        *)
            die "unsupported operating system: $os"
            ;;
    esac
}

main "$@"