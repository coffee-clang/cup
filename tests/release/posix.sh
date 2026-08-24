#!/usr/bin/env sh

# Validates one completed POSIX release candidate, native binary and generated installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT_DIR=$ROOT/scripts/release
. "$SCRIPT_DIR/common.sh"
. "$ROOT/tests/support/posix/uninstall.sh"

: "${PLATFORM:?PLATFORM is required}"
: "${VERSION:?VERSION is required}"
SHA=${SHA:-$(git -C "$ROOT" rev-parse HEAD)}
release_dir=${1:-release}

verify_checksum_file_exact "$release_dir" SHA256SUMS.common \
    packages.cfg install.cfg install.sh install.ps1
verify_checksum_file_exact "$release_dir" "SHA256SUMS.$PLATFORM" \
    "cup-$PLATFORM" release.txt SHA256SUMS.common

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
test_build_root=${CUP_TEST_BUILD_ROOT:-$ROOT/build}
helper="$test_build_root/$PLATFORM/${CUP_TEST_CONFIGURATION:-development}/tests/helpers/network-helper"
temporary_parent=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
test_root=$(mktemp -d "$temporary_parent/cup-release-test.XXXXXX")
ready="$test_root/http-ready"
server_log="$test_root/http.log"
test_home="$test_root/installer-home"
foreign_home="$test_root/foreign-home"
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if [ -d "$test_root" ] && [ ! -L "$test_root" ]; then
        rm -rf -- "$test_root"
    fi
}
exit_handler() {
    status=$?
    trap - EXIT HUP INT TERM
    cleanup
    exit "$status"
}
signal_handler() {
    status=$1
    trap - EXIT HUP INT TERM
    cleanup
    exit "$status"
}
trap exit_handler EXIT
trap 'signal_handler 129' HUP
trap 'signal_handler 130' INT
trap 'signal_handler 143' TERM

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

# An unrelated primary directory is preserved while the same release installs,
# runs and uninstalls from the stable fallback root.
mkdir -p "$foreign_home/.cup"
printf 'unrelated\n' > "$foreign_home/.cup/foreign.txt"
HOME="$foreign_home" \
CUP_INSTALL_ALLOW_INSECURE=1 \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
    sh "$release_dir/install.sh"
foreign_cup="$foreign_home/.coffee-cup/bin/cup"
test -f "$foreign_home/.cup/foreign.txt"
test -x "$foreign_cup"
test "$(sed -n '1p' "$foreign_home/.coffee-cup/root.txt")" = 'format=1'
test "$(sed -n '2p' "$foreign_home/.coffee-cup/root.txt")" = \
    'product=coffee-clang/cup'
test "$(sed -n '3p' "$foreign_home/.coffee-cup/root.txt")" = 'layout=1'
HOME="$foreign_home" "$foreign_cup" --version | grep -Fx "cup $VERSION"
foreign_doctor=$(HOME="$foreign_home" "$foreign_cup" doctor 2>&1)
printf '%s\n' "$foreign_doctor" | grep -F 'Doctor found no issues.' >/dev/null
HOME="$foreign_home" "$foreign_cup" uninstall --yes >/dev/null
cup_test_wait_for_uninstall "$foreign_home/.coffee-cup" "$foreign_home" ||
    fail 'fallback-root uninstall did not complete cleanly'
test -f "$foreign_home/.cup/foreign.txt"

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

# The update helper is derived from the installed executable, not a release asset. Repair
# regenerates a missing copy without changing the running executable.
update_helper="$test_home/.cup/helpers/update-helper"
rm -f "$update_helper"
if helper_repair_output=$(
    cd "$test_home"
    HOME="$test_home" \
    CUP_INSTALL_ALLOW_INSECURE=1 \
    CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
        "$installed_cup" repair 2>&1
); then
    :
else
    repair_status=$?
    printf '%s\n' "$helper_repair_output" >&2
    fail "installed cup helper repair failed with exit code $repair_status"
fi
printf '%s\n' "$helper_repair_output"
printf '%s\n' "$helper_repair_output" | \
    grep -F 'Regenerated native update helper from the installed executable.' >/dev/null
test -x "$update_helper"
test "$(hash_file "$update_helper")" = "$binary_hash_before"
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
helper_hash_before_update=$(hash_file "$update_helper")

# A pending or malformed canonical journal blocks bootstrap before any managed mutation.
# The installer must preserve both the journal evidence and the installed executable.
transaction="$test_home/.cup/transaction.txt"
printf 'invalid=1\n' > "$transaction"
if HOME="$test_home" \
CUP_INSTALL_ALLOW_INSECURE=1 \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
    sh "$release_dir/install.sh" >"$test_home/pending-transaction.out" 2>&1; then
    fail 'bootstrap unexpectedly ignored a malformed canonical transaction'
fi
grep -F 'verified cup bootstrap transaction was rejected' \
    "$test_home/pending-transaction.out" >/dev/null
