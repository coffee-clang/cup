#!/bin/sh

# Purpose: Verifies source-test environment resolution, explicit dependency
# preparation and build-prefix isolation.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"
. "$TESTS_ROOT/support/environment.sh"

test_begin environment

NATIVE_BUILD_PLATFORM=$(cup_test_detect_platform) ||
    fail 'could not resolve native build platform for repository tests'

create_private_prefix() {
    prefix=$1
    mkdir -p "$prefix/include/event2" "$prefix/lib"
    touch "$prefix/include/argtable3.h" \
        "$prefix/include/uthash.h" \
        "$prefix/include/unity.h" \
        "$prefix/include/unity_internals.h" \
        "$prefix/include/event2/event.h" \
        "$prefix/include/event2/http.h" \
        "$prefix/include/event2/bufferevent.h" \
        "$prefix/include/event2/listener.h" \
        "$prefix/lib/libargtable3.a" \
        "$prefix/lib/libunity.a" \
        "$prefix/lib/libevent_core.a" \
        "$prefix/lib/libevent_extra.a"
}

create_complete_prefix() {
    prefix=$1
    use_openssl=${2:-1}
    create_private_prefix "$prefix"
    mkdir -p "$prefix/bin" "$prefix/include/curl" \
        "$prefix/lib/pkgconfig"
    touch "$prefix/include/curl/curl.h" \
        "$prefix/include/archive.h" \
        "$prefix/include/archive_entry.h" \
        "$prefix/include/zlib.h" \
        "$prefix/include/lzma.h" \
        "$prefix/lib/libcurl.a" \
        "$prefix/lib/libarchive.a" \
        "$prefix/lib/libz.a" \
        "$prefix/lib/liblzma.a"
    if [ "$use_openssl" = 1 ]; then
        mkdir -p "$prefix/include/openssl"
        touch "$prefix/include/openssl/ssl.h" \
            "$prefix/lib/libssl.a" \
            "$prefix/lib/libcrypto.a"
    fi
    cat >"$prefix/bin/curl-config" <<EOF_CURL_CONFIG
#!/bin/sh
[ "\${1:-}" = --static-libs ] || exit 2
printf '%s\\n' '$prefix/lib/libcurl.a $prefix/lib/libssl.a $prefix/lib/libcrypto.a $prefix/lib/libz.a'
EOF_CURL_CONFIG
    chmod +x "$prefix/bin/curl-config"
    cat >"$prefix/lib/pkgconfig/libarchive.pc" <<EOF_ARCHIVE_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libarchive
Description: test metadata
Version: 1
Libs: \${libdir}/libarchive.a \${libdir}/liblzma.a \${libdir}/libz.a
Cflags: -I\${includedir}
EOF_ARCHIVE_PC
    cat >"$prefix/lib/pkgconfig/libevent_core.pc" <<EOF_EVENT_CORE_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libevent_core
Description: test metadata
Version: 1
Libs: -L\${libdir} -levent_core
Cflags: -I\${includedir}
EOF_EVENT_CORE_PC
    cat >"$prefix/lib/pkgconfig/libevent_extra.pc" <<EOF_EVENT_EXTRA_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libevent_extra
Description: test metadata
Version: 1
Requires.private: libevent_core
Libs: -L\${libdir} -levent_extra
Cflags: -I\${includedir}
EOF_EVENT_EXTRA_PC
}

(
    unset CUP_TEST_PLATFORM DEPS_PREFIX
    export HOME="$TMP_ROOT/home"
    export PLATFORM=linux/amd64

    uname() {
        case "$1" in
            -s) printf '%s\n' Linux ;;
            -m) printf '%s\n' x86_64 ;;
            *) return 1 ;;
        esac
    }

    cup_test_prepare_environment
    assert_equals "$CUP_TEST_PLATFORM" linux-x64
    assert_equals "$DEPS_PREFIX" "$HOME/deps/linux-x64/install"
)

(
    CUP_TEST_PLATFORM=macos-arm64
    DEPS_PREFIX="$TMP_ROOT/custom-prefix"
    export CUP_TEST_PLATFORM DEPS_PREFIX
    cup_test_prepare_environment
    assert_equals "$CUP_TEST_PLATFORM" macos-arm64
    assert_equals "$DEPS_PREFIX" "$TMP_ROOT/custom-prefix"
)

