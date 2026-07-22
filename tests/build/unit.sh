#!/usr/bin/env bash

# Purpose: Compiles the C unit-test binaries selected by the Makefile.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
cup_test_require_dependencies

# Build configuration and dependency selection.
PLATFORM="$CUP_TEST_PLATFORM"
case "$PLATFORM" in
    macos-*) CC="${CC:-clang}" ;;
    *) CC="${CC:-gcc}" ;;
esac
TEST_CONFIGURATION="${CUP_TEST_CONFIGURATION:-development}"
TEST_CPPFLAGS="-D_POSIX_C_SOURCE=200809L"
TEST_CFLAGS="${TEST_CFLAGS:-}"
TEST_LDFLAGS="${TEST_LDFLAGS:-}"
case "$PLATFORM" in
    macos-*)
        TEST_CPPFLAGS="$TEST_CPPFLAGS -D_DARWIN_C_SOURCE"
        TEST_CFLAGS="$TEST_CFLAGS -mmacosx-version-min=13.0"
        TEST_LDFLAGS="$TEST_LDFLAGS -mmacosx-version-min=13.0"
        ;;
esac
case "$TEST_CONFIGURATION" in
    development) ;;
    coverage)
        case "$PLATFORM" in
            macos-*)
                TEST_CFLAGS="$TEST_CFLAGS -fprofile-instr-generate -fcoverage-mapping"
                TEST_LDFLAGS="$TEST_LDFLAGS -fprofile-instr-generate -fcoverage-mapping"
                ;;
            *)
                TEST_CFLAGS="$TEST_CFLAGS --coverage -fprofile-arcs -ftest-coverage -fprofile-abs-path"
                TEST_LDFLAGS="$TEST_LDFLAGS --coverage"
                ;;
        esac
        ;;
    sanitizers)
        TEST_CFLAGS="$TEST_CFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer"
        TEST_LDFLAGS="$TEST_LDFLAGS -fsanitize=address,undefined"
        ;;
    *)
        printf 'Unsupported unit-test configuration: %s\n' "$TEST_CONFIGURATION" >&2
        exit 2
        ;;
esac
TEST_BUILD_DIR="$ROOT/build/$PLATFORM/$TEST_CONFIGURATION/tests/unit"
UNITY_LIB=$(cup_test_find_static_library unity) || {
    printf 'Unity static library was not found in %s.\n' "$DEPS_PREFIX" >&2
    exit 1
}
UNIT_ARCHIVE_LIBS=
if [ "$PLATFORM" != windows-x64 ]; then
    unit_pkg_config_path="$DEPS_PREFIX/lib/pkgconfig:$DEPS_PREFIX/lib64/pkgconfig"
    UNIT_ARCHIVE_LIBS=$(PKG_CONFIG_PATH="$unit_pkg_config_path" \
        PKG_CONFIG_LIBDIR="$unit_pkg_config_path" \
        PKG_CONFIG_SYSROOT_DIR= \
        pkg-config --static --libs libarchive) || {
        printf 'Pinned libarchive metadata was not usable in %s.\n' \
            "$DEPS_PREFIX" >&2
        exit 1
    }
    [ -n "$UNIT_ARCHIVE_LIBS" ] || {
        printf 'Pinned libarchive metadata was empty in %s.\n' \
            "$DEPS_PREFIX" >&2
        exit 1
    }
fi

mkdir -p "$TEST_BUILD_DIR"

# Compile one registered Unity suite with the same project flags and pinned libraries as CUP.
compile_test() {
    name=$1
    shift
    output="$TEST_BUILD_DIR/$name"
    printf '==> Compiling C unit test: %s\n' "$name"
    "$CC" -std=c11 $TEST_CPPFLAGS -Wall -Wextra -Werror \
        $TEST_CFLAGS \
        -I"$ROOT/tests/unit/fixtures" \
        -I"$ROOT/include" -I"$DEPS_PREFIX/include" \
        "$@" \
        "$UNITY_LIB" $TEST_LDFLAGS -o "$output"
}

# Suite ownership remains explicit so the Makefile can request individual binaries.
compile_test test_exit_status \
    "$ROOT/tests/unit/test_exit_status.c" \
    "$ROOT/src/exit_status.c"

compile_test test_package_selector \
    "$ROOT/tests/unit/test_package_selector.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/package_selector.c"

compile_test test_package_metadata \
    "$ROOT/tests/unit/test_package_metadata.c" \
    "$ROOT/src/package_metadata.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_checksum \
    "$ROOT/tests/unit/test_checksum.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/sha256.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c"

compile_test test_package_catalog \
    "$ROOT/tests/unit/test_package_catalog.c" \
    "$ROOT/src/package_catalog.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_install_policy \
    "$ROOT/tests/unit/test_install_policy.c" \
    "$ROOT/src/install_policy.c" \
    "$ROOT/src/tool_preferences.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_cup_assets \
    "$ROOT/tests/unit/test_cup_assets.c" \
    "$ROOT/src/cup_assets.c" \
    "$ROOT/src/text.c"

