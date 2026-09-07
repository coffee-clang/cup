#!/bin/sh

# Verifies release/source build identity matching without duplicating the source job's other checks.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"

test_begin source-build-config

VERIFIER=$PROJECT_ROOT/scripts/ci/verify-source-build-config.sh
SOURCE=$TMP_ROOT/source.txt
CANDIDATE=$TMP_ROOT/candidate.txt
LOCK_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
TOOLCHAIN_SHA=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

write_config() {
    output=$1
    platform=$2
    configuration=$3
    official=$4
    compiler_numeric=$5
    toolchain=$6
    profile=$7

    case "$platform" in
        windows-x64)
            host_system=MINGW64_NT
            host_machine=x86_64
            compiler_target=x86_64-w64-mingw32
            windres_command=windres
            windres_path=/ucrt64/bin/windres
            windres_version='GNU windres 14.0.0'
            windres_numeric=14.0.0
            windres_target=windows-x64
            ;;
        linux-x64)
            host_system=Linux
            host_machine=x86_64
            compiler_target=x86_64-linux-gnu
            windres_command=
            windres_path=
            windres_version=
            windres_numeric=
            windres_target=
            ;;
        *) fail "unsupported fixture platform: $platform" ;;
    esac

    cat >"$output" <<EOF_CONFIG
format=3
platform=$platform
configuration=$configuration
host_system=$host_system
host_machine=$host_machine
compiler_command=gcc
compiler_path=/usr/bin/gcc
compiler_target=$compiler_target
compiler_target_normalized=$platform
compiler_version=gcc fixture $compiler_numeric
compiler_numeric=$compiler_numeric
windres_command=$windres_command
windres_path=$windres_path
windres_version=$windres_version
windres_numeric=$windres_numeric
windres_target_normalized=$windres_target
cppflags=-D_POSIX_C_SOURCE=200809L
cflags=-std=c11
ldflags=
ldlibs=-lc
deps_prefix=/tmp/cup-source-build-config-prefix
dependency_prefix_format=5
dependency_platform=$platform
dependency_profile=$profile
dependency_build_revision=4
dependency_source_lock_sha256=$LOCK_SHA
dependency_toolchain_sha256=$toolchain
official_build=$official
EOF_CONFIG
}

write_pair() {
    platform=${1:-linux-x64}
    profile=${2:-gcc}
    write_config "$SOURCE" "$platform" development 0 14.0.0 "$TOOLCHAIN_SHA" "$profile"
    write_config "$CANDIDATE" "$platform" release 1 14.0.0 "$TOOLCHAIN_SHA" "$profile"
}

verify_ok() {
    "$VERIFIER" "$SOURCE" "$CANDIDATE" "${1:-linux-x64}" >/dev/null
}

expect_failure() {
    label=$1
    platform=$2
    if "$VERIFIER" "$SOURCE" "$CANDIDATE" "$platform" >"$TMP_ROOT/$label.out" 2>&1; then
        fail "$label build-config mismatch was accepted"
    fi
}

write_pair
verify_ok

write_pair
sed -i 's/^configuration=release$/configuration=development/' "$CANDIDATE"
expect_failure candidate-role linux-x64
assert_contains "$(cat "$TMP_ROOT/candidate-role.out")" 'candidate.configuration'

write_pair
sed -i 's/^platform=linux-x64$/platform=linux-arm64/' "$SOURCE"
expect_failure source-platform linux-x64
assert_contains "$(cat "$TMP_ROOT/source-platform.out")" 'source.platform'

write_pair
sed -i 's/^dependency_profile=gcc$/dependency_profile=foreign/' "$CANDIDATE"
expect_failure dependency-profile linux-x64
assert_contains "$(cat "$TMP_ROOT/dependency-profile.out")" 'candidate dependency_profile'

write_pair
sed -i 's/^dependency_toolchain_sha256=.*/dependency_toolchain_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc/' "$CANDIDATE"
expect_failure dependency-toolchain linux-x64
assert_contains "$(cat "$TMP_ROOT/dependency-toolchain.out")" 'candidate dependency_toolchain_sha256'

write_pair
sed -i 's/^dependency_source_lock_sha256=.*/dependency_source_lock_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd/' "$CANDIDATE"
expect_failure source-lock linux-x64
assert_contains "$(cat "$TMP_ROOT/source-lock.out")" 'candidate dependency_source_lock_sha256'

write_pair
sed -i 's/^compiler_numeric=14.0.0$/compiler_numeric=15.0.0/' "$CANDIDATE"
expect_failure compiler-version linux-x64
assert_contains "$(cat "$TMP_ROOT/compiler-version.out")" 'candidate compiler_numeric'

write_pair
printf 'unexpected=value\n' >> "$CANDIDATE"
expect_failure schema linux-x64
assert_contains "$(cat "$TMP_ROOT/schema.out")" 'unexpected schema or key order'

write_pair windows-x64 ucrt64-gcc
verify_ok windows-x64
sed -i 's/^windres_numeric=14.0.0$/windres_numeric=15.0.0/' "$CANDIDATE"
expect_failure windres-version windows-x64
assert_contains "$(cat "$TMP_ROOT/windres-version.out")" 'candidate windres_numeric'

write_pair
printf '\r' >> "$SOURCE"
expect_failure canonical-bytes linux-x64
assert_contains "$(cat "$TMP_ROOT/canonical-bytes.out")" 'NUL or carriage-return'

printf '%s\n' 'Source/release build-config identity tests passed.'
