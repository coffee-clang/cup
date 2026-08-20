#!/bin/sh

# Verifies package download through a local hostname and checksum rejection on POSIX.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOT=$(CDPATH= cd -- "$TESTS_ROOT/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin network
prepare_command_environment
require_test_binary
# Initialize the isolated runtime; repair behavior is owned by repair.sh.
run_cup repair >/dev/null

configuration=${CUP_TEST_CONFIGURATION:-development}
test_build_root=${CUP_TEST_BUILD_ROOT:-$ROOT/build}
helper="$test_build_root/$TEST_PLATFORM/$configuration/tests/helpers/network-helper"
assert_file "$helper"

server_root=$TMP_ROOT/server
ready_file=$TMP_ROOT/server.ready
server_log=$TMP_ROOT/server.log
mkdir -p "$server_root"
server_pid=

cleanup_network_processes() {
    test_stop_process "$server_pid"
}
network_exit_handler() {
    status=$?
    trap - 0 HUP INT TERM
    cleanup_network_processes
    test_cleanup_root || status=1
    exit "$status"
}
network_signal_handler() {
    status=$1
    trap - 0 HUP INT TERM
    cleanup_network_processes
    test_cleanup_root || :
    exit "$status"
}
trap network_exit_handler 0
trap 'network_signal_handler 129' HUP
trap 'network_signal_handler 130' INT
trap 'network_signal_handler 143' TERM

"$helper" http-server --root "$server_root" --port 0 \
    --ready-file "$ready_file" >"$server_log" 2>&1 &
server_pid=$!

attempt=0
while [ ! -s "$ready_file" ] && [ "$attempt" -lt 100 ]; do
    kill -0 "$server_pid" >/dev/null 2>&1 || {
        cat "$server_log" >&2 || true
        fail 'local HTTP fixture stopped before becoming ready'
    }
    attempt=$((attempt + 1))
    sleep 0.05
done
[ -s "$ready_file" ] || fail 'local HTTP fixture did not become ready'
port=$(sed -n '1p' "$ready_file")
case "$port" in
    ''|*[!0-9]*) fail "invalid local HTTP port: $port" ;;
esac

set_network_catalog() {
    catalog=$DEV_ROOT/config/packages.cfg
    temporary=$catalog.tmp
    awk -v platform="$TEST_PLATFORM" -v port="$port" '
        $0 ~ "^compiler\\.clang\\." platform "\\." platform "\\.url_template=" {
            print "compiler.clang." platform "." platform ".url_template=" \
                "http://localhost:" port "/{version}-{host_platform}-{target_platform}/" \
                "clang-{version}-{host_platform}-{target_platform}.{format}"
            next
        }
        $0 ~ "^compiler\\.clang\\." platform "\\." platform "\\.checksum_url_template=" {
            print "compiler.clang." platform "." platform ".checksum_url_template=" \
                "http://localhost:" port "/{version}-{host_platform}-{target_platform}/SHA256SUMS"
            next
        }
        { print }
    ' "$catalog" > "$temporary"
    mv "$temporary" "$catalog"
}

publish_package() {
    version=$1
    make_package_format compiler clang "$version" "$TEST_PLATFORM" tar.gz clang
    package_name=clang-$version-$TEST_PLATFORM-$TEST_PLATFORM
    cache_dir=$TEST_HOME/.cup/cache/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/$version
    release_dir=$server_root/$version-$TEST_PLATFORM-$TEST_PLATFORM
    mkdir -p "$release_dir"
    cp "$cache_dir/$package_name.tar.gz" "$release_dir/$package_name.tar.gz"
    cp "$cache_dir/SHA256SUMS" "$release_dir/SHA256SUMS"
    rm -rf "$cache_dir"
}

set_network_catalog
valid_version=97.0.1
package_catalog_edit compiler clang "$TEST_PLATFORM" available_versions \
    "$valid_version" prepend
package_catalog_edit compiler clang "$TEST_PLATFORM" default_format tar.gz replace
publish_package "$valid_version"

export CUP_INSTALL_ALLOW_INSECURE=1
export NO_PROXY=localhost,127.0.0.1
export no_proxy=localhost,127.0.0.1

printf '==> Downloading a package through the local hostname...\n'
run_cup install compiler "clang@$valid_version" >/dev/null

bad_version=97.0.2
package_catalog_edit compiler clang "$TEST_PLATFORM" available_versions \
    "$bad_version" prepend
publish_package "$bad_version"
printf '%064d  %s\n' 0 \
    "clang-$bad_version-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz" \
    > "$server_root/$bad_version-$TEST_PLATFORM-$TEST_PLATFORM/SHA256SUMS"

printf '==> Rejecting a package whose downloaded checksum does not match...\n'
if run_cup install compiler "clang@$bad_version" \
        >"$TMP_ROOT/checksum-mismatch.out" 2>&1; then
    fail 'package with a mismatched downloaded checksum was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/checksum-mismatch.out")" \
    'downloaded package failed SHA-256 verification'
bad_cache=$TEST_HOME/.cup/cache/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/$bad_version
assert_missing "$bad_cache/clang-$bad_version-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz"
assert_not_contains "$(run_cup list compiler 2>/dev/null || true)" \
    "compiler:clang@$bad_version"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

printf 'POSIX network integration tests passed for %s.\n' "$TEST_PLATFORM"