if (
    CUP_TEST_PLATFORM=linux/amd64
    export CUP_TEST_PLATFORM
    cup_test_prepare_environment
) >"$TMP_ROOT/invalid.out" 2>&1; then
    fail 'invalid CUP_TEST_PLATFORM was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/invalid.out")" 'Unsupported CUP_TEST_PLATFORM'

CUP_TEST_PLATFORM=linux-x64
DEPS_PREFIX="$TMP_ROOT/dependencies"
export CUP_TEST_PLATFORM DEPS_PREFIX
create_complete_prefix "$DEPS_PREFIX" 1
cup_test_dependencies_ready || fail 'complete test dependency prefix was rejected'
rm "$DEPS_PREFIX/include/uthash.h"
if cup_test_dependencies_ready; then
    fail 'incomplete test dependency prefix was accepted'
fi
printf 'Source-test environment contract tests passed.\n'

printf '==> Testing explicit dependency preparation...\n'
FAKE_ROOT="$TMP_ROOT/fake-project"
DEPENDENCY_BUILD_LOG="$TMP_ROOT/dependency-build.log"
mkdir -p "$FAKE_ROOT/scripts/dependencies"
cat >"$FAKE_ROOT/scripts/dependencies/build-posix.sh" <<EOF_BOOTSTRAP
#!/bin/sh
printf 'unexpected dependency build\\n' >'$DEPENDENCY_BUILD_LOG'
EOF_BOOTSTRAP
chmod +x "$FAKE_ROOT/scripts/dependencies/build-posix.sh"
DEPS_PREFIX="$TMP_ROOT/missing-explicit-prefix"
export DEPS_PREFIX
if cup_test_require_dependencies >"$TMP_ROOT/explicit.out" 2>&1; then
    fail 'test dependency check accepted a missing prefix'
fi
[ ! -e "$DEPENDENCY_BUILD_LOG" ] || fail 'test runner started a dependency build implicitly'
assert_contains "$(cat "$TMP_ROOT/explicit.out")" "Run 'make PLATFORM=linux-x64 deps'"
create_complete_prefix "$DEPS_PREFIX" 1
cup_test_require_dependencies || fail 'explicitly prepared test prefix was rejected'
printf 'Explicit dependency preparation tests passed.\n'

unset DEPS_PREFIX
make_output=$(
    cd "$PROJECT_ROOT"
    MAKEFLAGS= MAKEOVERRIDES= PLATFORM=linux/amd64 \
        make --no-print-directory -s version
)
[ -n "$make_output" ] || fail 'Makefile rejected an unrelated PLATFORM environment value'

if (
    cd "$PROJECT_ROOT"
    MAKEFLAGS= MAKEOVERRIDES= \
        make --no-print-directory -n PLATFORM=linux/amd64
) >"$TMP_ROOT/make-invalid.out" 2>&1; then
    fail 'invalid command-line PLATFORM selector was accepted by Makefile'
fi
assert_contains "$(cat "$TMP_ROOT/make-invalid.out")" 'Unsupported PLATFORM'

FAKE_UNAME_DIR="$TMP_ROOT/fake-uname"
mkdir -p "$FAKE_UNAME_DIR"
cat >"$FAKE_UNAME_DIR/uname" <<'EOF_UNAME'
#!/bin/sh
case "$1" in
    -s) printf '%s\n' UnknownOS ;;
    -m) printf '%s\n' unknown-architecture ;;
    *) exit 1 ;;
esac
EOF_UNAME
chmod +x "$FAKE_UNAME_DIR/uname"
if (
    cd "$PROJECT_ROOT"
    PATH="$FAKE_UNAME_DIR:$PATH" MAKEFLAGS= MAKEOVERRIDES= \
        PLATFORM=linux/amd64 make --no-print-directory -n all
) >"$TMP_ROOT/make-unsupported-native.out" 2>&1; then
    fail 'unsupported native architecture was silently treated as x64'
fi
assert_contains "$(cat "$TMP_ROOT/make-unsupported-native.out")" \
    "Unsupported PLATFORM 'unsupported'"
printf 'Makefile platform-selector isolation tests passed.\n'

