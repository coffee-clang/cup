#!/bin/sh

# Verifies Make target wiring, mandatory flags, stable build identity
# generation and platform/toolchain rejection without compiling production sources.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"
. "$TESTS_ROOT/support/environment.sh"

test_begin build-system

case "$(uname -s 2>/dev/null || true)" in
    MSYS*|MINGW*|CYGWIN*)
        printf '%s\n' \
            'Build-system repository contracts are POSIX-hosted; native Windows build tooling is covered by path-safety and source tests.'
        exit 0
        ;;
esac

# Make command-line variables from an outer repository gate must not alter
# the isolated dry runs below. Every scenario supplies the inputs it owns.
unset MAKEFLAGS MAKEOVERRIDES BUILD_DIR DEPS_ROOT DEPS_PREFIX \
    CUP_DEPENDENCY_PROFILE CUP_TEST_PLATFORM PLATFORM CC WINDRES \
    MACOSX_DEPLOYMENT_TARGET OS MSYSTEM MINGW_PREFIX \
    PROCESSOR_ARCHITECTURE PROCESSOR_ARCHITEW6432

# A recursive make's long --no-print-directory option is not the short -n flag.
# The build lock must remain active for that normal recursive invocation, while
# a genuine dry run still bypasses execution of the native lock helper.
normal_lock_prefix=$(
    cd "$PROJECT_ROOT"
    make -p -s PLATFORM=linux-x64 MAKEFLAGS=--no-print-directory help 2>/dev/null |
        sed -n 's/^BUILD_LOCK_PREFIX = //p' | sed -n '1p'
)
assert_contains "$normal_lock_prefix" 'cup_path_run_build'
dry_lock_prefix=$(
    cd "$PROJECT_ROOT"
    make -p -s PLATFORM=linux-x64 MAKEFLAGS=n help 2>/dev/null |
        sed -n 's/^BUILD_LOCK_PREFIX = //p' | sed -n '1p'
)
[ -z "$dry_lock_prefix" ] || fail 'GNU make -n did not disable the build lock command'

NATIVE_BUILD_PLATFORM=$(cup_test_detect_platform) ||
    fail 'could not resolve native build platform for build-system tests'

# Exercise GCC's runtime profile relocation with the same lifecycle used by
# transactionally built test binaries: compile in staging, publish, remove the
# staging pathname, execute, then consume paired final-owner notes/counters.
gcov_probe_cc=$(command -v gcc || true)
gcov_probe_tool=$(command -v gcov || true)
if [ -n "$gcov_probe_cc" ] && [ -n "$gcov_probe_tool" ]; then
    gcov_probe_identity=$($gcov_probe_cc --version 2>/dev/null | sed -n '1p')
    case "$gcov_probe_identity" in
        *GCC*|*gcc*)
            . "$PROJECT_ROOT/tests/support/posix/coverage.sh"
            gcov_probe_root=$TMP_ROOT/gcov-runtime-relocation
            gcov_probe_source=$gcov_probe_root/probe.c
            mkdir -p "$gcov_probe_root/tests"
            cat >"$gcov_probe_source" <<'EOF_GCOV_PROBE'
int probe_value(int value) { return value ? 7 : 3; }
int main(void) { return probe_value(1) == 7 ? 0 : 1; }
EOF_GCOV_PROBE

            run_gcov_relocation_probe() {
                family=$1
                stage=$gcov_probe_root/tests/.$family.semantic
                final=$gcov_probe_root/tests/$family
                mkdir -p "$stage"
                "$gcov_probe_cc" -O0 -g --coverage -fprofile-abs-path \
                    "$gcov_probe_source" -o "$stage/probe"
                mv "$stage" "$final"
                [ ! -e "$stage" ] ||
                    fail "GCC $family relocation probe retained its staging pathname"
                prefix=$final
                case "$NATIVE_BUILD_PLATFORM" in
                    windows-x64)
                        command -v cygpath >/dev/null 2>&1 ||
                            fail 'Windows GCC relocation probe requires cygpath'
                        prefix=$(cygpath -m "$final")
                        ;;
                esac
                strip=$(cup_coverage_gcov_strip_components "$prefix") ||
                    fail "could not derive GCC $family relocation strip count"
                binary=$(find "$final" -maxdepth 1 -type f \
                    \( -name probe -o -name 'probe.exe' \) -print -quit)
                [ -n "$binary" ] || fail "GCC $family relocation probe binary is missing"
                env GCOV_PREFIX="$prefix" GCOV_PREFIX_STRIP="$strip" "$binary" ||
                    fail "GCC $family relocation probe binary failed"
                data=$(find "$final" -maxdepth 1 -type f -name '*.gcda' -print -quit)
                [ -n "$data" ] ||
                    fail "GCC runtime relocation did not write a $family final-owner counter"
                note=${data%.gcda}.gcno
                [ -f "$note" ] ||
                    fail "GCC $family counter is not paired with its final-owner note"
                "$gcov_probe_tool" -n -o "$note" "$gcov_probe_source" \
                    >"$TMP_ROOT/gcov-runtime-$family.log" 2>&1 ||
                    fail "GCC $family relocated note/counter pair is not consumable"
            }

            run_gcov_relocation_probe unit
            run_gcov_relocation_probe helpers

            gcov_product_root=$gcov_probe_root/product
            mkdir -p "$gcov_product_root/obj" "$gcov_product_root/bin"
            "$gcov_probe_cc" -O0 -g --coverage -fprofile-abs-path -c \
                "$gcov_probe_source" -o "$gcov_product_root/obj/probe.o"
            "$gcov_probe_cc" --coverage "$gcov_product_root/obj/probe.o" \
                -o "$gcov_product_root/bin/probe"
            gcov_product_binary=$(find "$gcov_product_root/bin" -maxdepth 1 \
                -type f \( -name probe -o -name 'probe.exe' \) -print -quit)
            [ -n "$gcov_product_binary" ] || fail 'GCC product probe binary is missing'
            (unset GCOV_PREFIX GCOV_PREFIX_STRIP; "$gcov_product_binary") ||
                fail 'GCC product coverage probe binary failed'
            gcov_product_data=$(find "$gcov_product_root/obj" -maxdepth 1 \
                -type f -name '*.gcda' -print -quit)
            [ -n "$gcov_product_data" ] ||
                fail 'GCC product counter did not stay with its final object owner'
            gcov_product_note=${gcov_product_data%.gcda}.gcno
            [ -f "$gcov_product_note" ] ||
                fail 'GCC product counter is not paired with its final object note'
            "$gcov_probe_tool" -n -o "$gcov_product_note" "$gcov_probe_source" \
                >"$TMP_ROOT/gcov-runtime-product.log" 2>&1 ||
                fail 'GCC product final-owner note/counter pair is not consumable'
            printf 'GCC runtime relocation semantic probes passed.\n'
            ;;
    esac
fi

fake_bin=$TMP_ROOT/bin
prefix=$TMP_ROOT/prefix
build_root=$TMP_ROOT/build
mkdir -p "$fake_bin" "$prefix/bin" "$prefix/include/curl" \
    "$prefix/include/openssl" "$prefix/include/event2" \
    "$prefix/lib/pkgconfig"
for header in \
    argtable3.h uthash.h ares.h unity.h unity_internals.h \
    event2/event.h event2/http.h event2/bufferevent.h event2/listener.h \
    curl/curl.h archive.h archive_entry.h zlib.h lzma.h openssl/ssl.h; do
    printf '/* build-system dependency fixture */\n' > "$prefix/include/$header"
done
for archive in \
    libargtable3.a libcares.a libunity.a libevent_core.a libevent_extra.a \
    libcurl.a libarchive.a libz.a liblzma.a libssl.a libcrypto.a; do
    ar rcs "$prefix/lib/$archive"
done
cat >"$prefix/bin/curl-config" <<EOF_CURL_CONFIG
#!/bin/sh
case "\${1:-}" in
    --static-libs)
        printf '%s\n' '$prefix/lib/libcurl.a -L$prefix/lib -lcares -lssl -lcrypto -lz'
        ;;
    --features)
        printf '%s\n' AsynchDNS
        ;;
    --configure)
        printf "%s\n" " '--prefix=$prefix' '--enable-ares=$prefix'"
        ;;
    *) exit 2 ;;
