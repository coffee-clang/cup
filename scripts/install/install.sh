#!/usr/bin/env sh

# Downloads one immutable cup release generation, authenticates the native bootstrap
# inputs with release.txt, then delegates all managed-root mutation to that binary.
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
                [1-9][0-9][0-9][0-9][0-9]|[1-9][0-9][0-9][0-9][0-9][0-9]) ;;
            *) fail 'installer has an invalid release version' ;;
        esac
    done
    [ "$CUP_RELEASE_TAG" = "v$CUP_RELEASE_VERSION" ] ||
        fail 'installer release tag does not match its version'
    [ "${#CUP_RELEASE_COMMIT}" -eq 40 ] || fail 'installer has an invalid release commit'
    case "$CUP_RELEASE_COMMIT" in *[!0-9a-f]*) fail 'installer has an invalid release commit' ;; esac
}

validate_base_url() {
    while [ "${BASE_URL%/}" != "$BASE_URL" ]; do BASE_URL=${BASE_URL%/}; done
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
            fail 'installer test release base URL is invalid' ;;
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
    case "$port" in ''|*[!0-9]*) fail 'installer release base URL override has an invalid port' ;; esac
    [ "$port" -ge 1 ] 2>/dev/null && [ "$port" -le 65535 ] 2>/dev/null ||
        fail 'installer release base URL override has an invalid port'
    TRANSPORT_PROTOCOL=http
    MAX_REDIRECTS=0
    export BASE_URL TRANSPORT_PROTOCOL MAX_REDIRECTS
}

detect_platform() {
    os=$(uname -s) || fail 'could not detect the operating system'
    arch=$(uname -m) || fail 'could not detect the architecture'
    case "$os" in
        Linux) os=linux ;;
        Darwin) os=macos ;;
        MSYS*|MINGW*|CYGWIN*) run_windows_installer ;;
        *) fail "unsupported operating system: $os" ;;
    esac
    case "$arch" in
        x86_64|amd64) arch=x64 ;;
        arm64|aarch64) arch=arm64 ;;
        *) fail "unsupported architecture: $arch" ;;
    esac
    PLATFORM=$os-$arch
    case "$PLATFORM" in linux-x64|linux-arm64|macos-x64|macos-arm64) ;; *) fail "unsupported platform: $PLATFORM" ;; esac
    BINARY_ASSET=cup-$PLATFORM
    export PLATFORM BINARY_ASSET
}

select_hash_command() {
    if command -v sha256sum >/dev/null 2>&1; then HASH_COMMAND=sha256sum
    elif command -v shasum >/dev/null 2>&1; then HASH_COMMAND='shasum -a 256'
    else fail 'sha256sum or shasum is required'
    fi
    export HASH_COMMAND
}

require_commands() {
    for command_name in basename chmod cp curl dirname grep mkdir mktemp mv readlink rm uname wc; do
        command -v "$command_name" >/dev/null 2>&1 || fail "required command is unavailable: $command_name"
    done
    select_hash_command
}

create_work_directory() {
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/cup-install.XXXXXX") || fail 'could not create the private transport directory'
    chmod 0700 "$WORK" || fail 'could not protect the transport directory'
    WORK=$(CDPATH= cd -- "$WORK" && pwd -P) || fail 'could not resolve the private transport directory'
    case "$WORK" in /*) ;; *) fail 'transport directory is not absolute' ;; esac
}

download_asset() {
    asset=$1
    case "$asset" in ''|*/*|*\*|.|..) fail "unsafe release asset name: $asset" ;; esac
    case "$asset" in cup-*) maximum=$MAX_BINARY_BYTES ;; *) maximum=$MAX_TEXT_BYTES ;; esac
    destination=$WORK/$asset
    url=$BASE_URL/$asset
    curl -q --fail --location --silent --show-error \
        --proto "=$TRANSPORT_PROTOCOL" --proto-redir "=$TRANSPORT_PROTOCOL" \
        --max-redirs "$MAX_REDIRECTS" \
        --connect-timeout 15 --max-time 180 --speed-time 30 --speed-limit 1024 \
        --max-filesize "$maximum" --output "$destination" "$url" || fail "could not download $asset"
    [ -f "$destination" ] && [ ! -L "$destination" ] && [ -s "$destination" ] ||
        fail "downloaded asset is not a non-empty regular file: $asset"
    size=$(wc -c < "$destination") || fail "could not measure $asset"
    # shellcheck disable=SC2086
    set -- $size
    [ "$#" -eq 1 ] || fail "could not measure $asset"
    size=$1
    case "$size" in ''|*[!0-9]*) fail "could not measure $asset" ;; esac
    [ "$size" -le "$maximum" ] || fail "downloaded asset is too large: $asset"
}

