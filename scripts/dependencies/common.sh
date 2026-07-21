# Purpose: Shared dependency preparation, identity, download and prefix helpers.
# This library is sourced by build-posix.sh, build-windows.sh and verify.sh and
# is intentionally not executable.

CUP_DEPENDENCIES_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=sources.sh
source "$CUP_DEPENDENCIES_DIR/sources.sh"

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
        pkg-config --static --libs libarchive 2>/dev/null) || return 1
    event_flags=$(PKG_CONFIG_PATH="$pkg_config_path" \
        PKG_CONFIG_LIBDIR="$pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR="" \
        pkg-config --static --libs libevent_extra libevent_core 2>/dev/null) || return 1
    [ -n "$curl_flags" ] && [ -n "$archive_flags" ] && [ -n "$event_flags" ] || return 1
    case " $archive_flags " in
        *" -lacl "*)
            return 1
            ;;
    esac
    case "$curl_flags" in
        *"$expected_prefix"*)
            ;;
        *)
            return 1
            ;;
    esac
    case "$archive_flags" in
        *"$expected_prefix"*)
            ;;
        *)
            return 1
            ;;
    esac
    case "$event_flags" in
        *"$expected_prefix"*)
            ;;
        *)
            return 1
            ;;
    esac
    if [ "$prefix" != "$expected_prefix" ]; then
        case "$curl_flags $archive_flags $event_flags" in
            *"$prefix"*) return 1 ;;
        esac
    fi
}

dependency_prefix_complete() {
    local prefix="$1"
    local use_openssl="${2:-1}"
    local metadata_prefix="${3:-$prefix}"

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
    local final_windows=
    local staging_directory=${CUP_DEPS_STAGE_ROOT##*/}
    local metadata

    [ -n "$staged_prefix" ] && [ "$staged_prefix" != "$final_prefix" ] || return 0

    # MSYS-generated metadata can use POSIX paths, drive-letter paths with
    # forward slashes, or native paths with backslashes. Normalize every
    # spelling before the transactional directory is committed.
    if command -v cygpath >/dev/null 2>&1; then
        staged_native=$(cygpath -m "$staged_prefix")
        final_native=$(cygpath -m "$final_prefix")
        staged_windows=$(cygpath -w "$staged_prefix")
        final_windows=$(cygpath -w "$final_prefix")
    fi

    # Configure scripts may preserve dependency search paths even when their
    # own --prefix is the final installation path. Rewrite only generated text
    # metadata; archives and other binary payloads are intentionally untouched.
    while IFS= read -r -d '' metadata; do
        CUP_STAGED_PREFIX="$staged_prefix" CUP_FINAL_PREFIX="$final_prefix" \
        CUP_STAGED_NATIVE="$staged_native" CUP_FINAL_NATIVE="$final_native" \
        CUP_STAGED_WINDOWS="$staged_windows" CUP_FINAL_WINDOWS="$final_windows" \
            perl -0pi -e '
                sub path_pattern {
                    my ($path) = @_;
                    my @parts = split(/[\\\/]+/, $path, -1);
                    return join("[\\\\/]+", map { quotemeta($_) } @parts);
                }
                sub replace_path {
                    my ($from, $to) = @_;
                    return unless length $from;
                    my $pattern = path_pattern($from);
                    s/$pattern/$to/gi;
                }
                replace_path($ENV{CUP_STAGED_WINDOWS}, $ENV{CUP_FINAL_NATIVE});
                replace_path($ENV{CUP_STAGED_NATIVE}, $ENV{CUP_FINAL_NATIVE});
                replace_path($ENV{CUP_STAGED_PREFIX}, $ENV{CUP_FINAL_PREFIX});
            ' "$metadata"

        if LC_ALL=C grep -F -I -q -- "$staged_prefix" "$metadata" || \
            { [ -n "$staged_native" ] && \
              LC_ALL=C grep -F -I -q -- "$staged_native" "$metadata"; } || \
            { [ -n "$staged_windows" ] && \
              LC_ALL=C grep -F -I -q -- "$staged_windows" "$metadata"; } || \
            { [ -n "$staging_directory" ] && \
              LC_ALL=C grep -F -I -q -- "$staging_directory" "$metadata"; }; then
            echo "Error: generated metadata still contains the staging prefix: $metadata" >&2
            return 1
        fi
    done < <(find "$prefix" -type f \
        \( -name '*.pc' -o -name '*.la' -o -name '*.cmake' \
           -o -name '*-config' -o -name 'curl-config' \) -print0)
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
    if ! curl -fL --retry 3 --retry-delay 5 --retry-all-errors \
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
            exit 1
            ;;
    esac
}


# Test-only portable network dependency shared by platform bootstraps.
build_libevent_static() {
    local src_dir="$2"
    local build_dir="$3"
    local compiler="$4"
    local archiver="$5"
    local ranlib_tool="$6"
    local host_triple="${7:-}"
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
    CC="$compiler" AR="$archiver" RANLIB="$ranlib_tool" ./configure "$@"
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
        "$compiler" -O2 -I"$source/src" -c "$file" -o "$object"
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
    "$compiler" -O2 -I"$source/src" -c "$source/src/unity.c" \
        -o "$object_dir/unity.o"
    "$archiver" rcs "$prefix/lib/libunity.a" "$object_dir/unity.o"
    "$ranlib_tool" "$prefix/lib/libunity.a"
    cp "$source/src/unity.h" "$source/src/unity_internals.h" "$prefix/include/"
}