esac
EOF_CURL_CONFIG
chmod +x "$prefix/bin/curl-config"
cat >"$prefix/lib/pkgconfig/libcares.pc" <<EOF_CARES_PC
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: c-ares
Description: build-system fixture
Version: 1
Libs: -L\${libdir} -lcares
Cflags: -I\${includedir}
EOF_CARES_PC
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
        printf '%s %s\\n' "\${FAKE_COMPILER_NAME:-gcc}" \
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
        case "${0##*/}" in
            llvm-windres)
                printf '%s\n' \
                    'llvm-windres, compatible with GNU windres' \
                    'LLVM version 22.1.8'
                ;;
            *)
                printf '%s\n' 'fakewindres 1.0'
                ;;
        esac
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
assert_contains "$config_text" 'format=3'
assert_contains "$config_text" 'platform=linux-x64'
assert_contains "$config_text" 'configuration=development'
assert_contains "$config_text" 'compiler_command=fakecc'
assert_contains "$config_text" 'compiler_target=x86_64-unknown-linux-gnu'
assert_contains "$config_text" 'compiler_target_normalized=linux-x64'
assert_contains "$config_text" 'compiler_version=gcc 1.0'
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
assert_contains "$config_text" 'dependency_prefix_format=5'
assert_contains "$config_text" 'dependency_build_revision=4'
dependency_lock=$(sed -n 's/^dependency_source_lock_sha256=//p' "$config")
dependency_toolchain=$(sed -n 's/^dependency_toolchain_sha256=//p' "$config")
case "$dependency_lock:$dependency_toolchain" in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f]*:[0-9a-f][0-9a-f][0-9a-f][0-9a-f]*) ;;
    *) fail 'build config did not record dependency source and toolchain SHA-256 values' ;;
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
assert_contains "$(cat "$config")" 'compiler_version=gcc 2.0'
new_dependency_metadata=$(grep '^dependency_' "$config")
[ "$old_dependency_metadata" = "$new_dependency_metadata" ] ||
    fail 'application compiler unexpectedly changed dependency compatibility metadata'
assert_contains "$new_dependency_metadata" "dependency_source_lock_sha256=$dependency_lock"
assert_contains "$new_dependency_metadata" "dependency_toolchain_sha256=$dependency_toolchain"

# Semantic dependency graph transitions must invalidate the same build root, not only clean builds.
# Use the real project Makefile with deterministic local stubs so this repository contract exercises
# Make's timestamp graph without requiring native dependency compilation.
graph_project=$TMP_ROOT/semantic-graph-project
mkdir "$graph_project"
for graph_entry in Makefile VERSION certs config include scripts src; do
    cp -R "$PROJECT_ROOT/$graph_entry" "$graph_project/"
done

cat > "$graph_project/scripts/dependencies/verify.sh" <<'EOF_GRAPH_VERIFY'
#!/bin/sh
exit 0
EOF_GRAPH_VERIFY
cat > "$graph_project/scripts/build/validate-toolchain.sh" <<'EOF_GRAPH_TOOLCHAIN'
#!/bin/sh
exit 0
EOF_GRAPH_TOOLCHAIN
cat > "$graph_project/scripts/build/write-config.sh" <<'EOF_GRAPH_CONFIG'
#!/bin/sh
set -eu
out=$1
mkdir -p "$(dirname -- "$out")"
tmp=$out.tmp
printf '%s\n' 'format=graph-test' > "$tmp"
if [ -f "$out" ] && cmp -s "$tmp" "$out"; then
    rm -f "$tmp"
else
    mv "$tmp" "$out"
fi
EOF_GRAPH_CONFIG
cat > "$graph_project/scripts/version.sh" <<'EOF_GRAPH_VERSION'
#!/bin/sh
set -eu
[ "$1" = generate ] && [ "$#" -eq 2 ]
out=$2
version=$(tr -d '\n' < VERSION)
mkdir -p "$out"
write_if_different() {
    destination=$1
    temporary=$destination.tmp
    cat > "$temporary"
    if [ -f "$destination" ] && cmp -s "$temporary" "$destination"; then
        rm -f "$temporary"
    else
        mv "$temporary" "$destination"
    fi
}
cat <<EOF_VERSION_HEADER | write_if_different "$out/version.h"
#ifndef CUP_VERSION_H
#define CUP_VERSION_H
#define CUP_VERSION "$version-dev+graph"
#define CUP_VERSION_BASE "$version"
#define CUP_VERSION_COMMIT "graph"
#define CUP_VERSION_OFFICIAL 0
#define CUP_VERSION_MAJOR 0
#define CUP_VERSION_MINOR 0
#define CUP_VERSION_PATCH 0
#endif
EOF_VERSION_HEADER
printf '/* graph resource %s */\n' "$version" | write_if_different "$out/version.rc"
printf 'format=1\nversion=%s\ncommit=graph\n' "$version" | write_if_different "$out/release.txt"
EOF_GRAPH_VERSION
cat > "$graph_project/scripts/certs/generate-ca-bundle.sh" <<'EOF_GRAPH_CA'
#!/bin/sh
set -eu
[ "$#" -eq 2 ]
out=$2
marker=one
mkdir -p "$out"
write_if_different() {
    destination=$1
    temporary=$destination.tmp
    cat > "$temporary"
    if [ -f "$destination" ] && cmp -s "$temporary" "$destination"; then
        rm -f "$temporary"
    else
        mv "$temporary" "$destination"
    fi
}
cat <<EOF_CA_HEADER | write_if_different "$out/ca_bundle.h"
#ifndef CUP_CA_BUNDLE_H
#define CUP_CA_BUNDLE_H
#define CUP_CA_GRAPH_MARKER "$marker"
extern const unsigned char cup_ca_bundle[];
extern const size_t cup_ca_bundle_len;
#endif
EOF_CA_HEADER
cat <<'EOF_CA_SOURCE' | write_if_different "$out/ca_bundle.c"
#include <stddef.h>
#include "ca_bundle.h"
const unsigned char cup_ca_bundle[] = {0};
const size_t cup_ca_bundle_len = 0;
EOF_CA_SOURCE
EOF_GRAPH_CA
chmod +x "$graph_project/scripts/dependencies/verify.sh" \
    "$graph_project/scripts/build/validate-toolchain.sh" \
    "$graph_project/scripts/build/write-config.sh" \
    "$graph_project/scripts/version.sh" \
    "$graph_project/scripts/certs/generate-ca-bundle.sh"

graph_cc=$TMP_ROOT/graphcc
graph_trace=$TMP_ROOT/semantic-graph.trace
cat > "$graph_cc" <<'EOF_GRAPH_CC'
#!/bin/sh
set -eu
case "$*" in
    *-dumpmachine*) printf '%s\n' x86_64-unknown-linux-gnu; exit 0 ;;
    *'-dumpfullversion -dumpversion'*|*-dumpversion*) printf '%s\n' 1.0; exit 0 ;;
    *--version*) printf '%s\n' 'gcc graph-test 1.0'; exit 0 ;;
esac
output=
compile=0
source=
previous=
for argument in "$@"; do
    if [ "$previous" = -o ]; then
        output=$argument
        previous=
        continue
    fi
    if [ "$argument" = -o ]; then
        previous=-o
        continue
    fi
    if [ "$argument" = -c ]; then
        compile=1
        continue
    fi
    case "$argument" in *.c) source=$argument ;; esac
done
[ -n "$output" ] || exit 2
mkdir -p "$(dirname -- "$output")"
if [ "$compile" -eq 1 ]; then
    printf 'COMPILE %s -> %s\n' "$source" "$output" >> "${CUP_GRAPH_TRACE:?}"
    printf 'object %s\n' "$source" > "$output"
else
    printf 'LINK %s\n' "$*" >> "${CUP_GRAPH_TRACE:?}"
    printf '#!/bin/sh\nexit 0\n' > "$output"
    chmod +x "$output"
fi
EOF_GRAPH_CC
chmod +x "$graph_cc"

graph_build=$TMP_ROOT/semantic-graph-build
graph_make() {
    CUP_GRAPH_TRACE="$graph_trace" PATH="$fake_bin:$PATH" \
        make -C "$graph_project" --no-print-directory -s \
        PLATFORM=linux-x64 CUP_BUILD_CONFIGURATION=development \
        CUP_INTERNAL_DEPS_TARGET=deps-check BUILD_DIR="$graph_build" \
        DEPS_ROOT="$TMP_ROOT/semantic-deps-root" DEPS_PREFIX="$prefix" \
        CC="$graph_cc" all
}

: > "$graph_trace"
graph_make
assert_contains "$(cat "$graph_trace")" 'COMPILE '
assert_contains "$(cat "$graph_trace")" 'LINK '

# Forced generation stamps use write-if-different outputs: an unchanged invocation must not rebuild.
: > "$graph_trace"
graph_make
[ ! -s "$graph_trace" ] || fail 'unchanged semantic graph rebuilt production objects or binary'

