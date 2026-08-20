#!/bin/sh

# Verifies test environment resolution, explicit dependency use and
# platform-profile tool preparation.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/common.sh"
. "$TESTS_ROOT/support/environment.sh"

test_begin environment

NATIVE_BUILD_PLATFORM=$(cup_test_detect_platform) ||
    fail 'could not resolve native build platform for repository tests'

# Background fixture cleanup must not wait indefinitely when a child ignores
# TERM. The shared helper escalates after a bounded grace period and reaps the
# direct child before returning.
stop_ready=$TMP_ROOT/stop-process.ready
(
    trap '' TERM
    : >"$stop_ready"
    while :; do
        sleep 10
    done
) &
stubborn_pid=$!
stop_attempt=0
while [ ! -f "$stop_ready" ] && [ "$stop_attempt" -lt 100 ]; do
    sleep 0.01
    stop_attempt=$((stop_attempt + 1))
done
[ -f "$stop_ready" ] || fail 'TERM-resistant fixture did not become ready'
stop_started=$(date +%s)
test_stop_process "$stubborn_pid"
stop_elapsed=$(($(date +%s) - stop_started))
[ "$stop_elapsed" -lt 5 ] || fail 'background fixture cleanup was not bounded'
if kill -0 "$stubborn_pid" 2>/dev/null; then
    fail 'background fixture survived bounded cleanup'
fi

create_verifier_fixture() {
    root=$1
    mkdir -p "$root/scripts/dependencies"
    cat >"$root/scripts/dependencies/verify.sh" <<'EOF_VERIFY'
#!/bin/sh
[ "$#" -eq 2 ] || exit 2
[ -f "$2/.verified-prefix" ]
EOF_VERIFY
    chmod +x "$root/scripts/dependencies/verify.sh"
}

(
    unset CUP_TEST_PLATFORM DEPS_PREFIX
    export HOME="$TMP_ROOT/home"
    export PLATFORM=linux/amd64

    uname() {
        case "$1" in
            -s)
                printf '%s\n' Linux
                ;;
            -m)
                printf '%s\n' x86_64
                ;;
            *)
                return 1
                ;;
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

# Host-provided POSIX temp aliases are resolved once before C unit fixtures use
# them; managed descendants remain subject to their normal no-follow checks.
mkdir "$TMP_ROOT/physical-temp"
ln -s "$TMP_ROOT/physical-temp" "$TMP_ROOT/temp-alias"
(
    CUP_TEST_PLATFORM=macos-x64
    TMPDIR="$TMP_ROOT/temp-alias/"
    export CUP_TEST_PLATFORM TMPDIR
    cup_test_prepare_environment
    assert_equals "$TMPDIR" "$TMP_ROOT/physical-temp"
)

if (
    CUP_TEST_PLATFORM=linux/amd64
    export CUP_TEST_PLATFORM
    cup_test_prepare_environment
) >"$TMP_ROOT/invalid.out" 2>&1; then
    fail 'invalid CUP_TEST_PLATFORM was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/invalid.out")" 'Unsupported CUP_TEST_PLATFORM'

(
    CUP_TEST_PROJECT_ROOT='C:\fixture'
    CUP_TEST_BUILD_ROOT='C:\fixture\build'
    export CUP_TEST_PROJECT_ROOT CUP_TEST_BUILD_ROOT
    cygpath() {
        [ "$1" = -u ] && [ "$2" = 'C:\fixture\build' ] || return 2
        printf '%s\n' /c/fixture/build
    }
    assert_equals "$(cup_test_build_root)" /c/fixture/build
)

CUP_TEST_PLATFORM=linux-x64
DEPS_PREFIX="$TMP_ROOT/dependencies"
CUP_TEST_PROJECT_ROOT="$TMP_ROOT/verifier-project"
export CUP_TEST_PLATFORM DEPS_PREFIX CUP_TEST_PROJECT_ROOT
create_verifier_fixture "$CUP_TEST_PROJECT_ROOT"
mkdir -p "$DEPS_PREFIX"
printf '%s\n' verified >"$DEPS_PREFIX/.verified-prefix"
cup_test_dependencies_ready || fail 'verified test dependency prefix was rejected'
rm "$DEPS_PREFIX/.verified-prefix"
if cup_test_dependencies_ready; then
    fail 'dependency verifier failure was ignored'
fi

