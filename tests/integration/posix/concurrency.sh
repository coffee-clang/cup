#!/bin/sh

# Exercises a synchronized overlapping install and verifies that an
# active operation blocks a second mutation without corrupting final state.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin concurrency
prepare_command_environment
run_cup repair >/dev/null
make_package compiler clang 22.1.5 "$TEST_PLATFORM" clang clang++

configuration=${CUP_TEST_CONFIGURATION:-development}
test_build_root=${CUP_TEST_BUILD_ROOT:-$PROJECT_ROOT/build}
helper=$test_build_root/$TEST_PLATFORM/$configuration/tests/helpers/network-helper
server_root=$TMP_ROOT/http-root
ready=$TMP_ROOT/http-ready
request_ready=$TMP_ROOT/http-request-ready
server_log=$TMP_ROOT/http-server.log
port=0
server_pid=
pid_a=

cleanup_concurrency_processes() {
    test_stop_process "$pid_a"
    test_stop_process "$server_pid"
}
concurrency_exit_handler() {
    status=$?
    trap - EXIT HUP INT TERM
    cleanup_concurrency_processes
    test_cleanup_root || status=1
    exit "$status"
}
concurrency_signal_handler() {
    status=$1
    trap - EXIT HUP INT TERM
    cleanup_concurrency_processes
    test_cleanup_root || :
    exit "$status"
}
trap concurrency_exit_handler EXIT
trap 'concurrency_signal_handler 129' HUP
trap 'concurrency_signal_handler 130' INT
trap 'concurrency_signal_handler 143' TERM

assert_file "$helper"
process_group_helper=$(test_process_group_helper)
assert_file "$process_group_helper"
test_start_process_group sh -c 'trap "" TERM; sleep 30 & wait'
process_tree_pid=$CUP_TEST_PROCESS_GROUP_PID
test_stop_process_group "$process_tree_pid"
process_tree_pid=

mkdir -p "$server_root"
cache_dir=$TEST_HOME/.cup/cache/compiler/clang/$TEST_PLATFORM/$TEST_PLATFORM/22.1.5
archive_name=clang-22.1.5-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz
mv "$cache_dir/$archive_name" "$server_root/$archive_name"
checksum_root=$server_root/22.1.5/$TEST_PLATFORM/$TEST_PLATFORM
mkdir -p "$checksum_root"
mv "$cache_dir/SHA256SUMS" "$checksum_root/SHA256SUMS"
rm -rf "$TEST_HOME/.cup/cache/compiler/clang"

rm -f "$ready" "$request_ready"
"$helper" http-server --root "$server_root" --port "$port" \
    --ready-file "$ready" --request-file "$request_ready" --delay-ms 3000 \
    >"$server_log" 2>&1 &
server_pid=$!
attempt=0
while [ ! -f "$ready" ] && [ "$attempt" -lt 100 ]; do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        wait "$server_pid" || true
        cat "$server_log" >&2 || true
        fail 'concurrency package server exited before becoming ready'
    fi
    sleep 0.05
    attempt=$((attempt + 1))
done
[ -f "$ready" ] || {
    cat "$server_log" >&2 || true
    fail 'concurrency package server did not become ready'
}
port=$(cat "$ready")
case "$port" in
    ''|*[!0-9]*) fail "concurrency package server reported invalid port: $port" ;;
esac
catalog=$DEV_ROOT/config/packages.cfg
catalog_backup=$TMP_ROOT/packages.cfg.original
cp "$catalog" "$catalog_backup"
temporary=$catalog.tmp
awk -v key="compiler.clang.$TEST_PLATFORM.$TEST_PLATFORM" \
    -v base="http://127.0.0.1:$port" '
    BEGIN { changed = 0 }
    index($0, key ".url_template=") == 1 {
        package = "/clang-{version}-{host_platform}-{target_platform}.{format}"
        print key ".url_template=" base package
        changed++
        next
    }
    index($0, key ".checksum_url_template=") == 1 {
        checksum = "/{version}/{host_platform}/{target_platform}/SHA256SUMS"
        print key ".checksum_url_template=" base checksum
        changed++
        next
    }
    { print }
    END { if (changed != 2) exit 2 }
' "$catalog" > "$temporary" || fail 'could not configure the concurrency package server'
mv "$temporary" "$catalog"

(
    cd "$DEV_ROOT"
    exec env HOME="$TEST_HOME" CUP_INSTALL_ALLOW_INSECURE=1 \
        "$CUP" install compiler clang@stable
) >"$TMP_ROOT/install-a.out" 2>&1 &
pid_a=$!

transaction=$TEST_HOME/.cup/transaction.txt
attempt=0
while [ ! -f "$request_ready" ] && [ "$attempt" -lt 200 ]; do
    if ! kill -0 "$pid_a" 2>/dev/null; then
        wait "$pid_a" || true
        pid_a=
        cat "$TMP_ROOT/install-a.out" >&2 || true
        fail 'first install exited before reaching the synchronized download'
    fi
    sleep 0.05
    attempt=$((attempt + 1))
done
[ -f "$request_ready" ] || fail 'first install did not reach the synchronized download'

status_b=0
(
    cd "$DEV_ROOT"
    HOME="$TEST_HOME" CUP_INSTALL_ALLOW_INSECURE=1 \
        "$CUP" install compiler clang@stable
) >"$TMP_ROOT/install-b.out" 2>&1 || status_b=$?

if [ "$status_b" -eq 0 ]; then
    printf 'overlapping install status/output: %s\n%s\n' \
        "$status_b" "$(cat "$TMP_ROOT/install-b.out")" >&2
    fail 'overlapping install was not blocked while the first operation was active'
fi

status_a=0
wait "$pid_a" || status_a=$?
pid_a=
if [ "$status_a" -ne 0 ]; then
    printf 'first install status/output: %s\n%s\n' \
        "$status_a" "$(cat "$TMP_ROOT/install-a.out")" >&2
    fail 'first synchronized install did not complete successfully'
fi

first_text=$(cat "$TMP_ROOT/install-a.out")
second_text=$(cat "$TMP_ROOT/install-b.out")
assert_contains "$first_text" 'Installed compiler clang@22.1.5'
case "$second_text" in
    *'another cup operation is currently running'* | \
        *'a package transaction is active or requires recovery'*) ;;
    *)
        fail 'overlapping install did not report the active operation or transaction'
        ;;
esac
assert_not_contains "$second_text" 'already installed'

cp "$catalog_backup" "$catalog"
assert_cup_healthy
assert_missing "$TEST_HOME/.cup/transaction.txt"
if find "$TEST_HOME/.cup/staging" -mindepth 1 -print -quit | grep . >/dev/null; then
    find "$TEST_HOME/.cup/staging" -mindepth 1 -maxdepth 1 -print >&2
    fail 'concurrent installs left temporary paths behind'
fi
assert_contains "$(run_cup info compiler)" "compiler [$TEST_PLATFORM]: clang@22.1.5 (stable)"
assert_equals "$(run_native_wrapper clang)" "clang-22.1.5-$TEST_PLATFORM:clang"

printf 'Concurrency integration tests passed for %s.\n' "$TEST_PLATFORM"