# VERSION is a semantic input to generated version.h; all consumers must be invalidated in-place.
graph_current_version=$(tr -d '\r\n' < "$graph_project/VERSION")
graph_major=${graph_current_version%%.*}
graph_rest=${graph_current_version#*.}
graph_minor=${graph_rest%%.*}
graph_patch=${graph_rest#*.}
if [ "$graph_patch" -lt 999999 ]; then
    graph_patch=$((graph_patch + 1))
else
    graph_patch=0
fi
graph_version="$graph_major.$graph_minor.$graph_patch"
sleep 1
printf '%s\n' "$graph_version" > "$graph_project/VERSION"
: > "$graph_trace"
graph_make
assert_contains "$(cat "$graph_trace")" 'COMPILE '
assert_contains "$(cat "$graph_trace")" 'LINK '
assert_contains "$(cat "$graph_build/linux-x64/development/generated/version.h")" "$graph_version-dev+graph"

# A generator change that changes ca_bundle.h must invalidate compiled consumers in the same root.
sleep 1
sed -i 's/marker=one/marker=two/' "$graph_project/scripts/certs/generate-ca-bundle.sh"
: > "$graph_trace"
graph_make
assert_contains "$(cat "$graph_trace")" 'COMPILE '
assert_contains "$(cat "$graph_trace")" 'LINK '
assert_contains "$(cat "$graph_build/linux-x64/development/generated/ca_bundle.h")" 'CUP_CA_GRAPH_MARKER "two"'

# Deleting a checked-in header changes the header-directory semantic input and must not leave stale objects.
sleep 1
rm "$graph_project/include/registry.h"
: > "$graph_trace"
graph_make
assert_contains "$(cat "$graph_trace")" 'COMPILE '
assert_contains "$(cat "$graph_trace")" 'LINK '

# Changing the Makefile/source graph itself must relink even when surviving source bytes are unchanged.
sleep 1
sed -i '/^[[:space:]]*src\/registry.c \\$/d' "$graph_project/Makefile"
: > "$graph_trace"
graph_make
assert_contains "$(cat "$graph_trace")" 'LINK '
assert_not_contains "$(grep '^LINK ' "$graph_trace")" '/registry.o'

# Metadata values are single-line and dependency fields are unique; empty
# duplicate fields must not be ignored.
carriage_platform=$(printf 'linux-x64\rforged')
if PATH="$fake_bin:$PATH" \
        CUP_BUILD_PLATFORM="$carriage_platform" \
        CUP_BUILD_CONFIGURATION=development \
        CUP_BUILD_CC=fakecc \
        CUP_BUILD_DEPS_PREFIX="$prefix" \
        CUP_BUILD_OFFICIAL=0 \
        "$PROJECT_ROOT/scripts/build/write-config.sh" \
        "$TMP_ROOT/carriage-build-config.txt" \
        >"$TMP_ROOT/carriage-build-config.out" 2>&1; then
    fail 'build configuration accepted a carriage return in metadata'
fi
assert_contains "$(cat "$TMP_ROOT/carriage-build-config.out")" \
    'platform must be a single-line value'

duplicate_prefix=$TMP_ROOT/duplicate-prefix
cp -R "$prefix" "$duplicate_prefix"
printf 'platform=\n' >>"$duplicate_prefix/.cup-dependencies"
if PATH="$fake_bin:$PATH" \
        CUP_BUILD_PLATFORM=linux-x64 \
        CUP_BUILD_CONFIGURATION=development \
        CUP_BUILD_CC=fakecc \
        CUP_BUILD_DEPS_PREFIX="$duplicate_prefix" \
        CUP_BUILD_OFFICIAL=0 \
        "$PROJECT_ROOT/scripts/build/write-config.sh" \
        "$TMP_ROOT/duplicate-build-config.txt" \
        >"$TMP_ROOT/duplicate-build-config.out" 2>&1; then
    fail 'build configuration accepted a duplicate empty dependency field'
fi
assert_contains "$(cat "$TMP_ROOT/duplicate-build-config.out")" \
    'exactly one non-empty platform field'

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
    'requires native Linux x64'

printf '%s\n' x86_64 >"$TMP_ROOT/host-machine"
printf '%s\n' MINGW64_NT-10.0 >"$TMP_ROOT/host-system"
printf '%s\n' x86_64-w64-mingw32 >"$TMP_ROOT/compiler-target"
if FAKE_COMPILER_NAME=gcc MSYSTEM=MINGW64 MINGW_PREFIX=/mingw64 MSYSTEM_CARCH=x86_64 \
        PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc fakewindres development >"$TMP_ROOT/windows-runtime.out" 2>&1; then
    fail 'non-UCRT64 Windows toolchain was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/windows-runtime.out")" 'development requires MSYSTEM=UCRT64'

if (
    unset MINGW_PREFIX
    FAKE_COMPILER_NAME=gcc MSYSTEM=UCRT64 MSYSTEM_CARCH=x86_64 \
        PATH="$fake_bin:$PATH" "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc fakewindres development >"$TMP_ROOT/windows-path.out" 2>&1
); then
    fail 'Windows toolchain accepted a compiler outside its selected MSYS2 prefix'
fi
assert_contains "$(cat "$TMP_ROOT/windows-path.out")" \
    'compiler is outside the selected MSYS2 toolchain'
assert_not_contains "$(cat "$TMP_ROOT/windows-path.out")" 'MINGW_PREFIX'

if FAKE_COMPILER_NAME=clang MSYSTEM=UCRT64 MINGW_PREFIX=/ucrt64 \
        MSYSTEM_CARCH=x86_64 PATH="$fake_bin:$PATH" \
        "$PROJECT_ROOT/scripts/build/validate-toolchain.sh" \
        windows-x64 fakecc fakewindres sanitizers \
        >"$TMP_ROOT/windows-sanitizer-runtime.out" 2>&1; then
    fail 'Windows sanitizer toolchain accepted UCRT64'
fi
assert_contains "$(cat "$TMP_ROOT/windows-sanitizer-runtime.out")" \
    'sanitizers require MSYSTEM=CLANG64'

# A resource compiler may place a compatibility banner before its version.
# Build evidence keeps the banner but extracts the complete numeric version.
# Keep path-ops on this repository-test host; CUP_BUILD_PLATFORM only controls
# the build-config semantics exercised below.
printf '%s\n' Linux >"$TMP_ROOT/host-system"
printf '%s\n' x86_64 >"$TMP_ROOT/host-machine"
windres_config=$TMP_ROOT/windows-windres-config.txt
PATH="$fake_bin:$PATH" \
CUP_BUILD_PLATFORM=windows-x64 CUP_BUILD_CONFIGURATION=sanitizers \
CUP_BUILD_CC=fakecc CUP_BUILD_WINDRES=llvm-windres \
CUP_BUILD_DEPS_PREFIX="$prefix" CUP_BUILD_OFFICIAL=0 \
    "$PROJECT_ROOT/scripts/build/write-config.sh" "$windres_config"
assert_contains "$(cat "$windres_config")" \
    'windres_version=llvm-windres, compatible with GNU windres'
assert_contains "$(cat "$windres_config")" 'windres_numeric=22.1.8'

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
assert_contains "$(cat "$TMP_ROOT/macos-make-db.out")" \
    '-Wl,-no_warn_duplicate_libraries'
# Native coverage jobs own compiler-specific entry-point compatibility. The repository
# contract checks only public build modes and leaves symbols, macros and helper ownership private.
MAKEFLAGS= MAKEOVERRIDES= make -C "$PROJECT_ROOT" --no-print-directory -pn \
    PLATFORM=windows-x64 help >"$TMP_ROOT/windows-make-db.out"
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" 'CC := gcc'
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" 'WINDRES := windres'
assert_contains "$(cat "$TMP_ROOT/windows-make-db.out")" '-D_WIN32_WINNT=0x0A00'

# Verify only stable user-facing entry points. Internal build helpers may be
# documented, renamed or removed without becoming a repository contract.
help_output=$(MAKEFLAGS= MAKEOVERRIDES= \
    make -C "$PROJECT_ROOT" --no-print-directory -s help)
for target in \
    debug coverage sanitizers release clean help deps deps-check deps-force \
    deps-clean check-toolchain check-binary test test-unit test-integration \
    quality check test-coverage test-sanitizers test-portability-linux \
    test-windows test-release version check-ca-bundle update-ca-bundle \
    docs-assets docs serve reset-dev-home; do
    assert_contains "$help_output" "make $target"
done
assert_not_contains "$help_output" 'make _build'
assert_contains "$help_output" 'Supported platforms:'


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
    -s)
        printf '%s\n' Linux
        ;;
    -m)
        printf '%s\n' unknown-architecture
        ;;
    *)
        exit 1
        ;;
esac
EOF_UNAME
chmod +x "$FAKE_UNAME_DIR/uname"
if (
    cd "$PROJECT_ROOT"
    PATH="$FAKE_UNAME_DIR:$PATH" OS= PROCESSOR_ARCHITEW6432= \
        PROCESSOR_ARCHITECTURE= MAKEFLAGS= MAKEOVERRIDES= \
        PLATFORM=linux/amd64 make --no-print-directory -n all
) >"$TMP_ROOT/make-unsupported-native.out" 2>&1; then
    fail 'unsupported native architecture was silently treated as x64'
fi
assert_contains "$(cat "$TMP_ROOT/make-unsupported-native.out")" \
    "Unsupported PLATFORM 'unsupported'"
printf 'Makefile platform-selector isolation tests passed.\n'