BUILD_PROJECT="$TMP_ROOT/build-project"
CUP_TEST_PROJECT_ROOT="$BUILD_PROJECT"
CUP_TEST_BUILD_ROOT="$BUILD_PROJECT/build"
export CUP_TEST_PROJECT_ROOT CUP_TEST_BUILD_ROOT
mkdir -p "$BUILD_PROJECT/scripts/lib" "$CUP_TEST_BUILD_ROOT/reports/existing" "$TMP_ROOT/outside"
cp "$PROJECT_ROOT/scripts/lib/path-safety.sh" "$PROJECT_ROOT/scripts/lib/path-ops.sh" \
    "$PROJECT_ROOT/scripts/lib/path-ops.c" "$BUILD_PROJECT/scripts/lib/"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' >"$CUP_TEST_BUILD_ROOT/.cup-build-root"
printf '%s\n' stale >"$CUP_TEST_BUILD_ROOT/reports/existing/.stale"
printf '%s\n' keep >"$TMP_ROOT/outside/sentinel"
cup_test_reset_output_directory "$CUP_TEST_BUILD_ROOT/reports/existing"
[ -d "$CUP_TEST_BUILD_ROOT/reports/existing" ] &&
    [ ! -e "$CUP_TEST_BUILD_ROOT/reports/existing/.stale" ] ||
    fail 'owned test output was not reset exactly'
if cup_test_reset_output_directory \
    "$CUP_TEST_BUILD_ROOT/reports/../../outside" >"$TMP_ROOT/traversal.out" 2>&1; then
    fail 'test output traversal was accepted'
fi
ln -s "$TMP_ROOT/outside" "$CUP_TEST_BUILD_ROOT/link"
if cup_test_reset_output_directory \
    "$CUP_TEST_BUILD_ROOT/link/report" >"$TMP_ROOT/symlink.out" 2>&1; then
    fail 'test output symlink was followed'
fi
assert_equals "$(cat "$TMP_ROOT/outside/sentinel")" keep
mkdir -p "$TMP_ROOT/build-parent/actual-build"
printf '%s\n' \
    'format=1' \
    'product=coffee-clang/cup' \
    'kind=build-root' \
    'layout=1' >"$TMP_ROOT/build-parent/actual-build/.cup-build-root"
ln -s "$TMP_ROOT/build-parent" "$TMP_ROOT/build-parent-link"
CUP_TEST_BUILD_ROOT="$TMP_ROOT/build-parent-link/actual-build"
export CUP_TEST_BUILD_ROOT
if cup_test_build_root_owned >"$TMP_ROOT/build-root-link.out" 2>&1; then
    fail 'test build root with a symlink parent was accepted'
fi

# Integration setup removes ambient transport policy before any repair or
# download. Individual suites may set explicit values after preparation.
(
    unset CUP_TEST_PROJECT_ROOT CUP_TEST_BUILD_ROOT DEPS_PREFIX
    CUP_TEST_PLATFORM=$NATIVE_BUILD_PLATFORM
    CUP_TEST_BINARY="$TMP_ROOT/fake-cup"
    export CUP_TEST_PLATFORM CUP_TEST_BINARY
    printf '#!/bin/sh\nexit 0\n' > "$CUP_TEST_BINARY"
    chmod +x "$CUP_TEST_BINARY"
    . "$TESTS_ROOT/support/posix/cli.sh"
    CUP_INSTALL_BASE_URL=https://ambient.invalid
    CUP_INSTALL_ALLOW_INSECURE=1
    HTTP_PROXY=http://ambient.invalid
    HTTPS_PROXY=http://ambient.invalid
    ALL_PROXY=http://ambient.invalid
    NO_PROXY=ambient.invalid
    http_proxy=http://ambient.invalid
    https_proxy=http://ambient.invalid
    all_proxy=http://ambient.invalid
    no_proxy=ambient.invalid
    export CUP_INSTALL_BASE_URL CUP_INSTALL_ALLOW_INSECURE \
        HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY \
        http_proxy https_proxy all_proxy no_proxy
    prepare_command_environment
    for variable in \
        CUP_INSTALL_BASE_URL CUP_INSTALL_ALLOW_INSECURE \
        HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY \
        http_proxy https_proxy all_proxy no_proxy; do
        eval "value=\${$variable-}"
        [ -z "$value" ] || fail "ambient test variable survived: $variable"
    done
)

printf 'Source-test environment contract tests passed.\n'

printf '==> Testing macOS dependency tool preparation...\n'
FAKE_BREW_DIR="$TMP_ROOT/fake-brew"
FAKE_BREW_LOG="$TMP_ROOT/brew.log"
mkdir -p "$FAKE_BREW_DIR"
cat >"$FAKE_BREW_DIR/brew" <<EOF_BREW
#!/bin/sh
printf '%s\n' "\$*" >>'$FAKE_BREW_LOG'
case "\${1:-}" in
    list) exit 0 ;;
    install|update) exit 0 ;;
    *) exit 2 ;;
