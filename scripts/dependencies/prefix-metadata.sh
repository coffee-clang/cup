# Describes, validates and atomically publishes one complete dependency prefix.
# Sourced by common.sh; not executable.

dependency_lock_sha256() {
    local package version checksum

    {
        printf '%s\n' 'format=2'
        for package in $(all_source_packages); do
            version=$(version_for_package "$package") || return 1
            checksum=$(sha256_for_package "$package") || return 1
            printf '%s.version=%s\n' "$package" "$version"
            printf '%s.sha256=%s\n' "$package" "$checksum"
        done
    } | stream_sha256
}

dependency_profile() {
    local platform="$1"
    local requested="${CUP_DEPENDENCY_PROFILE:-}"
    local profile

    if [ -n "$requested" ]; then
        profile=$requested
    else
        case "$platform" in
            linux-x64|linux-arm64) profile=gcc ;;
            macos-x64|macos-arm64) profile=apple-clang ;;
            windows-x64)
                case "${MSYSTEM:-}" in
                    UCRT64) profile=ucrt64-gcc ;;
                    CLANG64) profile=clang64 ;;
                    *) echo "Error: Windows dependencies require UCRT64 or CLANG64." >&2; return 1 ;;
                esac
                ;;
            *) echo "Error: unsupported dependency platform: $platform" >&2; return 1 ;;
        esac
    fi
    case "$platform:$profile" in
        linux-x64:gcc|linux-arm64:gcc|macos-x64:apple-clang|macos-arm64:apple-clang|\
        windows-x64:ucrt64-gcc|windows-x64:clang64) printf '%s\n' "$profile" ;;
        *) echo "Error: dependency profile '$profile' is invalid for $platform." >&2; return 1 ;;
    esac
}

dependency_uses_openssl() {
    case "$1" in
        windows-x64) printf '%s\n' 0 ;;
        linux-x64|linux-arm64|macos-x64|macos-arm64) printf '%s\n' 1 ;;
        *) return 1 ;;
    esac
}

dependency_tool_version_line() {
    local tool="$1"
    local output=
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Error: required tool '$tool' was not found while fingerprinting the toolchain." >&2
        return 1
    }
    output=$("$tool" --version 2>&1 | sed -n '1p' || true)
    if [ -z "$output" ]; then
        output=$("$tool" -V 2>&1 | sed -n '1p' || true)
    fi
    [ -n "$output" ] || {
        echo "Error: could not identify tool '$tool'." >&2
        return 1
    }
    printf '%s\n' "$output"
}

dependency_toolchain_sha256() {
    local platform="$1" profile="$2"
    local compiler archiver ranlib target compiler_version archiver_version ranlib_version sdk=none

    profile=$(CUP_DEPENDENCY_PROFILE="$profile" dependency_profile "$platform") || return 1
    # Dependency compatibility is tied to the canonical profile toolchain,
    # not to the application compiler selected by a consuming Make invocation.
    # Builders already use these exact tools; ignoring ambient CC/AR/RANLIB here
    # prevents an application compiler matrix from invalidating one verified prefix.
    case "$profile" in
        gcc|ucrt64-gcc) compiler=gcc; archiver=ar; ranlib=ranlib ;;
        apple-clang) compiler=clang; archiver=ar; ranlib=ranlib ;;
        clang64) compiler=clang; archiver=llvm-ar; ranlib=llvm-ranlib ;;
    esac
    target=$("$compiler" -dumpmachine 2>/dev/null || "$compiler" -print-target-triple 2>/dev/null) || return 1
    compiler_version=$(dependency_tool_version_line "$compiler") || return 1
    archiver_version=$(dependency_tool_version_line "$archiver") || return 1
    ranlib_version=$(dependency_tool_version_line "$ranlib") || return 1
    case "$platform" in
        macos-*)
            command -v xcrun >/dev/null 2>&1 || return 1
            sdk=$(xcrun --sdk macosx --show-sdk-version 2>/dev/null) || return 1
            ;;
    esac
    {
        printf 'platform=%s\n' "$platform"
        printf 'profile=%s\n' "$profile"
        printf 'compiler_target=%s\n' "$target"
        printf 'compiler_version=%s\n' "$compiler_version"
        printf 'archiver_version=%s\n' "$archiver_version"
        printf 'ranlib_version=%s\n' "$ranlib_version"
        printf 'sdk=%s\n' "$sdk"
        printf 'macos_deployment_target=%s\n' "${MACOSX_DEPLOYMENT_TARGET:-none}"
        printf 'msystem=%s\n' "${MSYSTEM:-none}"
    } | stream_sha256
}