compile_test test_command_update \
    "$ROOT/tests/unit/test_command_update.c" \
    "$ROOT/src/command_update.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/text.c"

compile_test test_package_install \
    "$ROOT/tests/unit/test_package_install.c" \
    "$ROOT/src/package_install.c" \
    "$ROOT/src/text.c"

compile_test test_command_install \
    "$ROOT/tests/unit/test_command_install.c" \
    "$ROOT/src/command_install.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/text.c"

compile_test test_command_config \
    "$ROOT/tests/unit/test_command_config.c" \
    "$ROOT/src/command_config.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/text.c"

compile_test test_command_remove \
    "$ROOT/tests/unit/test_command_remove.c" \
    "$ROOT/src/command_remove.c"

compile_test test_command_doctor \
    "$ROOT/tests/unit/test_command_doctor.c" \
    "$ROOT/src/command_doctor.c"

compile_test test_command_context \
    "$ROOT/tests/unit/test_command_context.c" \
    "$ROOT/src/command_context.c" \
    "$ROOT/src/installed_package.c" \
    "$ROOT/src/package_request.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

if [ "$PLATFORM" != windows-x64 ]; then
    compile_test test_interrupt \
        "$ROOT/tests/unit/test_interrupt.c" \
        "$ROOT/src/interrupt.c"

    compile_test test_command_queries \
        "$ROOT/tests/unit/test_command_queries.c" \
        "$ROOT/src/command_list.c" \
        "$ROOT/src/command_default.c" \
        "$ROOT/src/command_info.c" \
        "$ROOT/src/command_search.c" \
        "$ROOT/src/command_inspect.c"

    compile_test test_package \
        "$ROOT/tests/unit/test_package.c" \
        "$ROOT/src/package.c" \
        "$ROOT/src/package_selector.c" \
        "$ROOT/src/package_metadata.c" \
        "$ROOT/src/platform.c" \
        "$ROOT/src/registry.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c" \
        "$ROOT/src/system_posix.c"

    compile_test test_package_archive \
        "$ROOT/tests/unit/test_package_archive.c" \
        "$ROOT/src/package_archive_format.c" \
        "$ROOT/src/package_archive.c" \
        "$ROOT/src/interrupt.c" \
        "$ROOT/src/system_posix.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c" \
        $UNIT_ARCHIVE_LIBS

    compile_test test_archive_faults \
        "$ROOT/tests/unit/test_archive_faults.c"

    compile_test test_package_cache \
        "$ROOT/tests/unit/test_package_cache.c" \
        "$ROOT/src/download.c" \
        "$ROOT/src/package_cache.c" \
        "$ROOT/src/layout.c" \
        "$ROOT/src/filesystem.c" \
        "$ROOT/src/system_posix.c" \
        "$ROOT/src/platform.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c"

    compile_test test_storage \
        "$ROOT/tests/unit/test_storage.c" \
        "$ROOT/tests/unit/test_system_posix.c" \
        "$ROOT/tests/unit/test_filesystem.c" \
        "$ROOT/tests/unit/test_layout.c" \
        "$ROOT/src/layout.c" \
        "$ROOT/src/filesystem.c" \
        "$ROOT/src/system_posix.c" \
        "$ROOT/src/interrupt.c" \
        "$ROOT/src/platform.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c"

    compile_test test_state \
        "$ROOT/tests/unit/test_state.c" \
        "$ROOT/src/state.c" \
        "$ROOT/src/system_posix.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c"

    compile_test test_package_transaction \
        "$ROOT/tests/unit/test_package_transaction.c" \
        "$ROOT/src/package_transaction.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c"

    compile_test test_cup_update_journal \
        "$ROOT/tests/unit/test_cup_update_journal.c" \
        "$ROOT/src/cup_update_journal.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c"

    compile_test test_runtime_journal \
        "$ROOT/tests/unit/test_runtime_journal.c" \
        "$ROOT/src/runtime_journal.c" \
        "$ROOT/src/text.c"

    compile_test test_wrappers \
        "$ROOT/tests/unit/test_wrappers.c" \
        "$ROOT/src/wrappers.c" \
        "$ROOT/src/package_metadata.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c"

    compile_test test_command_repair \
        "$ROOT/tests/unit/test_command_repair.c" \
        "$ROOT/src/command_repair.c" \
        "$ROOT/src/text.c"

    compile_test test_package_extract \
        "$ROOT/tests/unit/test_package_extract.c" \
        "$ROOT/src/package_archive_format.c" \
        "$ROOT/src/package_extract.c" \
        "$ROOT/src/path.c" \
        "$ROOT/src/text.c" \
        $UNIT_ARCHIVE_LIBS
fi

compile_test test_cup_update \
    "$ROOT/tests/unit/test_cup_update.c" \
    "$ROOT/src/cup_update.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_command_uninstall \
    "$ROOT/tests/unit/test_command_uninstall.c" \
    "$ROOT/src/command_uninstall.c"

printf 'All C unit-test binaries compiled for %s (%s).\n' "$PLATFORM" "$TEST_CONFIGURATION"
