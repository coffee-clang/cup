#!/bin/sh

# Purpose: Verifies mandatory flags, stable build identity generation and
# platform/toolchain rejection without compiling the production sources.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"

test_begin build-system

fake_bin=$TMP_ROOT/bin
prefix=$TMP_ROOT/prefix
build_root=$TMP_ROOT/build
mkdir -p "$fake_bin" "$prefix/bin" "$prefix/include/curl" \
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
[ "\${1:-}" = --static-libs ] || exit 2
printf '%s\n' '$prefix/lib/libcurl.a -L$prefix/lib -lssl -lcrypto -lz'
EOF_CURL_CONFIG
chmod +x "$prefix/bin/curl-config"
cat >"$prefix/lib/pkgconfig/libarchive.pc" <<EOF_ARCHIVE_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libarchive
Description: build-system fixture
Version: 1
Libs: \${libdir}/libarchive.a \${libdir}/liblzma.a \${libdir}/libz.a
Cflags: -I\${includedir}
EOF_ARCHIVE_PC
cat >"$prefix/lib/pkgconfig/libevent_core.pc" <<EOF_EVENT_CORE_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libevent_core
Description: build-system fixture
Version: 1
Libs: -L\${libdir} -levent_core
Cflags: -I\${includedir}
EOF_EVENT_CORE_PC
cat >"$prefix/lib/pkgconfig/libevent_extra.pc" <<EOF_EVENT_EXTRA_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: libevent_extra
Description: build-system fixture
Version: 1
Requires.private: libevent_core
Libs: -L\${libdir} -levent_extra
Cflags: -I\${includedir}
EOF_EVENT_EXTRA_PC
printf '%s\n' Linux >"$TMP_ROOT/host-system"
printf '%s\n' x86_64 >"$TMP_ROOT/host-machine"
printf '%s\n' x86_64-unknown-linux-gnu >"$TMP_ROOT/compiler-target"
printf '%s\n' 1.0 >"$TMP_ROOT/compiler-version"

cat >"$fake_bin/uname" <<EOF_UNAME
#!/bin/sh
case "\${1:-}" in
    -s)
        cat '$TMP_ROOT/host-system'
        ;;
    -m)
        cat '$TMP_ROOT/host-machine'
        ;;
    *)
        exit 2
        ;;
esac
EOF_UNAME

cat >"$fake_bin/fakecc" <<EOF_CC
#!/bin/sh
case "\$*" in
    -dumpmachine)
        cat '$TMP_ROOT/compiler-target'
        ;;
    '-dumpfullversion -dumpversion'|-dumpversion)
        cat '$TMP_ROOT/compiler-version'
        ;;
    --version)
        printf '%s %s\\n' "\${FAKE_COMPILER_NAME:-fakecc}" \
            "\$(cat '$TMP_ROOT/compiler-version')"
        ;;
    *)
        printf 'unexpected fake compiler arguments: %s\\n' "\$*" >&2
        exit 9
        ;;
esac
EOF_CC

cat >"$fake_bin/fakewindres" <<'EOF_WINDRES'
#!/bin/sh
case "${1:-}" in
    --version)
        printf '%s\n' 'fakewindres 1.0'
        ;;
    *)
        exit 0
        ;;
esac
EOF_WINDRES
cp "$fake_bin/fakewindres" "$fake_bin/llvm-windres"
chmod +x "$fake_bin/uname" "$fake_bin/fakecc" "$fake_bin/fakewindres" \
    "$fake_bin/llvm-windres"

write_prefix_metadata() {
    bash -eu -c '
        common=$1
        prefix=$2
        . "$common"
        dependency_metadata linux-x64 gcc >"$prefix/.cup-dependencies"
    ' sh "$PROJECT_ROOT/scripts/dependencies/common.sh" "$prefix"
}
write_prefix_metadata

