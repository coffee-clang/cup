#!/usr/bin/env sh

# Purpose: Validates one completed POSIX release candidate, native binary and generated installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/scripts/release/common.sh"
. "$ROOT/tests/support/uninstall.sh"

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

next_test_version() {
    old=$1
    old_ifs=$IFS
    IFS=.
    set -- $old
    IFS=$old_ifs
    [ "$#" -eq 3 ] || fail "invalid release version for update fixture: $old"
    major=$1
    minor=$2
    patch=$3

    candidate="$major.$minor.$((patch + 1))"
    if [ "${#candidate}" -ne "${#old}" ]; then
        candidate="$major.$((minor + 1)).0"
    fi
    if [ "${#candidate}" -ne "${#old}" ]; then
        candidate="$((major + 1)).0.0"
    fi
    [ "${#candidate}" -eq "${#old}" ] ||
        fail "could not create a same-length update version from $old"
    printf '%s\n' "$candidate"
}

port=0
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
while [ "$attempt" -lt 50 ] && [ ! -f "$ready" ]; do
    attempt=$((attempt + 1))
    sleep 0.2
done
if [ ! -f "$ready" ]; then
    cat "$server_log" >&2 || true
    fail 'local release test server did not become ready'
fi
port=$(cat "$ready")
case "$port" in
    ''|*[!0-9]*) fail "local release server reported invalid port: $port" ;;
esac
curl -fsS "http://127.0.0.1:$port/release.txt" >/dev/null || {
    cat "$server_log" >&2 || true
    fail 'local release test server did not serve release metadata'
}

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
if repair_output=$(
    cd "$test_home"
    HOME="$test_home" \
    CUP_INSTALL_ALLOW_INSECURE=1 \
    CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
        "$installed_cup" repair 2>&1
); then
    :
else
    repair_status=$?
    printf '%s\n' "$repair_output" >&2
    fail "installed cup repair failed with exit code $repair_status"
fi
printf '%s\n' "$repair_output"
test -d "$test_home/.cup/staging"
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $VERSION"

# An official installation can restore its packaged uninstall asset without changing cup.
uninstall_asset="$test_home/.cup/helpers/uninstall.sh"
rm -f "$uninstall_asset"
if asset_repair_output=$(
    cd "$test_home"
    HOME="$test_home" \
    CUP_INSTALL_ALLOW_INSECURE=1 \
    CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
        "$installed_cup" repair 2>&1
); then
    :
else
    repair_status=$?
    printf '%s\n' "$asset_repair_output" >&2
    fail "installed cup asset repair failed with exit code $repair_status"
fi
printf '%s\n' "$asset_repair_output"
printf '%s\n' "$asset_repair_output" | grep -F 'Restoring uninstall script.' >/dev/null
test -f "$uninstall_asset"
test -x "$uninstall_asset"
uninstall_mode=$(ls -ld "$uninstall_asset" | awk '{print $1}')
case "$uninstall_mode" in
    *w*) fail 'release repair did not restore read-only uninstall.sh' ;;
esac
test "$(hash_file "$installed_cup")" = "$binary_hash_before"

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

# A local immutable release fixture exercises the complete detached update path. The binary
# patcher changes only same-length embedded version strings, so the served executable remains the
# tested candidate with a distinct observable version.
next_version=$(next_test_version "$VERSION")
update_root="$release_dir/update-fixture"
version_root="$update_root/$next_version"
patch_helper="$ROOT/build/$PLATFORM/${CUP_TEST_CONFIGURATION:-development}/tests/helpers/binary-patch"
rm -rf "$update_root"
mkdir -p "$version_root"
[ -x "$patch_helper" ] || fail "binary patch helper is not built: $patch_helper"

cp "$release_dir/packages.cfg" "$version_root/packages.cfg"
cp "$release_dir/install.cfg" "$version_root/install.cfg"
cp "$release_dir/install.sh" "$version_root/install.sh"
cp "$release_dir/install.ps1" "$version_root/install.ps1"
cp "$release_dir/uninstall.sh" "$version_root/uninstall.sh"
"$patch_helper" "$release_dir/cup-$PLATFORM" "$version_root/cup-$PLATFORM"     "$VERSION" "$next_version" >/dev/null
chmod +x "$version_root/cup-$PLATFORM" "$version_root/uninstall.sh"
{
    printf 'format=1\n'
    printf 'version=%s\n' "$next_version"
    printf 'commit=%s\n' "$SHA"
} > "$version_root/release.txt"
cp "$version_root/release.txt" "$update_root/release.txt"
(
    cd "$version_root"
    : > SHA256SUMS.common
    for asset in packages.cfg install.cfg install.sh install.ps1; do
        printf '%s  %s\n' "$(hash_file "$asset")" "$asset" >> SHA256SUMS.common
    done
    : > "SHA256SUMS.$PLATFORM"
    for asset in "cup-$PLATFORM" uninstall.sh release.txt; do
        printf '%s  %s\n' "$(hash_file "$asset")" "$asset" >> "SHA256SUMS.$PLATFORM"
    done
)

test "$("$version_root/cup-$PLATFORM" --version)" = "cup $next_version"
update_output=$(
    cd "$test_home"
    HOME="$test_home"     CUP_INSTALL_ALLOW_INSECURE=1     CUP_INSTALL_BASE_URL="http://127.0.0.1:$port/update-fixture"         "$installed_cup" update cup 2>&1
)
printf '%s\n' "$update_output"
printf '%s\n' "$update_output" | grep -F     "Verified update from cup $VERSION to $next_version scheduled." >/dev/null

update_result="$test_home/.cup/cup-update-result.txt"
attempt=0
while [ "$attempt" -lt 200 ] && [ ! -f "$update_result" ]; do
    attempt=$((attempt + 1))
    sleep 0.1
done
[ -f "$update_result" ] || fail 'cup update helper did not publish a result'
grep -Fx 'status=success' "$update_result" >/dev/null
grep -Fx 'error=0' "$update_result" >/dev/null
grep -Fx "version=$next_version" "$update_result" >/dev/null
test ! -e "$test_home/.cup/transaction.txt"
test "$(hash_file "$installed_cup")" = "$(hash_file "$version_root/cup-$PLATFORM")"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $next_version"
updated_doctor=$(HOME="$test_home" "$installed_cup" doctor 2>&1)
printf '%s\n' "$updated_doctor"
printf '%s\n' "$updated_doctor" | grep -F 'Doctor found no issues.' >/dev/null
if find "$test_home/.cup/staging" -mindepth 1 -name 'cup-update-*' -print -quit |
    grep . >/dev/null; then
    fail 'successful cup update left update staging behind'
fi

# The assembled release performs its detached uninstall smoke test.
uninstall_output=$(HOME="$test_home" "$installed_cup" uninstall --yes 2>&1)
printf '%s\n' "$uninstall_output"
printf '%s\n' "$uninstall_output" | grep -F \
    'Uninstall started. The PATH entry was not removed.' >/dev/null
if ! cup_test_wait_for_uninstall "$test_home/.cup" "$test_home"; then
    residue=$(cup_test_uninstall_residue "$test_home")
    [ ! -e "$test_home/.cup" ] && [ ! -L "$test_home/.cup" ] ||
        fail 'release uninstall did not remove the cup root'
    fail "release uninstall left staging behind: $residue"
fi
