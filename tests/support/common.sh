
# Provides POSIX test assertions, hashing, temporary directories and cleanup to sourced test scripts.

: "${TESTS_ROOT:?TESTS_ROOT must be set before sourcing tests/support/common.sh}"
PROJECT_ROOT=$(CDPATH= cd -- "$TESTS_ROOT/.." && pwd)
export PROJECT_ROOT

fail() {
    printf 'TEST FAILED: %s\n' "$*" >&2
    exit 1
}

assert_contains() (
    haystack=$1
    needle=$2
    printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null 2>&1 ||
        fail "expected output to contain: $needle"
)

assert_not_contains() (
    haystack=$1
    needle=$2
    if printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null 2>&1; then
        fail "expected output not to contain: $needle"
    fi
)

assert_file() {
    [ -f "$1" ] && [ ! -L "$1" ] ||
        fail "expected regular non-symlink file: $1"
}

assert_missing() {
    [ ! -e "$1" ] && [ ! -L "$1" ] || fail "expected missing path: $1"
}

assert_equals() (
    actual=$1
    expected=$2
    [ "$actual" = "$expected" ] ||
        fail "expected '$expected', got '$actual'"
)

test_cleanup_root() {
    [ -n "${TMP_ROOT:-}" ] || return 0
    case "$TMP_ROOT" in
        /|"")
            return 1
            ;;
    esac
    if [ -L "$TMP_ROOT" ]; then
        rm -f -- "$TMP_ROOT"
    elif [ -d "$TMP_ROOT" ]; then
        rm -rf -- "$TMP_ROOT"
    elif [ -e "$TMP_ROOT" ]; then
        rm -f -- "$TMP_ROOT"
    fi
}

test_stop_process() {
    cup_test_stop_pid=${1:-}
    [ -n "$cup_test_stop_pid" ] || return 0

    if kill -0 "$cup_test_stop_pid" 2>/dev/null; then
        kill "$cup_test_stop_pid" 2>/dev/null || true
        cup_test_stop_attempts=0
        while kill -0 "$cup_test_stop_pid" 2>/dev/null &&
            [ "$cup_test_stop_attempts" -lt 20 ]; do
            sleep 0.05
            cup_test_stop_attempts=$((cup_test_stop_attempts + 1))
        done
        if kill -0 "$cup_test_stop_pid" 2>/dev/null; then
            kill -KILL "$cup_test_stop_pid" 2>/dev/null || true
        fi
    fi
    wait "$cup_test_stop_pid" 2>/dev/null || true
    unset cup_test_stop_pid cup_test_stop_attempts
}

test_process_group_helper() {
    cup_test_group_configuration=${CUP_TEST_CONFIGURATION:-development}
    cup_test_group_build_root=${CUP_TEST_BUILD_ROOT:-$PROJECT_ROOT/build}
    printf '%s\n' \
        "$cup_test_group_build_root/$TEST_PLATFORM/$cup_test_group_configuration/tests/helpers/process-group"
}

test_start_process_group() {
    cup_test_group_helper=$(test_process_group_helper) || return 1
    [ -x "$cup_test_group_helper" ] || fail "process-group fixture is unavailable: $cup_test_group_helper"
    "$cup_test_group_helper" run "$@" &
    CUP_TEST_PROCESS_GROUP_PID=$!

    cup_test_group_attempts=0
    while ! "$cup_test_group_helper" alive "$CUP_TEST_PROCESS_GROUP_PID" 2>/dev/null; do
        if ! kill -0 "$CUP_TEST_PROCESS_GROUP_PID" 2>/dev/null; then
            wait "$CUP_TEST_PROCESS_GROUP_PID" 2>/dev/null || true
            fail "process-group fixture exited before creating its process group"
        fi
        [ "$cup_test_group_attempts" -lt 20 ] || {
            kill -KILL "$CUP_TEST_PROCESS_GROUP_PID" 2>/dev/null || true
            wait "$CUP_TEST_PROCESS_GROUP_PID" 2>/dev/null || true
            fail "process-group fixture did not create its process group"
        }
        sleep 0.05
        cup_test_group_attempts=$((cup_test_group_attempts + 1))
    done

    export CUP_TEST_PROCESS_GROUP_PID
    unset cup_test_group_helper cup_test_group_attempts
}

test_stop_process_group() {
    cup_test_group_pid=${1:-}
    [ -n "$cup_test_group_pid" ] || return 0
    cup_test_group_helper=$(test_process_group_helper) || return 1

    if "$cup_test_group_helper" alive "$cup_test_group_pid" 2>/dev/null; then
        "$cup_test_group_helper" term "$cup_test_group_pid" 2>/dev/null || true
        cup_test_group_attempts=0
        while "$cup_test_group_helper" alive "$cup_test_group_pid" 2>/dev/null &&
            [ "$cup_test_group_attempts" -lt 20 ]; do
            sleep 0.05
            cup_test_group_attempts=$((cup_test_group_attempts + 1))
        done
        if "$cup_test_group_helper" alive "$cup_test_group_pid" 2>/dev/null; then
            "$cup_test_group_helper" kill "$cup_test_group_pid" 2>/dev/null || true
        fi
    fi
    wait "$cup_test_group_pid" 2>/dev/null || true
    cup_test_group_attempts=0
    while "$cup_test_group_helper" alive "$cup_test_group_pid" 2>/dev/null &&
        [ "$cup_test_group_attempts" -lt 20 ]; do
        sleep 0.05
        cup_test_group_attempts=$((cup_test_group_attempts + 1))
    done
    if "$cup_test_group_helper" alive "$cup_test_group_pid" 2>/dev/null; then
        fail "process group remained alive after cleanup: $cup_test_group_pid"
    fi
    unset cup_test_group_pid cup_test_group_helper cup_test_group_attempts
}

test_exit_handler() {
    status=$?
    trap - 0 HUP INT TERM
    test_cleanup_root || status=1
    exit "$status"
}

test_signal_handler() {
    status=$1
    trap - 0 HUP INT TERM
    test_cleanup_root || :
    exit "$status"
}

test_begin() {
    name=$1
    temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/cup-$name-tests.XXXXXX") ||
        fail "failed to create temporary test directory"

    TMP_ROOT=$(CDPATH= cd -- "$temporary_root" && pwd -P) || {
        rm -rf -- "$temporary_root"
        fail "failed to resolve temporary test directory"
    }

    export TMP_ROOT
    trap test_exit_handler 0
    trap 'test_signal_handler 129' HUP
    trap 'test_signal_handler 130' INT
    trap 'test_signal_handler 143' TERM
}

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}

hash_text() {
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        printf '%s' "$1" | shasum -a 256 | awk '{print $1}'
    else
        fail 'neither sha256sum nor shasum is available'
    fi
}