printf '==> Testing target-based build configurations...\n'
print_dependency_prefix() {
    (
        cd "$PROJECT_ROOT"
        printf '%s\n' \
            'print-deps-prefix:' \
            '	@printf "%s\n" "$(DEPS_PREFIX)"' |
            make --no-print-directory -s -f Makefile -f - \
                "$@" print-deps-prefix
    )
}

PINNED_PREFIX="$prefix"
resolved_prefix=$(print_dependency_prefix DEPS_PREFIX="$PINNED_PREFIX")
assert_equals "$resolved_prefix" "$PINNED_PREFIX"

if print_dependency_prefix \
        DEPS_PREFIX="$PINNED_PREFIX/../$(basename "$PINNED_PREFIX")" \
        >"$TMP_ROOT/deps-prefix-traversal.out" 2>&1; then
    fail 'DEPS_PREFIX containing .. was normalized and accepted'
fi
assert_contains "$(cat "$TMP_ROOT/deps-prefix-traversal.out")" \
    'DEPS_PREFIX must not contain .. path components'

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        BUILD_DIR="$TMP_ROOT/a/../outside" help \
        >"$TMP_ROOT/build-dir-traversal.out" 2>&1; then
    fail 'BUILD_DIR containing .. was normalized and accepted'
fi
assert_contains "$(cat "$TMP_ROOT/build-dir-traversal.out")" \
    'BUILD_DIR must not contain .. path components'

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        BUILD_DIR="$TMP_ROOT/build output" help \
        >"$TMP_ROOT/build-dir-space.out" 2>&1; then
    fail 'BUILD_DIR containing whitespace was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/build-dir-space.out")" \
    'BUILD_DIR must not contain whitespace'

# Make inputs are frozen literally before validation. Embedded Make functions
# must never execute while a caller-controlled value is parsed.
make_expansion_marker=$TMP_ROOT/make-expansion-executed
if make -C "$PROJECT_ROOT" --no-print-directory -n \
        "BUILD_DIR=\$(shell touch $make_expansion_marker)" help \
        >"$TMP_ROOT/make-expansion.out" 2>&1; then
    fail 'caller-controlled Make expression was accepted'
fi
assert_missing "$make_expansion_marker"
assert_contains "$(cat "$TMP_ROOT/make-expansion.out")" \
    'Caller-controlled Make expressions are not supported'

release_expansion_marker=$TMP_ROOT/release-expansion-executed
if make -C "$PROJECT_ROOT" --no-print-directory -n \
        "RELEASE_COMMON_ROOT=\${shell touch $release_expansion_marker}" help \
        >"$TMP_ROOT/release-expansion.out" 2>&1; then
    fail 'caller-controlled release-path Make expression was accepted'
fi
assert_missing "$release_expansion_marker"
assert_contains "$(cat "$TMP_ROOT/release-expansion.out")" \
    'Caller-controlled Make expressions are not supported'

# Internal derived paths cannot be replaced from the command line.
if make -C "$PROJECT_ROOT" --no-print-directory -n \
        TARGET="$TMP_ROOT/foreign-target" help \
        >"$TMP_ROOT/private-make-variable.out" 2>&1; then
    fail 'private Make target path was accepted from the command line'
fi
assert_contains "$(cat "$TMP_ROOT/private-make-variable.out")" \
    'Private Make variables cannot be overridden'

# Path spellings that GNU Make or the shell interpret structurally are rejected
# before they can become targets or recipe fragments.
for unsafe_path in \
    "$TMP_ROOT/a:b" \
    "$TMP_ROOT/a;b" \
    "$TMP_ROOT/a'b" \
    "$TMP_ROOT/a%25" \
    "$TMP_ROOT/a#b" \
    "$TMP_ROOT/a\$b"; do
    if make -C "$PROJECT_ROOT" --no-print-directory -n \
            BUILD_DIR="$unsafe_path" help \
            >"$TMP_ROOT/unsafe-make-path.out" 2>&1; then
        fail "unsafe BUILD_DIR spelling was accepted: $unsafe_path"
    fi
done

# No depfile is parsed before build-root ownership has been established. A
# foreign .d file containing a Make function must remain inert.
depfile_root=$TMP_ROOT/depfile-root
mkdir -p "$depfile_root/linux-x64/development/obj"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$depfile_root/.cup-build-root"
depfile_marker=$TMP_ROOT/depfile-executed
printf '\$(shell touch %s):\n' "$depfile_marker" > \
    "$depfile_root/linux-x64/development/obj/main.d"
make -C "$PROJECT_ROOT" --no-print-directory -n \
    BUILD_DIR="$depfile_root" help >/dev/null
assert_missing "$depfile_marker"
if grep -Eq '^-include[[:space:]]+\$\(DEP\)' "$PROJECT_ROOT/Makefile"; then
    fail 'Makefile still parses generated depfiles before the build boundary'
fi

# Even the conventional build/ spelling is not ownership proof. Every existing
# unmarked root is preserved and rejected, whether empty or non-empty.
foreign_build_root=$TMP_ROOT/foreign-build-root
mkdir "$foreign_build_root"
printf '%s\n' foreign > "$foreign_build_root/sentinel"
if make -C "$PROJECT_ROOT" --no-print-directory -s \
        BUILD_DIR="$foreign_build_root" \
        "$foreign_build_root/.cup-build-root" \
        >"$TMP_ROOT/foreign-build-root.out" 2>&1; then
    fail 'non-empty unmarked build root was adopted'
fi
assert_contains "$(cat "$TMP_ROOT/foreign-build-root.out")" \
    'invalid build root marker'
assert_file "$foreign_build_root/sentinel"
assert_missing "$foreign_build_root/.cup-build-root"

empty_foreign_build_root=$TMP_ROOT/empty-foreign-build-root
mkdir "$empty_foreign_build_root"
if make -C "$PROJECT_ROOT" --no-print-directory -s \
        BUILD_DIR="$empty_foreign_build_root" \
        "$empty_foreign_build_root/.cup-build-root" \
        >"$TMP_ROOT/empty-foreign-build-root.out" 2>&1; then
    fail 'empty unmarked build root was adopted'
fi
assert_contains "$(cat "$TMP_ROOT/empty-foreign-build-root.out")" \
    'invalid build root marker'
assert_missing "$empty_foreign_build_root/.cup-build-root"

# Public build entry points and clean use the canonical marker lock operations.
grep -Fq "cup_path_run_build '\$(BUILD_ROOT)' --" "$PROJECT_ROOT/Makefile" ||
    fail 'public builds are not coordinated through the build-root marker lock'
grep -Fq 'cup_path_clean_build_root "$$root"' "$PROJECT_ROOT/Makefile" ||
    fail 'make clean does not use the canonical build-root clean operation'

# Release tests build helpers in the selected configuration. The Windows
# runner must receive the same value instead of falling back to development.
release_windows_block=$(awk '
    /^test-release:/ { in_release = 1 }
    in_release && /windows-x64\)/ { in_windows = 1 }
    in_windows { print }
    in_windows && /;;/ { exit }
' "$PROJECT_ROOT/Makefile")
assert_contains "$release_windows_block" 'tests/release/windows.ps1'
assert_contains "$release_windows_block" \
    "CUP_TEST_CONFIGURATION='\$(CUP_TEST_CONFIGURATION)'"
release_target_block=$(awk '
    /^test-release:/ { in_release = 1 }
    in_release { print }
    in_release && /^test-coverage:/ { exit }
' "$PROJECT_ROOT/Makefile")
assert_contains "$release_target_block" "CUP_BUILD_DIR='\$(BUILD_DIR)'"
assert_contains "$release_target_block" 'tests/release/update-fixture.sh'
assert_contains "$release_target_block" '@set -e;'

# Every existing component of a managed build path is inspected before a
# marker is created or a tree is removed. A symlinked ancestor must never turn
# BUILD_DIR into an alias for an external directory.
external_build_parent=$TMP_ROOT/external-build-parent
linked_build_parent=$TMP_ROOT/linked-build-parent
mkdir -p "$external_build_parent"
ln -s "$external_build_parent" "$linked_build_parent"
unsafe_build_root=$linked_build_parent/output
if make -C "$PROJECT_ROOT" --no-print-directory -s \
        BUILD_DIR="$unsafe_build_root" "$unsafe_build_root/.cup-build-root" \
        >"$TMP_ROOT/build-parent-link.out" 2>&1; then
    fail 'build root marker followed a symlinked parent'
fi
[ ! -e "$external_build_parent/output" ] ||
    fail 'build root marker created an external directory through a symlink'

mkdir -p "$external_build_parent/output"
printf '%s\n' format=1 product=coffee-clang/cup kind=build-root layout=1 \
    > "$external_build_parent/output/.cup-build-root"
printf 'outside\n' > "$external_build_parent/output/sentinel"
if make -C "$PROJECT_ROOT" --no-print-directory -s \
        BUILD_DIR="$unsafe_build_root" clean \
        >"$TMP_ROOT/build-clean-parent-link.out" 2>&1; then
    fail 'make clean followed a symlinked build-root parent'
fi
assert_file "$external_build_parent/output/sentinel"

if make -C "$PROJECT_ROOT" --no-print-directory -n \
        DEPS_PREFIX="$TMP_ROOT/dependency prefix" help \
        >"$TMP_ROOT/deps-prefix-space.out" 2>&1; then
    fail 'DEPS_PREFIX containing whitespace was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/deps-prefix-space.out")" \
    'DEPS_PREFIX must not contain whitespace'

custom_build_root=$TMP_ROOT/custom-build-output
posix_test_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -n PLATFORM="$NATIVE_BUILD_PLATFORM" \
        BUILD_DIR="$custom_build_root" DEPS_PREFIX="$PINNED_PREFIX" \
        CUP_INTERNAL_DEPS_TARGET=deps-check test-unit
)
assert_contains "$posix_test_command" \
    "CUP_TEST_BUILD_ROOT='$custom_build_root'"