printf '==> Testing dependency-prefix transactions...\n'
DEPENDENCY_COMMON="$PROJECT_ROOT/scripts/dependencies/common.sh"
TRANSACTION_PREFIX="$TMP_ROOT/transaction-prefix"
mkdir -p "$TRANSACTION_PREFIX"
printf 'partial\n' >"$TRANSACTION_PREFIX/.cup-deps-building"
printf 'old\n' >"$TRANSACTION_PREFIX/old.txt"

bash -eu -o pipefail -c '
    common=$1
    final=$2
    . "$common"

    create_complete() {
        prefix=$1
        embedded_prefix=$2
        mkdir -p "$prefix/bin" "$prefix/include/curl" \
            "$prefix/include/openssl" "$prefix/include/event2" \
            "$prefix/lib/pkgconfig"
        touch "$prefix/include/argtable3.h" "$prefix/include/uthash.h" \
            "$prefix/include/unity.h" "$prefix/include/unity_internals.h" \
            "$prefix/include/event2/event.h" "$prefix/include/event2/http.h" \
            "$prefix/include/event2/bufferevent.h" \
            "$prefix/include/event2/listener.h" \
            "$prefix/include/curl/curl.h" "$prefix/include/archive.h" \
            "$prefix/include/archive_entry.h" "$prefix/include/zlib.h" \
            "$prefix/include/lzma.h" "$prefix/include/openssl/ssl.h" \
            "$prefix/lib/libargtable3.a" "$prefix/lib/libunity.a" \
            "$prefix/lib/libevent_core.a" "$prefix/lib/libevent_extra.a" \
            "$prefix/lib/libcurl.a" "$prefix/lib/libarchive.a" \
            "$prefix/lib/libz.a" "$prefix/lib/liblzma.a" \
            "$prefix/lib/libssl.a" "$prefix/lib/libcrypto.a"
        cat >"$prefix/bin/curl-config" <<EOF_CURL_CONFIG
#!/bin/sh
printf "%s\n" "-L$embedded_prefix/lib -lcurl"
EOF_CURL_CONFIG
        chmod +x "$prefix/bin/curl-config"
        cat >"$prefix/lib/pkgconfig/libarchive.pc" <<EOF_LIBARCHIVE_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: libarchive
Description: test metadata
Version: 1
Libs: -L\${libdir} -larchive
EOF_LIBARCHIVE_PC
        cat >"$prefix/lib/pkgconfig/libevent_core.pc" <<EOF_EVENT_CORE_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: libevent_core
Description: test metadata
Version: 1
Libs: -L\${libdir} -levent_core
EOF_EVENT_CORE_PC
        cat >"$prefix/lib/pkgconfig/libevent_extra.pc" <<EOF_EVENT_EXTRA_PC
prefix=$embedded_prefix
libdir=\${prefix}/lib
Name: libevent_extra
Description: test metadata
Version: 1
Requires.private: libevent_core
Libs: -L\${libdir} -levent_extra
EOF_EVENT_EXTRA_PC
    }

    recipe_root=$(mktemp -d)
    printf "%s\n" "recipe one" >"$recipe_root/one.sh"
    printf "%s\n" "recipe two" >"$recipe_root/two.sh"
    id=$(dependency_id test-platform test-toolchain 1 "$recipe_root" \
        "$recipe_root/one.sh" "$recipe_root/two.sh")
    metadata=$(dependency_metadata test-platform "$id")
    [ "$(printf "%s\n" "$metadata" | sed -n "1p")" = \
        "prefix_format=3" ]
    [ "$(printf "%s\n" "$metadata" | sed -n "2p")" = \
        "platform=test-platform" ]
    [ "$(printf "%s\n" "$metadata" | sed -n "3p")" = \
        "dependency_id=$id" ]
    dependency_metadata_valid "$metadata"
    old_id=$id
    printf "%s\n" "recipe changed" >>"$recipe_root/two.sh"
    id=$(dependency_id test-platform test-toolchain 1 "$recipe_root" \
        "$recipe_root/one.sh" "$recipe_root/two.sh")
    [ "$id" != "$old_id" ]
    if dependency_metadata_valid "prefix_format=2
