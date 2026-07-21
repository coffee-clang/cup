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
    "$prefix/include/openssl" "$prefix/lib/pkgconfig"
touch "$prefix/include/argtable3.h" "$prefix/include/uthash.h" \
    "$prefix/include/unity.h" "$prefix/include/unity_internals.h" \
    "$prefix/include/curl/curl.h" "$prefix/include/archive.h" \
    "$prefix/include/archive_entry.h" "$prefix/include/zlib.h" \
    "$prefix/include/lzma.h" "$prefix/include/openssl/ssl.h" \
    "$prefix/lib/libargtable3.a" "$prefix/lib/libunity.a" \
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
printf '%s\n' Linux >"$TMP_ROOT/host-system"
printf '%s\n' x86_64 >"$TMP_ROOT/host-machine"
printf '%s\n' x86_64-unknown-linux-gnu >"$TMP_ROOT/compiler-target"
printf '%s\n' 1.0 >"$TMP_ROOT/compiler-version"

cat >"$fake_bin/uname" <<EOF_UNAME
#!/bin/sh
case "\${1:-}" in
    -s) cat '$TMP_ROOT/host-system' ;;
    -m) cat '$TMP_ROOT/host-machine' ;;
    *) exit 2 ;;
esac
EOF_UNAME

cat >"$fake_bin/fakecc" <<EOF_CC
#!/bin/sh
case "\$*" in
    -dumpmachine) cat '$TMP_ROOT/compiler-target' ;;
    '-dumpfullversion -dumpversion'|-dumpversion) cat '$TMP_ROOT/compiler-version' ;;
    --version) printf 'fakecc %s\\n' "\$(cat '$TMP_ROOT/compiler-version')" ;;
    *) printf 'unexpected fake compiler arguments: %s\\n' "\$*" >&2; exit 9 ;;
esac
EOF_CC

cat >"$fake_bin/fakewindres" <<'EOF_WINDRES'
#!/bin/sh
case "${1:-}" in
    --version) printf '%s\n' 'fakewindres 1.0' ;;
    *) exit 0 ;;
esac
EOF_WINDRES
chmod +x "$fake_bin/uname" "$fake_bin/fakecc" "$fake_bin/fakewindres"

write_prefix_metadata() {
    PATH="$fake_bin:$PATH" bash -eu -c '
        common=$1
        project_root=$2
        prefix=$3
        . "$common"
        toolchain=$(dependency_posix_toolchain_identity gcc ar ranlib linux-x86_64 glibc-native)
        id=$(dependency_id linux-x64 "$toolchain" 1 "$project_root" \
            "$project_root/scripts/dependencies/sources.sh" \
            "$project_root/scripts/dependencies/common.sh" \
            "$project_root/scripts/dependencies/build-posix.sh")
        dependency_metadata linux-x64 "$id" >"$prefix/.cup-deps-config"
    ' sh "$PROJECT_ROOT/scripts/dependencies/common.sh" \
        "$PROJECT_ROOT" "$prefix"
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
assert_contains "$config_text" 'cflags=-Wall -Wextra -Werror -std=c11 -O0 -g3 -DCUP_EXTRA_C'
assert_contains "$config_text" '-DCUP_EXTRA_CPP'
assert_contains "$config_text" '-Wl,--build-id=none'
assert_contains "$config_text" "$prefix/lib/libcurl.a"
assert_contains "$config_text" "$prefix/lib/libarchive.a"
assert_not_contains "$config_text" 'libcurl.so'
assert_not_contains "$config_text" 'libarchive.so'
assert_contains "$config_text" '-lm'
assert_contains "$config_text" 'official_build=0'
assert_not_contains "$config_text" 'dependency_identity=missing'

# An identical invocation must preserve the stamp timestamp.
sleep 1
touch "$TMP_ROOT/reference"
run_config_make
[ "$TMP_ROOT/reference" -nt "$config" ] ||
    fail 'unchanged build configuration rewrote build-config.txt'

# The application compiler identity is independent from the canonical toolchain
# that built the dependency prefix. Changing CC invalidates objects without
# requiring a second dependency prefix for the Linux compiler matrix.
old_dependency=$(sed -n 's/^dependency_identity=//p' "$config")
printf '%s\n' 2.0 >"$TMP_ROOT/compiler-version"
sleep 1
run_config_make
[ "$config" -nt "$TMP_ROOT/reference" ] ||
    fail 'compiler identity change did not rewrite build-config.txt'
assert_contains "$(cat "$config")" 'compiler_version=fakecc 2.0'
new_dependency=$(sed -n 's/^dependency_identity=//p' "$config")
metadata_dependency=$(sed -n 's/^dependency_id=//p' "$prefix/.cup-deps-config")
[ "$old_dependency" = "$new_dependency" ] ||
    fail 'application compiler unexpectedly changed the dependency identity'
[ "$new_dependency" = "$metadata_dependency" ] ||
    fail 'build config did not record the canonical dependency_id'

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
MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 MSYSTEM_CARCH=x86_64 \
    PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
    windows-x64 fakecc fakewindres
if MSYSTEM=MINGW64 MINGW_PREFIX=/mingw64 MSYSTEM_CARCH=x86_64 \
        PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc fakewindres >"$TMP_ROOT/windows-runtime.out" 2>&1; then
    fail 'non-UCRT64 Windows toolchain was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/windows-runtime.out")" 'requires MSYSTEM=UCRT64'
if MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 MSYSTEM_CARCH=x86_64 \
        PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc missing-windres >"$TMP_ROOT/windres.out" 2>&1; then
    fail 'missing Windows resource compiler was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/windres.out")" \
    "resource compiler 'missing-windres' was not found"

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        PLATFORM=macos-x64 MACOSX_DEPLOYMENT_TARGET=12.0 help \
        >"$TMP_ROOT/macos-target.out" 2>&1; then
    fail 'unsupported macOS deployment target was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/macos-target.out")" \
    'macOS builds require MACOSX_DEPLOYMENT_TARGET=13.0'
make -C "$PROJECT_ROOT" --no-print-directory -pn PLATFORM=macos-x64 help \
    >"$TMP_ROOT/macos-make-db.out"
assert_contains "$(cat "$TMP_ROOT/macos-make-db.out")" \
    '-mmacosx-version-min=13.0'
make -C "$PROJECT_ROOT" --no-print-directory -pn PLATFORM=windows-x64 help \
    >"$TMP_ROOT/windows-make-db.out"
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" 'CC := gcc'
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" 'WINDRES := windres'
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" '-D_WIN32_WINNT=0x0A00'

printf '%s\n' 'Build-system contract tests passed.'
