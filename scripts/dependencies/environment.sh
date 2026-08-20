# Normalizes dependency build inputs and owns compiler, hashing and reproducibility helpers.
# Sourced by common.sh; not executable.

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
    # A fixed nonzero epoch keeps generated build metadata deterministic and
    # avoids tools that interpret zero as an unset timestamp.
    export SOURCE_DATE_EPOCH=1
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
    local temp_base
    local source
    local object
    local path
    local flag
    local flags=

    temp_base=$(cup_path_resolve_host_temporary_directory \
        'dependency compiler-probe temporary parent') || return 1
    work=$(cup_path_create_unique_directory \
        "$temp_base/cup-prefix-map.XXXXXX" \
        'dependency compiler-probe directory' 0700) || return 1
    source="$work/probe.c"
    object="$work/probe.o"
    printf '%s\n' 'int cup_prefix_map_probe(void) { return 0; }' > "$source"
    for path in "$@"; do
        [ -n "$path" ] || continue
        while IFS= read -r variant; do
            case "$variant" in
                *\\*)
                    continue
                    ;;
            esac
            for option in file debug macro; do
                flag="-f${option}-prefix-map=$variant=/usr/src/cup-dependencies"
                if "$compiler" "$flag" -c "$source" -o "$object" >/dev/null 2>&1; then
                    case " $flags " in
                        *" $flag "*)
                            ;;
                        *)
                            flags="$flags $flag"
                            ;;
                    esac
                fi
            done
        done < <(dependency_path_variants "$path")
    done
    cup_path_remove_directory_tree "$work" 'dependency compiler-probe directory' || return 1
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
            local found_variant=
            [ -n "$forbidden" ] || continue
            while IFS= read -r variant; do
                [ -n "$variant" ] || continue
                if [ -z "$found_variant" ] &&
                    LC_ALL=C grep -aF -q -- "$variant" "$archive"; then
                    found_variant=$variant
                fi
            done < <(dependency_path_variants "$forbidden")
            if [ -n "$found_variant" ]; then
                echo "Error: compiled dependency contains forbidden path" \
                    "'$found_variant': $archive" >&2
                return 1
            fi
        done
        staging_pattern='[\\/]\.install\.staging([\\/]|$)'
        if LC_ALL=C grep -aE -q -- "$staging_pattern" "$archive"; then
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