hash_file() {
    if [ "$HASH_COMMAND" = sha256sum ]; then result=$(sha256sum "$1") || return 1
    else result=$(shasum -a 256 "$1") || return 1
    fi
    printf '%s\n' "${result%% *}"
}

validate_hash() {
    [ "${#1}" -eq 64 ] || return 1
    case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

manifest_record_required_hash() {
    name=$1
    hash=$2
    case "$name" in
        "$BINARY_ASSET") [ -z "${BINARY_SHA:-}" ] || fail "release manifest duplicates $name"; BINARY_SHA=$hash ;;
        LICENSE) [ -z "${LICENSE_SHA:-}" ] || fail 'release manifest duplicates LICENSE'; LICENSE_SHA=$hash ;;
        THIRD_PARTY_NOTICES.txt) [ -z "${NOTICES_SHA:-}" ] || fail 'release manifest duplicates THIRD_PARTY_NOTICES.txt'; NOTICES_SHA=$hash ;;
        catalog.cfg) [ -z "${CATALOG_SHA:-}" ] || fail 'release manifest duplicates catalog.cfg'; CATALOG_SHA=$hash ;;
        install.ps1) [ -z "${INSTALL_PS1_SHA:-}" ] || fail 'release manifest duplicates install.ps1'; INSTALL_PS1_SHA=$hash ;;
    esac
}

validate_release_manifest() {
    metadata=$WORK/release.txt
    BINARY_SHA= LICENSE_SHA= NOTICES_SHA= CATALOG_SHA= INSTALL_PS1_SHA=
    exec 3< "$metadata" || fail 'could not read release metadata'
    IFS= read -r line <&3 || fail 'release metadata is incomplete'; [ "$line" = format=2 ] || fail 'release metadata has an unsupported format'
    IFS= read -r line <&3 || fail 'release metadata is incomplete'; [ "$line" = "version=$CUP_RELEASE_VERSION" ] || fail 'release metadata version does not match the installer'
    IFS= read -r line <&3 || fail 'release metadata is incomplete'; [ "$line" = "commit=$CUP_RELEASE_COMMIT" ] || fail 'release metadata commit does not match the installer'
    IFS= read -r line <&3 || fail 'release metadata is incomplete'; [ "$line" = root_layout=2 ] || fail 'release metadata root layout is incompatible'
    IFS= read -r line <&3 || fail 'release metadata is incomplete'; [ "$line" = catalog_format=1 ] || fail 'release metadata catalog format is incompatible'
    IFS= read -r line <&3 || fail 'release metadata is incomplete'
    case "$line" in asset_count=*) asset_count=${line#asset_count=} ;; *) fail 'release metadata asset count is missing' ;; esac
    case "$asset_count" in ''|*[!0-9]*) fail 'release metadata asset count is invalid' ;; esac
    [ "$asset_count" -gt 0 ] && [ "$asset_count" -le 256 ] || fail 'release metadata asset count is invalid'
    seen='|'
    index=0
    while [ "$index" -lt "$asset_count" ]; do
        IFS= read -r name_line <&3 || fail 'release metadata asset record is incomplete'
        IFS= read -r sha_line <&3 || fail 'release metadata asset record is incomplete'
        name_prefix=asset.$index.name=
        sha_prefix=asset.$index.sha256=
        case "$name_line" in "$name_prefix"*) name=${name_line#"$name_prefix"} ;; *) fail 'release metadata asset record is not contiguous' ;; esac
        case "$sha_line" in "$sha_prefix"*) sha=${sha_line#"$sha_prefix"} ;; *) fail 'release metadata asset record is not contiguous' ;; esac
        case "$name" in ''|*[!A-Za-z0-9._-]*) fail 'release metadata contains an unsafe asset name' ;; esac
        validate_hash "$sha" || fail "release metadata has an invalid digest for $name"
        case "$seen" in *"|$name|"*) fail "release metadata duplicates asset $name" ;; esac
        seen=$seen$name'|'
        manifest_record_required_hash "$name" "$sha"
        index=$((index + 1))
    done
    extra=
    if IFS= read -r extra <&3 || [ -n "$extra" ]; then fail 'release metadata has unexpected records'; fi
    exec 3<&-
}

