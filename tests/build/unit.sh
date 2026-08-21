#!/usr/bin/env bash

# Compiles C unit-test binaries under the Makefile-owned build policy.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
PERSISTENT_FILE_FIXTURE="$ROOT/tests/unit/persistent_file_fixture.c"

test_applies_to_platform() {
    local name=$1
    local platform=$2

    case "$name:$platform" in
        test_storage:windows-x64)
            return 1
            ;;
        test_system_windows:windows-x64|test_storage_windows:windows-x64)
            return 0
            ;;
        test_system_windows:*|test_storage_windows:*)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

list_registered_tests() {
    local platform=$1
    local name
    awk '/^[[:space:]]*compile_test[[:space:]]+test_[A-Za-z0-9_]+/ { print $2 }' "$0" |
        while IFS= read -r name; do
            test_applies_to_platform "$name" "$platform" || continue
            case "$platform" in
                windows-x64) printf '%s.exe\n' "$name" ;;
                *) printf '%s\n' "$name" ;;
            esac
        done
}

if [ "${1:-}" = --list ]; then
    list_platform=${2:-${CUP_TEST_PLATFORM:-}}
    case "$list_platform" in
        linux-x64|linux-arm64|macos-x64|macos-arm64|windows-x64) ;;
        *)
            printf 'Usage: %s --list <platform>\n' "$0" >&2
            exit 2
            ;;
    esac
    list_registered_tests "$list_platform"
    exit 0
fi

. "$ROOT/tests/support/environment.sh"
cup_test_prepare_environment
cup_test_require_dependencies

# Build configuration and dependency selection. Make owns all platform and
# configuration flags so unit tests cannot drift from the production policy.
PLATFORM="$CUP_TEST_PLATFORM"
CC="${CC:-}"
[ -n "$CC" ] || {
    printf 'CC must be provided by the Makefile.\n' >&2
    exit 2
}
TEST_CONFIGURATION="${CUP_TEST_CONFIGURATION:-development}"
case "$TEST_CONFIGURATION" in
    development|debug|coverage|sanitizers|release) ;;
    *)
        printf 'Unsupported unit-test configuration: %s\n' "$TEST_CONFIGURATION" >&2
        exit 2
        ;;
esac
: "${CUP_TEST_CPPFLAGS?CUP_TEST_CPPFLAGS must be provided by the Makefile}"
: "${CUP_TEST_CFLAGS?CUP_TEST_CFLAGS must be provided by the Makefile}"
: "${CUP_TEST_LDFLAGS?CUP_TEST_LDFLAGS must be provided by the Makefile}"
read -a TEST_CPPFLAGS <<<"$CUP_TEST_CPPFLAGS"
read -a TEST_CFLAGS <<<"$CUP_TEST_CFLAGS"
read -a TEST_LDFLAGS <<<"$CUP_TEST_LDFLAGS"
TEST_BUILD_ROOT=$(cup_test_build_root) || exit 2
TEST_BUILD_FINAL="$TEST_BUILD_ROOT/$PLATFORM/$TEST_CONFIGURATION/tests/unit"
TEST_BUILD_PARENT=${TEST_BUILD_FINAL%/unit}
COVERAGE_ENTRY_SOURCE=
case "$PLATFORM:$TEST_CONFIGURATION" in
    macos-*:coverage)
        COVERAGE_ENTRY_SOURCE="$ROOT/tests/helpers/coverage-entry.c"
        ;;
esac
UNITY_LIB=$(cup_test_find_static_library unity) || {
    printf 'Unity static library was not found in %s.\n' "$DEPS_PREFIX" >&2
    exit 1
}
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
UNIT_CURL_LIBS=$("$DEPS_PREFIX/bin/curl-config" --static-libs) || {
    printf 'Pinned curl metadata was not usable in %s.\n' "$DEPS_PREFIX" >&2
    exit 1
}
[ -n "$UNIT_CURL_LIBS" ] || {
    printf 'Pinned curl metadata was empty in %s.\n' "$DEPS_PREFIX" >&2
    exit 1
}

cup_test_load_path_safety || exit 1
cup_test_build_root_owned || exit 1
cup_path_prepare_child_directory "$TEST_BUILD_ROOT" "$TEST_BUILD_PARENT" \
    'unit-test output parent' || exit 1
TEST_BUILD_DIR=$(cup_path_create_unique_directory \
    "$TEST_BUILD_PARENT/.unit.XXXXXX" 'unit-test staging' 0755) || exit 1