assert_not_contains "$posix_test_command" \
    "CUP_TEST_BUILD_ROOT='$PROJECT_ROOT/build'"

windows_test_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -n PLATFORM=windows-x64 \
        BUILD_DIR="$custom_build_root" DEPS_PREFIX="$PINNED_PREFIX" \
        CUP_INTERNAL_DEPS_TARGET=deps-check test-integration
)
assert_contains "$windows_test_command" \
    "CUP_TEST_BUILD_ROOT=\"\$(cygpath -w '$custom_build_root')\""
assert_contains "$windows_test_command" \
    "$custom_build_root/windows-x64/development/bin/cup.exe"
assert_not_contains "$windows_test_command" \
    "$PROJECT_ROOT/build/windows-x64/development/bin/cup.exe"

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

for coverage_platform in linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64; do
    coverage_command=$(
        cd "$PROJECT_ROOT"
        make --no-print-directory -B -n PLATFORM="$coverage_platform" \
            DEPS_PREFIX="$PINNED_PREFIX" coverage
    )
    case "$coverage_platform" in
        windows-x64)
            coverage_binary=cup.exe
            ;;
        *)
            coverage_binary=cup
            ;;
    esac
    assert_contains "$coverage_command" \
        "build/$coverage_platform/coverage/bin/$coverage_binary"
    case "$coverage_platform" in
        macos-*)
            assert_contains "$coverage_command" '-fprofile-instr-generate'
            assert_contains "$coverage_command" \
                "-fcoverage-prefix-map=$PROJECT_ROOT=$PROJECT_ROOT"
            ;;
        *)
            assert_contains "$coverage_command" '--coverage'
            ;;
    esac
    assert_contains "$coverage_command" "$PINNED_PREFIX/lib/libcurl.a"
    assert_contains "$coverage_command" "$PINNED_PREFIX/lib/libarchive.a"
    assert_not_contains "$coverage_command" ' -static '
done

coverage_runner_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -n PLATFORM=macos-arm64 \
        DEPS_PREFIX="$PINNED_PREFIX" test-coverage
)
assert_contains "$coverage_runner_command" "CUP_TEST_PLATFORM='macos-arm64'"
assert_contains "$coverage_runner_command" "DEPS_PREFIX='$PINNED_PREFIX'"


# Linux keeps its proven compile-time profile mapping. Windows GCC instead
# leaves the staging path hardwired and relocates counters at execution time.
for coverage_build_script in tests/build/unit.sh tests/build/helpers.sh; do
    coverage_build_text=$(cat "$PROJECT_ROOT/$coverage_build_script")
    assert_contains "$coverage_build_text" 'linux-*:coverage)'
    assert_contains "$coverage_build_text" 'windows-x64:coverage)'
    assert_contains "$coverage_build_text" '-fprofile-dir='
    assert_contains "$coverage_build_text" '-fprofile-prefix-path='
    assert_contains "$coverage_build_text" 'if [ -n "$GCOV_PROFILE_DIR" ]'
    windows_coverage_block=$(printf '%s\n' "$coverage_build_text" | awk '
        /windows-x64:coverage\)/ { capture = 1 }
        capture { print }
        capture && /^[[:space:]]*;;[[:space:]]*$/ { exit }
    ')
    assert_contains "$windows_coverage_block" 'GCOV_OUTPUT_DIR='
    assert_not_contains "$windows_coverage_block" 'GCOV_PROFILE_DIR='
    assert_not_contains "$windows_coverage_block" 'GCOV_PROFILE_PREFIX='
done

. "$PROJECT_ROOT/tests/support/posix/coverage.sh"
assert_equals 9 "$(cup_coverage_gcov_strip_components \
    'D:/a/cup/cup/build/windows-x64/coverage/tests/unit')"
assert_equals 8 "$(cup_coverage_gcov_strip_components \
    '/a/cup/cup/build/windows-x64/coverage/tests/unit')"

gcov_fixture=$TMP_ROOT/gcov-profile-ownership
mkdir -p "$gcov_fixture/unit" "$gcov_fixture/helpers"
printf '%s\n' note > "$gcov_fixture/unit/test_alpha-source.gcno"
printf '%s\n' counter > "$gcov_fixture/unit/test_alpha-source.gcda"
printf '%s\n' helper-note > "$gcov_fixture/helpers/network-helper-source.gcno"
printf '%s\n' helper-counter > "$gcov_fixture/helpers/network-helper-source.gcda"
. "$PROJECT_ROOT/tests/support/posix/coverage.sh"
cup_coverage_verify_gcov_profile_owners "$gcov_fixture"

mkdir -p "$gcov_fixture/.unit.retired"
printf '%s\n' counter > "$gcov_fixture/.unit.retired/test_alpha-source.gcda"
if cup_coverage_verify_gcov_profile_owners "$gcov_fixture" \
        >"$TMP_ROOT/gcov-retired.out" 2>&1; then
    fail 'coverage ownership accepted a recreated staging directory'
fi
assert_contains "$(cat "$TMP_ROOT/gcov-retired.out")" \
    'recreated retired build staging'
rm -rf "$gcov_fixture/.unit.retired"

rm -f "$gcov_fixture/unit/test_alpha-source.gcno"
if cup_coverage_verify_gcov_profile_owners "$gcov_fixture" \
        >"$TMP_ROOT/gcov-missing.out" 2>&1; then
    fail 'coverage ownership accepted a counter without its final note'
fi
assert_contains "$(cat "$TMP_ROOT/gcov-missing.out")" \
    'has no note in the same final owner'

sanitizer_runner_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -n PLATFORM=windows-x64 \
        DEPS_PREFIX="$PINNED_PREFIX" test-sanitizers
)
assert_contains "$sanitizer_runner_command" "CUP_TEST_PLATFORM='windows-x64'"
assert_contains "$sanitizer_runner_command" "DEPS_PREFIX='$PINNED_PREFIX'"

consumer_test_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -n PLATFORM="$NATIVE_BUILD_PLATFORM" \
        DEPS_PREFIX="$PINNED_PREFIX" CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_TEST_CONFIGURATION=coverage test-unit-build
)
assert_contains "$consumer_test_command" "scripts/dependencies/verify.sh"
assert_not_contains "$consumer_test_command" "scripts/dependencies/build-"

# Test binaries must compile repository-owned sources from the repository root
# with relative source operands. Passing absolute source operands through the
# project file-prefix map rewrites GCC coverage notes to /usr/src/cup and makes
# gcovr discard otherwise valid unit coverage as outside the checkout root.
unit_builder_text=$(cat "$PROJECT_ROOT/tests/build/unit.sh")
assert_contains "$unit_builder_text" 'compile_args+=("${compile_arg#"$ROOT"/}")'
assert_contains "$unit_builder_text" 'compile_command=("$CC"'
assert_contains "$unit_builder_text" '(cd "$ROOT" && "${compile_command[@]}"'
assert_not_contains "$unit_builder_text" 'GCOV_PROFILE_FLAGS=()'
helper_builder_text=$(cat "$PROJECT_ROOT/tests/build/helpers.sh")
windows_helper_list=$("$PROJECT_ROOT/tests/build/helpers.sh" --list windows-x64)
assert_not_contains "$windows_helper_list" 'binary-patch.exe'
assert_contains "$windows_helper_list" 'network-helper.exe'
assert_not_contains "$windows_helper_list" 'process-group'
assert_contains "$windows_helper_list" 'archive-fixture.exe'
posix_helper_list=$("$PROJECT_ROOT/tests/build/helpers.sh" --list linux-x64)
assert_contains "$posix_helper_list" 'archive-fixture'
assert_contains "$posix_helper_list" 'process-group'
assert_contains "$helper_builder_text" 'source=${source#"$ROOT"/}'
assert_contains "$helper_builder_text" 'compile_command=("$CC"'
assert_contains "$helper_builder_text" '(cd "$ROOT" && "${compile_command[@]}"'
assert_not_contains "$helper_builder_text" 'GCOV_PROFILE_FLAGS=()'
assert_not_contains "$helper_builder_text" 'PLATFORM_LIBS=()'
assert_contains "$helper_builder_text" 'compile_helper all archive-fixture'
windows_common_text=$(cat "$PROJECT_ROOT/tests/support/windows/common.ps1")
assert_contains "$windows_common_text" 'function New-ZipPackageFixture'
assert_contains "$windows_common_text" "Get-TestHelperPath -Name 'archive-fixture'"
[ ! -e "$PROJECT_ROOT/tests/support/windows/archive-fixtures.ps1" ] ||
    fail 'retired Windows archive-fixtures.ps1 still exists'