verify_asset() {
    asset=$1
    expected=$2
    [ -n "$expected" ] || fail "release manifest does not authenticate $asset"
    actual=$(hash_file "$WORK/$asset") || fail "could not hash $asset"
    [ "$actual" = "$expected" ] || fail "release manifest digest mismatch for $asset"
}

run_windows_installer() {
    PLATFORM=windows-x64
    BINARY_ASSET=cup-windows-x64.exe
    export PLATFORM BINARY_ASSET
    for command_name in chmod curl cygpath mktemp powershell.exe rm wc; do
        command -v "$command_name" >/dev/null 2>&1 || fail "required Windows handoff command is unavailable: $command_name"
    done
    select_hash_command
    create_work_directory
    download_asset release.txt
    validate_release_manifest
    download_asset install.ps1
    verify_asset install.ps1 "$INSTALL_PS1_SHA"
    grep -F "\$ReleaseVersion = \"$CUP_RELEASE_VERSION\"" "$WORK/install.ps1" >/dev/null || fail 'Windows installer release version does not match the shell installer'
    grep -F "\$ReleaseTag = \"$CUP_RELEASE_TAG\"" "$WORK/install.ps1" >/dev/null || fail 'Windows installer release tag does not match the shell installer'
    grep -F "\$ReleaseCommit = \"$CUP_RELEASE_COMMIT\"" "$WORK/install.ps1" >/dev/null || fail 'Windows installer release commit does not match the shell installer'
    windows_installer=$(cygpath -w "$WORK/install.ps1") || fail 'could not translate the Windows installer path'
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$windows_installer" || fail 'Windows installer failed'
    exit 0
}

canonical_directory() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 1
    (CDPATH= cd -- "$1" && pwd -P)
}

path_entry_normalize() {
    value=$1
    while [ "$value" != / ] && [ "${value%/}" != "$value" ]; do value=${value%/}; done
    printf '%s\n' "$value"
}

path_contains_directory() {
    wanted=$(path_entry_normalize "$1")
    old_ifs=$IFS; IFS=:
    for entry in ${PATH:-}; do
        [ -n "$entry" ] || entry=.
        entry=$(path_entry_normalize "$entry")
        [ "$entry" = "$wanted" ] && { IFS=$old_ifs; return 0; }
    done
    IFS=$old_ifs
    return 1
}

