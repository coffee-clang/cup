# Purpose: Shared dependency preparation, identity, download and prefix helpers.
# This library is sourced by build-posix.sh, build-windows.sh and verify.sh and
# is intentionally not executable.

CUP_DEPENDENCIES_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sources.sh
source "$CUP_DEPENDENCIES_DIR/sources.sh"

# Build tools pass compiler flags as shell words, so transactional dependency
# paths are intentionally restricted to whitespace-free absolute locations.
dependency_require_whitespace_free_path() {
    local label="$1"
    local path="$2"

    case "$path" in
        *[[:space:]]*)
            echo "Error: $label must not contain whitespace: $path" >&2
            return 1
            ;;
    esac
}

# Establishes the controlled environment used by every dependency builder.
# Ambient compiler and package-discovery flags must not silently alter the
# pinned graph or make a prefix depend on the caller's shell configuration.
dependency_normalize_build_environment() {
    export LC_ALL=C
    export LANG=C
    export TZ=UTC
    umask 022

    for variable in \
        CFLAGS CPPFLAGS LDFLAGS LIBS \
        CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH \
        LIBRARY_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR \
        PKG_CONFIG_SYSROOT_DIR CONFIG_SITE CCACHE CCACHE_DIR \
        MAKEFLAGS MFLAGS MAKEOVERRIDES GNUMAKEFLAGS; do
        unset "$variable" 2>/dev/null || true
    done
}

# Prints every relevant spelling of a path. MSYS2 tools may preserve the POSIX
# path or convert it to drive-letter form before recording it in an object.
dependency_path_variants() {
    local path="$1"
    local mixed=
    local windows=

    [ -n "$path" ] || return 0
    printf '%s\n' "$path"
    if command -v cygpath >/dev/null 2>&1; then
        mixed=$(cygpath -m "$path" 2>/dev/null || true)
        windows=$(cygpath -w "$path" 2>/dev/null || true)
        [ -z "$mixed" ] || [ "$mixed" = "$path" ] || printf '%s\n' "$mixed"
        [ -z "$windows" ] || [ "$windows" = "$path" ] || \
            [ "$windows" = "$mixed" ] || printf '%s\n' "$windows"
    fi
}

# Returns reproducible-path flags supported by the selected C compiler. The
# probe is compiled instead of inferring capabilities from the compiler name,
# which also covers Apple Clang and MSYS2 Clang consistently.
dependency_reproducible_cflags() {
    local compiler="$1"
    shift
    local work
    local source
    local object
    local path
    local flag
    local flags=

    work=$(mktemp -d "${TMPDIR:-/tmp}/cup-prefix-map.XXXXXX") || return 1
    source="$work/probe.c"
    object="$work/probe.o"
    printf '%s\n' 'int cup_prefix_map_probe(void) { return 0; }' > "$source"
    for path in "$@"; do
        [ -n "$path" ] || continue
        while IFS= read -r variant; do
            case "$variant" in *\\*) continue ;; esac
            for option in file debug macro; do
                flag="-f${option}-prefix-map=$variant=/usr/src/cup-dependencies"
                if "$compiler" "$flag" -c "$source" -o "$object" >/dev/null 2>&1; then
                    case " $flags " in *" $flag "*) ;; *) flags="$flags $flag" ;; esac
                fi
            done
        done < <(dependency_path_variants "$path")
    done
    rm -rf "$work"
    if [ -n "$flags" ]; then
        printf '%s\n' "-O2${flags}"
    else
        printf '%s\n' '-O2'
    fi
}

# Removes path-remapping options from flags that a dependency exposes as
# runtime build information. OpenSSL records its configured CFLAGS in
# libcrypto, so retaining these options would preserve the original host and
# transactional paths as literal strings even though the compiler remaps
# __FILE__ and debug data correctly.
dependency_buildinfo_safe_cflags() {
    local raw_flags="$1"
    local flag
    local -a kept=()
    local -a words=()

    read -r -a words <<<"$raw_flags"
    for flag in "${words[@]}"; do
        case "$flag" in
            -ffile-prefix-map=*|-fdebug-prefix-map=*|-fmacro-prefix-map=*)
                continue
                ;;
        esac
        kept+=("$flag")
    done

    if [ "${#kept[@]}" -eq 0 ]; then
        printf '%s\n' '-O2'
    else
        printf '%s\n' "${kept[*]}"
    fi
}

# Refuses dependency archives containing transient or machine-specific roots.
# Runtime-neutral OpenSSL defaults under /__cup_runtime__ are intentionally
# allowed; actual runner, checkout and staging paths are not.
dependency_compiled_paths_valid() {
    local prefix="$1"
    shift
    local archive
    local forbidden

    while IFS= read -r -d '' archive; do
        for forbidden in "$@"; do
            [ -n "$forbidden" ] || continue
            while IFS= read -r variant; do
                [ -n "$variant" ] || continue
                if LC_ALL=C grep -aF -q -- "$variant" "$archive"; then
                    echo "Error: compiled dependency contains forbidden path '$variant': $archive" >&2
                    return 1
                fi
            done < <(dependency_path_variants "$forbidden")
        done
        if LC_ALL=C grep -aE -q -- '/\.[^/]*install\.staging\.|[\\/]\.[^\\/]*install\.staging\.' "$archive"; then
            echo "Error: compiled dependency contains a transactional staging path: $archive" >&2
            return 1
        fi
    done < <(find "$prefix" -type f \( -name '*.a' -o -name '*.dll.a' \) -print0)
}

