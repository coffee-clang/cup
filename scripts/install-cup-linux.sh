#!/usr/bin/env sh
set -eu

REPO_OWNER="coffee-clang"
REPO_NAME="cup"
RELEASE_TAG="cup-bootstrap"

BASE_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases/download/${RELEASE_TAG}"

CUP_HOME="${CUP_HOME:-"$HOME/.cup"}"
CUP_BIN_DIR="$CUP_HOME/bin"
CUP_CONFIG_DIR="$CUP_HOME/config"

CUP_BIN="$CUP_BIN_DIR/cup"
PACKAGES_CFG="$CUP_CONFIG_DIR/packages.cfg"

CUP_ASSET="cup-linux-x64"
PACKAGES_ASSET="packages.cfg"

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

detect_platform() {
    os="$(uname -s 2>/dev/null || true)"
    arch="$(uname -m 2>/dev/null || true)"

    case "$os" in
        Linux)
            ;;
        *)
            die "unsupported operating system: $os. This installer currently supports Linux only."
            ;;
    esac

    case "$arch" in
        x86_64|amd64)
            ;;
        *)
            die "unsupported architecture: $arch. This installer currently supports x86_64 only."
            ;;
    esac
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

offer_path_update() {
    profile="$(detect_shell_profile)"

    if printf '%s' ":$PATH:" | grep -F ":$CUP_BIN_DIR:" >/dev/null 2>&1; then
        info "cup bin directory is already available in PATH for this shell."
        return 0
    fi

    if path_line_exists "$profile"; then
        info "PATH entry already exists in $profile."
        info "Restart your shell or run:"
        info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
        return 0
    fi

    printf '\nAdd %s to PATH in %s? [y/N] ' "$CUP_BIN_DIR" "$profile"
    read answer || answer=""

    case "$answer" in
        y|Y|yes|YES)
            {
                printf '\n# cup\n'
                printf 'export PATH="$HOME/.cup/bin:$PATH"\n'
            } >> "$profile"

            info "PATH updated in $profile."
            info "Restart your shell or run:"
            info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
            ;;
        *)
            info "PATH not modified."
            info "To use cup without the full path, add this line to your shell profile:"
            info "  export PATH=\"\$HOME/.cup/bin:\$PATH\""
            ;;
    esac
}

main() {
    detect_platform

    need_command chmod
    need_command mkdir
    need_command mv
    need_command rm
    need_command uname

    info "Installing cup into $CUP_HOME"

    mkdir -p "$CUP_BIN_DIR"
    mkdir -p "$CUP_CONFIG_DIR"

    info "Downloading cup binary..."
    download_file "$BASE_URL/$CUP_ASSET" "$CUP_BIN"
    chmod +x "$CUP_BIN"

    info "Downloading package manifest..."
    download_file "$BASE_URL/$PACKAGES_ASSET" "$PACKAGES_CFG"

    info ""
    info "cup installed successfully."
    info "Binary:   $CUP_BIN"
    info "Manifest: $PACKAGES_CFG"
    info ""

    offer_path_update

    info ""
    info "You can test the installation with:"
    info "  $CUP_BIN --help"
}

main "$@"