resolve_command_path() {
    path=$1
    case "$path" in /*) ;; *) return 1 ;; esac
    hops=0
    while [ -L "$path" ]; do
        [ "$hops" -lt 32 ] || return 1
        target=$(readlink "$path") || return 1
        case "$target" in /*) path=$target ;; *) path=$(dirname -- "$path")/$target ;; esac
        hops=$((hops + 1))
    done
    [ -f "$path" ] && [ ! -L "$path" ] && [ -x "$path" ] || return 1
    directory=$(canonical_directory "$(dirname -- "$path")") || return 1
    printf '%s/%s\n' "$directory" "$(basename -- "$path")"
}

native_root_probe() { "$WORK/$BINARY_ASSET" --internal-root-probe "$1" >/dev/null 2>&1; }

installed_version() {
    output=$("$1" --version 2>/dev/null) || return 1
    case "$output" in 'cup '*) printf '%s\n' "${output#cup }" ;; *) return 1 ;; esac
}

find_path_installation() {
    PATH_ROOT= PATH_BINARY=
    command_path=$(command -v cup 2>/dev/null || true); [ -n "$command_path" ] || return 1
    command_path=$(resolve_command_path "$command_path") || return 1
    [ "$(basename -- "$command_path")" = cup ] || return 1
    bin_dir=$(canonical_directory "$(dirname -- "$command_path")") || return 1
    [ "$(basename -- "$bin_dir")" = bin ] || return 1
    root=$(canonical_directory "$(dirname -- "$bin_dir")") || return 1
    case "$(basename -- "$root")" in .cup|.coffee-cup) ;; *) return 1 ;; esac
    native_root_probe "$root" || return 1
    PATH_ROOT=$root; PATH_BINARY=$command_path; export PATH_ROOT PATH_BINARY
}

select_root_for_base() {
    base=$1
    selected=$("$WORK/$BINARY_ASSET" --internal-select-root "$base") || fail "could not select a canonical cup root below $base"
    case "$selected" in "$base/.cup"|"$base/.coffee-cup") ;; *) fail 'native root selection returned an unexpected path' ;; esac
    SELECTED_ROOT=$selected; SELECTED_BASE=$base; export SELECTED_ROOT SELECTED_BASE
}

choose_installation() {
    if [ -n "${CUP_INSTALL_BASE_DIR:-}" ]; then
        base=$(canonical_directory "$CUP_INSTALL_BASE_DIR") || fail 'CUP_INSTALL_BASE_DIR must name an existing real directory'
        select_root_for_base "$base"; return 0
    fi
    if find_path_installation && [ -t 0 ]; then
        current_version=$(installed_version "$PATH_BINARY") || fail 'authenticated PATH installation has an invalid version response'
        printf 'Found cup %s at %s. Use this installation? [Y/n] ' "$current_version" "$PATH_ROOT"
        IFS= read -r answer || answer=
        case "$answer" in ''|y|Y|yes|YES|Yes)
            SELECTED_ROOT=$PATH_ROOT
            SELECTED_BASE=$(canonical_directory "$(dirname -- "$PATH_ROOT")") || fail 'could not resolve the PATH installation base'
            export SELECTED_ROOT SELECTED_BASE; return 0 ;;
        esac
    fi
    base=$HOME
    if [ -t 0 ]; then
        printf 'Choose the parent/base directory for cup [%s]: ' "$HOME"
        IFS= read -r answer || answer=; [ -z "$answer" ] || base=$answer
    fi
    base=$(canonical_directory "$base") || fail 'selected cup base must be an existing real directory'
    select_root_for_base "$base"
}

check_target_version() {
    FRESH_INSTALL=1
    if native_root_probe "$SELECTED_ROOT"; then FRESH_INSTALL=0; fi
    # The verified native bootstrap owns healthy-downgrade refusal and can repair a missing or
    # corrupt canonical binary from the authenticated target generation.
    export FRESH_INSTALL
}

parse_bootstrap_root() {
    bootstrap_root=; root_records=0
    while IFS= read -r line; do
        case "$line" in CUP_BOOTSTRAP_ROOT=*) bootstrap_root=${line#CUP_BOOTSTRAP_ROOT=}; root_records=$((root_records + 1)) ;; *) printf '%s\n' "$line" ;; esac
    done <<EOF_BOOTSTRAP
$1
EOF_BOOTSTRAP
    [ "$root_records" -eq 1 ] || fail 'bootstrap did not report one canonical root'
    [ "$bootstrap_root" = "$SELECTED_ROOT" ] || fail 'bootstrap changed the selected canonical root'
    BOOTSTRAP_ROOT=$bootstrap_root; export BOOTSTRAP_ROOT
}

validate_committed_root() {
    INSTALLED_BINARY=$BOOTSTRAP_ROOT/bin/cup
    native_root_probe "$BOOTSTRAP_ROOT" || fail 'installed cup root did not validate after bootstrap'
    [ -f "$INSTALLED_BINARY" ] && [ ! -L "$INSTALLED_BINARY" ] && [ -x "$INSTALLED_BINARY" ] || fail 'installed cup binary is unavailable after bootstrap'
    [ "$(installed_version "$INSTALLED_BINARY")" = "$CUP_RELEASE_VERSION" ] || fail 'installed cup version does not match the verified release'
    export INSTALLED_BINARY
}

attempt_fresh_coffee() {
    [ "$FRESH_INSTALL" -eq 1 ] || return 0
    if "$INSTALLED_BINARY" install coffee; then
        printf 'Coffee installed successfully.\n'
        return 0
    fi
    if [ -e "$SELECTED_ROOT/transaction.txt" ] || [ -L "$SELECTED_ROOT/transaction.txt" ]; then
        fail 'optional Coffee installation left an unresolved cup transaction; run cup repair'
    fi
    coffee_state=$("$INSTALLED_BINARY" list package-manager 2>/dev/null || true)
    case "$coffee_state" in
        *'package-manager: coffee@'*)
            printf 'Warning: Coffee was installed, but derived commands need repair; run cup repair.\n' >&2
            return 0
            ;;
    esac
    printf 'Warning: Coffee was not installed; the cup core installation is ready.\n' >&2
}

path_block_start='# >>> cup PATH >>>'
path_block_end='# <<< cup PATH <<<'

write_path_block() {
    profile=$1
    shell_kind=$2
    bin=$SELECTED_ROOT/bin
    directory=$(dirname -- "$profile")

    if [ -e "$profile" ] || [ -L "$profile" ]; then
        if [ ! -f "$profile" ] || [ -L "$profile" ]; then
            printf 'Warning: cup PATH target %s is not a regular file; preserved unchanged.
' "$profile" >&2
            return 1
        fi
    fi
    mkdir -p -- "$directory" || return 1
    start_count=0
    end_count=0
    if [ -f "$profile" ]; then
        start_count=$(grep -Fxc -- "$path_block_start" "$profile" 2>/dev/null || true)
        end_count=$(grep -Fxc -- "$path_block_end" "$profile" 2>/dev/null || true)
        if [ "$start_count" -ne "$end_count" ] || [ "$start_count" -gt 1 ]; then
            printf 'Warning: cup PATH markers in %s are malformed; preserved for manual repair.\n' "$profile" >&2
            return 1
        fi
        if [ "$start_count" -eq 1 ] && grep -F -- "$bin" "$profile" >/dev/null 2>&1; then
            return 0
        fi
    fi

    temporary=$(mktemp "$directory/.cup-path.XXXXXX") || return 1
    if [ -f "$profile" ]; then
        cp -p -- "$profile" "$temporary" || { rm -f -- "$temporary"; return 1; }
        : > "$temporary" || { rm -f -- "$temporary"; return 1; }
        skipping=0
        while IFS= read -r line || [ -n "$line" ]; do
            if [ "$line" = "$path_block_start" ]; then skipping=1; continue; fi
            if [ "$line" = "$path_block_end" ]; then skipping=0; continue; fi
            [ "$skipping" -eq 1 ] || printf '%s\n' "$line" >> "$temporary" || {
                rm -f -- "$temporary"; return 1;
            }
        done < "$profile"
        [ "$skipping" -eq 0 ] || { rm -f -- "$temporary"; return 1; }
    else
        chmod 0644 "$temporary" || { rm -f -- "$temporary"; return 1; }
    fi

    {
        printf '\n%s\n' "$path_block_start"
        if [ "$shell_kind" = fish ]; then
            printf 'if not contains -- %s $PATH\n' "'$bin'"
            printf '    set -gx PATH %s $PATH\n' "'$bin'"
            printf 'end\n'
        else
            printf 'case ":$PATH:" in *:%s:*) ;; *) export PATH=%s:"$PATH" ;; esac\n' "'$bin'" "'$bin'"
        fi
        printf '%s\n' "$path_block_end"
    } >> "$temporary" || { rm -f -- "$temporary"; return 1; }

    mv -f -- "$temporary" "$profile" || { rm -f -- "$temporary"; return 1; }
    return 0
}

add_posix_path() {
    bin=$SELECTED_ROOT/bin
    path_contains_directory "$bin" && return 0
    case "$bin" in
        *"'"*|*[[:cntrl:]]*)
            printf 'PATH integration skipped because the cup path contains shell-unsafe quoting/control content.\n' >&2
            return 0
            ;;
    esac

    shell_name=${SHELL##*/}
    case "$shell_name" in
        bash)
            bashrc=$HOME/.bashrc
            login=
            for candidate in "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.profile"; do
                if [ -r "$candidate" ]; then login=$candidate; break; fi
            done
            [ -n "$login" ] || login=$HOME/.bash_profile
            first_ok=1
            second_ok=1
            write_path_block "$bashrc" sh || first_ok=0
            write_path_block "$login" sh || second_ok=0
            if [ "$first_ok" -ne 1 ] || [ "$second_ok" -ne 1 ]; then
                printf 'Warning: cup could not update every Bash startup file; configure %s manually where needed.\n' "$bin" >&2
                return 0
            fi
            ;;
        zsh)
            zdot=${ZDOTDIR:-$HOME}
            write_path_block "$zdot/.zshrc" sh || {
                printf 'Warning: cup could not update the Zsh PATH configuration; add %s manually.\n' "$bin" >&2
                return 0
            }
            ;;
        fish)
            fish_config=${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/cup.fish
            write_path_block "$fish_config" fish || {
                printf 'Warning: cup could not update the Fish PATH configuration; add %s manually.\n' "$bin" >&2
                return 0
            }
            ;;
        *)
            printf 'Automatic PATH integration is not defined for shell %s; add %s manually.\n' "${shell_name:-unknown}" "$bin" >&2
            return 0
            ;;
    esac
    printf 'Added %s to your user shell PATH configuration. Open a new shell to use it.\n' "$bin"
}