# Four jobs is the conservative default used by the verified Linux x64 build.
# Callers may explicitly choose another positive integer through JOBS.
dependency_resolve_jobs() {
    local jobs="${JOBS:-4}"

    case "$jobs" in
        ''|*[!0-9]*)
            echo "Error: JOBS must be a positive integer, got '$jobs'." >&2
            return 1
            ;;
    esac
    if [ "$jobs" -lt 1 ]; then
        echo "Error: JOBS must be at least 1." >&2
        return 1
    fi
    printf '%s\n' "$jobs"
}

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: required tool '$1' was not found." >&2
        exit 1
    fi
}

require_sha256_tool() {
    if ! command -v sha256sum >/dev/null 2>&1 &&
        ! command -v shasum >/dev/null 2>&1; then
        echo "Error: neither sha256sum nor shasum is available." >&2
        exit 1
    fi
}

file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "Error: neither sha256sum nor shasum is available." >&2
        return 1
    fi
}

stream_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{print $1}'
    else
        echo "Error: neither sha256sum nor shasum is available." >&2
        return 1
    fi
}

verify_source_checksum() {
    local package="$1"
    local file="$2"
    local expected
    local actual

    expected="$(sha256_for_package "$package")"
    actual="$(file_sha256 "$file")"
    if [ "$actual" != "$expected" ]; then
        echo "Error: SHA-256 verification failed for $(basename "$file")." >&2
        echo "Expected: $expected" >&2
        echo "Actual:   $actual" >&2
        return 1
    fi
}

# Canonical dependency identity and transactional prefix commit marker.
#
# Third-party build metadata often embeds the configured installation prefix.
# The bootstrap therefore configures every package for the final prefix and
# installs it below a separate DESTDIR staging root. Moving the staged payload
# into place keeps replacement rollback-safe without leaving .pc files,
# curl-config or compiled library defaults pointing at a deleted temporary
# prefix.
CUP_DEPS_PREFIX_READY=0
CUP_DEPS_FINAL_PREFIX=
CUP_DEPS_STAGE_ROOT=
CUP_DEPS_BUILD_PREFIX=
CUP_DEPS_USE_OPENSSL=1

