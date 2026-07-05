#!/bin/sh

# Shared helpers for the POSIX test suites. This file is sourced, not executed.

: "${TEST_SCRIPT_DIR:=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}"
PROJECT_ROOT=$(CDPATH= cd -- "$TEST_SCRIPT_DIR/../../.." && pwd)

fail() {
    printf 'TEST FAILED: %s\n' "$*" >&2
    exit 1
}

assert_contains() {
    haystack=$1
    needle=$2
    printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null 2>&1 ||
        fail "expected output to contain: $needle"
}

assert_not_contains() {
    haystack=$1
    needle=$2
    if printf '%s\n' "$haystack" | grep -F -- "$needle" >/dev/null 2>&1; then
        fail "expected output not to contain: $needle"
    fi
}

assert_file() {
    [ -f "$1" ] || fail "expected file: $1"
}

assert_missing() {
    [ ! -e "$1" ] && [ ! -L "$1" ] || fail "expected missing path: $1"
}

assert_equals() {
    actual=$1
    expected=$2
    [ "$actual" = "$expected" ] ||
        fail "expected '$expected', got '$actual'"
}

detect_test_platform() {
    os=$(uname -s)
    arch=$(uname -m)

    case "$os" in
        Linux) os=linux ;;
        Darwin) os=macos ;;
        *) fail "unsupported POSIX test operating system: $os" ;;
    esac

    case "$arch" in
        x86_64|amd64) arch=x64 ;;
        arm64|aarch64) arch=arm64 ;;
        *) fail "unsupported POSIX test architecture: $arch" ;;
    esac

    printf '%s-%s\n' "$os" "$arch"
}

TEST_PLATFORM=${CUP_TEST_PLATFORM:-$(detect_test_platform)}
TEST_BINARY=${CUP_TEST_BINARY:-$PROJECT_ROOT/build/$TEST_PLATFORM/dynamic/bin/cup}
export PROJECT_ROOT TEST_PLATFORM TEST_BINARY

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

prepare_command_environment() {
    TEST_HOME=$TMP_ROOT/home
    DEV_ROOT=$TMP_ROOT/development-root
    CUP=$TEST_BINARY
    export TEST_HOME DEV_ROOT CUP

    assert_file "$CUP"
    mkdir -p "$TEST_HOME" "$DEV_ROOT/config" "$DEV_ROOT/scripts/install"
    cp "$PROJECT_ROOT/config/packages.cfg" "$DEV_ROOT/config/packages.cfg"

    if [ "${TEST_PLATFORM%%-*}" = windows ]; then
        fail 'POSIX command environment cannot target Windows'
    fi

    cp "$PROJECT_ROOT/scripts/install/uninstall-cup.sh" \
        "$DEV_ROOT/scripts/install/uninstall-cup.sh"
    chmod +x "$DEV_ROOT/scripts/install/uninstall-cup.sh"
}

run_cup() {
    (cd "$DEV_ROOT" && HOME="$TEST_HOME" "$CUP" "$@")
}

run_cup_expect_failure() {
    output_file=$1
    shift
    if run_cup "$@" >"$output_file" 2>&1; then
        fail "command unexpectedly succeeded: cup $*"
    fi
}

hash_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

hash_text() {
    if command -v sha256sum >/dev/null 2>&1; then
        printf '%s' "$1" | sha256sum | awk '{print $1}'
    else
        printf '%s' "$1" | shasum -a 256 | awk '{print $1}'
    fi
}

manifest_add_version() {
    component=$1
    tool=$2
    target=$3
    version=$4
    manifest=$DEV_ROOT/config/packages.cfg
    key="$component.$tool.$TEST_PLATFORM.$target.available_versions"
    temporary=$manifest.tmp

    awk -v key="$key" -v version="$version" '
        BEGIN { found = 0 }
        index($0, key "=") == 1 {
            value = substr($0, length(key) + 2)
            print key "=" version "," value
            found = 1
            next
        }
        { print }
        END { if (!found) exit 2 }
    ' "$manifest" > "$temporary" || {
        rm -f "$temporary"
        fail "manifest entry not found: $key"
    }
    mv "$temporary" "$manifest"
}

make_package() {
    component=$1
    tool=$2
    version=$3
    target=$4
    shift 4

    host=$TEST_PLATFORM
    package_name=$tool-$version-$host-$target
    package_root=$TMP_ROOT/packages/$package_name
    cache_dir=$TEST_HOME/.cup/cache/$component/$tool/$host/$target/$version
    archive=$cache_dir/$package_name.tar.gz

    rm -rf "$package_root"
    mkdir -p "$package_root/bin" "$cache_dir"
    {
        printf 'package.component=%s\n' "$component"
        printf 'package.tool=%s\n' "$tool"
        printf 'package.version=%s\n' "$version"
        printf 'platform.host=%s\n' "$host"
        printf 'platform.target=%s\n' "$target"
        for entry in "$@"; do
            printf 'entry.%s=bin/%s\n' "$entry" "$entry"
        done
    } > "$package_root/info.txt"

    for entry in "$@"; do
        cat > "$package_root/bin/$entry" <<SCRIPT
#!/bin/sh
printf '%s\\n' '$tool-$version-$target:$entry'
SCRIPT
        chmod +x "$package_root/bin/$entry"
    done

    tar -czf "$archive" -C "$TMP_ROOT/packages" "$package_name"
    {
        printf '%s  %s\n' "$(hash_file "$archive")" "$(basename "$archive")"
    } > "$cache_dir/SHA256SUMS"
}

native_entrypoint_path() {
    printf '%s/.cup/bin/%s\n' "$TEST_HOME" "$1"
}

run_native_entrypoint() {
    entry=$1
    shift
    HOME="$TEST_HOME" "$(native_entrypoint_path "$entry")" "$@"
}

require_test_binary() {
    assert_file "$TEST_BINARY"
    [ -x "$TEST_BINARY" ] || fail "test binary is not executable: $TEST_BINARY"
}