offer_path_integration() {
    bin=$SELECTED_ROOT/bin
    case "$bin" in
        *:*)
            printf "Automatic user PATH integration is unavailable for %s because the path contains ':'. Configure PATH manually or use the full path to cup.\n" "$bin" >&2
            return 0
            ;;
    esac
    path_contains_directory "$bin" && return 0
    if [ "${CUP_INSTALL_NO_PATH_PROMPT:-0}" = 1 ]; then
        printf 'cup was not added to PATH. Add %s manually if desired.\n' "$bin"
        return 0
    fi
    if { [ ! -t 1 ] && [ ! -t 2 ]; } || ! (: </dev/tty) 2>/dev/null; then
        printf 'No interactive terminal is available; cup was not added to PATH. Add %s manually if desired.\n' "$bin"
        return 0
    fi
    printf 'Add %s to your user PATH? [y/N] ' "$bin" >/dev/tty
    IFS= read -r answer </dev/tty || answer=
    case "$answer" in y|Y|yes|YES|Yes) add_posix_path ;; esac
}

validate_identity
validate_base_url
detect_platform
require_commands
create_work_directory

download_asset release.txt
validate_release_manifest
for asset in "$BINARY_ASSET" LICENSE THIRD_PARTY_NOTICES.txt catalog.cfg; do download_asset "$asset"; done
verify_asset "$BINARY_ASSET" "$BINARY_SHA"
verify_asset LICENSE "$LICENSE_SHA"
verify_asset THIRD_PARTY_NOTICES.txt "$NOTICES_SHA"
verify_asset catalog.cfg "$CATALOG_SHA"
chmod 0700 "$WORK/$BINARY_ASSET" || fail 'could not make the verified bootstrap executable'

choose_installation
check_target_version
printf 'cup will be installed in %s\n' "$SELECTED_ROOT"
bootstrap_output=$("$WORK/$BINARY_ASSET" --internal-bootstrap "$WORK" "$SELECTED_BASE") || fail 'the verified cup bootstrap transaction was rejected'
parse_bootstrap_root "$bootstrap_output"
validate_committed_root
attempt_fresh_coffee
printf 'cup %s installed successfully.\n' "$CUP_RELEASE_VERSION"
printf 'Binary: %s\n' "$INSTALLED_BINARY"
offer_path_integration