platform=test-platform
dependency_id=$id"; then
        exit 1
    fi

    metadata=$(dependency_metadata test-platform \
        aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa)
    if prepare_dependency_prefix relative-prefix "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix / "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/install/ "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/../escape "$metadata" 1; then
        exit 1
    fi
    if prepare_dependency_prefix /tmp/./install "$metadata" 1; then
        exit 1
    fi
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ -n "$CUP_DEPS_STAGE_ROOT" ]
    [ "$CUP_DEPS_BUILD_PREFIX" = "$CUP_DEPS_STAGE_ROOT$final" ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    [ -f "$final/old.txt" ]
    create_complete "$CUP_DEPS_BUILD_PREFIX" "$CUP_DEPS_BUILD_PREFIX"
    printf "new\n" >"$CUP_DEPS_BUILD_PREFIX/new.txt"

    cygpath() {
        case "$1" in
            -m) printf "D:/msys64%s\n" "$2" ;;
            -w)
                converted=$(printf "%s" "$2" | sed "s#/#\\\\#g")
                printf "D:\\msys64%s\n" "$converted"
                ;;
            *) return 1 ;;
        esac
    }
    staged_native=$(cygpath -m "$CUP_DEPS_BUILD_PREFIX")
    final_native=$(cygpath -m "$final")
    staged_windows=$(cygpath -w "$CUP_DEPS_BUILD_PREFIX")
    final_windows=$(cygpath -w "$final")
    mixed_native=$(printf "%s" "$staged_native" | sed "s#/#\\\\#3g")
    cat >"$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig/windows-paths.cmake" <<EOF_WINDOWS_PATHS
posix=$CUP_DEPS_BUILD_PREFIX
native=$staged_native
windows=$staged_windows
mixed=$mixed_native
EOF_WINDOWS_PATHS
    dependency_metadata_contains_staging "$mixed_native" ||
        fail "mixed-separator staging path was not detected"

    normalize_dependency_metadata "$CUP_DEPS_BUILD_PREFIX" \
        "$CUP_DEPS_BUILD_PREFIX" "$final"
    ! find "$CUP_DEPS_BUILD_PREFIX" -type f \
        \( -name '*.pc' -o -name '*.la' -o -name '*.cmake' \
           -o -name '*-config' -o -name 'curl-config' \) \
        -exec grep -F -l "$CUP_DEPS_STAGE_ROOT" {} + | grep .
    [ "$("$CUP_DEPS_BUILD_PREFIX/bin/curl-config")" = "-L$final/lib -lcurl" ]
    windows_metadata="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig/windows-paths.cmake"
    grep -F "posix=$final" "$windows_metadata" >/dev/null
    grep -F "native=$final_native" "$windows_metadata" >/dev/null
    grep -F "windows=$final_native" "$windows_metadata" >/dev/null
    grep -F "mixed=$final_native" "$windows_metadata" >/dev/null
    ! grep -F "$CUP_DEPS_BUILD_PREFIX" "$windows_metadata" >/dev/null
    ! grep -F "$staged_native" "$windows_metadata" >/dev/null
    ! grep -F "$staged_windows" "$windows_metadata" >/dev/null
    archive_flags=$(PKG_CONFIG_PATH="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig" \
        PKG_CONFIG_LIBDIR="$CUP_DEPS_BUILD_PREFIX/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="" \
        pkg-config --libs libarchive)
    [ "$archive_flags" = "-L$final/lib -larchive " ] || \
        [ "$archive_flags" = "-L$final/lib -larchive" ]
    finish_dependency_prefix "$CUP_DEPS_BUILD_PREFIX"
    [ -f "$final/new.txt" ]
    [ ! -e "$final/old.txt" ]
    [ ! -e "$final/.cup-deps-building" ]
    [ "$(cat "$final/.cup-deps-config")" = "$metadata" ]
    [ "$("$final/bin/curl-config")" = "-L$final/lib -lcurl" ]

    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 1 ]
    [ "$CUP_DEPS_BUILD_PREFIX" = "$final" ]

    cp "$final/bin/curl-config" "$final/bin/curl-config.valid"
    printf "#!/bin/sh\nexit 0\n" >"$final/bin/curl-config"
    chmod +x "$final/bin/curl-config"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/bin/curl-config.valid" "$final/bin/curl-config"

    cp "$final/lib/pkgconfig/libarchive.pc" \
        "$final/lib/pkgconfig/libarchive.pc.valid"
    printf "Libs.private: -lacl\n" >> \
        "$final/lib/pkgconfig/libarchive.pc"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    abort_dependency_prefix
    mv "$final/lib/pkgconfig/libarchive.pc.valid" \
        "$final/lib/pkgconfig/libarchive.pc"

    rm "$final/include/uthash.h"
    prepare_dependency_prefix "$final" "$metadata" 1
    [ "$CUP_DEPS_PREFIX_READY" = 0 ]
    [ "$CUP_DEPS_BUILD_PREFIX" != "$final" ]
    [ -f "$final/new.txt" ]
    abort_dependency_prefix
