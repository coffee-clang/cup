
# Purpose: Sourced POSIX test library for assertions, hashing, temporary directories and cleanup.

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
    [ -f "$1" ] || fail "expected file: $1"
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

test_begin() {
    name=$1
    temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/cup-$name-tests.XXXXXX") ||
        fail "failed to create temporary test directory"

    TMP_ROOT=$(CDPATH= cd -- "$temporary_root" && pwd -P) || {
        rm -rf "$temporary_root"
        fail "failed to resolve temporary test directory"
    }

    export TMP_ROOT
    trap 'rm -rf "$TMP_ROOT"' 0 HUP INT TERM
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