windows_release_text=$(cat "$PROJECT_ROOT/tests/release/windows.ps1")
assert_contains "$windows_release_text" 'slow-http-server --ready-file'
assert_not_contains "$windows_release_text" 'slow-http-server.ps1'
[ ! -e "$PROJECT_ROOT/tests/support/windows/slow-http-server.ps1" ] ||
    fail 'retired Windows slow-http-server.ps1 still exists'
update_fixture_builder="$PROJECT_ROOT/tests/release/update-fixture.sh"
[ "$($update_fixture_builder --next-version 9.9.9)" = 9.9.10 ] ||
    fail 'release update fixture still depends on same-length versions'
[ "$($update_fixture_builder --next-version 1.2.999999)" = 1.3.0 ] ||
    fail 'release update fixture did not carry at the patch limit'
if "$update_fixture_builder" --next-version 999999.999999.999999 >/dev/null 2>&1; then
    fail 'release update fixture accepted a version beyond the supported SemVer space'
fi
if "$update_fixture_builder" --next-version '1.*.3' >/dev/null 2>&1; then
    fail 'release update fixture accepted a non-SemVer version'
fi
update_fixture_text=$(cat "$update_fixture_builder")
assert_contains "$update_fixture_text" 'CUP_VERSION_FILE=$version_file'
assert_contains "$update_fixture_text" 'cup_path_resolve_host_temporary_directory'
assert_contains "$update_fixture_text" 'mktemp "$temporary_parent/cup-release-update-version.XXXXXX"'
assert_not_contains "$update_fixture_text" '${TMPDIR:-/tmp}/cup-release-update-version.XXXXXX'
assert_not_contains "$update_fixture_text" 'version_file=$BUILD_ROOT/'
assert_contains "$update_fixture_text" 'set -- make -C "$ROOT" --no-print-directory release-candidate'
assert_contains "$update_fixture_text" 'RELEASE_COMMON_DIR=$fixture_common_dir'
assert_contains "$update_fixture_text" 'CUP_BUILD_DIR'
assert_not_contains "$update_fixture_text" 'binary-patch'
posix_release_text=$(cat "$PROJECT_ROOT/tests/release/posix.sh")
windows_release_text=$(cat "$PROJECT_ROOT/tests/release/windows.ps1")
assert_not_contains "$posix_release_text" 'binary-patch'
assert_not_contains "$windows_release_text" 'binary-patch'
assert_contains "$posix_release_text" 'CUP_TEST_SERVER_ROOT'
assert_contains "$posix_release_text" 'validate_release_asset_modes'
assert_not_contains "$posix_release_text" 'chmod +x "$release_dir/'
assert_contains "$windows_release_text" 'CUP_TEST_SERVER_ROOT'
assert_contains "$windows_release_text" 'CUP_TEST_BUILD_ROOT'
assert_contains "$windows_release_text" 'Assert-ExactCandidateFiles'
assert_contains "$windows_release_text" '$process.WaitForExit()'
assert_not_contains "$windows_release_text" '$testRoot'
assert_contains "$windows_release_text" 'Get-Command sh.exe'
assert_contains "$windows_release_text" 'Get-Command cygpath.exe'
coverage_runner_text=$(cat "$PROJECT_ROOT/tests/runners/coverage.sh")
unit_runner_text=$(cat "$PROJECT_ROOT/tests/runners/unit.sh")
windows_common_text=$(cat "$PROJECT_ROOT/tests/support/windows/common.ps1")
assert_contains "$unit_runner_text" 'env GCOV_PREFIX="$GCOV_PREFIX_VALUE"'
assert_contains "$unit_runner_text" 'GCOV_PREFIX_STRIP="$GCOV_PREFIX_STRIP_VALUE"'
assert_contains "$coverage_runner_text" 'CUP_TEST_GCOV_HELPER_PREFIX="$helper_profile_prefix"'
assert_contains "$coverage_runner_text" 'CUP_TEST_GCOV_HELPER_STRIP="$helper_profile_strip"'
assert_not_contains "$coverage_runner_text" 'export GCOV_PREFIX='
assert_contains "$windows_common_text" 'function Start-TestHelperProcess'
assert_contains "$windows_common_text" '$env:GCOV_PREFIX = $env:CUP_TEST_GCOV_HELPER_PREFIX'
assert_contains "$windows_common_text" 'Remove-Item -LiteralPath Env:GCOV_PREFIX'
assert_contains "$coverage_runner_text" 'CUP_COVERAGE_REPORT_JOBS:-1'
profile_assignment=$(printf '%s\n' "$coverage_runner_text" | grep 'export LLVM_PROFILE_FILE=')
if ! printf '%s\n' "$profile_assignment" | grep -Eq '%[0-9]*m'; then
    fail 'LLVM coverage profiles are not merged by binary signature'
fi
assert_not_contains "$profile_assignment" '%p'
assert_contains "$coverage_runner_text" '[ "$REPORT_JOBS" -gt 1 ]'
assert_contains "$coverage_runner_text" 'report_jobs=%s'
assert_not_contains "$coverage_runner_text" 'backend_args=()'
assert_contains "$consumer_test_command" "CUP_TEST_CFLAGS='"
case "$NATIVE_BUILD_PLATFORM" in
    macos-*) assert_contains "$consumer_test_command" '-fprofile-instr-generate' ;;
    *) assert_contains "$consumer_test_command" '--coverage' ;;
esac

debug_test_command=$(
    cd "$PROJECT_ROOT"
    make --no-print-directory -n PLATFORM="$NATIVE_BUILD_PLATFORM" \
        DEPS_PREFIX="$PINNED_PREFIX" CUP_INTERNAL_DEPS_TARGET=deps-check \
        CUP_TEST_CONFIGURATION=debug test-unit-build
)
assert_contains "$debug_test_command" '-fno-omit-frame-pointer'

for sanitizer_platform in linux-x64 linux-arm64 macos-x64 macos-arm64 windows-x64; do
    sanitizer_command=$(
        cd "$PROJECT_ROOT"
        make --no-print-directory -B -n PLATFORM="$sanitizer_platform" \
            DEPS_PREFIX="$PINNED_PREFIX" sanitizers
    )
    case "$sanitizer_platform" in
        windows-x64)
            sanitizer_binary=cup.exe
            ;;
        *)
            sanitizer_binary=cup
            ;;
    esac
    assert_contains "$sanitizer_command" \
        "build/$sanitizer_platform/sanitizers/bin/$sanitizer_binary"
    assert_contains "$sanitizer_command" '-fsanitize=address,undefined'
    assert_contains "$sanitizer_command" "$PINNED_PREFIX/lib/libcurl.a"
    assert_contains "$sanitizer_command" "$PINNED_PREFIX/lib/libarchive.a"
    assert_not_contains "$sanitizer_command" ' -static '
done

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

printf 'Target-based build configuration tests passed.\n'


copy_path_ops_sources() {
    destination=$1

    mkdir -p "$destination/scripts/lib" "$destination/include" "$destination/src"
    cp "$PROJECT_ROOT/scripts/lib/path-safety.sh" \
        "$PROJECT_ROOT/scripts/lib/path-ops.sh" \
        "$PROJECT_ROOT/scripts/lib/path-ops.c" \
        "$destination/scripts/lib/"
    cp "$PROJECT_ROOT/include/constants.h" \
        "$PROJECT_ROOT/include/domain_registry.h" \
        "$PROJECT_ROOT/include/error.h" \
        "$PROJECT_ROOT/include/path.h" \
        "$PROJECT_ROOT/include/system.h" \
        "$PROJECT_ROOT/include/text.h" \
        "$destination/include/"
    cp "$PROJECT_ROOT/src/system.c" \
        "$PROJECT_ROOT/src/system_posix.c" \
        "$PROJECT_ROOT/src/path.c" \
        "$PROJECT_ROOT/src/text.c" \
        "$destination/src/"
}

# Source-test evidence is staged outside build/ so later clean operations for
# secondary compilers and portability cannot remove the primary evidence.
source_fixture=$TMP_ROOT/source-evidence-project
mkdir -p "$source_fixture/scripts/ci" "$source_fixture/scripts/build" \
    "$source_fixture/bin" "$source_fixture/evidence"