' sh "$DEPENDENCY_COMMON" "$TRANSACTION_PREFIX"

ZLIB_VERSION=0 ZLIB_URL=https://invalid.example/zlib.tar.gz bash -eu -c '
    . "$1"
    [ "$ZLIB_VERSION" = 1.3.2 ]
    case "$ZLIB_URL" in
        https://github.com/madler/zlib/*) ;;
        *) exit 1 ;;
    esac
' sh "$DEPENDENCY_COMMON"

FAILED_PREFIX="$TMP_ROOT/failed-prefix"
bash -eu -o pipefail -c '
    common=$1
    final=$2
    . "$common"

    metadata=$(dependency_metadata test-platform \
        bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb)
    prepare_dependency_prefix "$final" "$metadata" 1
    build=$CUP_DEPS_BUILD_PREFIX
    printf "partial\n" >"$build/partial.txt"
    if finish_dependency_prefix "$build"; then
        exit 1
    fi
    [ ! -e "$final" ]
    [ -d "$build" ]
    abort_dependency_prefix
    [ ! -e "$build" ]
    [ ! -e "$final" ]
' sh "$DEPENDENCY_COMMON" "$FAILED_PREFIX"

leftovers=$(find "$TMP_ROOT" -maxdepth 1 -name '.*.staging.*' -print)
[ -z "$leftovers" ] || fail "dependency staging directories were not cleaned: $leftovers"
printf 'Dependency-prefix transaction tests passed.\n'

printf '==> Testing failed dependency build cleanup and retry...\n'
FAILED_DEPENDENCY_ROOT="$TMP_ROOT/failed-dependency-build"
FAILED_DEPENDENCY_PREFIX="$FAILED_DEPENDENCY_ROOT/install"
FAKE_CURL_DIR="$TMP_ROOT/failing-curl"
mkdir -p "$FAKE_CURL_DIR"
cat >"$FAKE_CURL_DIR/curl" <<'EOF_CURL'
#!/bin/sh
exit 7
EOF_CURL
chmod +x "$FAKE_CURL_DIR/curl"

attempt=1
while [ "$attempt" -le 2 ]; do
    if (
        cd "$PROJECT_ROOT"
        PATH="$FAKE_CURL_DIR:$PATH" \
            DEPS_ROOT="$FAILED_DEPENDENCY_ROOT" \
            DEPS_PREFIX="$FAILED_DEPENDENCY_PREFIX" \
            PLATFORM=linux-x64 \
            bash ./scripts/dependencies/build-posix.sh
    ) >"$TMP_ROOT/failed-dependency-build-$attempt.out" 2>&1; then
        fail 'dependency build unexpectedly succeeded with a failing downloader'
    fi
    [ ! -e "$FAILED_DEPENDENCY_PREFIX" ] ||
        fail 'failed dependency build exposed a partial final prefix'
    leftovers=$(find "$FAILED_DEPENDENCY_ROOT" -maxdepth 1 \
        -name '.install.staging.*' -print 2>/dev/null || true)
    [ -z "$leftovers" ] ||
        fail "failed dependency build left staging directories: $leftovers"
    attempt=$((attempt + 1))
done
printf 'Failed dependency build cleanup and retry tests passed.\n'

printf '==> Testing target-based build configurations...\n'
PINNED_PREFIX="$TMP_ROOT/pinned-prefix"
create_complete_prefix "$PINNED_PREFIX" 1
resolved_prefix=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -s DEPS_PREFIX="$PINNED_PREFIX" \
        --eval='print-deps-prefix: ; @printf "%s\n" "$(DEPS_PREFIX)"' \
        print-deps-prefix
)
assert_equals "$resolved_prefix" "$PINNED_PREFIX"

