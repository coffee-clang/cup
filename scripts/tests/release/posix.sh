#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/../workflow/common.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${VERSION:?VERSION is required}"
release_dir=${1:-release}

verify_checksums "$release_dir" SHA256SUMS.common
verify_checksums "$release_dir" "SHA256SUMS.$PLATFORM"

chmod +x "$release_dir/cup-$PLATFORM" "$release_dir/install.sh"
test "$("$release_dir/cup-$PLATFORM" --version)" = "cup $VERSION"

CUP_TEST_SKIP_BUILD=1 \
CUP_TEST_PLATFORM="$PLATFORM" \
CUP_TEST_BINARY="$PWD/$release_dir/cup-$PLATFORM" \
    ./scripts/tests/run.sh

port=$(( 18080 + (RANDOM % 1000) ))
python3 -m http.server "$port" --directory "$release_dir" >/tmp/cup-http.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
    curl -fsS "http://127.0.0.1:$port/release.txt" >/dev/null && break
    sleep 0.2
done

test_home="${RUNNER_TEMP:-/tmp}/cup-installer-home"
rm -rf "$test_home"
mkdir -p "$test_home"
HOME="$test_home" \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
CUP_INSTALL_NO_PATH_PROMPT=1 \
    sh "$release_dir/install.sh"
"$test_home/.cup/bin/cup" --version | grep -Fx "cup $VERSION"
"$test_home/.cup/bin/cup" doctor