config=$build_root/linux-x64/development/build-config.txt
run_config_make() {
    PATH="$fake_bin:$PATH" MAKEFLAGS= MAKEOVERRIDES= \
        make -C "$PROJECT_ROOT" --no-print-directory -s \
        PLATFORM=linux-x64 CUP_BUILD_CONFIGURATION=development \
        BUILD_DIR="$build_root" DEPS_PREFIX="$prefix" CC=fakecc \
        EXTRA_CPPFLAGS=-DCUP_EXTRA_CPP \
        EXTRA_CFLAGS=-DCUP_EXTRA_C \
        EXTRA_LDFLAGS=-Wl,--build-id=none \
        EXTRA_LDLIBS=-lm \
        "$config"
}

run_config_make
assert_file "$config"
config_text=$(cat "$config")
assert_contains "$config_text" 'format=1'
assert_contains "$config_text" 'platform=linux-x64'
assert_contains "$config_text" 'configuration=development'
assert_contains "$config_text" 'compiler_command=fakecc'
assert_contains "$config_text" 'compiler_target=x86_64-unknown-linux-gnu'
assert_contains "$config_text" 'compiler_version=fakecc 1.0'
assert_contains "$config_text" 'cflags=-Wall -Wextra -Werror -std=c11'
assert_contains "$config_text" '-fdebug-prefix-map='
assert_contains "$config_text" '-O0 -g3 -DCUP_EXTRA_C'
assert_contains "$config_text" '-DCUP_EXTRA_CPP'
assert_contains "$config_text" '-Wl,--build-id=none'
assert_contains "$config_text" "$prefix/lib/libcurl.a"
assert_contains "$config_text" "$prefix/lib/libarchive.a"
assert_not_contains "$config_text" 'libcurl.so'
assert_not_contains "$config_text" 'libarchive.so'
config_ldlibs=$(sed -n 's/^ldlibs=//p' "$config")
assert_not_contains "$config_ldlibs" '-levent'
assert_not_contains "$config_ldlibs" 'libevent_'
assert_contains "$config_text" '-lm'
assert_contains "$config_text" 'official_build=0'
assert_contains "$config_text" 'dependency_platform=linux-x64'
assert_contains "$config_text" 'dependency_profile=gcc'
assert_contains "$config_text" 'dependency_recipe=1'
dependency_lock=$(sed -n 's/^dependency_lock_sha256=//p' "$config")
case "$dependency_lock" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
    *) fail 'build config did not record the dependency lock SHA-256' ;;
esac

# An identical invocation must preserve the stamp timestamp.
sleep 1
touch "$TMP_ROOT/reference"
run_config_make
[ "$TMP_ROOT/reference" -nt "$config" ] ||
    fail 'unchanged build configuration rewrote build-config.txt'

# The application compiler identity is independent from the canonical toolchain
# that built the dependency prefix. Changing CC invalidates objects without
# requiring a second dependency prefix for the Linux compiler matrix.
old_dependency_metadata=$(grep '^dependency_' "$config")
printf '%s\n' 2.0 >"$TMP_ROOT/compiler-version"
sleep 1
run_config_make
[ "$config" -nt "$TMP_ROOT/reference" ] ||
    fail 'compiler identity change did not rewrite build-config.txt'
assert_contains "$(cat "$config")" 'compiler_version=fakecc 2.0'
new_dependency_metadata=$(grep '^dependency_' "$config")
[ "$old_dependency_metadata" = "$new_dependency_metadata" ] ||
    fail 'application compiler unexpectedly changed dependency compatibility metadata'
assert_contains "$new_dependency_metadata" "dependency_lock_sha256=$dependency_lock"

# Ambient flag variables are ignored; direct replacements are rejected.
env_config=$TMP_ROOT/environment-build/linux-x64/development/build-config.txt
CFLAGS=-DENV_REPLACEMENT PATH="$fake_bin:$PATH" MAKEFLAGS= MAKEOVERRIDES= \
    make -C "$PROJECT_ROOT" --no-print-directory -s \
    PLATFORM=linux-x64 CUP_BUILD_CONFIGURATION=development \
    BUILD_DIR="$TMP_ROOT/environment-build" DEPS_PREFIX="$prefix" CC=fakecc \
    "$env_config"