normalized_prefix=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -s \
        DEPS_PREFIX="$PINNED_PREFIX/../$(basename "$PINNED_PREFIX")" \
        --eval='print-deps-prefix: ; @printf "%s\n" "$(DEPS_PREFIX)"' \
        print-deps-prefix
)
assert_equals "$normalized_prefix" "$PINNED_PREFIX"

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        BUILD_DIR="$TMP_ROOT/build output" help \
        >"$TMP_ROOT/build-dir-space.out" 2>&1; then
    fail 'BUILD_DIR containing whitespace was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/build-dir-space.out")" \
    'BUILD_DIR must not contain whitespace'

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        DEPS_PREFIX="$TMP_ROOT/dependency prefix" help \
        >"$TMP_ROOT/deps-prefix-space.out" 2>&1; then
    fail 'DEPS_PREFIX containing whitespace was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/deps-prefix-space.out")" \
    'DEPS_PREFIX must not contain whitespace'

development_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n DEPS_PREFIX="$PINNED_PREFIX" all
)
assert_contains "$development_command" "build/$NATIVE_BUILD_PLATFORM/development/bin/cup"
assert_contains "$development_command" "-I$PINNED_PREFIX/include"
assert_contains "$development_command" "-L$PINNED_PREFIX/lib"
assert_contains "$development_command" "$PINNED_PREFIX/lib/libargtable3.a"
assert_contains "$development_command" "$PINNED_PREFIX/lib/libcurl.a"
assert_contains "$development_command" "$PINNED_PREFIX/lib/libarchive.a"
assert_not_contains "$development_command" 'libcurl.so'
assert_not_contains "$development_command" 'libarchive.so'
assert_not_contains "$development_command" ' -static '

debug_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n DEPS_PREFIX="$PINNED_PREFIX" debug
)
assert_contains "$debug_command" "build/$NATIVE_BUILD_PLATFORM/debug/bin/cup"
assert_contains "$debug_command" '-fno-omit-frame-pointer'
assert_contains "$debug_command" "$PINNED_PREFIX/lib/libcurl.a"
assert_contains "$debug_command" "$PINNED_PREFIX/lib/libarchive.a"
assert_not_contains "$debug_command" ' -static '

coverage_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n DEPS_PREFIX="$PINNED_PREFIX" coverage
)
assert_contains "$coverage_command" "build/$NATIVE_BUILD_PLATFORM/coverage/bin/cup"
assert_contains "$coverage_command" '--coverage'
assert_contains "$coverage_command" "$PINNED_PREFIX/lib/libcurl.a"
assert_contains "$coverage_command" "$PINNED_PREFIX/lib/libarchive.a"
assert_not_contains "$coverage_command" ' -static '

sanitizer_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n DEPS_PREFIX="$PINNED_PREFIX" sanitizers
)
assert_contains "$sanitizer_command" "build/$NATIVE_BUILD_PLATFORM/sanitizers/bin/cup"
assert_contains "$sanitizer_command" '-fsanitize=address,undefined'
assert_contains "$sanitizer_command" "$PINNED_PREFIX/lib/libcurl.a"
assert_contains "$sanitizer_command" "$PINNED_PREFIX/lib/libarchive.a"
assert_not_contains "$sanitizer_command" ' -static '

windows_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n PLATFORM=windows-x64 \
        DEPS_PREFIX="$PINNED_PREFIX" all
)
assert_contains "$windows_command" 'build/windows-x64/development/generated/version.rc'
assert_contains "$windows_command" 'version-resource.o'
assert_contains "$windows_command" "$PINNED_PREFIX/lib/libcurl.a"
assert_contains "$windows_command" "$PINNED_PREFIX/lib/libarchive.a"
assert_contains "$windows_command" '-DCURL_STATICLIB'

release_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n DEPS_PREFIX="$PINNED_PREFIX" release
)
assert_contains "$release_command" "build/$NATIVE_BUILD_PLATFORM/release/bin/cup"
assert_contains "$release_command" "-I$PINNED_PREFIX/include"
assert_contains "$release_command" "-L$PINNED_PREFIX/lib"
assert_contains "$release_command" "$PINNED_PREFIX/lib/libargtable3.a"
assert_contains "$release_command" "$PINNED_PREFIX/lib/libcurl.a"
assert_contains "$release_command" "$PINNED_PREFIX/lib/libarchive.a"
assert_contains "$release_command" '-static'

