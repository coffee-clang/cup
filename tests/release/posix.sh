#!/usr/bin/env sh

# Purpose: Validates one completed POSIX release candidate, native binary and generated installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/scripts/release/common.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${VERSION:?VERSION is required}"
SHA=${SHA:-$(git -C "$ROOT" rev-parse HEAD)}
release_dir=${1:-release}

verify_checksum_file_exact "$release_dir" SHA256SUMS.common packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$release_dir" "SHA256SUMS.$PLATFORM" "cup-$PLATFORM" uninstall.sh release.txt

test "$(sed -n 's/^format=//p' "$release_dir/release.txt")" = 1
test "$(sed -n 's/^version=//p' "$release_dir/release.txt")" = "$VERSION"
test "$(sed -n 's/^commit=//p' "$release_dir/release.txt")" = "$SHA"
test "$(wc -l < "$release_dir/release.txt" | tr -d '[:space:]')" = 3

chmod +x "$release_dir/cup-$PLATFORM" "$release_dir/install.sh"
test "$("$release_dir/cup-$PLATFORM" --version)" = "cup $VERSION"

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
installed_cup="$test_home/.cup/bin/cup"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $VERSION"
test "$(hash_file "$installed_cup")" = \
    "$(hash_file "$release_dir/cup-$PLATFORM")"
doctor_output=$(
    cd "$test_home"
    HOME="$test_home" "$installed_cup" doctor 2>&1
)
printf '%s\n' "$doctor_output"
case "$doctor_output" in
    *'development cup assets'*|*'development catalog'*)
        fail 'official installation unexpectedly used development cup assets'
        ;;
esac
printf '%s\n' "$doctor_output" | grep -F 'Doctor found no issues.' >/dev/null

# Repair may recreate mutable runtime paths, but it must never replace or remove the
# currently installed executable on either POSIX or Windows.
binary_hash_before=$(hash_file "$installed_cup")
rm -rf "$test_home/.cup/staging"
repair_output=$(
    cd "$test_home"
    HOME="$test_home" "$installed_cup" repair 2>&1
)
printf '%s\n' "$repair_output"
test -d "$test_home/.cup/staging"
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $VERSION"

# A completion marker is accepted only for a complete installed generation. The failed
# recovery must preserve both the executable and transaction evidence.
bootstrap_staging="$test_home/.cup/.bootstrap"
saved_update_helper="$test_home/saved-cup-update-helper"
mkdir "$bootstrap_staging"
: > "$bootstrap_staging/committed"
mv "$test_home/.cup/helpers/cup-update-helper" "$saved_update_helper"
if HOME="$test_home" \
CUP_INSTALL_ALLOW_INSECURE=1 \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
CUP_INSTALL_NO_PATH_PROMPT=1 \
    sh "$release_dir/install.sh" >"$test_home/incomplete-bootstrap.out" 2>&1; then
    fail 'incomplete committed bootstrap staging unexpectedly succeeded'
fi
grep -F 'completed bootstrap staging does not match a complete installed generation' \
    "$test_home/incomplete-bootstrap.out" >/dev/null
test -f "$bootstrap_staging/committed"
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
mv "$saved_update_helper" "$test_home/.cup/helpers/cup-update-helper"

# Reinstalling the same tested candidate now completes cleanup and leaves the executable valid.
HOME="$test_home" \
CUP_INSTALL_ALLOW_INSECURE=1 \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
CUP_INSTALL_NO_PATH_PROMPT=1 \
    sh "$release_dir/install.sh"
test ! -e "$bootstrap_staging"
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $VERSION"

# The assembled release performs its detached uninstall smoke test.
uninstall_output=$(HOME="$test_home" "$installed_cup" uninstall --yes 2>&1)
printf '%s\n' "$uninstall_output"
printf '%s\n' "$uninstall_output" | grep -F \
    'Uninstall started. The PATH entry was not removed.' >/dev/null
attempt=0
while [ "$attempt" -lt 200 ] && [ -e "$test_home/.cup" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
done
[ ! -e "$test_home/.cup" ] || fail 'release uninstall did not remove the cup root'
for residue in "$test_home"/.cup-uninstall.*; do
    [ ! -e "$residue" ] && [ ! -L "$residue" ] ||
        fail "release uninstall left staging behind: $residue"
done