dependency_metadata() {
    local platform="$1" profile="$2"
    local source_lock_sha256 toolchain_sha256

    profile=$(CUP_DEPENDENCY_PROFILE="$profile" dependency_profile "$platform") || return 1
    source_lock_sha256=$(dependency_lock_sha256) || return 1
    toolchain_sha256=$(dependency_toolchain_sha256 "$platform" "$profile") || return 1
    printf '%s\n' \
        "prefix_format=$CUP_DEPENDENCY_PREFIX_FORMAT" \
        'product=coffee-clang/cup' \
        'kind=dependency-prefix' \
        "platform=$platform" \
        "profile=$profile" \
        "build_revision=$DEPENDENCY_BUILD_REVISION" \
        "source_lock_sha256=$source_lock_sha256" \
        "toolchain_sha256=$toolchain_sha256"
}

dependency_metadata_valid() {
    local metadata="$1" platform profile expected
    platform=$(printf '%s\n' "$metadata" | sed -n 's/^platform=//p')
    profile=$(printf '%s\n' "$metadata" | sed -n 's/^profile=//p')
    expected=$(dependency_metadata "$platform" "$profile" 2>/dev/null) || return 1
    [ "$metadata" = "$expected" ]
}

dependency_cache_key() {
    local platform="$1" profile="$2"
    local source_lock_sha256 toolchain_sha256
    profile=$(CUP_DEPENDENCY_PROFILE="$profile" dependency_profile "$platform") || return 1
    source_lock_sha256=$(dependency_lock_sha256) || return 1
    toolchain_sha256=$(dependency_toolchain_sha256 "$platform" "$profile") || return 1
    printf 'cup-deps-f%s-%s-%s-b%s-%s-%s\n' \
        "$CUP_DEPENDENCY_PREFIX_FORMAT" "$platform" "$profile" "$DEPENDENCY_BUILD_REVISION" \
        "$source_lock_sha256" "$toolchain_sha256"
}

dependency_regular_nonempty_file() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ -s "$1" ]
}

dependency_archive_tool() {
    if [ -n "${AR:-}" ] && command -v "$AR" >/dev/null 2>&1; then
        printf '%s\n' "$AR"
    elif command -v ar >/dev/null 2>&1; then
        printf '%s\n' ar
    elif command -v llvm-ar >/dev/null 2>&1; then
        printf '%s\n' llvm-ar
    else
        return 1
    fi
}

dependency_archive_readable() {
    local archive="$1" tool
    dependency_regular_nonempty_file "$archive" || return 1
    tool=$(dependency_archive_tool) || return 1
    "$tool" t "$archive" >/dev/null 2>&1
}

dependency_library_exists() {
    local prefix="$1" name="$2" directory candidate
    for directory in "$prefix/lib" "$prefix/lib64"; do
        for candidate in "$directory/lib$name.a" "$directory/lib$name.dll.a"; do
            if [ -e "$candidate" ] || [ -L "$candidate" ]; then
                dependency_archive_readable "$candidate" && return 0
                return 1
            fi
        done
    done
    return 1
}

dependency_tree_has_no_symlinks() {
    [ -d "$1" ] && [ ! -L "$1" ] || return 1
    ! find "$1" -type l -print -quit | grep -q .
}

application_dependency_prefix_complete() {
    local prefix="$1"

    dependency_regular_nonempty_file "$prefix/include/argtable3.h" &&
        dependency_regular_nonempty_file "$prefix/include/uthash.h" &&
        dependency_regular_nonempty_file "$prefix/include/ares.h" &&
        dependency_library_exists "$prefix" argtable3 &&
        dependency_library_exists "$prefix" cares
}