for removed in \
        'LINK_MODE=dynamic' \
        'BUILD_MODE=development' \
        'DEBUG_SYMBOLS=1' \
        'COVERAGE=1' \
        'SANITIZERS=1' \
        'RELEASE_BUILD=1'; do
    if (
        cd "$PROJECT_ROOT"
        make --no-print-directory -n PLATFORM=linux-x64 $removed
    ) >"$TMP_ROOT/removed-selector.out" 2>&1; then
        fail "removed build selector was accepted: $removed"
    fi
    assert_contains "$(cat "$TMP_ROOT/removed-selector.out")" \
        'were removed; use target-based builds and EXTRA_* flags'
done
help_output=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -s help
)
assert_contains "$help_output" 'make debug'
assert_contains "$help_output" 'make coverage'
assert_contains "$help_output" 'make sanitizers'
assert_contains "$help_output" 'make release'
printf 'Target-based build configuration tests passed.\n'

printf '==> Testing dependency diagnostics...\n'
missing_prefix="$TMP_ROOT/missing-prefix"
if (
    cd "$PROJECT_ROOT"
    make --no-print-directory -s DEPS_PREFIX="$missing_prefix" all
) >"$TMP_ROOT/missing-prefix.out" 2>&1; then
    fail 'development build accepted an incomplete dependency prefix'
fi
assert_contains "$(cat "$TMP_ROOT/missing-prefix.out")" \
    "Run 'make PLATFORM=linux-x64 deps' first."

deps_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -B -n PLATFORM=linux-x64 deps
)
assert_contains "$deps_command" 'build-posix.sh'
assert_contains "$deps_command" "JOBS=''"
assert_contains "$deps_command" "$HOME/deps/linux-x64/install"
assert_not_contains "$deps_command" 'CUP_DEPS_SCOPE'
printf 'Dependency diagnostic tests passed.\n'

printf '==> Testing dependency entry points and platform rejection...\n'
DEPENDENCY_DIR="$PROJECT_ROOT/scripts/dependencies"
for library in sources.sh common.sh; do
    path="$DEPENDENCY_DIR/$library"
    [ -f "$path" ] || fail "dependency library is missing: $library"
    [ ! -x "$path" ] || fail "dependency library must not be executable: $library"
done
for executable in build-posix.sh build-windows.sh verify.sh; do
    path="$DEPENDENCY_DIR/$executable"
    [ -x "$path" ] || fail "dependency entry point is missing or not executable: $executable"
done
for retired in \
        bootstrap-common.sh bootstrap-posix-deps.sh bootstrap-linux-deps.sh \
        bootstrap-macos-deps.sh bootstrap-windows-deps.sh check-prefix.sh \
        source-windows-deps.sh; do
    if find "$PROJECT_ROOT/scripts" -type f -name "$retired" | grep -q .; then
        fail "retired dependency script remains: $retired"
    fi
done
if PLATFORM=macos-x64 MACOSX_DEPLOYMENT_TARGET=12.0 \
        bash "$DEPENDENCY_DIR/build-posix.sh" \
        >"$TMP_ROOT/macos-deps-floor.out" 2>&1; then
    fail 'macOS dependency builder accepted the wrong deployment target'
fi
assert_contains "$(cat "$TMP_ROOT/macos-deps-floor.out")" \
    'require MACOSX_DEPLOYMENT_TARGET=13.0'
if MSYSTEM=MINGW64 MINGW_PREFIX=/mingw64 \
        bash "$DEPENDENCY_DIR/build-windows.sh" \
        >"$TMP_ROOT/windows-deps-runtime.out" 2>&1; then
    fail 'Windows dependency builder accepted a non-UCRT64 shell'
fi
assert_contains "$(cat "$TMP_ROOT/windows-deps-runtime.out")" \
    'require an MSYS2 UCRT64 shell'
printf 'Dependency entry-point and platform rejection tests passed.\n'

printf '==> Testing dependency inventory, scopes and notices...\n'
DEPENDENCY_SOURCES="$DEPENDENCY_DIR/sources.sh"
DEPENDENCY_NOTICES="$DEPENDENCY_DIR/THIRD_PARTY_NOTICES.txt"
[ -f "$DEPENDENCY_NOTICES" ] || fail 'third-party notices file is missing'
packages=$(sh -eu -c '. "$1"; all_source_packages' sh "$DEPENDENCY_SOURCES")
expected_packages='zlib
xz
openssl
curl
libarchive
argtable3
uthash
unity
libevent'
[ "$packages" = "$expected_packages" ] ||
    fail 'canonical dependency inventory changed unexpectedly'