assert_not_contains "$(cat "$env_config")" '-DENV_REPLACEMENT'
assert_contains "$(cat "$env_config")" '-Wall -Wextra -Werror -std=c11'

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        CFLAGS=-DREPLACED all >"$TMP_ROOT/direct-flags.out" 2>&1; then
    fail 'direct CFLAGS replacement was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/direct-flags.out")" \
    'use EXTRA_CPPFLAGS, EXTRA_CFLAGS, EXTRA_LDFLAGS or EXTRA_LDLIBS'

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        CONFIGURATION=release all >"$TMP_ROOT/direct-configuration.out" 2>&1; then
    fail 'direct CONFIGURATION selector was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/direct-configuration.out")" \
    'CONFIGURATION is internal'

# Toolchain validation checks both native host and compiler target.
printf '%s\n' aarch64-unknown-linux-gnu >"$TMP_ROOT/compiler-target"
if PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        linux-x64 fakecc >"$TMP_ROOT/target-mismatch.out" 2>&1; then
    fail 'mismatched compiler target was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/target-mismatch.out")" \
    "does not match linux-x64"

printf '%s\n' x86_64-unknown-linux-gnu >"$TMP_ROOT/compiler-target"
printf '%s\n' aarch64 >"$TMP_ROOT/host-machine"
if PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        linux-x64 fakecc >"$TMP_ROOT/host-mismatch.out" 2>&1; then
    fail 'mismatched native host was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/host-mismatch.out")" \
    'requires a native Linux x64 host'

printf '%s\n' x86_64 >"$TMP_ROOT/host-machine"
printf '%s\n' MINGW64_NT-10.0 >"$TMP_ROOT/host-system"
printf '%s\n' x86_64-w64-mingw32 >"$TMP_ROOT/compiler-target"
FAKE_COMPILER_NAME=gcc MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 MSYSTEM_CARCH=x86_64 \
    PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
    windows-x64 fakecc fakewindres development
printf '%s\n' x86_64-w64-windows-gnu >"$TMP_ROOT/compiler-target"
FAKE_COMPILER_NAME=gcc MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 MSYSTEM_CARCH=x86_64 \
    PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
    windows-x64 fakecc fakewindres development
printf '%s\n' x86_64-w64-mingw32 >"$TMP_ROOT/compiler-target"
if FAKE_COMPILER_NAME=gcc MSYSTEM=MINGW64 MINGW_PREFIX=/mingw64 MSYSTEM_CARCH=x86_64 \
        PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc fakewindres development >"$TMP_ROOT/windows-runtime.out" 2>&1; then
    fail 'non-UCRT64 Windows toolchain was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/windows-runtime.out")" 'development builds require MSYSTEM=UCRT64'
if FAKE_COMPILER_NAME=gcc MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 MSYSTEM_CARCH=x86_64 \
        PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc missing-windres development >"$TMP_ROOT/windres.out" 2>&1; then
    fail 'missing Windows resource compiler was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/windres.out")" \
    "resource compiler 'missing-windres' was not found"

FAKE_COMPILER_NAME=clang MSYSTEM=CLANG64 MINGW_PREFIX=/clang64 \
    MSYSTEM_CARCH=x86_64 PATH="$fake_bin:$PATH" \
    "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
    windows-x64 fakecc llvm-windres sanitizers
if FAKE_COMPILER_NAME=clang MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 \
        MSYSTEM_CARCH=x86_64 PATH="$fake_bin:$PATH" \
        "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc fakewindres sanitizers \
        >"$TMP_ROOT/windows-sanitizer-runtime.out" 2>&1; then
    fail 'Windows sanitizer toolchain accepted UCRT64'