copy_path_ops_sources "$source_fixture"
cp "$PROJECT_ROOT/scripts/ci/source-posix.sh" \
    "$PROJECT_ROOT/scripts/ci/evidence-common.sh" \
    "$PROJECT_ROOT/scripts/ci/write-source-evidence.sh" \
    "$PROJECT_ROOT/scripts/ci/verify-source-evidence.sh" "$source_fixture/scripts/ci/"
cp "$PROJECT_ROOT/scripts/version.sh" "$source_fixture/scripts/"
printf '%s\n' 0.2.2 > "$source_fixture/VERSION"
cat > "$source_fixture/scripts/build/validate-toolchain.sh" <<'EOF_VALIDATE'
#!/bin/sh
exit 0
EOF_VALIDATE
cat > "$source_fixture/bin/uname" <<'EOF_UNAME_SOURCE'
#!/bin/sh
case "${1:-}" in
    -s) printf '%s\n' Linux ;;
    -m) printf '%s\n' x86_64 ;;
    *) exit 2 ;;
esac
EOF_UNAME_SOURCE
cat > "$source_fixture/bin/git" <<'EOF_GIT_SOURCE'
#!/bin/sh
if [ "${1:-}" = -C ]; then
    shift 2
fi
[ "$#" -eq 2 ] && [ "$1" = rev-parse ] && [ "$2" = HEAD ] || exit 2
printf '%s\n' 0123456789abcdef0123456789abcdef01234567
EOF_GIT_SOURCE
cat > "$source_fixture/bin/make" <<'EOF_MAKE_SOURCE'
#!/bin/sh
set -eu
target=
secondary=0
secondary_role=0
for argument in "$@"; do
    case "$argument" in
        CC=clang) secondary=1 ;;
        CUP_INTERNAL_TOOLCHAIN_ROLE=secondary) secondary_role=1 ;;
        *=*) ;;
        *) target=$argument ;;
    esac
done
if [ "$secondary" -eq 1 ] && [ "$secondary_role" -ne 1 ]; then
    echo 'secondary compiler invocation omitted CUP_INTERNAL_TOOLCHAIN_ROLE=secondary' >&2
    exit 3
fi
case "$target" in
    clean)
        rm -rf -- build
        ;;
    check-development)
        output=build/linux-x64/development
        mkdir -p "$output/generated"
        if [ "$secondary" -eq 1 ]; then identity=secondary; else identity=primary; fi
        cat > "$output/build-config.txt" <<EOF_CONFIG
format=3
platform=linux-x64
configuration=development
host_system=Linux
host_machine=x86_64
compiler_command=$identity
compiler_path=/usr/bin/cc
compiler_target=x86_64-linux-gnu
compiler_target_normalized=linux-x64
compiler_version=fixture
compiler_numeric=1.0.0
windres_command=
windres_path=
windres_version=
windres_numeric=
windres_target_normalized=
cppflags=-D_POSIX_C_SOURCE=200809L
cflags=-std=c11
ldflags=
ldlibs=-lc
deps_prefix=/tmp/cup-fixture-dependencies
dependency_prefix_format=5
dependency_platform=linux-x64
dependency_profile=gcc
dependency_build_revision=4
dependency_source_lock_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
dependency_toolchain_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
official_build=0
EOF_CONFIG
        cat > "$output/generated/release.txt" <<'EOF_RELEASE'
format=1
version=0.2.2
commit=0123456789abcdef0123456789abcdef01234567
EOF_RELEASE
        cat > "$output/binary-inspection.txt" <<EOF_INSPECTION
format=2
platform=linux-x64
configuration=development
inspection_policy=build
binary=cup
sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
object_format=ELF
architecture=x86_64
elf_class=ELF64
elf_data=little-endian
elf_type=DYN
machine=Advanced Micro Devices X86-64
entry_point=0x1000
linkage=dynamic-system
interpreter=/lib64/ld-linux-x86-64.so.2
needed_count=1
needed=libc.so.6
runtime_search_path=none
file_description=$identity
EOF_INSPECTION
        ;;
esac
EOF_MAKE_SOURCE
chmod +x "$source_fixture/scripts/ci/source-posix.sh" \
    "$source_fixture/scripts/ci/write-source-evidence.sh" \
    "$source_fixture/scripts/ci/verify-source-evidence.sh" \
    "$source_fixture/scripts/version.sh" \
    "$source_fixture/scripts/build/validate-toolchain.sh" \
    "$source_fixture/bin/uname" "$source_fixture/bin/git" "$source_fixture/bin/make"
(
    cd "$source_fixture"
    PATH="$source_fixture/bin:$PATH" PLATFORM=linux-x64 FAMILY=linux \
        CUP_SOURCE_EVIDENCE_ROOT="$source_fixture/evidence" \
        scripts/ci/source-posix.sh
)
assert_contains "$(cat "$source_fixture/evidence/linux-x64/build-config.txt")" \
    'compiler_command=primary'
assert_equals "$(cat "$source_fixture/evidence/linux-x64/release.txt")" \
    "$(printf 'format=1\nversion=0.2.2\ncommit=0123456789abcdef0123456789abcdef01234567')"
assert_contains "$(cat "$source_fixture/evidence/linux-x64/binary-inspection.txt")" \
    'file_description=primary'
assert_not_contains "$(cat "$source_fixture/evidence/linux-x64/build-config.txt")" secondary

# Unit tests and helper programs are built in private staging directories. A
# compiler failure must preserve the previous complete output, while success
# publishes exactly the registered executable set.
test_build_fixture=$TMP_ROOT/test-build-publication
mkdir -p \
    "$test_build_fixture/tests/build" \
    "$test_build_fixture/tests/support" \
    "$test_build_fixture/scripts/dependencies" \
    "$test_build_fixture/prefix/lib" \
    "$test_build_fixture/prefix/bin" \
    "$test_build_fixture/bin" \
    "$test_build_fixture/build/linux-x64/development/tests/unit" \
    "$test_build_fixture/build/linux-x64/development/tests/helpers"
cp "$PROJECT_ROOT/tests/build/unit.sh" "$PROJECT_ROOT/tests/build/helpers.sh" \
    "$test_build_fixture/tests/build/"
cp "$PROJECT_ROOT/tests/support/environment.sh" \
    "$test_build_fixture/tests/support/environment.sh"
copy_path_ops_sources "$test_build_fixture"
cat > "$test_build_fixture/scripts/dependencies/verify.sh" <<'EOF_VERIFY_TEST_DEPS'
#!/bin/sh
exit 0
EOF_VERIFY_TEST_DEPS
chmod +x "$test_build_fixture/scripts/dependencies/verify.sh" \
    "$test_build_fixture/tests/build/unit.sh" \
    "$test_build_fixture/tests/build/helpers.sh" \
    "$test_build_fixture/scripts/lib/path-ops.sh"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$test_build_fixture/build/.cup-build-root"
printf '%s\n' previous-unit > \
    "$test_build_fixture/build/linux-x64/development/tests/unit/sentinel.txt"
printf '%s\n' previous-helper > \
    "$test_build_fixture/build/linux-x64/development/tests/helpers/sentinel.txt"
: > "$test_build_fixture/prefix/lib/libunity.a"
cat > "$test_build_fixture/bin/pkg-config" <<'EOF_FAKE_PKG_CONFIG'
#!/bin/sh
printf '%s\n' -lfixture
EOF_FAKE_PKG_CONFIG
cat > "$test_build_fixture/prefix/bin/curl-config" <<'EOF_FAKE_CURL_CONFIG'
#!/bin/sh
[ "${1:-}" = --static-libs ] || exit 2
printf '%s\n' -lfixture-curl
EOF_FAKE_CURL_CONFIG
cat > "$test_build_fixture/bin/fakecc" <<'EOF_FAKE_TEST_CC'
#!/bin/sh
set -eu
output=
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then
        [ "$#" -ge 2 ] || exit 2
        output=$2
        shift 2
    else
        shift
    fi
done
[ -n "$output" ] || exit 2
counter_file=${CUP_FAKE_CC_COUNTER:?}
count=0
[ ! -f "$counter_file" ] || count=$(cat "$counter_file")
count=$((count + 1))
printf '%s\n' "$count" > "$counter_file"
if [ "${CUP_FAKE_CC_FAIL_AFTER:-0}" -gt 0 ] &&
    [ "$count" -gt "$CUP_FAKE_CC_FAIL_AFTER" ]; then
    exit 9
fi
mkdir -p "$(dirname -- "$output")"
printf '#!/bin/sh\nexit 0\n' > "$output"
chmod +x "$output"
EOF_FAKE_TEST_CC
chmod +x "$test_build_fixture/bin/pkg-config" \
    "$test_build_fixture/bin/fakecc" \
    "$test_build_fixture/prefix/bin/curl-config"

