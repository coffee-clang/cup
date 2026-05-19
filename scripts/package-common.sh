#!/usr/bin/env bash
set -euo pipefail

CUP_REPO_OWNER="${CUP_REPO_OWNER:-coffee-clang}"
CUP_REPO_NAME="${CUP_REPO_NAME:-cup}"

CUP_ROOT="${CUP_ROOT:-$(pwd)}"
CUP_WORK_DIR="${CUP_WORK_DIR:-$CUP_ROOT/.cup-build}"
CUP_SRC_DIR="${CUP_SRC_DIR:-$CUP_WORK_DIR/src}"
CUP_BUILD_DIR="${CUP_BUILD_DIR:-$CUP_WORK_DIR/build}"
CUP_STAGE_DIR="${CUP_STAGE_DIR:-$CUP_WORK_DIR/stage}"
CUP_OUT_DIR="${CUP_OUT_DIR:-$CUP_ROOT/dist}"

if [ -z "${CUP_JOBS:-}" ]; then
    if [ "${RUNNER_OS:-}" = "Windows" ] && [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
        CUP_JOBS="$NUMBER_OF_PROCESSORS"
    else
        CUP_JOBS="$(nproc)"
    fi
fi

DEFAULT_GCC_VERSION="${DEFAULT_GCC_VERSION:-16.1.0}"
DEFAULT_GDB_VERSION="${DEFAULT_GDB_VERSION:-17.1}"
DEFAULT_BINUTILS_VERSION="${DEFAULT_BINUTILS_VERSION:-2.46.0}"
DEFAULT_MINGW_VERSION="${DEFAULT_MINGW_VERSION:-14.0.0}"
DEFAULT_LLVM_VERSION="${DEFAULT_LLVM_VERSION:-22.1.5}"
DEFAULT_VALGRIND_VERSION="${DEFAULT_VALGRIND_VERSION:-3.27.0}"

log() {
    printf '[cup-build] %s\n' "$*" >&2
}

die() {
    printf '[cup-build:error] %s\n' "$*" >&2
    exit 1
}

need() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

make_dirs() {
    mkdir -p "$CUP_SRC_DIR" "$CUP_BUILD_DIR" "$CUP_STAGE_DIR" "$CUP_OUT_DIR"
}

resolve_version() {
    local tool="$1"
    local requested="$2"

    if [ "$requested" != "latest" ] && [ "$requested" != "stable" ]; then
        printf '%s\n' "$requested"
        return 0
    fi

    case "$tool" in
        gcc) printf '%s\n' "$DEFAULT_GCC_VERSION" ;;
        gdb) printf '%s\n' "$DEFAULT_GDB_VERSION" ;;
        binutils) printf '%s\n' "$DEFAULT_BINUTILS_VERSION" ;;
        mingw|mingw-w64) printf '%s\n' "$DEFAULT_MINGW_VERSION" ;;
        clang|lld|lldb|clangd|clang-format|clang-tidy|llvm) printf '%s\n' "$DEFAULT_LLVM_VERSION" ;;
        valgrind) printf '%s\n' "$DEFAULT_VALGRIND_VERSION" ;;
        *) die "cannot resolve default version for tool: $tool" ;;
    esac
}

platform_triple() {
    local platform="$1"

    case "$platform" in
        linux-x64) printf '%s\n' "x86_64-linux-gnu" ;;
        windows-x64) printf '%s\n' "x86_64-w64-mingw32" ;;
        *) die "unsupported platform: $platform" ;;
    esac
}

platform_family() {
    local platform="$1"

    case "$platform" in
        linux-x64|windows-x64) printf '%s\n' "gnu" ;;
        *) die "unsupported platform: $platform" ;;
    esac
}

platform_runtime() {
    local platform="$1"

    case "$platform" in
        linux-x64) printf '%s\n' "glibc" ;;
        windows-x64) printf '%s\n' "ucrt" ;;
        *) die "unsupported platform: $platform" ;;
    esac
}

platform_thread_model() {
    local platform="$1"

    case "$platform" in
        linux-x64) printf '%s\n' "posix" ;;
        windows-x64) printf '%s\n' "posix" ;;
        *) die "unsupported platform: $platform" ;;
    esac
}

host_extension() {
    local host_platform="$1"

    case "$host_platform" in
        windows-x64) printf '%s\n' ".exe" ;;
        *) printf '%s\n' "" ;;
    esac
}

is_windows_platform() {
    [ "$1" = "windows-x64" ]
}

is_cross_build() {
    [ "$1" != "$2" ]
}