dependency_recipe_digest() {
    local recipe_root="$1"
    local recipe
    local recipe_name
    shift

    for recipe in "$@"; do
        [ -f "$recipe" ] || {
            echo "Error: dependency recipe is missing: $recipe" >&2
            return 1
        }
        case "$recipe" in
            "$recipe_root"/*) ;;
            *)
                echo "Error: dependency recipe is outside the repository: $recipe" >&2
                return 1
                ;;
        esac
    done

    {
        for recipe in "$@"; do
            recipe_name=${recipe#"$recipe_root"/}
            printf 'file=%s\n' "$recipe_name"
            cat "$recipe"
            printf '\n'
        done
    } | stream_sha256
}

dependency_canonical_config() {
    local platform="$1"
    local toolchain="$2"
    local use_openssl="$3"
    local recipe_digest="$4"

    printf '%s\n' \
        'identity_format=1' \
        "platform=$platform" \
        "toolchain=$toolchain" \
        "use_openssl=$use_openssl" \
        "recipe_digest=$recipe_digest" \
        "zlib.version=$ZLIB_VERSION" \
        "zlib.url=$ZLIB_URL" \
        "zlib.sha256=$ZLIB_SHA256" \
        "zlib.min_bytes=$ZLIB_MIN_BYTES" \
        "xz.version=$XZ_VERSION" \
        "xz.url=$XZ_URL" \
        "xz.sha256=$XZ_SHA256" \
        "xz.min_bytes=$XZ_MIN_BYTES"
    if [ "$use_openssl" = 1 ]; then
        printf '%s\n' \
            "openssl.version=$OPENSSL_VERSION" \
            "openssl.url=$OPENSSL_URL" \
            "openssl.sha256=$OPENSSL_SHA256" \
            "openssl.min_bytes=$OPENSSL_MIN_BYTES"
    fi
    printf '%s\n' \
        "curl.version=$CURL_VERSION" \
        "curl.url=$CURL_URL" \
        "curl.sha256=$CURL_SHA256" \
        "curl.min_bytes=$CURL_MIN_BYTES" \
        "libarchive.version=$LIBARCHIVE_VERSION" \
        "libarchive.url=$LIBARCHIVE_URL" \
        "libarchive.sha256=$LIBARCHIVE_SHA256" \
        "libarchive.min_bytes=$LIBARCHIVE_MIN_BYTES" \
        "argtable3.version=$ARGTABLE3_VERSION" \
        "argtable3.url=$ARGTABLE3_URL" \
        "argtable3.sha256=$ARGTABLE3_SHA256" \
        "argtable3.min_bytes=$ARGTABLE3_MIN_BYTES" \
        "uthash.version=$UTHASH_VERSION" \
        "uthash.url=$UTHASH_URL" \
        "uthash.sha256=$UTHASH_SHA256" \
        "uthash.min_bytes=$UTHASH_MIN_BYTES" \
        "unity.version=$UNITY_VERSION" \
        "unity.url=$UNITY_URL" \
        "unity.sha256=$UNITY_SHA256" \
        "unity.min_bytes=$UNITY_MIN_BYTES" \
        "libevent.version=$LIBEVENT_VERSION" \
        "libevent.url=$LIBEVENT_URL" \
        "libevent.sha256=$LIBEVENT_SHA256" \
        "libevent.min_bytes=$LIBEVENT_MIN_BYTES"
}

dependency_id() {
    local platform="$1"
    local toolchain="$2"
    local use_openssl="$3"
    local recipe_root="$4"
    local recipe_digest
    shift 4

    [ -n "$platform" ] && [ -n "$toolchain" ] || return 1
    case "$use_openssl" in
        0 | 1)
            ;;
        *)
            return 1
            ;;
    esac
    recipe_digest=$(dependency_recipe_digest "$recipe_root" "$@") || return 1
    dependency_canonical_config "$platform" "$toolchain" \
        "$use_openssl" "$recipe_digest" | stream_sha256
}

dependency_metadata() {
    local platform="$1"
    local id="$2"

    case "$platform" in
        ''|*[!a-z0-9-]*)
            echo "Error: invalid dependency platform '$platform'." >&2
            return 1
            ;;
    esac
    case "$id" in
        *[!0-9a-f]*|'')
            echo "Error: invalid dependency identity '$id'." >&2
            return 1
            ;;
    esac
    [ "${#id}" -eq 64 ] || {
        echo "Error: dependency identity must contain 64 lowercase hexadecimal characters." >&2
        return 1
    }
    printf '%s\n' \
        'prefix_format=3' \
        "platform=$platform" \
        "dependency_id=$id"
}

dependency_metadata_valid() {
    local metadata="$1"
    local platform
    local id
    local expected

    platform=$(printf '%s\n' "$metadata" | sed -n 's/^platform=//p')
    id=$(printf '%s\n' "$metadata" | sed -n 's/^dependency_id=//p')
    expected=$(dependency_metadata "$platform" "$id" 2>/dev/null) || return 1
    [ "$metadata" = "$expected" ]
}

first_command_line() {
    "$@" 2>/dev/null | sed -n '1p' | tr -d '\r'
}

# Some platform tools, notably Apple ar and ranlib, do not expose a GNU-style
# --version option. Prefer a real version string, then fall back to the resolved
# executable path so the dependency identity remains stable and non-empty.
tool_identity_line() {
    local tool="$1"
    local value

    value=$(first_command_line "$tool" --version) || value=
    if [ -z "$value" ]; then
        value=$(first_command_line "$tool" -V) || value=
    fi
    if [ -z "$value" ]; then
        value=$(command -v "$tool") || return 1
    fi
    printf '%s\n' "$value"
}

dependency_posix_toolchain_identity() {
    local compiler="$1"
    local archiver="$2"
    local ranlib_tool="$3"
    local openssl_target="$4"
    local platform_policy="$5"
    local compiler_target
    local compiler_version
    local archiver_version
    local ranlib_version

    compiler_target=$(first_command_line "$compiler" -dumpmachine) || return 1
    compiler_version=$(first_command_line "$compiler" --version) || return 1
    archiver_version=$(tool_identity_line "$archiver") || return 1
    ranlib_version=$(tool_identity_line "$ranlib_tool") || return 1
    [ -n "$compiler_target" ] && [ -n "$compiler_version" ] &&
        [ -n "$archiver_version" ] && [ -n "$ranlib_version" ] || return 1
    printf 'cc=%s|target=%s|version=%s|ar=%s|version=%s|' \
        "$compiler" "$compiler_target" "$compiler_version" \
        "$archiver" "$archiver_version"
    printf 'ranlib=%s|version=%s|openssl_target=%s|policy=%s\n' \
        "$ranlib_tool" "$ranlib_version" "$openssl_target" "$platform_policy"
}

dependency_windows_toolchain_identity() {
    local host_triple="$1"
    local compiler="$2"
    local archiver="$3"
    local ranlib_tool="$4"
    local strip_tool="$5"
    local resource_compiler="$6"
    local runtime_policy="$7"
    local compiler_target
    local compiler_version
    local archiver_version
    local ranlib_version
    local strip_version
    local resource_version

    compiler_target=$(first_command_line "$compiler" -dumpmachine) || return 1
    compiler_version=$(first_command_line "$compiler" --version) || return 1
    archiver_version=$(tool_identity_line "$archiver") || return 1
    ranlib_version=$(tool_identity_line "$ranlib_tool") || return 1
    strip_version=$(first_command_line "$strip_tool" --version) || return 1
    resource_version=$(first_command_line "$resource_compiler" --version) || return 1
    [ -n "$compiler_target" ] && [ -n "$compiler_version" ] &&
        [ -n "$archiver_version" ] && [ -n "$ranlib_version" ] &&
        [ -n "$strip_version" ] && [ -n "$resource_version" ] || return 1
    printf 'host=%s|cc=%s|target=%s|version=%s|ar=%s|version=%s|' \
        "$host_triple" "$compiler" "$compiler_target" "$compiler_version" \
        "$archiver" "$archiver_version"
    printf 'ranlib=%s|version=%s|strip=%s|version=%s|' \
        "$ranlib_tool" "$ranlib_version" "$strip_tool" "$strip_version"
    printf 'windres=%s|version=%s|runtime=%s\n' \
        "$resource_compiler" "$resource_version" "$runtime_policy"
}

dependency_library_exists() {
    local prefix="$1"
    local name="$2"
    local directory

    for directory in "$prefix/lib" "$prefix/lib64"; do
        [ -f "$directory/lib$name.a" ] && return 0
        [ -f "$directory/lib$name.dll.a" ] && return 0
    done
    return 1
}

application_dependency_prefix_complete() {
    local prefix="$1"
    [ -f "$prefix/include/argtable3.h" ] &&
        [ -f "$prefix/include/uthash.h" ] &&
        dependency_library_exists "$prefix" argtable3
}

test_dependency_prefix_complete() {
    local prefix="$1"
    application_dependency_prefix_complete "$prefix" &&
        [ -f "$prefix/include/unity.h" ] &&
        [ -f "$prefix/include/unity_internals.h" ] &&
        [ -f "$prefix/include/event2/event.h" ] &&
        [ -f "$prefix/include/event2/http.h" ] &&
        [ -f "$prefix/include/event2/bufferevent.h" ] &&
        [ -f "$prefix/include/event2/listener.h" ] &&
        dependency_library_exists "$prefix" unity &&
        dependency_library_exists "$prefix" event_core &&
        dependency_library_exists "$prefix" event_extra &&
        { [ -f "$prefix/lib/pkgconfig/libevent_core.pc" ] ||
          [ -f "$prefix/lib64/pkgconfig/libevent_core.pc" ]; } &&
        { [ -f "$prefix/lib/pkgconfig/libevent_extra.pc" ] ||
          [ -f "$prefix/lib64/pkgconfig/libevent_extra.pc" ]; }
}

dependency_pkg_config() {
    # pkg-config on Windows may relocate an installed prefix to the physical
    # directory containing the .pc file. During transactional builds that
    # directory is the private staging tree, even though the metadata already
    # names the final prefix. Disable that behavior whenever the implementation
    # supports it so generated link flags remain stable on every platform.
    if pkg-config --help 2>&1 | grep -F -- '--dont-define-prefix' >/dev/null; then
        pkg-config --dont-define-prefix "$@"
    else
        pkg-config "$@"
    fi
}

# Accepts only paths contained in the final private dependency prefix. Path
# variants cover MSYS2 metadata that may use POSIX, mixed or Windows spelling.
dependency_link_path_is_private() {
    local path="$1"
    local expected_prefix="$2"
    local variant

    while IFS= read -r variant; do
        case "$path" in
            "$variant"|"$variant"/*|"$variant"\\*)
                return 0
                ;;
        esac
    done < <(dependency_path_variants "$expected_prefix")
    return 1
}

dependency_text_references_path() {
    local flags="$1"
    local path="$2"
    local variant

    while IFS= read -r variant; do
        case "$flags" in
            *"$variant"*) return 0 ;;
        esac
    done < <(dependency_path_variants "$path")
    return 1
}

# Validates path-bearing linker flags independently from ordinary system
# libraries such as -ldl, -lpthread or Windows import-library names. A flags
# string cannot become valid merely by mentioning the private prefix once and
# then adding an ambient host search directory or absolute library.
dependency_link_flags_valid() {
    local flags="$1"
    local prefix="$2"
    local expected_prefix="$3"
    local path=
    local token
    local part
    local index
    local part_index
    local -a words
    local -a linker_parts

    read -r -a words <<< "$flags"
    for ((index = 0; index < ${#words[@]}; index++)); do
        token=${words[index]}
        path=
        case "$token" in
            -L|-F|-R|-rpath|--rpath|--rpath-link|--library-path|--sysroot|-B)
                ((index + 1 < ${#words[@]})) || return 1
                index=$((index + 1))
                path=${words[index]}
                ;;
            -L?*|-F?*|-R?*|-B?*)
                path=${token:2}
                ;;
            -rpath=*|--rpath=*|--rpath-link=*|--library-path=*|--sysroot=*)
                path=${token#*=}
                ;;
            -Wl,*)
                IFS=, read -r -a linker_parts <<< "${token#-Wl,}"
                for ((part_index = 0; part_index < ${#linker_parts[@]}; part_index++)); do
                    part=${linker_parts[part_index]}
                    path=
                    case "$part" in
                        -L|-F|-R|-rpath|--rpath|-rpath-link|--rpath-link|--library-path|--sysroot|-B)
                            ((part_index + 1 < ${#linker_parts[@]})) || return 1
                            part_index=$((part_index + 1))
                            path=${linker_parts[part_index]}
                            ;;
                        -L?*|-F?*|-R?*|-B?*)
                            path=${part:2}
                            ;;
                        -rpath=*|--rpath=*|-rpath-link=*|--rpath-link=*|--library-path=*|--sysroot=*)
                            path=${part#*=}
                            ;;
                        /*|[A-Za-z]:[\\/]*)
                            path=$part
                            ;;
                    esac
                    if [ -n "$path" ] &&
                        ! dependency_link_path_is_private "$path" "$expected_prefix"; then
                        echo "Error: dependency link metadata references a path outside" \
                            "the private prefix: $path" >&2
                        return 1
                    fi
                done
                continue
                ;;
            /*|[A-Za-z]:[\\/]*)
                path=$token
                ;;
        esac
        if [ -n "$path" ] &&
            ! dependency_link_path_is_private "$path" "$expected_prefix"; then
            echo "Error: dependency link metadata references a path outside" \
                "the private prefix: $path" >&2
            return 1
        fi
    done

    case " $flags " in
        *" -lacl "*) return 1 ;;
    esac
    if ! dependency_text_references_path "$flags" "$expected_prefix"; then
        echo "Error: dependency link metadata does not reference the private prefix:" \
            "$expected_prefix" >&2
        return 1
    fi
    if [ "$prefix" != "$expected_prefix" ] &&
        dependency_text_references_path "$flags" "$prefix"; then
        echo "Error: dependency link metadata still references the staging prefix:" \
            "$prefix" >&2
        return 1
    fi
}

dependency_link_metadata_valid() {
    local prefix="$1"
    local expected_prefix="${2:-$prefix}"
    local pkg_config_path="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig"
    local curl_flags=
    local archive_flags=
    local event_flags=

    [ -x "$prefix/bin/curl-config" ] || return 1
    command -v pkg-config >/dev/null 2>&1 || return 1
    curl_flags=$("$prefix/bin/curl-config" --static-libs 2>/dev/null) || return 1
    archive_flags=$(PKG_CONFIG_PATH="$pkg_config_path" \
        PKG_CONFIG_LIBDIR="$pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libarchive 2>/dev/null) || return 1
    event_flags=$(PKG_CONFIG_PATH="$pkg_config_path" \
        PKG_CONFIG_LIBDIR="$pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libevent_extra libevent_core 2>/dev/null) || return 1
    [ -n "$curl_flags" ] && [ -n "$archive_flags" ] && [ -n "$event_flags" ] || return 1
    dependency_link_flags_valid "$curl_flags" "$prefix" "$expected_prefix" &&
        dependency_link_flags_valid "$archive_flags" "$prefix" "$expected_prefix" &&
        dependency_link_flags_valid "$event_flags" "$prefix" "$expected_prefix"
}

dependency_prefix_complete() {
    local prefix="$1"
    local use_openssl="${2:-1}"
    local metadata_prefix="${3:-$prefix}"

    case "$use_openssl" in
        0 | 1) ;;
        *) return 1 ;;
    esac

    test_dependency_prefix_complete "$prefix" &&
        [ -x "$prefix/bin/curl-config" ] &&
        [ -f "$prefix/include/curl/curl.h" ] &&
        [ -f "$prefix/include/archive.h" ] &&
        [ -f "$prefix/include/archive_entry.h" ] &&
        [ -f "$prefix/include/zlib.h" ] &&
        [ -f "$prefix/include/lzma.h" ] &&
        dependency_library_exists "$prefix" curl &&
        dependency_library_exists "$prefix" archive &&
        dependency_library_exists "$prefix" z &&
        dependency_library_exists "$prefix" lzma &&
        { [ -f "$prefix/lib/pkgconfig/libarchive.pc" ] ||
          [ -f "$prefix/lib64/pkgconfig/libarchive.pc" ]; } &&
        dependency_link_metadata_valid "$prefix" "$metadata_prefix" || return 1

    if [ "$use_openssl" = 1 ]; then
        [ -f "$prefix/include/openssl/ssl.h" ] &&
            dependency_library_exists "$prefix" ssl &&
            dependency_library_exists "$prefix" crypto
    fi
}

dependency_prefix_matches() {
    local prefix="$1"
    local metadata="$2"
    local use_openssl="${3:-1}"
    local config="$prefix/.cup-deps-config"

    dependency_metadata_valid "$metadata" &&
        [ -f "$config" ] && [ ! -e "$prefix/.cup-deps-building" ] &&
        [ "$(cat "$config")" = "$metadata" ] &&
        dependency_prefix_complete "$prefix" "$use_openssl" "$prefix"
}

prepare_dependency_prefix() {
    local final_prefix="$1"
    local metadata="$2"
    local use_openssl="${3:-1}"
    local parent
    local name

    parent="$(dirname "$final_prefix")"
    name="$(basename "$final_prefix")"

    dependency_metadata_valid "$metadata" || {
        echo "Error: invalid dependency prefix metadata." >&2
        return 1
    }
    case "$use_openssl" in
        0 | 1) ;;
        *)
            echo "Error: dependency OpenSSL policy must be 0 or 1." >&2
            return 1
            ;;
    esac
    dependency_require_whitespace_free_path "dependency prefix" "$final_prefix" || return 1

    case "$final_prefix" in
        /)
            echo "Error: dependency prefix cannot be the filesystem root." >&2
            return 1
            ;;
        */)
            echo "Error: dependency prefix must not have a trailing slash: $final_prefix" >&2
            return 1
            ;;
        /*) ;;
        *)
            echo "Error: dependency prefix must be absolute: $final_prefix" >&2
            return 1
            ;;
    esac
    case "$final_prefix/" in
        */../*|*/./*)
            echo "Error: dependency prefix must not contain '.' or '..' path segments: $final_prefix" >&2
            return 1
            ;;
    esac

    CUP_DEPS_PREFIX_READY=0
    CUP_DEPS_FINAL_PREFIX="$final_prefix"
    CUP_DEPS_STAGE_ROOT=
    CUP_DEPS_BUILD_PREFIX=
    CUP_DEPS_USE_OPENSSL="$use_openssl"

    if dependency_prefix_matches "$final_prefix" "$metadata" "$use_openssl"; then
        CUP_DEPS_PREFIX_READY=1
        CUP_DEPS_BUILD_PREFIX="$final_prefix"
        echo "==> Reusing dependency prefix $final_prefix"
        return 0
    fi

    mkdir -p "$parent"
    CUP_DEPS_STAGE_ROOT="$(mktemp -d "$parent/.${name}.staging.XXXXXX")"
    CUP_DEPS_BUILD_PREFIX="$CUP_DEPS_STAGE_ROOT$final_prefix"
    mkdir -p "$CUP_DEPS_BUILD_PREFIX"
    printf '%s\n' "$metadata" > "$CUP_DEPS_BUILD_PREFIX/.cup-deps-building"
}

abort_dependency_prefix() {
    if [ "$CUP_DEPS_PREFIX_READY" != 1 ] &&
        [ -n "$CUP_DEPS_STAGE_ROOT" ]; then
        rm -rf -- "$CUP_DEPS_STAGE_ROOT"
    fi
    CUP_DEPS_PREFIX_READY=0
    CUP_DEPS_FINAL_PREFIX=
    CUP_DEPS_STAGE_ROOT=
    CUP_DEPS_BUILD_PREFIX=
}

dependency_metadata_contains_staging() {
    local value="$1"
    local stage_root="${CUP_DEPS_STAGE_ROOT:-}"
    local stage_name

    [ -n "$stage_root" ] || return 1
    stage_name=${stage_root##*/}
    case "$value" in
        *"$stage_root"*|*"$stage_name"*) return 0 ;;
    esac
    return 1
}