test_dependency_prefix_complete() {
    local prefix="$1"

    dependency_regular_nonempty_file "$prefix/include/unity.h" &&
        dependency_regular_nonempty_file "$prefix/include/unity_internals.h" &&
        dependency_regular_nonempty_file "$prefix/include/event2/event.h" &&
        dependency_regular_nonempty_file "$prefix/include/event2/http.h" &&
        dependency_regular_nonempty_file "$prefix/include/event2/bufferevent.h" &&
        dependency_regular_nonempty_file "$prefix/include/event2/listener.h" &&
        dependency_library_exists "$prefix" unity &&
        dependency_library_exists "$prefix" event_core &&
        dependency_library_exists "$prefix" event_extra &&
        { dependency_regular_nonempty_file "$prefix/lib/pkgconfig/libevent_core.pc" ||
          dependency_regular_nonempty_file "$prefix/lib64/pkgconfig/libevent_core.pc"; } &&
        { dependency_regular_nonempty_file "$prefix/lib/pkgconfig/libevent_extra.pc" ||
          dependency_regular_nonempty_file "$prefix/lib64/pkgconfig/libevent_extra.pc"; }
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
    local normalized=${path//\\//}
    local variant
    local matched=1

    case "/$normalized/" in
        */./*|*/../*) return 1 ;;
    esac
    while IFS= read -r variant; do
        case "$path" in
            "$variant"|"$variant"/*|"$variant"\\*)
                matched=0
                ;;
        esac
    done < <(dependency_path_variants "$expected_prefix")
    return "$matched"
}

dependency_text_references_path() {
    local flags="$1"
    local path="$2"
    local variant
    local matched=1

    while IFS= read -r variant; do
        case "$flags" in
            *"$variant"*)
                matched=0
                ;;
        esac
    done < <(dependency_path_variants "$path")
    return "$matched"
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
                        -L|-F|-R|-rpath|--rpath|-rpath-link|--rpath-link|\
                        --library-path|--sysroot|-B)
                            ((part_index + 1 < ${#linker_parts[@]})) || return 1
                            part_index=$((part_index + 1))
                            path=${linker_parts[part_index]}
                            ;;
                        -L?*|-F?*|-R?*|-B?*)
                            path=${part:2}
                            ;;
                        -rpath=*|--rpath=*|-rpath-link=*|--rpath-link=*|\
                        --library-path=*|--sysroot=*)
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
        *" -lacl "*)
            return 1
            ;;
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
    local cares_flags=
    local curl_flags=
    local curl_configure=
    local archive_flags=
    local event_flags=

    [ -x "$prefix/bin/curl-config" ] || return 1
    command -v pkg-config >/dev/null 2>&1 || return 1

    cares_flags=$(PKG_CONFIG_PATH="$pkg_config_path" \
        PKG_CONFIG_LIBDIR="$pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libcares 2>/dev/null) || return 1
    curl_flags=$("$prefix/bin/curl-config" --static-libs 2>/dev/null) || return 1
    curl_configure=$("$prefix/bin/curl-config" --configure 2>/dev/null) || return 1
    archive_flags=$(PKG_CONFIG_PATH="$pkg_config_path" \
        PKG_CONFIG_LIBDIR="$pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libarchive 2>/dev/null) || return 1
    event_flags=$(PKG_CONFIG_PATH="$pkg_config_path" \
        PKG_CONFIG_LIBDIR="$pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR="" \
        dependency_pkg_config --static --libs libevent_extra libevent_core 2>/dev/null) || return 1
    [ -n "$cares_flags" ] && [ -n "$curl_flags" ] && \
        [ -n "$curl_configure" ] && [ -n "$archive_flags" ] && \
        [ -n "$event_flags" ] || return 1

    if ! "$prefix/bin/curl-config" --features 2>/dev/null | \
        grep -Fx AsynchDNS >/dev/null; then
        echo "Error: curl was not built with asynchronous hostname resolution." >&2
        return 1
    fi
    case " $curl_flags " in
        *" -lcares "*|*"/libcares.a "*)
            ;;
        *)
            echo "Error: curl static link metadata does not include c-ares." >&2
            return 1
            ;;
    esac

    case "$curl_configure" in
        *--enable-ares=*)
            ;;
        *)
            echo "Error: curl configure metadata does not enable c-ares." >&2
            return 1
            ;;
    esac

    if ! dependency_text_references_path "$curl_configure" "$expected_prefix"; then
        echo "Error: curl configure metadata does not reference the final prefix:" \
            "$expected_prefix" >&2
        return 1
    fi
    if [ "$prefix" != "$expected_prefix" ] &&
        dependency_text_references_path "$curl_configure" "$prefix"; then
        echo "Error: curl configure metadata still references the staging prefix:" \
            "$prefix" >&2
        return 1
    fi

    dependency_link_flags_valid "$cares_flags" "$prefix" "$expected_prefix" &&
        dependency_link_flags_valid "$curl_flags" "$prefix" "$expected_prefix" &&
        dependency_link_flags_valid "$archive_flags" "$prefix" "$expected_prefix" &&
        dependency_link_flags_valid "$event_flags" "$prefix" "$expected_prefix"
}

dependency_prefix_complete() {
    local prefix="$1"
    local use_openssl="${2:-1}"
    local metadata_prefix="${3:-$prefix}"

    case "$use_openssl" in
        0|1)
            ;;
        *)
            return 1
            ;;
    esac
    dependency_tree_has_no_symlinks "$prefix" &&
        application_dependency_prefix_complete "$prefix" &&
        test_dependency_prefix_complete "$prefix" &&
        dependency_regular_nonempty_file "$prefix/bin/curl-config" &&
        [ -x "$prefix/bin/curl-config" ] &&
        dependency_regular_nonempty_file "$prefix/include/curl/curl.h" &&
        dependency_regular_nonempty_file "$prefix/include/archive.h" &&
        dependency_regular_nonempty_file "$prefix/include/archive_entry.h" &&
        dependency_regular_nonempty_file "$prefix/include/zlib.h" &&
        dependency_regular_nonempty_file "$prefix/include/lzma.h" &&
        dependency_library_exists "$prefix" curl &&
        dependency_library_exists "$prefix" archive &&
        dependency_library_exists "$prefix" z &&
        dependency_library_exists "$prefix" lzma &&
        { dependency_regular_nonempty_file "$prefix/lib/pkgconfig/libcares.pc" ||
          dependency_regular_nonempty_file "$prefix/lib64/pkgconfig/libcares.pc"; } &&
        { dependency_regular_nonempty_file "$prefix/lib/pkgconfig/libarchive.pc" ||
          dependency_regular_nonempty_file "$prefix/lib64/pkgconfig/libarchive.pc"; } &&
        dependency_link_metadata_valid "$prefix" "$metadata_prefix" || return 1

    if [ "$use_openssl" = 1 ]; then
        dependency_regular_nonempty_file "$prefix/include/openssl/ssl.h" &&
            dependency_library_exists "$prefix" ssl &&
            dependency_library_exists "$prefix" crypto
    fi
}

dependency_prefix_matches() {
    local prefix="$1" metadata="$2" use_openssl="${3:-1}"
    local config="$prefix/.cup-dependencies"
    dependency_metadata_valid "$metadata" &&
        dependency_regular_nonempty_file "$config" &&
        [ ! -e "$prefix/.cup-deps-building" ] && [ ! -L "$prefix/.cup-deps-building" ] &&
        printf '%s\n' "$metadata" | dependency_stream_matches_file "$config" &&
        dependency_prefix_complete "$prefix" "$use_openssl" "$prefix"
}

prepare_dependency_prefix() {
    local final_prefix="$1"
    local metadata="$2"
    local use_openssl="${3:-1}"
    local expected_prefix="$DEPS_ROOT/install"
    local existing_marker

    dependency_metadata_valid "$metadata" || {
        echo "Error: invalid dependency prefix metadata." >&2
        return 1
    }
    case "$use_openssl" in
        0|1)
            ;;
        *)
            return 1
            ;;
    esac
    dependency_prepare_root "$DEPS_ROOT" || return 1
    [ "$final_prefix" = "$expected_prefix" ] || {
        echo "Error: dependency builders may write only DEPS_ROOT/install." >&2
        echo "Expected: $expected_prefix" >&2
        echo "Actual:   $final_prefix" >&2
        return 1
    }

    CUP_DEPS_PREFIX_READY=0
    CUP_DEPS_FINAL_PREFIX=$final_prefix
    CUP_DEPS_STAGE_ROOT=
    CUP_DEPS_BUILD_PREFIX=
    CUP_DEPS_USE_OPENSSL=$use_openssl

    if [ "${CUP_DEPS_FORCE:-0}" != 1 ] &&
        dependency_prefix_matches "$final_prefix" "$metadata" "$use_openssl"; then
        CUP_DEPS_PREFIX_READY=1
        CUP_DEPS_BUILD_PREFIX=$final_prefix
        echo "==> Reusing dependency prefix $final_prefix"
        return 0
    fi

    if [ -e "$final_prefix" ] || [ -L "$final_prefix" ]; then
        if [ -L "$final_prefix" ] || [ ! -d "$final_prefix" ]; then
            echo "Error: dependency prefix is not a real directory: $final_prefix" >&2
            return 1
        fi
        if ! dependency_directory_empty "$final_prefix" && ! dependency_prefix_owned "$final_prefix"; then
            echo "Error: refusing to replace a dependency prefix not owned by cup: $final_prefix" >&2
            return 1
        fi
    fi

    CUP_DEPS_STAGE_ROOT="$DEPS_ROOT/.install.staging"
    [ ! -e "$CUP_DEPS_STAGE_ROOT" ] && [ ! -L "$CUP_DEPS_STAGE_ROOT" ] || {
        echo "Error: dependency staging path was not recovered: $CUP_DEPS_STAGE_ROOT" >&2
        return 1
    }
    cup_path_prepare_child_directory "$DEPS_ROOT" "$CUP_DEPS_STAGE_ROOT" \
        "dependency staging directory" || return 1
    CUP_DEPS_BUILD_PREFIX="$CUP_DEPS_STAGE_ROOT$final_prefix"
    cup_path_prepare_child_directory "$DEPS_ROOT" "$CUP_DEPS_BUILD_PREFIX" \
        "dependency build prefix" || return 1
    printf '%s\n' "$metadata" |
        cup_path_write_file "$CUP_DEPS_BUILD_PREFIX/.cup-deps-building" 0644 replace ||
        return 1
}

abort_dependency_prefix() {
    if [ "$CUP_DEPS_PREFIX_READY" != 1 ] && [ -n "$CUP_DEPS_STAGE_ROOT" ]; then
        case "$CUP_DEPS_STAGE_ROOT" in
            "$DEPS_ROOT/.install.staging")
                cup_path_remove_child_tree "$DEPS_ROOT" "$CUP_DEPS_STAGE_ROOT" \
                    'dependency staging directory' || return 1
                ;;
            *) echo "Error: refusing to remove unexpected dependency staging path." >&2 ;;
        esac
    fi
    CUP_DEPS_PREFIX_READY=0
    CUP_DEPS_FINAL_PREFIX=
    CUP_DEPS_STAGE_ROOT=
    CUP_DEPS_BUILD_PREFIX=
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
                # incorrectly applies cup bootstrap mappings to later builds.
                s{(^|[\s"\x27=])-f(?:file|debug|macro)-prefix-map=[^\s"\x27();]+}{$1}gm;
            ' "$metadata"

        if [ -n "$staged_prefix" ] && [ "$staged_prefix" != "$final_prefix" ] &&
            dependency_text_references_path "$(cat "$metadata")" "$staged_prefix"; then
            echo "Error: generated metadata still contains the staged payload prefix: $metadata" >&2
            return 1
        fi
        if [ -n "$stage_root" ]; then
            found_variant=
            while IFS= read -r variant; do
                [ -n "$variant" ] || continue
                if [ -z "$found_variant" ] &&
                    LC_ALL=C grep -F -I -q -- "$variant" "$metadata"; then
                    found_variant=$variant
                fi
            done < <(dependency_path_variants "$stage_root")
            if [ -n "$found_variant" ]; then
                echo "Error: generated metadata still contains the staging" \
                    "root '$found_variant': $metadata" >&2
                return 1
            fi
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

    [ "$build_prefix" = "$CUP_DEPS_BUILD_PREFIX" ] || {
        echo "Error: dependency build prefix does not match the prepared transaction." >&2
        return 1
    }
    [ "$CUP_DEPS_STAGE_ROOT" = "$DEPS_ROOT/.install.staging" ] || {
        echo "Error: dependency staging root is not initialized." >&2
        return 1
    }
    dependency_prefix_complete "$build_prefix" "$CUP_DEPS_USE_OPENSSL" "$final_prefix" || {
        echo "Error: refusing to commit an incomplete dependency prefix." >&2
        return 1
    }
    dependency_compiled_paths_valid "$build_prefix" \
        "$CUP_DEPS_STAGE_ROOT" "$DEPS_ROOT" "${BUILD_DIR:-}" || return 1
    cup_path_move_entry "$build_prefix/.cup-deps-building" \
        "$build_prefix/.cup-dependencies" || return 1

    if [ -e "$final_prefix" ] || [ -L "$final_prefix" ]; then
        if [ -L "$final_prefix" ] ||
            { ! dependency_directory_empty "$final_prefix" &&
              ! dependency_prefix_owned "$final_prefix"; }; then
            echo "Error: refusing to replace a dependency prefix not owned by cup." >&2
            return 1
        fi
        cup_path_remove_child_tree "$DEPS_ROOT" "$final_prefix" \
            'dependency prefix' || return 1
    fi

    cup_path_move_entry "$build_prefix" "$final_prefix" || return 1
    cup_path_remove_child_tree "$DEPS_ROOT" "$CUP_DEPS_STAGE_ROOT" \
        'dependency staging directory' || return 1
    CUP_DEPS_PREFIX_READY=1
    CUP_DEPS_STAGE_ROOT=
    CUP_DEPS_BUILD_PREFIX=$final_prefix
}

# Verified source download and extraction.