GCOV_OUTPUT_DIR=
GCOV_PROFILE_DIR=
GCOV_PROFILE_PREFIX=
case "$PLATFORM:$TEST_CONFIGURATION" in
    linux-*:coverage)
        GCOV_OUTPUT_DIR=$(realpath --relative-to="$ROOT" "$TEST_BUILD_DIR") || exit 1
        GCOV_PROFILE_DIR=$TEST_BUILD_FINAL
        GCOV_PROFILE_PREFIX="$ROOT/$GCOV_OUTPUT_DIR"
        ;;
    windows-x64:coverage)
        GCOV_OUTPUT_DIR=$(realpath --relative-to="$ROOT" "$TEST_BUILD_DIR") || exit 1
        ;;
esac
cleanup_unit_staging() {
    if [ -n "${TEST_BUILD_DIR:-}" ] &&
        { [ -e "$TEST_BUILD_DIR" ] || [ -L "$TEST_BUILD_DIR" ]; }; then
        cup_path_remove_child_tree "$TEST_BUILD_ROOT" "$TEST_BUILD_DIR" \
            'unit-test staging' >/dev/null 2>&1 || true
    fi
}
trap cleanup_unit_staging EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Compile one registered Unity suite with the same project flags and pinned libraries as cup.
compile_test() {
    name=$1
    shift
    test_applies_to_platform "$name" "$PLATFORM" || return 0
    output="$TEST_BUILD_DIR/$name"
    output_arg=$output
    [ -z "$GCOV_OUTPUT_DIR" ] || output_arg="$GCOV_OUTPUT_DIR/$name"
    compile_args=()
    for compile_arg in "$@"; do
        case "$compile_arg" in
            "$ROOT"/*) compile_args+=("${compile_arg#"$ROOT"/}") ;;
            *) compile_args+=("$compile_arg") ;;
        esac
    done
    printf '==> Compiling C unit test: %s\n' "$name"
    compile_command=("$CC" "${TEST_CPPFLAGS[@]}" "${TEST_CFLAGS[@]}")
    if [ -n "$GCOV_PROFILE_DIR" ]; then
        compile_command+=(
            "-fprofile-dir=$GCOV_PROFILE_DIR"
            "-fprofile-prefix-path=$GCOV_PROFILE_PREFIX"
        )
    fi
    if [ -n "$COVERAGE_ENTRY_SOURCE" ]; then
        coverage_body="cup_coverage_${name}_main"
        coverage_entry="cup_coverage_${name}_entry"
        coverage_entry_source=${COVERAGE_ENTRY_SOURCE#"$ROOT"/}
        (cd "$ROOT" && "${compile_command[@]}" \
            "-Dmain=$coverage_body" \
            "-DCUP_COVERAGE_VOID_ENTRY=$coverage_body" \
            "-DCUP_COVERAGE_ENTRY=$coverage_entry" \
            -I"$ROOT/tests/unit/fixtures" \
            -I"$ROOT/include" -I"$DEPS_PREFIX/include" \
            "${compile_args[@]}" "$coverage_entry_source" \
            "$UNITY_LIB" "${TEST_LDFLAGS[@]}" -o "$output_arg")
    else
        (cd "$ROOT" && "${compile_command[@]}" \
            -I"$ROOT/tests/unit/fixtures" \
            -I"$ROOT/include" -I"$DEPS_PREFIX/include" \
            "${compile_args[@]}" "$UNITY_LIB" "${TEST_LDFLAGS[@]}" -o "$output_arg")
    fi
}

# Suite registration remains explicit; --list and compilation use the same
# platform applicability predicate. Platform-neutral suites compile everywhere.
compile_test test_command_queries \
    "$ROOT/tests/unit/test_command_queries.c" \
    "$ROOT/src/command_list.c" \
    "$ROOT/src/command_default.c" \
    "$ROOT/src/command_info.c" \
    "$ROOT/src/command_search.c" \
    "$ROOT/src/command_inspect.c"

compile_test test_package_transaction \
    "$ROOT/tests/unit/test_package_transaction.c" \
    "$ROOT/src/package_transaction.c" \
    "$ROOT/src/runtime_journal.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_cup_update_journal \
    "$ROOT/tests/unit/test_cup_update_journal.c" \
    "$ROOT/src/cup_update_journal.c" \
    "$ROOT/src/release_metadata.c" \
    "$ROOT/src/runtime_journal.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/third_party/sha256.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_cup_update_helper \
    "$ROOT/tests/unit/test_cup_update_helper.c" \
    "$ROOT/src/cup_update_helper.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_runtime_journal \
    "$ROOT/tests/unit/test_runtime_journal.c" \
    "$ROOT/src/runtime_journal.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_uninstall_journal \
    "$ROOT/tests/unit/test_uninstall_journal.c" \
    "$ROOT/src/uninstall_journal.c" \
    "$ROOT/src/runtime_journal.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_command_repair \
    "$ROOT/tests/unit/test_command_repair.c" \
    "$ROOT/src/command_repair.c" \
    "$ROOT/src/text.c"


compile_test test_exit_status \
    "$ROOT/tests/unit/test_exit_status.c" \
    "$ROOT/src/exit_status.c"

compile_test test_release_metadata \
    "$ROOT/tests/unit/test_release_metadata.c" \
    "$ROOT/src/release_metadata.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_text \
    "$ROOT/tests/unit/test_text.c" \
    "$ROOT/src/text.c"

compile_test test_path \
    "$ROOT/tests/unit/test_path.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_package_selector \
    "$ROOT/tests/unit/test_package_selector.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/package_selector.c"

compile_test test_package_request \
    "$ROOT/tests/unit/test_package_request.c" \
    "$ROOT/src/package_request.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c"

compile_test test_package_metadata \
    "$ROOT/tests/unit/test_package_metadata.c" \
    "$ROOT/src/package_metadata.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_checksum \
    "$ROOT/tests/unit/test_checksum.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/third_party/sha256.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/path.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_package_catalog \
    "$ROOT/tests/unit/test_package_catalog.c" \
    "$ROOT/src/package_catalog.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/third_party/sha256.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_install_policy \
    "$ROOT/tests/unit/test_install_policy.c" \
    "$ROOT/src/install_policy.c" \
    "$ROOT/src/tool_preferences.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

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
    "$ROOT/src/package_archive_format.c" \
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

compile_test test_interrupt \
    "$ROOT/tests/unit/test_interrupt.c" \
    "$ROOT/src/interrupt.c"

case "$PLATFORM" in
    windows-x64)
        PACKAGE_SYSTEM_SOURCE="$ROOT/src/system_windows.c"
        PACKAGE_SYSTEM_LIBS=-ladvapi32
        ;;
    *)
        PACKAGE_SYSTEM_SOURCE="$ROOT/src/system_posix.c"
        PACKAGE_SYSTEM_LIBS=
        ;;
esac
compile_test test_package \
    "$ROOT/tests/unit/test_package.c" \
    "$ROOT/src/package.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/package_metadata.c" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$ROOT/src/system.c" \
    "$PACKAGE_SYSTEM_SOURCE" \
    -DCUP_PERSISTENT_FIXTURE_NATIVE_SYSTEM \
    "$PERSISTENT_FILE_FIXTURE" \
    $PACKAGE_SYSTEM_LIBS

compile_test test_package_artifact \
    "$ROOT/tests/unit/test_package_artifact.c" \
    "$ROOT/src/package_artifact.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_package_cache \
    "$ROOT/tests/unit/test_package_cache.c" \
    "$ROOT/src/download.c" \
    "$ROOT/src/package_cache.c" \
    "$ROOT/src/layout.c" \
    "$ROOT/src/filesystem.c" \
    "$ROOT/src/system.c" \
    "$PACKAGE_SYSTEM_SOURCE" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    $PACKAGE_SYSTEM_LIBS

compile_test test_download_url \
    "$ROOT/tests/unit/test_download_url.c" \
    "$ROOT/src/download_url.c" \
    $UNIT_CURL_LIBS

compile_test test_package_archive \
    "$ROOT/tests/unit/test_package_archive.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/package_archive.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/system.c" \
    "$PACKAGE_SYSTEM_SOURCE" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    $PACKAGE_SYSTEM_LIBS \
    $UNIT_ARCHIVE_LIBS

compile_test test_package_extract \
    "$ROOT/tests/unit/test_package_extract.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/package_extract.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    $UNIT_ARCHIVE_LIBS

compile_test test_package_extract_registration \
    "$ROOT/tests/unit/test_package_extract_registration.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    $UNIT_ARCHIVE_LIBS

compile_test test_storage \
    "$ROOT/tests/unit/test_storage.c" \
    "$ROOT/tests/unit/test_system_posix.c" \
    "$ROOT/tests/unit/test_filesystem.c" \
    "$ROOT/tests/unit/test_layout.c" \
    "$ROOT/src/layout.c" \
    "$ROOT/src/filesystem.c" \
    "$ROOT/src/system.c" \
    "$ROOT/src/system_posix.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/third_party/sha256.c" \
    "$ROOT/src/package_catalog.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/install_policy.c" \
    "$ROOT/src/tool_preferences.c" \
    "$ROOT/src/state.c" \
    "$ROOT/src/package.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/package_metadata.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

compile_test test_system_windows \
    "$ROOT/tests/unit/test_system_windows.c" \
    "$ROOT/src/system.c" \
    "$ROOT/src/system_windows.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    -ladvapi32

compile_test test_storage_windows \
    "$ROOT/tests/unit/test_storage_windows.c" \
    "$ROOT/tests/unit/test_filesystem.c" \
    "$ROOT/tests/unit/test_layout.c" \
    "$ROOT/src/layout.c" \
    "$ROOT/src/filesystem.c" \
    "$ROOT/src/system.c" \
    "$ROOT/src/system_windows.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/third_party/sha256.c" \
    "$ROOT/src/package_catalog.c" \
    "$ROOT/src/package_archive_format.c" \
    "$ROOT/src/install_policy.c" \
    "$ROOT/src/tool_preferences.c" \
    "$ROOT/src/state.c" \
    "$ROOT/src/package.c" \
    "$ROOT/src/package_selector.c" \
    "$ROOT/src/package_metadata.c" \
    "$ROOT/src/registry.c" \
    "$ROOT/src/platform.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    -ladvapi32
compile_test test_wrappers \
    "$ROOT/tests/unit/test_wrappers.c" \
    "$ROOT/src/wrappers.c" \
    "$ROOT/src/package_metadata.c" \
    "$ROOT/src/checksum.c" \
    "$ROOT/src/third_party/sha256.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

case "$PLATFORM" in
    windows-x64)
        STATE_SYSTEM_SOURCE="$ROOT/src/system_windows.c"
        STATE_SYSTEM_LIBS=-ladvapi32
        ;;
    *)
        STATE_SYSTEM_SOURCE="$ROOT/src/system_posix.c"
        STATE_SYSTEM_LIBS=
        ;;
esac
compile_test test_state \
    "$ROOT/tests/unit/test_state.c" \
    "$ROOT/src/state.c" \
    "$ROOT/src/filesystem.c" \
    "$ROOT/src/interrupt.c" \
    "$ROOT/src/system.c" \
    "$STATE_SYSTEM_SOURCE" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    $STATE_SYSTEM_LIBS

compile_test test_cup_update \
    "$ROOT/tests/unit/test_cup_update.c" \
    "$ROOT/src/cup_update.c" \
    "$ROOT/src/release_metadata.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c" \
    "$PERSISTENT_FILE_FIXTURE"

compile_test test_command_uninstall \
    "$ROOT/tests/unit/test_command_uninstall.c" \
    "$ROOT/src/command_uninstall.c" \
    "$ROOT/src/path.c" \
    "$ROOT/src/text.c"

expected_list="$TEST_BUILD_DIR/.expected-tests"
actual_list="$TEST_BUILD_DIR/.actual-tests"
list_registered_tests "$PLATFORM" | LC_ALL=C sort > "$expected_list"
: > "$actual_list"
for test_binary in "$TEST_BUILD_DIR"/test_*; do
    [ -f "$test_binary" ] || continue
    case "$test_binary" in
        *.gcda|*.gcno) continue ;;
    esac
    [ -x "$test_binary" ] || {
        printf 'Compiled unit-test output is not executable: %s\n' "$test_binary" >&2
        exit 1
    }
    basename "$test_binary" >> "$actual_list"
done
LC_ALL=C sort -o "$actual_list" "$actual_list"
if [ "$(cat "$expected_list")" != "$(cat "$actual_list")" ]; then
    printf 'Expected unit-test binaries:\n' >&2
    cat "$expected_list" >&2
    printf 'Compiled unit-test binaries:\n' >&2
    cat "$actual_list" >&2
    exit 1
fi
rm -f -- "$expected_list" "$actual_list"

if [ -e "$TEST_BUILD_FINAL" ] || [ -L "$TEST_BUILD_FINAL" ]; then
    cup_path_check_directory_chain "$TEST_BUILD_FINAL" 0 \
        'previous unit-test output' || exit 1
    cup_path_remove_child_tree "$TEST_BUILD_ROOT" "$TEST_BUILD_FINAL" \
        'previous unit-test output' || exit 1
fi
cup_path_move_entry "$TEST_BUILD_DIR" "$TEST_BUILD_FINAL" ||
    { printf 'Could not publish complete unit-test output.\n' >&2; exit 1; }
TEST_BUILD_DIR=
trap - EXIT HUP INT TERM
printf 'All C unit-test binaries compiled for %s (%s).\n' "$PLATFORM" "$TEST_CONFIGURATION"