package_uses_revision_in_name() {
    local tool="$1"
    local host_platform="$2"
    local target_platform="$3"

    if [ "$tool" = "gcc" ]; then
        return 0
    fi

    if is_cross_build "$host_platform" "$target_platform"; then
        return 0
    fi

    return 1
}

package_version_name() {
    local tool="$1"
    local version="$2"
    local host_platform="$3"
    local target_platform="$4"
    local revision="$5"

    if package_uses_revision_in_name "$tool" "$host_platform" "$target_platform"; then
        printf '%s-rev%s\n' "$version" "$revision"
    else
        printf '%s\n' "$version"
    fi
}

package_base_name() {
    local tool="$1"
    local version="$2"
    local host_platform="$3"
    local target_platform="$4"
    local revision="$5"

    local package_version
    package_version="$(package_version_name "$tool" "$version" "$host_platform" "$target_platform" "$revision")"

    printf '%s-%s-%s-%s\n' "$tool" "$package_version" "$host_platform" "$target_platform"
}

release_tag_for_package() {
    package_base_name "$@"
}

source_url_gcc() {
    local version="$1"
    printf 'https://ftp.gnu.org/gnu/gcc/gcc-%s/gcc-%s.tar.xz\n' "$version" "$version"
}

source_url_gdb() {
    local version="$1"
    printf 'https://ftp.gnu.org/gnu/gdb/gdb-%s.tar.xz\n' "$version"
}

source_url_binutils() {
    local version="$1"
    printf 'https://ftp.gnu.org/gnu/binutils/binutils-%s.tar.xz\n' "$version"
}

source_url_mingw() {
    local version="$1"
    printf 'https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/mingw-w64-v%s.tar.bz2/download\n' "$version"
}

source_url_llvm_project() {
    local version="$1"
    printf 'https://github.com/llvm/llvm-project/releases/download/llvmorg-%s/llvm-project-%s.src.tar.xz\n' "$version" "$version"
}

source_url_valgrind() {
    local version="$1"
    printf 'https://sourceware.org/pub/valgrind/valgrind-%s.tar.bz2\n' "$version"
}

archive_name_from_url() {
    local url="$1"
    local fallback="$2"
    local base

    base="$(basename "$url")"
    if [ "$base" = "download" ] || [ -z "$base" ]; then
        printf '%s\n' "$fallback"
    else
        printf '%s\n' "$base"
    fi
}

fetch() {
    local url="$1"
    local output="$2"

    if [ -f "$output" ]; then
        log "using cached archive: $output"
        return 0
    fi

    log "downloading: $url"

    if ! curl -fL --retry 3 --retry-delay 5 --connect-timeout 20 -o "$output" "$url"; then
        rm -f "$output"
        return 1
    fi
}

extract_archive() {
    local archive="$1"
    local destination="$2"

    rm -rf "$destination"
    mkdir -p "$destination"

    case "$archive" in
        *.tar.xz) tar -xJf "$archive" -C "$destination" --strip-components=1 ;;
        *.tar.gz|*.tgz) tar -xzf "$archive" -C "$destination" --strip-components=1 ;;
        *.tar.bz2|*.tbz2) tar -xjf "$archive" -C "$destination" --strip-components=1 ;;
        *.zip) unzip -q "$archive" -d "$destination" ;;
        *) die "unsupported archive format: $archive" ;;
    esac
}

prepare_source_tree() {
    local name="$1"
    local version="$2"
    local url="$3"
    local fallback_archive="$4"

    local archive
    local source_dir

    archive="$CUP_SRC_DIR/$(archive_name_from_url "$url" "$fallback_archive")"
    source_dir="$CUP_SRC_DIR/$name-$version"

    fetch "$url" "$archive"
    extract_archive "$archive" "$source_dir"

    printf '%s\n' "$source_dir"
}

write_info_file() {
    local prefix="$1"
    shift

    mkdir -p "$prefix"
    : > "$prefix/info.txt"

    local line
    for line in "$@"; do
        printf '%s\n' "$line" >> "$prefix/info.txt"
    done
}

package_formats_for_host() {
    local host_platform="$1"

    if is_windows_platform "$host_platform"; then
        printf '%s\n' "zip tar.xz tar.gz"
    else
        printf '%s\n' "tar.xz tar.gz zip"
    fi
}

package_formats_csv() {
    local host_platform="$1"
    package_formats_for_host "$host_platform" | paste -sd, -
}