test -f "$transaction"
test "$(cat "$transaction")" = 'invalid=1'
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
rm -f "$transaction"

# Reinstalling the same tested candidate leaves no journal or staging residue.
HOME="$test_home" \
CUP_INSTALL_ALLOW_INSECURE=1 \
CUP_INSTALL_BASE_URL="http://127.0.0.1:$port" \
    sh "$release_dir/install.sh"
test ! -e "$transaction"
if find "$test_home/.cup/staging" -mindepth 1 -print -quit | grep -q .; then
    fail 'reinstall left canonical staging residue'
fi
test "$(hash_file "$installed_cup")" = "$binary_hash_before"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $VERSION"

# A local immutable release fixture exercises the complete detached update path. The binary
# patcher changes only same-length embedded version strings, so the served executable remains the
# tested candidate with a distinct observable version.
next_version=$(next_test_version "$VERSION")
update_root="$release_dir/update-fixture"
version_root="$update_root/$next_version"
configuration=${CUP_TEST_CONFIGURATION:-development}
patch_helper="$test_build_root/$PLATFORM/$configuration/tests/helpers/binary-patch"
rm -rf "$update_root"
mkdir -p "$version_root"
[ -x "$patch_helper" ] || fail "binary patch helper is not built: $patch_helper"

cp "$release_dir/packages.cfg" "$version_root/packages.cfg"
cp "$release_dir/install.cfg" "$version_root/install.cfg"
cp "$release_dir/install.sh" "$version_root/install.sh"
cp "$release_dir/install.ps1" "$version_root/install.ps1"
"$patch_helper" "$release_dir/cup-$PLATFORM" \
    "$version_root/cup-$PLATFORM" "$VERSION" "$next_version" >/dev/null
chmod +x "$version_root/cup-$PLATFORM"
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
        printf '%s  %s\n' "$(hash_file "$version_root/$asset")" "$asset" >> SHA256SUMS.common
    done
    : > "SHA256SUMS.$PLATFORM"
    for asset in "cup-$PLATFORM" release.txt SHA256SUMS.common; do
        printf '%s  %s\n' "$(hash_file "$version_root/$asset")" "$asset" >> "SHA256SUMS.$PLATFORM"
    done
)

test "$("$version_root/cup-$PLATFORM" --version)" = "cup $next_version"
update_output=$(
    cd "$test_home"
    HOME="$test_home" \
        CUP_INSTALL_ALLOW_INSECURE=1 \
        CUP_INSTALL_BASE_URL="http://127.0.0.1:$port/update-fixture" \
        "$installed_cup" update cup 2>&1
)
printf '%s\n' "$update_output"
printf '%s\n' "$update_output" | \
    grep -F "Verified update from cup $VERSION to $next_version scheduled." \
        >/dev/null

attempt=0
while [ "$attempt" -lt 200 ]; do
    if [ ! -e "$test_home/.cup/transaction.txt" ] &&
        HOME="$test_home" "$installed_cup" --version 2>/dev/null |
            grep -Fx "cup $next_version" >/dev/null &&
        HOME="$test_home" "$installed_cup" doctor >/dev/null 2>&1; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done
[ "$attempt" -lt 200 ] || fail 'cup update helper did not complete the verified update'
test ! -e "$test_home/.cup/transaction.txt"
test "$(hash_file "$installed_cup")" = "$(hash_file "$version_root/cup-$PLATFORM")"
test "$(hash_file "$update_helper")" = "$helper_hash_before_update"
test "$(hash_file "$update_helper")" != "$(hash_file "$installed_cup")"
HOME="$test_home" "$installed_cup" --version | grep -Fx "cup $next_version"
updated_doctor=$(HOME="$test_home" "$installed_cup" doctor 2>&1)
printf '%s\n' "$updated_doctor"
printf '%s\n' "$updated_doctor" | grep -F 'Doctor found no issues.' >/dev/null
if find "$test_home/.cup/staging" -mindepth 1 -name 'cup-update-*' -print -quit |
    grep . >/dev/null; then
    fail 'successful cup update left update staging behind'
fi

# The assembled release performs its detached uninstall smoke test.
uninstall_started_message='Uninstall started; cleanup continues in the background. '
uninstall_started_message="${uninstall_started_message}You can close this terminal. "
uninstall_started_message="${uninstall_started_message}The PATH entry was not removed."
uninstall_output=$(HOME="$test_home" "$installed_cup" uninstall --yes 2>&1)
printf '%s\n' "$uninstall_output"
printf '%s\n' "$uninstall_output" | grep -F "$uninstall_started_message" >/dev/null
if ! cup_test_wait_for_uninstall "$test_home/.cup" "$test_home"; then
    residue=$(cup_test_uninstall_residue "$test_home")
    [ ! -e "$test_home/.cup" ] && [ ! -L "$test_home/.cup" ] ||
        fail 'release uninstall did not remove the cup root'
    fail "release uninstall left staging behind: $residue"
fi