esac
EOF_BREW
chmod +x "$FAKE_BREW_DIR/brew"
cat >"$FAKE_BREW_DIR/uname" <<'EOF_UNAME'
#!/bin/sh
case "${1:-}" in
    -s) printf '%s\n' Darwin ;;
    -m) printf '%s\n' arm64 ;;
    *) exit 2 ;;
esac
EOF_UNAME
chmod +x "$FAKE_BREW_DIR/uname"
for tool in ar clang dsymutil dwarfdump gcovr lipo otool ranlib strings xcrun; do
    cat >"$FAKE_BREW_DIR/$tool" <<'EOF_TOOL'
#!/bin/sh
exit 0
EOF_TOOL
    chmod +x "$FAKE_BREW_DIR/$tool"
done


prepare_macos_profile() {
    profile=$1
    shift
    : >"$FAKE_BREW_LOG"
    PATH="$FAKE_BREW_DIR:$PATH" FAMILY=macos PLATFORM=macos-arm64 \
        "$PROJECT_ROOT/scripts/ci/prepare-posix.sh" "$profile"
    for package in "$@"; do
        grep -Fq "list --formula $package" "$FAKE_BREW_LOG" ||
            fail "macOS $profile preparation omitted $package"
    done
}

prepare_macos_profile dependencies perl pkg-config xz
if grep -Fq 'list --formula coreutils' "$FAKE_BREW_LOG"; then
    fail 'macOS dependency preparation installed source-test-only coreutils'
fi
prepare_macos_profile source coreutils perl pkg-config xz
prepare_macos_profile coverage coreutils gcovr perl pkg-config xz
prepare_macos_profile sanitizers coreutils perl pkg-config xz
prepare_macos_profile debug perl pkg-config xz
if grep -Fq 'list --formula coreutils' "$FAKE_BREW_LOG"; then
    fail 'macOS debug preparation installed source-test-only coreutils'
fi
printf 'macOS CI profile preparation tests passed.\n'

printf '==> Testing explicit dependency preparation...\n'
FAKE_ROOT="$TMP_ROOT/explicit-project"
DEPENDENCY_BUILD_LOG="$TMP_ROOT/dependency-build.log"
create_verifier_fixture "$FAKE_ROOT"
cat >"$FAKE_ROOT/scripts/dependencies/build-posix.sh" <<EOF_BOOTSTRAP
#!/bin/sh
printf 'unexpected dependency build\n' >'$DEPENDENCY_BUILD_LOG'
EOF_BOOTSTRAP
chmod +x "$FAKE_ROOT/scripts/dependencies/build-posix.sh"
CUP_TEST_PROJECT_ROOT="$FAKE_ROOT"
DEPS_PREFIX="$TMP_ROOT/missing-explicit-prefix"
export CUP_TEST_PROJECT_ROOT DEPS_PREFIX
if cup_test_require_dependencies >"$TMP_ROOT/explicit.out" 2>&1; then
    fail 'test dependency check accepted a missing prefix'
fi
[ ! -e "$DEPENDENCY_BUILD_LOG" ] || fail 'test runner started a dependency build implicitly'
assert_contains "$(cat "$TMP_ROOT/explicit.out")" "Run 'make PLATFORM=linux-x64 deps'"
mkdir -p "$DEPS_PREFIX"
printf '%s\n' verified >"$DEPS_PREFIX/.verified-prefix"
cup_test_require_dependencies || fail 'explicitly prepared test prefix was rejected'
printf 'Explicit dependency preparation tests passed.\n'

# Quality-tool guidance must follow the active Windows toolchain instead of
# recommending UCRT64 packages from the isolated CLANG64 sanitizer shell.
clang_hint=$(CUP_TEST_PLATFORM=windows-x64 MSYSTEM=CLANG64 \
    sh -c '. "$1"; cup_test_tool_hint clang' sh \
    "$TESTS_ROOT/support/environment.sh" 2>&1)
assert_contains "$clang_hint" 'Install LLVM tools in CLANG64'
assert_contains "$clang_hint" 'mingw-w64-clang-x86_64-compiler-rt'
assert_not_contains "$clang_hint" 'mingw-w64-ucrt-x86_64-compiler-rt'

printf 'Quality-tool guidance tests passed.\n'