for package in zlib xz openssl curl libarchive argtable3 uthash; do
    scope=$(sh -eu -c '. "$1"; dependency_scope_for_package "$2"' \
        sh "$DEPENDENCY_SOURCES" "$package")
    [ "$scope" = runtime ] || fail "$package does not have runtime scope"
done
for package in unity libevent; do
    scope=$(sh -eu -c '. "$1"; dependency_scope_for_package "$2"' \
        sh "$DEPENDENCY_SOURCES" "$package")
    [ "$scope" = test ] || fail "$package does not have test scope"
done
[ "$(sh -eu -c '. "$1"; dependency_usage_for_package uthash' \
    sh "$DEPENDENCY_SOURCES")" = header-only ] ||
    fail 'uthash usage classification is incorrect'
[ "$(sh -eu -c '. "$1"; dependency_usage_for_package libevent' \
    sh "$DEPENDENCY_SOURCES")" = network-test-library ] ||
    fail 'libevent usage classification is incorrect'
if sh -eu -c '. "$1"; dependency_scope_for_package unknown' \
        sh "$DEPENDENCY_SOURCES" >/dev/null 2>&1; then
    fail 'dependency scope lookup accepted an unknown package'
fi
notices_content=$(cat "$DEPENDENCY_NOTICES")
assert_contains "$notices_content" 'CUP THIRD-PARTY NOTICES'
assert_contains "$notices_content" 'Scope: runtime'
assert_contains "$notices_content" 'Scope: test'
assert_contains "$notices_content" \
    'Usage: static libraries linked only into the test network helper'
printf 'Dependency inventory, scope and notice tests passed.\n'

printf '==> Testing normalized dependency build environment...\n'
bash -eu -o pipefail -c '
    common=$1
    . "$common"

    CFLAGS=ambient-cflags
    CPPFLAGS=ambient-cppflags
    LDFLAGS=ambient-ldflags
    LIBS=ambient-libs
    CPATH=/ambient/include
    LIBRARY_PATH=/ambient/lib
    PKG_CONFIG_PATH=/ambient/pkgconfig
    CONFIG_SITE=/ambient/config.site
    CCACHE=ambient-ccache
    MAKEFLAGS=ambient-makeflags
    export CFLAGS CPPFLAGS LDFLAGS LIBS CPATH LIBRARY_PATH
    export PKG_CONFIG_PATH CONFIG_SITE CCACHE MAKEFLAGS

    dependency_normalize_build_environment
    [ "$LC_ALL" = C ]
    [ "$LANG" = C ]
    [ "$TZ" = UTC ]
    [ "$(umask)" = 0022 ] || [ "$(umask)" = 022 ]
    for variable in CFLAGS CPPFLAGS LDFLAGS LIBS CPATH LIBRARY_PATH \
            PKG_CONFIG_PATH CONFIG_SITE CCACHE MAKEFLAGS; do
        if [[ -v $variable ]]; then
            exit 1
        fi
    done

    [ "$(dependency_resolve_jobs)" = 4 ]
    JOBS=7
    [ "$(dependency_resolve_jobs)" = 7 ]
    JOBS=0
    if dependency_resolve_jobs >/dev/null 2>&1; then
        exit 1
    fi
    JOBS=invalid
    if dependency_resolve_jobs >/dev/null 2>&1; then
        exit 1
    fi
' sh "$DEPENDENCY_COMMON"
assert_contains "$(cat "$DEPENDENCY_DIR/build-posix.sh")" \
    'JOBS="$(dependency_resolve_jobs)"'
assert_contains "$(cat "$DEPENDENCY_DIR/build-windows.sh")" \
    'JOBS="$(dependency_resolve_jobs)"'
assert_not_contains "$(cat "$DEPENDENCY_DIR/build-posix.sh")" '$(nproc)'
assert_not_contains "$(cat "$DEPENDENCY_DIR/build-posix.sh")" 'hw.ncpu'
assert_not_contains "$(cat "$DEPENDENCY_DIR/build-windows.sh")" '$(nproc)'
printf 'Normalized dependency build environment tests passed.\n'