normalize_dependency_metadata() {
    local prefix="$1"
    local staged_prefix="$2"
    local final_prefix="$3"
    local staged_native=
    local final_native=
    local staged_windows=
    local stage_root="${CUP_DEPS_STAGE_ROOT:-}"
    local staging_directory=
    local metadata

    [ -d "$prefix" ] || {
        echo "Error: dependency metadata prefix does not exist: $prefix" >&2
        return 1
    }
    [ -n "$final_prefix" ] || {
        echo "Error: final dependency prefix is empty." >&2
        return 1
    }

    if [ -n "$stage_root" ]; then
        staging_directory=${stage_root##*/}
    fi

    # MSYS-generated metadata can use POSIX paths, drive-letter paths with
    # forward slashes, or native paths with backslashes. Normalize every
    # spelling of the installed payload to the final prefix. Compiler
    # prefix-map options are build-only and must not be propagated by .pc,
    # CMake or *-config files, so remove those options instead of rewriting
    # their transient source roots to another machine-specific directory.
    if [ -n "$staged_prefix" ] && [ "$staged_prefix" != "$final_prefix" ] &&
        command -v cygpath >/dev/null 2>&1; then
        staged_native=$(cygpath -m "$staged_prefix" 2>/dev/null || true)
        final_native=$(cygpath -m "$final_prefix" 2>/dev/null || true)
        staged_windows=$(cygpath -w "$staged_prefix" 2>/dev/null || true)
    fi

    while IFS= read -r -d '' metadata; do
        if ! perl -0777 -e 'my $data = <>; exit(index($data, "\0") < 0 ? 0 : 1)' "$metadata"; then
            echo "Error: generated dependency metadata is not a text file: $metadata" >&2
            return 1
        fi

        CUP_STAGED_PREFIX="$staged_prefix" CUP_FINAL_PREFIX="$final_prefix" \
        CUP_STAGED_NATIVE="$staged_native" CUP_FINAL_NATIVE="$final_native" \
        CUP_STAGED_WINDOWS="$staged_windows" \
            perl -0777 -pi -e '
                sub path_pattern {
                    my ($path) = @_;
                    my @parts = split(/[\\\/]+/, $path, -1);
                    return join("[\\\\/]+", map { quotemeta($_) } @parts);
                }
                sub replace_path {
                    my ($from, $to, $case_insensitive) = @_;
                    return unless length($from) && $from ne $to;
                    my $pattern = path_pattern($from);
                    if ($case_insensitive) {
                        s/$pattern/$to/gi;
                    } else {
                        s/$pattern/$to/g;
                    }
                }

                replace_path($ENV{CUP_STAGED_WINDOWS}, $ENV{CUP_FINAL_NATIVE}, 1);
                replace_path($ENV{CUP_STAGED_NATIVE}, $ENV{CUP_FINAL_NATIVE}, 1);
                replace_path($ENV{CUP_STAGED_PREFIX}, $ENV{CUP_FINAL_PREFIX}, 0);

                # These flags describe the dependency build itself. Keeping
                # them in consumer metadata both leaks transactional paths and
                # incorrectly applies CUP bootstrap mappings to later builds.
                s{(^|[\s"\x27=])-f(?:file|debug|macro)-prefix-map=[^\s"\x27();]+}{$1}gm;
            ' "$metadata"

        if [ -n "$staged_prefix" ] && [ "$staged_prefix" != "$final_prefix" ] &&
            dependency_text_references_path "$(cat "$metadata")" "$staged_prefix"; then
            echo "Error: generated metadata still contains the staged payload prefix: $metadata" >&2
            return 1
        fi
        if [ -n "$stage_root" ]; then
            while IFS= read -r variant; do
                [ -n "$variant" ] || continue
                if LC_ALL=C grep -F -I -q -- "$variant" "$metadata"; then
                    echo "Error: generated metadata still contains the staging root '$variant': $metadata" >&2
                    return 1
                fi
            done < <(dependency_path_variants "$stage_root")
            if [ -n "$staging_directory" ] &&
                LC_ALL=C grep -F -I -q -- "$staging_directory" "$metadata"; then
                echo "Error: generated metadata still contains the staging directory" \
                    "'$staging_directory': $metadata" >&2
                return 1
            fi
        fi
    done < <(find "$prefix" -type f \
        \( -name '*.pc' -o -name '*.la' -o -name '*.cmake' \
           -o -name '*-config' \) -print0)
}

finish_dependency_prefix() {
    local build_prefix="$1"
    local final_prefix="$CUP_DEPS_FINAL_PREFIX"
    local parent
    local name
    local previous_prefix=

    parent="$(dirname "$final_prefix")"
    name="$(basename "$final_prefix")"

    [ "$build_prefix" = "$CUP_DEPS_BUILD_PREFIX" ] || {
        echo "Error: dependency build prefix does not match the prepared transaction." >&2
        return 1
    }
    [ -n "$CUP_DEPS_STAGE_ROOT" ] || {
        echo "Error: dependency staging root is not initialized." >&2
        return 1
    }
    dependency_prefix_complete "$build_prefix" "$CUP_DEPS_USE_OPENSSL" \
        "$final_prefix" || {
        echo "Error: refusing to commit an incomplete dependency prefix." >&2
        return 1
    }
    dependency_compiled_paths_valid "$build_prefix" \
        "${CUP_DEPS_STAGE_ROOT:-}" "${DEPS_ROOT:-}" "${BUILD_DIR:-}" || return 1
    mv "$build_prefix/.cup-deps-building" "$build_prefix/.cup-deps-config"

    if [ -e "$final_prefix" ] || [ -L "$final_prefix" ]; then
        previous_prefix="$(mktemp -d "$parent/.${name}.previous.XXXXXX")"
        rmdir "$previous_prefix"
        mv "$final_prefix" "$previous_prefix"
    fi

    if ! mv "$build_prefix" "$final_prefix"; then
        if [ -n "$previous_prefix" ] &&
            { [ -e "$previous_prefix" ] || [ -L "$previous_prefix" ]; }; then
            mv "$previous_prefix" "$final_prefix" || true
        fi
        return 1
    fi

    rm -rf -- "$CUP_DEPS_STAGE_ROOT"
    if [ -n "$previous_prefix" ]; then
        rm -rf -- "$previous_prefix"
    fi
    CUP_DEPS_PREFIX_READY=1
    CUP_DEPS_STAGE_ROOT=
    CUP_DEPS_BUILD_PREFIX="$final_prefix"
}

# Verified source download and extraction.
download_source() {
    local package="$1"
    local output="$2"
    local url
    local min_bytes
    local size=
    local tmp_output

    url="$(source_url_for_package "$package")"
    min_bytes="$(minimum_bytes_for_package "$package")"

    if [ -f "$output" ]; then
        size="$(wc -c < "$output" | tr -d '[:space:]')"
        if [ "$size" -ge "$min_bytes" ] &&
            verify_source_checksum "$package" "$output"; then
            echo "==> Using cached $(basename "$output")"
            return 0
        fi

        echo "==> Removing suspicious cached $(basename "$output") (${size} bytes)"
        rm -f "$output"
    fi

    tmp_output="$(mktemp "${output}.tmp.XXXXXX")"
    echo "==> Downloading $url"
    if ! curl -fL --proto '=https' --proto-redir '=https' \
        --retry 3 --retry-delay 5 --retry-all-errors \
        "$url" -o "$tmp_output"; then
        rm -f "$tmp_output"
        return 1
    fi

    size="$(wc -c < "$tmp_output" | tr -d '[:space:]')"
    if [ "$size" -lt "$min_bytes" ]; then
        echo "Error: downloaded $package archive is unexpectedly small: ${size} bytes." >&2
        echo "URL: $url" >&2
        echo "File: $tmp_output" >&2
        if command -v file >/dev/null 2>&1; then
            file "$tmp_output" >&2 || true
        fi
        echo "First bytes:" >&2
        head -c 300 "$tmp_output" >&2 || true
        echo >&2
        rm -f "$tmp_output"
        return 1
    fi

    if ! verify_source_checksum "$package" "$tmp_output"; then
        rm -f "$tmp_output"
        return 1
    fi
    mv "$tmp_output" "$output"
}

extract_archive() {
    local archive="$1"
    local destination="$2"

    rm -rf "$destination"
    mkdir -p "$destination"

    case "$archive" in
        *.tar.gz|*.tgz)
            tar -xzf "$archive" -C "$destination" --strip-components=1
            ;;
        *.tar.xz)
            tar -xJf "$archive" -C "$destination" --strip-components=1
            ;;
        *)
            echo "Error: unsupported archive format '$archive'." >&2
            return 1
            ;;
    esac
}