windows_runtime_dll_allowed_path() {
    local dll_path="$1"

    case "$dll_path" in
        /ucrt64/bin/*|/mingw64/bin/*|/clang64/bin/*|/clangarm64/bin/*|/mingw32/bin/*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

windows_runtime_dll_is_system_path() {
    local dll_path="$1"
    local lower

    lower="$(printf '%s\n' "$dll_path" | tr '[:upper:]' '[:lower:]')"

    case "$lower" in
        /c/windows/*|/c/windows/system32/*|/c/windows/syswow64/*|c:\\windows\\*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

extract_windows_dll_path_from_ldd_line() {
    local line="$1"
    local path

    path="$(printf '%s\n' "$line" | sed -nE 's/.*=>[[:space:]]+([^[:space:]]+\.[dD][lL][lL]).*/\1/p')"
    if [ -n "$path" ]; then
        printf '%s\n' "$path"
        return 0
    fi

    path="$(printf '%s\n' "$line" | sed -nE 's/^[[:space:]]*([^[:space:]]+\.[dD][lL][lL]).*/\1/p')"
    if [ -n "$path" ]; then
        printf '%s\n' "$path"
    fi
}

copy_windows_runtime_dlls() {
    local bin_dir="$1"
    local item
    local current
    local line
    local dll_path
    local dll_name
    local destination
    local copied_count=0
    local index=0
    local queue=()
    local seen=()

    if [ "${HOST_PLATFORM:-}" != "windows-x64" ]; then
        return 0
    fi

    [ -d "$bin_dir" ] || return 0

    if ! command -v ldd >/dev/null 2>&1; then
        die "ldd is required to collect Windows runtime DLLs"
    fi

    shopt -s nullglob
    for item in "$bin_dir"/*.exe "$bin_dir"/*.dll; do
        [ -f "$item" ] || continue
        queue+=("$item")
    done
    shopt -u nullglob

    log "collecting Windows runtime DLLs for $bin_dir"

    while [ "$index" -lt "${#queue[@]}" ]; do
        current="${queue[$index]}"
        index=$((index + 1))

        case " ${seen[*]} " in
            *" $current "*)
                continue
                ;;
        esac
        seen+=("$current")

        while IFS= read -r line; do
            dll_path="$(extract_windows_dll_path_from_ldd_line "$line")"
            [ -n "$dll_path" ] || continue
            [ -f "$dll_path" ] || continue

            if windows_runtime_dll_is_system_path "$dll_path"; then
                continue
            fi

            if ! windows_runtime_dll_allowed_path "$dll_path"; then
                log "  skipping non-package DLL dependency: $dll_path"
                continue
            fi

            dll_name="$(basename "$dll_path")"
            destination="$bin_dir/$dll_name"

            if [ ! -f "$destination" ]; then
                cp -f "$dll_path" "$destination"
                chmod +x "$destination" 2>/dev/null || true
                copied_count=$((copied_count + 1))
                log "  copied: $dll_name"
                queue+=("$destination")
            fi
        done < <(ldd "$current" 2>/dev/null || true)
    done

    log "Windows runtime DLL collection completed: $copied_count copied"
}

create_archive() {
    local format="$1"
    local package_base="$2"
    local package_root="$3"
    local output_dir="$4"

    local output
    output="$output_dir/$package_base.$format"

    rm -f "$output"

    case "$format" in
        tar.xz)
            tar -C "$(dirname "$package_root")" -cJf "$output" "$(basename "$package_root")"
            ;;
        tar.gz)
            tar -C "$(dirname "$package_root")" -czf "$output" "$(basename "$package_root")"
            ;;
        zip)
            (cd "$(dirname "$package_root")" && zip -qr "$output" "$(basename "$package_root")")
            ;;
        *)
            die "unsupported package format: $format"
            ;;
    esac

    log "created package: $output"
}

create_packages() {
    local tool="$1"
    local version="$2"
    local host_platform="$3"
    local target_platform="$4"
    local revision="$5"
    local prefix="$6"

    local package_base
    local release_tag
    local package_root
    local format

    package_base="$(package_base_name "$tool" "$version" "$host_platform" "$target_platform" "$revision")"
    release_tag="$(release_tag_for_package "$tool" "$version" "$host_platform" "$target_platform" "$revision")"
    package_root="$CUP_OUT_DIR/package-root/$package_base"

    rm -rf "$package_root"
    mkdir -p "$(dirname "$package_root")"
    cp -a "$prefix" "$package_root"

    for format in $(package_formats_for_host "$host_platform"); do
        create_archive "$format" "$package_base" "$package_root" "$CUP_OUT_DIR"
    done

    cat > "$CUP_OUT_DIR/release.env" <<EOF_ENV
release_tag=$release_tag
package_base=$package_base
EOF_ENV
}