run_fixture_builder() {
    builder=$1
    PATH="$test_build_fixture/bin:$PATH" \
    CUP_TEST_PROJECT_ROOT="$test_build_fixture" \
    CUP_TEST_PLATFORM=linux-x64 \
    CUP_TEST_CONFIGURATION=development \
    CUP_TEST_BUILD_ROOT="$test_build_fixture/build" \
    DEPS_PREFIX="$test_build_fixture/prefix" \
    CC="$test_build_fixture/bin/fakecc" \
    CUP_TEST_CPPFLAGS= \
    CUP_TEST_CFLAGS= \
    CUP_TEST_LDFLAGS= \
    CUP_FAKE_CC_COUNTER="$test_build_fixture/compiler-count" \
    CUP_FAKE_CC_FAIL_AFTER="${CUP_FAKE_CC_FAIL_AFTER:-0}" \
        "$test_build_fixture/tests/build/$builder"
}

: > "$test_build_fixture/compiler-count"
if CUP_FAKE_CC_FAIL_AFTER=2 run_fixture_builder unit.sh \
        >"$TMP_ROOT/unit-staging-failure.out" 2>&1; then
    fail 'unit-test builder accepted an interrupted compilation'
fi
assert_file "$test_build_fixture/build/linux-x64/development/tests/unit/sentinel.txt"
if find "$test_build_fixture/build/linux-x64/development/tests" \
        -maxdepth 1 -name '.unit.*' -print -quit | grep -q .; then
    fail 'failed unit-test build left a staging directory'
fi

rm -f "$test_build_fixture/compiler-count"
if ! run_fixture_builder unit.sh >"$TMP_ROOT/unit-staging-success.out" 2>&1; then
    cat "$TMP_ROOT/unit-staging-success.out" >&2
    fail 'complete unit-test staging could not be published'
fi
assert_missing "$test_build_fixture/build/linux-x64/development/tests/unit/sentinel.txt"
expected_units=$("$test_build_fixture/tests/build/unit.sh" --list linux-x64 | wc -l | tr -d '[:space:]')
actual_units=$(find "$test_build_fixture/build/linux-x64/development/tests/unit" \
    -maxdepth 1 -type f -name 'test_*' ! -name '*.gcno' ! -name '*.gcda' |
    wc -l | tr -d '[:space:]')
assert_equals "$actual_units" "$expected_units"

rm -f "$test_build_fixture/compiler-count"
if CUP_FAKE_CC_FAIL_AFTER=1 run_fixture_builder helpers.sh \
        >"$TMP_ROOT/helper-staging-failure.out" 2>&1; then
    fail 'test-helper builder accepted an interrupted compilation'
fi
assert_file "$test_build_fixture/build/linux-x64/development/tests/helpers/sentinel.txt"
if find "$test_build_fixture/build/linux-x64/development/tests" \
        -maxdepth 1 -name '.helpers.*' -print -quit | grep -q .; then
    fail 'failed test-helper build left a staging directory'
fi

rm -f "$test_build_fixture/compiler-count"
if ! run_fixture_builder helpers.sh >"$TMP_ROOT/helper-staging-success.out" 2>&1; then
    cat "$TMP_ROOT/helper-staging-success.out" >&2
    fail 'complete test-helper staging could not be published'
fi
assert_missing "$test_build_fixture/build/linux-x64/development/tests/helpers/sentinel.txt"
expected_helpers=$("$test_build_fixture/tests/build/helpers.sh" --list linux-x64 | wc -l | tr -d '[:space:]')
actual_helpers=$(find "$test_build_fixture/build/linux-x64/development/tests/helpers" \
    -maxdepth 1 -type f -perm -u+x | wc -l | tr -d '[:space:]')
assert_equals "$actual_helpers" "$expected_helpers"


# Release-output replacement must retain the managed build-root identity until
# the previous generation has been removed after the atomic swap.
release_replace_root=$TMP_ROOT/release-output-replacement
release_replace_output=$release_replace_root/output
mkdir -p "$release_replace_root"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$release_replace_root/.cup-build-root"
SCRIPT_DIR="$PROJECT_ROOT/scripts/release" sh -eu -c '
    output=$1
    build_root=$2
    . "$3/common.sh"
    prepare_output_staging "$output" "$build_root"
    printf "first\n" > "$OUTPUT_STAGING/value.txt"
    commit_output_staging "$output"
    prepare_output_staging "$output" "$build_root"
    printf "second\n" > "$OUTPUT_STAGING/value.txt"
    commit_output_staging "$output"
' sh "$release_replace_output" "$release_replace_root" \
    "$PROJECT_ROOT/scripts/release"
assert_contains "$(cat "$release_replace_output/value.txt")" 'second'
assert_missing "$release_replace_root/.output.previous"

# Finalization owns the complete bundle before publication. A failure in the
# last inspector step must preserve the previous output and expose only
# absolute staging paths to the inspector.
finalizer_fixture=$TMP_ROOT/finalizer-publication
finalizer_build=$finalizer_fixture/build
mkdir -p "$finalizer_build/input" "$finalizer_build/meta" \
    "$finalizer_build/final" "$finalizer_fixture/tools"
chmod 2755 "$finalizer_build"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' > "$finalizer_build/.cup-build-root"
printf 'previous\n' > "$finalizer_build/final/sentinel.txt"
cat > "$finalizer_build/input/main.c" <<'EOF_FINALIZER_C'
int main(void) { return 0; }
EOF_FINALIZER_C
"${CC:-cc}" -g "$finalizer_build/input/main.c" -o "$finalizer_build/input/cup"
printf 'format=1\nplatform=linux-x64\n' > "$finalizer_build/meta/build-config.txt"
printf 'format=1\nversion=0.2.2\ncommit=0123456789abcdef0123456789abcdef01234567\n' > \
    "$finalizer_build/meta/release.txt"
cat > "$finalizer_fixture/tools/inspect" <<'EOF_FINALIZER_INSPECT'
#!/bin/sh
set -eu
[ "$#" -eq 5 ]
case "$3" in /*) ;; *) exit 81 ;; esac
case "$4" in /*) ;; *) exit 82 ;; esac
[ -f "$(dirname -- "$4")/build-config.txt" ]
[ -f "$(dirname -- "$4")/release.txt" ]
printf '%s\n%s\n' "$3" "$4" > "${CUP_FINALIZER_TRACE:?}"
[ "${CUP_FINALIZER_FAIL:-0}" -eq 0 ] || exit 9
printf 'format=3\nplatform=%s\nconfiguration=%s\n' "$1" "$2" > "$4"
EOF_FINALIZER_INSPECT
cat > "$finalizer_fixture/tools/path-check" <<'EOF_FINALIZER_PATH'
#!/bin/sh
exit 0
EOF_FINALIZER_PATH
chmod +x "$finalizer_fixture/tools/inspect" "$finalizer_fixture/tools/path-check"

if CUP_BUILD_ROOT="$finalizer_build" \
    CUP_FINALIZER_TRACE="$finalizer_fixture/failed.trace" CUP_FINALIZER_FAIL=1 \
    "$PROJECT_ROOT/scripts/build/finalize-release.sh" linux-x64 debug \
        "$finalizer_build/input/cup" "$finalizer_build/final" \
        "$finalizer_build/meta/build-config.txt" "$finalizer_build/meta/release.txt" \
        build "$finalizer_fixture/tools/inspect" "$finalizer_fixture/tools/path-check" \
        >"$TMP_ROOT/finalizer-failure.out" 2>&1; then
    fail 'late finalizer failure unexpectedly replaced the previous bundle'
fi
assert_file "$finalizer_build/final/sentinel.txt"
assert_missing "$finalizer_build/.final.staging"
while IFS= read -r observed_path; do
    case "$observed_path" in "$finalizer_build/.final.staging"/*) ;;
        *) fail "inspector did not receive an absolute staging path: $observed_path" ;;
    esac
done < "$finalizer_fixture/failed.trace"

CUP_BUILD_ROOT="$finalizer_build" \
CUP_FINALIZER_TRACE="$finalizer_fixture/success.trace" CUP_FINALIZER_FAIL=0 \
    "$PROJECT_ROOT/scripts/build/finalize-release.sh" linux-x64 debug \
        "$finalizer_build/input/cup" "$finalizer_build/final" \
        "$finalizer_build/meta/build-config.txt" "$finalizer_build/meta/release.txt" \
        build "$finalizer_fixture/tools/inspect" "$finalizer_fixture/tools/path-check" \
        >"$TMP_ROOT/finalizer-success.out" 2>&1
assert_missing "$finalizer_build/final/sentinel.txt"
for final_file in bin/cup symbols/cup.debug build-config.txt release.txt \
        binary-inspection.txt finalization.txt; do
    assert_file "$finalizer_build/final/$final_file"
done
assert_missing "$finalizer_build/.final.staging"
assert_missing "$finalizer_build/.final.previous"
assert_not_contains "$(cat "$TMP_ROOT/finalizer-success.out")" \
    'Unable to find program interpreter name'

printf '%s\n' 'Build-system contract tests passed.'
