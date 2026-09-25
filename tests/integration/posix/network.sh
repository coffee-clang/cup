#!/bin/sh

# Verifies current package/catalog network boundaries through an explicitly allowed loopback origin.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ROOT=$(CDPATH= cd -- "$TESTS_ROOT/.." && pwd)
export TESTS_ROOT
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin network
prepare_command_environment
require_test_binary

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

oversized_catalog_path=/catalog-too-large
"$helper" http-server --root "$server_root" --port 0 \
    --ready-file "$ready_file" \
    --gzip-path "$oversized_catalog_path" --gzip-bytes 4194305 \
    >"$server_log" 2>&1 &
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

export CUP_INSTALL_ALLOW_INSECURE=1
export NO_PROXY=127.0.0.1
export no_proxy=127.0.0.1

publish_artifact() {
    version=$1
    archive=$TMP_ROOT/artifacts/clang-$version-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz
    name=$(basename "$archive")
    sha=$(hash_file "$archive")

    cp "$archive" "$server_root/$name"
    rm -f "$TEST_HOME/.cup/cache/$sha"
    package_catalog_rewrite_artifact clang "$version" tar.gz \
        "http://127.0.0.1:$port/$name" "$sha"
}

valid_version=97.0.1
make_package_format compiler clang "$valid_version" "$TEST_PLATFORM" tar.gz clang
publish_artifact "$valid_version"

printf '==> Downloading a concrete package artifact through loopback...\n'
run_cup install compiler "clang@$valid_version" >/dev/null
assert_contains "$(run_cup list compiler 2>/dev/null)" "compiler:clang@$valid_version"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

bad_version=97.0.2
make_package_format compiler clang "$bad_version" "$TEST_PLATFORM" tar.gz clang
bad_archive=$TMP_ROOT/artifacts/clang-$bad_version-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz
bad_name=$(basename "$bad_archive")
bad_real_sha=$(hash_file "$bad_archive")
bad_expected_sha=$(printf '%064d' 0)
cp "$bad_archive" "$server_root/$bad_name"
rm -f "$TEST_HOME/.cup/cache/$bad_real_sha" "$TEST_HOME/.cup/cache/$bad_expected_sha"
package_catalog_rewrite_artifact clang "$bad_version" tar.gz \
    "http://127.0.0.1:$port/$bad_name" "$bad_expected_sha"

printf '==> Rejecting an artifact whose bytes do not match the catalog digest...\n'
if run_cup install compiler "clang@$bad_version" \
        >"$TMP_ROOT/digest-mismatch.out" 2>&1; then
    fail 'package with a mismatched catalog digest was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/digest-mismatch.out")" \
    'downloaded package failed SHA-256 verification'
assert_missing "$TEST_HOME/.cup/cache/$bad_expected_sha"
assert_not_contains "$(run_cup list compiler 2>/dev/null)" \
    "compiler:clang@$bad_version"
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

printf '==> Rejecting catalog metadata whose decompressed body exceeds the metadata limit...\n'
runtime_catalog=$TEST_HOME/.cup/config/catalog.cfg
catalog_before=$(hash_file "$runtime_catalog")
temporary=$runtime_catalog.tmp
awk -v url="http://127.0.0.1:$port$oversized_catalog_path" '
    /^update_url=/ { print "update_url=" url; next }
    { print }
' "$runtime_catalog" > "$temporary"
mv "$temporary" "$runtime_catalog"
catalog_before=$(hash_file "$runtime_catalog")

if run_cup update catalog >"$TMP_ROOT/catalog-limit.out" 2>&1; then
    fail 'oversized decompressed catalog metadata was accepted'
fi
assert_contains "$(cat "$TMP_ROOT/catalog-limit.out")" \
    'download exceeded the configured size limit'
assert_equals "$catalog_before" "$(hash_file "$runtime_catalog")" \
    'failed catalog refresh changed the local snapshot'
assert_missing "$TEST_HOME/.cup/transaction.txt"
assert_cup_healthy

printf 'POSIX network integration tests passed for %s.\n' "$TEST_PLATFORM"