fi
assert_contains "$(cat "$TMP_ROOT/windows-sanitizer-runtime.out")" \
    'sanitizers require MSYSTEM=CLANG64'

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        PLATFORM=macos-x64 MACOSX_DEPLOYMENT_TARGET=12.0 help \
        >"$TMP_ROOT/macos-target.out" 2>&1; then
    fail 'unsupported macOS deployment target was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/macos-target.out")" \
    'macOS builds require MACOSX_DEPLOYMENT_TARGET=13.0'
MAKEFLAGS= MAKEOVERRIDES= make -C "$PROJECT_ROOT" --no-print-directory -pn \
    PLATFORM=macos-x64 help >"$TMP_ROOT/macos-make-db.out"
assert_contains "$(cat "$TMP_ROOT/macos-make-db.out")" \
    '-mmacosx-version-min=13.0'
MAKEFLAGS= MAKEOVERRIDES= make -C "$PROJECT_ROOT" --no-print-directory -pn \
    PLATFORM=windows-x64 help >"$TMP_ROOT/windows-make-db.out"
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" 'CC := gcc'
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" 'WINDRES := windres'
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" '-D_WIN32_WINNT=0x0A00'

# The help output is the public index of supported Make entry points. Keep it
# complete so contributors do not need to inspect recipes to discover commands.
help_output=$(MAKEFLAGS= MAKEOVERRIDES= \
    make -C "$PROJECT_ROOT" --no-print-directory -s help)
for target in \
    debug coverage sanitizers release clean help deps deps-check deps-force \
    deps-clean check-toolchain check-binary test test-unit test-integration \
    quality check test-coverage test-sanitizers test-portability-linux \
    test-windows test-release test-unit-build test-helpers test-build version \
    validate-release release-metadata finalize-release check-ca-bundle \
    update-ca-bundle docs-assets docs serve reset-dev-home; do
    assert_contains "$help_output" "make $target"
done
assert_not_contains "$help_output" 'make _build'
assert_contains "$help_output" 'Supported platforms:'

# Unit suites that are compiled natively on Windows must use the test-only
# compatibility layer for temporary directories and POSIX-style mkdir calls.
for source in \
    test_package_metadata.c \
    test_checksum.c \
    test_package_catalog.c \
    test_install_policy.c \
    test_cup_update.c \
    test_command_uninstall.c; do
    source_path="$PROJECT_ROOT/tests/unit/$source"
    assert_contains "$(cat "$source_path")" '#include "test_platform.h"'
    assert_contains "$(cat "$source_path")" 'test_make_temp_directory('
    if grep -Eq '(^|[^[:alnum:]_])mkdir\([^,]+,[[:space:]]*0[0-7]+' "$source_path"; then
        fail "Windows-owned unit suite calls POSIX mkdir directly: $source"
    fi
    if grep -Fq '"/tmp/' "$source_path"; then
        fail "Windows-owned unit suite hardcodes /tmp: $source"
    fi
done
platform_header=$(cat "$PROJECT_ROOT/tests/unit/test_platform.h")
assert_contains "$platform_header" '_mkdir('
assert_contains "$platform_header" '_mktemp_s('
assert_contains "$platform_header" 'RUNNER_TEMP'

# Test runners must receive the same explicit platform, dependency prefix and
# compiler used for the binaries they execute. This also keeps custom prefixes
# working through nested make and shell entry points.
makefile_text=$(cat "$PROJECT_ROOT/Makefile")
assert_contains "$makefile_text" \
    "CUP_TEST_PLATFORM='\$(PLATFORM)' DEPS_PREFIX='\$(DEPS_PREFIX)'"
assert_contains "$makefile_text" \
    "CC='\$(CC)' CUP_TEST_SKIP_BUILD=1"
assert_contains "$makefile_text" \
    "CUP_TEST_CONFIGURATION='\$(CUP_TEST_CONFIGURATION)'"

printf '%s\n' 'Build-system contract tests passed.'