# Test-only portable network dependency shared by platform bootstraps.
build_libevent_static() {
    local src_dir="$1"
    local build_dir="$2"
    local compiler="$3"
    local archiver="$4"
    local ranlib_tool="$5"
    local host_triple="${6:-}"
    local archive="$src_dir/libevent-${LIBEVENT_VERSION}.tar.gz"
    local source="$build_dir/libevent-${LIBEVENT_VERSION}"

    download_source libevent "$archive"
    extract_archive "$archive" "$source"

    set -- \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-openssl \
        --disable-thread-support \
        --disable-malloc-replacement \
        --disable-libevent-regress \
        --disable-samples
    if [ -n "$host_triple" ]; then
        set -- --host="$host_triple" "$@"
    fi

    echo "==> Building libevent ${LIBEVENT_VERSION}"
    cd "$source"
    # shellcheck disable=SC2086
    CC="$compiler" AR="$archiver" RANLIB="$ranlib_tool" \
        CFLAGS="${CUP_DEPENDENCY_CFLAGS:-}" ./configure "$@"
    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
}

# Lightweight direct dependencies shared by platform bootstraps.
build_argtable3_uthash_unity() {
    local prefix="$1"
    local src_dir="$2"
    local build_dir="$3"
    local compiler="$4"
    local archiver="$5"
    local ranlib_tool="$6"
    local archive=
    local source=
    local object_dir="$build_dir/cup-small-deps"
    local file
    local object
    local uthash_header

    mkdir -p "$prefix/include" "$prefix/lib" "$object_dir"

    archive="$src_dir/argtable3-${ARGTABLE3_VERSION}.tar.gz"
    source="$build_dir/argtable3-${ARGTABLE3_VERSION}"
    download_source argtable3 "$archive"
    extract_archive "$archive" "$source"
    rm -f "$object_dir"/argtable3-*.o "$prefix/lib/libargtable3.a"
    for file in "$source"/src/*.c; do
        object="$object_dir/argtable3-$(basename "${file%.c}").o"
        # shellcheck disable=SC2086
        "$compiler" ${CUP_DEPENDENCY_CFLAGS:--O2} -I"$source/src" -c "$file" -o "$object"
    done
    "$archiver" rcs "$prefix/lib/libargtable3.a" "$object_dir"/argtable3-*.o
    "$ranlib_tool" "$prefix/lib/libargtable3.a"
    cp "$source/src/argtable3.h" "$prefix/include/argtable3.h"

    archive="$src_dir/uthash-${UTHASH_VERSION}.tar.gz"
    download_source uthash "$archive"
    uthash_header="$prefix/include/uthash.h.tmp.$$"
    if ! tar -xOzf "$archive" \
        "uthash-${UTHASH_VERSION}/src/uthash.h" > "$uthash_header"; then
        rm -f "$uthash_header"
        echo "Error: could not extract uthash.h from $(basename "$archive")." >&2
        return 1
    fi
    [ -s "$uthash_header" ] || {
        rm -f "$uthash_header"
        echo "Error: extracted uthash.h is empty." >&2
        return 1
    }
    mv "$uthash_header" "$prefix/include/uthash.h"

    archive="$src_dir/unity-${UNITY_VERSION}.tar.gz"
    source="$build_dir/unity-${UNITY_VERSION}"
    download_source unity "$archive"
    extract_archive "$archive" "$source"
    # shellcheck disable=SC2086
    "$compiler" ${CUP_DEPENDENCY_CFLAGS:--O2} -I"$source/src" -c "$source/src/unity.c" \
        -o "$object_dir/unity.o"
    "$archiver" rcs "$prefix/lib/libunity.a" "$object_dir/unity.o"
    "$ranlib_tool" "$prefix/lib/libunity.a"
    cp "$source/src/unity.h" "$source/src/unity_internals.h" "$prefix/include/"
}
