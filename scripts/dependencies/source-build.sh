# Downloads, extracts and builds the shared pinned dependency sources.
# Sourced by common.sh; not executable.

download_source() {
    local package="$1"
    local output="$2"
    local url
    local min_bytes
    local size=
    local max_bytes=268435456
    local tmp_output
    local tmp_dir
    local temp_base

    url="$(source_url_for_package "$package")"
    min_bytes="$(minimum_bytes_for_package "$package")"

    if [ -e "$output" ] || [ -L "$output" ]; then
        if ! cup_path_require_regular_file "$output" 'cached dependency archive'; then
            echo "Error: cached dependency archive is not a safe regular file: $output" >&2
            return 1
        fi
        size="$(wc -c < "$output" | tr -d '[:space:]')"
        if [ "$size" -ge "$min_bytes" ] &&
            verify_source_checksum "$package" "$output"; then
            echo "==> Using cached $(basename "$output")"
            return 0
        fi

        echo "==> Removing suspicious cached $(basename "$output") (${size} bytes)"
        cup_path_remove_file "$output" 'suspicious dependency archive' || return 1
    fi

    cup_path_prepare_child_file "$DEPS_ROOT" "$output" \
        "dependency source archive" || return 1
    temp_base=${TMPDIR:-/tmp}
    case "$temp_base" in
        /) ;;
        */) temp_base=${temp_base%/} ;;
    esac
    case "$temp_base" in /*) ;; *) temp_base=$(pwd -P)/$temp_base ;; esac
    cup_path_validate_absolute_clean "$temp_base" \
        'dependency download temporary parent' || return 1
    if ! temp_base=$(CDPATH= cd -- "$temp_base" 2>/dev/null && pwd -P); then
        echo "Error: dependency download temporary parent is not an existing directory: $temp_base" >&2
        return 1
    fi
    cup_path_check_directory_chain "$temp_base" 0 \
        'dependency download temporary parent' || return 1
    tmp_dir=$(cup_path_create_unique_directory \
        "$temp_base/cup-dependency-download.XXXXXX" \
        'dependency download directory' 0700) || return 1
    tmp_output=$tmp_dir/$(basename "$output")
    echo "==> Downloading $url"
    if ! curl -fL --proto '=https' --proto-redir '=https' \
        --connect-timeout 30 --max-time 900 \
        --speed-limit 1024 --speed-time 60 \
        --max-filesize "$max_bytes" \
        --retry 3 --retry-delay 5 --retry-all-errors \
        "$url" -o "$tmp_output"; then
        cup_path_remove_directory_tree "$tmp_dir" 'dependency download directory' >/dev/null 2>&1 || true
        return 1
    fi

    size="$(wc -c < "$tmp_output" | tr -d '[:space:]')"
    if [ "$size" -gt "$max_bytes" ]; then
        echo "Error: downloaded $package archive exceeds the maximum size: ${size} bytes." >&2
        cup_path_remove_directory_tree "$tmp_dir" 'dependency download directory' >/dev/null 2>&1 || true
        return 1
    fi
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
        cup_path_remove_directory_tree "$tmp_dir" 'dependency download directory' >/dev/null 2>&1 || true
        return 1
    fi

    if ! verify_source_checksum "$package" "$tmp_output"; then
        cup_path_remove_directory_tree "$tmp_dir" 'dependency download directory' >/dev/null 2>&1 || true
        return 1
    fi
    if ! cup_path_copy_file "$tmp_output" "$output" 0644 replace; then
        cup_path_remove_directory_tree "$tmp_dir" 'dependency download directory' >/dev/null 2>&1 || true
        return 1
    fi
    cup_path_remove_directory_tree "$tmp_dir" 'dependency download directory' || return 1
}

extract_archive() {
    local archive="$1"
    local destination="$2"

    cup_path_require_within "$DEPS_ROOT" "$destination" \
        "dependency extraction directory" || return 1
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        cup_path_check_directory_chain "$destination" 0 \
            "dependency extraction directory" || return 1
        cup_path_remove_child_tree "$DEPS_ROOT" "$destination" \
            'dependency extraction directory' || return 1
    fi
    cup_path_prepare_child_directory "$DEPS_ROOT" "$destination" \
        "dependency extraction directory" || return 1

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

# Static asynchronous resolver used by libcurl on every supported platform.
build_cares_static() {
    local src_dir="$1"
    local build_dir="$2"
    local compiler="$3"
    local archiver="$4"
    local ranlib_tool="$5"
    local host_triple="${6:-}"
    local extra_libs="${7:-}"
    local archive="$src_dir/c-ares-${CARES_VERSION}.tar.gz"
    local source="$build_dir/c-ares-${CARES_VERSION}"

    download_source cares "$archive"
    extract_archive "$archive" "$source"

    set -- \
        --prefix="$INSTALL_PREFIX" \
        --disable-shared \
        --enable-static \
        --disable-tests
    if [ -n "$host_triple" ]; then
        set -- --host="$host_triple" "$@"
    fi

    echo "==> Building c-ares ${CARES_VERSION}"
    cd "$source"
    # shellcheck disable=SC2086
    CC="$compiler" AR="$archiver" RANLIB="$ranlib_tool" \
        CFLAGS="${CUP_DEPENDENCY_CFLAGS:-}" \
        LDFLAGS="-L$PREFIX/lib -L$PREFIX/lib64" \
        LIBS="$extra_libs" ./configure "$@"
    make -j"$JOBS"
    make install DESTDIR="$DESTDIR"
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

    cup_path_prepare_child_directory "$DEPS_ROOT" "$prefix/include" "dependency include directory" || return 1
    cup_path_prepare_child_directory "$DEPS_ROOT" "$prefix/lib" "dependency library directory" || return 1
    cup_path_prepare_child_directory "$DEPS_ROOT" "$object_dir" "dependency object directory" || return 1

    archive="$src_dir/argtable3-${ARGTABLE3_VERSION}.tar.gz"
    source="$build_dir/argtable3-${ARGTABLE3_VERSION}"
    download_source argtable3 "$archive"
    extract_archive "$archive" "$source"
    for object in "$object_dir"/argtable3-*.o; do
        [ -e "$object" ] || [ -L "$object" ] || continue
        cup_path_remove_file "$object" 'stale argtable3 object' || return 1
    done
    if [ -e "$prefix/lib/libargtable3.a" ] || [ -L "$prefix/lib/libargtable3.a" ]; then
        cup_path_remove_file "$prefix/lib/libargtable3.a" 'stale argtable3 archive' || return 1
    fi
    for file in "$source"/src/*.c; do
        object="$object_dir/argtable3-$(basename "${file%.c}").o"
        # shellcheck disable=SC2086
        "$compiler" ${CUP_DEPENDENCY_CFLAGS:--O2} -I"$source/src" -c "$file" -o "$object"
    done
    "$archiver" rcs "$prefix/lib/libargtable3.a" "$object_dir"/argtable3-*.o
    "$ranlib_tool" "$prefix/lib/libargtable3.a"
    cup_path_copy_file "$source/src/argtable3.h" "$prefix/include/argtable3.h" 0644 replace

    archive="$src_dir/uthash-${UTHASH_VERSION}.tar.gz"
    download_source uthash "$archive"
    uthash_header="$prefix/include/uthash.h.tmp.$$"
    if ! tar -xOzf "$archive" \
        "uthash-${UTHASH_VERSION}/src/uthash.h" | \
            cup_path_write_file "$uthash_header" 0644 replace; then
        cup_path_remove_file "$uthash_header" 'temporary uthash header' >/dev/null 2>&1 || true
        echo "Error: could not extract uthash.h from $(basename "$archive")." >&2
        return 1
    fi
    [ -s "$uthash_header" ] || {
        cup_path_remove_file "$uthash_header" 'temporary uthash header' >/dev/null 2>&1 || true
        echo "Error: extracted uthash.h is empty." >&2
        return 1
    }
    cup_path_copy_file "$uthash_header" "$prefix/include/uthash.h" 0644 replace
    cup_path_remove_file "$uthash_header" 'temporary uthash header'

    archive="$src_dir/unity-${UNITY_VERSION}.tar.gz"
    source="$build_dir/unity-${UNITY_VERSION}"
    download_source unity "$archive"
    extract_archive "$archive" "$source"
    # shellcheck disable=SC2086
    "$compiler" ${CUP_DEPENDENCY_CFLAGS:--O2} -I"$source/src" -c "$source/src/unity.c" \
        -o "$object_dir/unity.o"
    "$archiver" rcs "$prefix/lib/libunity.a" "$object_dir/unity.o"
    "$ranlib_tool" "$prefix/lib/libunity.a"
    cup_path_copy_file "$source/src/unity.h" "$prefix/include/unity.h" 0644 replace
    cup_path_copy_file "$source/src/unity_internals.h" "$prefix/include/unity_internals.h" 0644 replace
}
