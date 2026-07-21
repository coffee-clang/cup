#!/usr/bin/env sh

# Purpose: Validates one completed POSIX release candidate, native binary and generated installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/scripts/release/common.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${VERSION:?VERSION is required}"
release_dir=${1:-release}

verify_checksum_file_exact "$release_dir" SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$release_dir" "SHA256SUMS.$PLATFORM" "cup-$PLATFORM" uninstall.sh release.txt

chmod +x "$release_dir/cup-$PLATFORM" "$release_dir/install.sh"
test "$("$release_dir/cup-$PLATFORM" --version)" = "cup $VERSION"

CUP_TEST_SKIP_BUILD=1 \
CUP_TEST_PLATFORM="$PLATFORM" \
CUP_TEST_BINARY="$PWD/$release_dir/cup-$PLATFORM" \
CUP_TEST_CONFIGURATION="${CUP_TEST_CONFIGURATION:-development}" \
    "$ROOT/tests/runners/integration-posix.sh"

port=$((18080 + ($$ % 1000)))
helper="$ROOT/build/$PLATFORM/${CUP_TEST_CONFIGURATION:-development}/tests/helpers/network-helper"
temporary_root=${RUNNER_TEMP:-/tmp}
ready="$temporary_root/cup-http-ready.$$"
server_log="$temporary_root/cup-http.$$.log"
test_home="$temporary_root/cup-installer-home.$$"
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -f "$ready" "$server_log"
    rm -rf "$test_home"
}
trap cleanup EXIT HUP INT TERM

[ -x "$helper" ] || fail "HTTP test helper is not built: $helper"
rm -f "$ready"
"$helper" http-server --root "$release_dir" --port "$port" --ready-file "$ready" \
    >"$server_log" 2>&1 &
server_pid=$!

attempt=0
while [ "$attempt" -lt 50 ]; do
    if [ -f "$ready" ] &&
        curl -fsS "http://127.0.0.1:$port/release.txt" >/dev/null; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 0.2
done
if [ "$attempt" -ge 50 ]; then
    cat "$server_log" >&2 || true
    fail 'local release test server did not become ready'
fi

mkdir -p "$test_home"
HOME="$test_home" \
CUP_INSTALL_ALLOW_INSECURE=1 \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
CUP_INSTALL_NO_PATH_PROMPT=1 \
    sh "$release_dir/install.sh"
"$test_home/.cup/bin/cup" --version | grep -Fx "cup $VERSION"
"$test_home/.cup/bin/cup" doctor
