#!/usr/bin/env bash

# Verifies the public installers remain transport-only and hand one authenticated,
# concrete release generation to the native synchronous bootstrap.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cup-installer-behavior.XXXXXX")"
VERSION=$(tr -d '\n' < "$ROOT/VERSION")
TAG=v$VERSION
SHA=0123456789abcdef0123456789abcdef01234567
CUP_TEST_CP=$(command -v cp) || exit 1
CUP_TEST_MKDIR=$(command -v mkdir) || exit 1
export CUP_TEST_CP CUP_TEST_MKDIR

cleanup() { [ ! -e "$WORK" ] || rm -rf -- "$WORK"; }
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }

for shell_cmd in "sh" "dash" "busybox sh"; do
    set -- $shell_cmd
    command -v "$1" >/dev/null 2>&1 || continue
    "$@" -n "$ROOT/scripts/install/install.sh" || fail "$shell_cmd rejected the POSIX installer"
done

SCRIPT_DIR="$ROOT/scripts/release"
# shellcheck source=scripts/release/common.sh
. "$SCRIPT_DIR/common.sh"
prepare_installer "$ROOT/scripts/install/install.sh" "$WORK/install.sh" 0755
prepare_installer "$ROOT/scripts/install/install.ps1" "$WORK/install.ps1" 0644
[ -x "$WORK/install.sh" ] || fail 'prepared POSIX installer is not executable'

# PATH paths containing ':' remain valid roots but cannot be represented as one POSIX PATH entry.
path_functions=$WORK/install-path-functions.sh
awk '/^validate_identity$/ { exit } { print }' "$WORK/install.sh" > "$path_functions"
colon_home=$WORK/path-colon-home
colon_root=$WORK/'path:colon'/.cup
mkdir -p "$colon_home" "$colon_root/bin"
colon_output=$(HOME="$colon_home" SHELL=/bin/sh PATH="$PATH" \
    sh -eu -c '. "$1"; SELECTED_ROOT=$2; export SELECTED_ROOT; offer_path_integration' \
    sh "$path_functions" "$colon_root" 2>&1)
printf '%s\n' "$colon_output" | grep -F 'Automatic user PATH integration is unavailable' >/dev/null ||
    fail 'POSIX installer did not explain an unrepresentable PATH entry'
[ ! -e "$colon_home/.profile" ] || fail 'POSIX installer wrote an unrepresentable PATH entry'

# Bash startup files are updated independently and unsafe targets are preserved.
partial_home=$WORK/path-partial-home
partial_root=$WORK/path-partial-root/.cup
mkdir -p "$partial_home/.bashrc" "$partial_root/bin"
partial_output=$(HOME="$partial_home" SHELL=/bin/bash PATH="$PATH"     sh -eu -c '. "$1"; SELECTED_ROOT=$2; export SELECTED_ROOT; add_posix_path'     sh "$path_functions" "$partial_root" 2>&1)
printf '%s
' "$partial_output" | grep -F 'could not update every Bash startup file' >/dev/null ||
    fail 'POSIX installer did not report a partial Bash PATH update'
[ -d "$partial_home/.bashrc" ] || fail 'POSIX installer replaced a non-file Bash startup target'
grep -F "$partial_root/bin" "$partial_home/.bash_profile" >/dev/null ||
    fail 'POSIX installer did not update the independent valid Bash login file'
[ -z "$(find "$partial_home/.bashrc" -mindepth 1 -print -quit)" ] ||
    fail 'POSIX installer wrote temporary content inside a startup-file directory'

symlink_home=$WORK/path-symlink-home
symlink_root=$WORK/path-symlink-root/.cup
mkdir -p "$symlink_home" "$symlink_root/bin"
printf 'keep-me
' > "$symlink_home/target"
ln -s target "$symlink_home/.zshrc"
symlink_output=$(HOME="$symlink_home" SHELL=/bin/zsh PATH="$PATH"     sh -eu -c '. "$1"; SELECTED_ROOT=$2; export SELECTED_ROOT; add_posix_path'     sh "$path_functions" "$symlink_root" 2>&1)
printf '%s
' "$symlink_output" | grep -F 'could not update the Zsh PATH configuration' >/dev/null ||
    fail 'POSIX installer did not report a symlink PATH target'
[ "$(cat "$symlink_home/target")" = 'keep-me' ] ||
    fail 'POSIX installer followed and modified a symlink PATH target'
[ -L "$symlink_home/.zshrc" ] || fail 'POSIX installer replaced a symlink PATH target'

# The interactive PATH prompt is opt-in: Enter keeps external shell state untouched,
# an explicit yes adds one idempotent entry. Use a real pseudo-TTY because the public
# installer deliberately skips prompts for non-interactive stdin.
if command -v script >/dev/null 2>&1; then
    path_prompt_home=$WORK/path-prompt-home
    path_prompt_root=$WORK/path-prompt-root/.cup
    mkdir -p "$path_prompt_home" "$path_prompt_root/bin"
    prompt_command=". '$path_functions'; SELECTED_ROOT='$path_prompt_root'; export SELECTED_ROOT; offer_path_integration"

    printf '\n' | HOME="$path_prompt_home" SHELL=/bin/bash PATH="$PATH" \
        script -qfec "sh -eu -c \"$prompt_command\"" /dev/null > "$WORK/path-default-no.out" 2>&1 ||
        fail 'POSIX installer PATH default-no prompt failed'
    grep -F '[y/N]' "$WORK/path-default-no.out" >/dev/null ||
        fail 'POSIX installer did not show the opt-in PATH prompt'
    [ ! -e "$path_prompt_home/.bashrc" ] && [ ! -e "$path_prompt_home/.bash_profile" ] ||
        fail 'POSIX installer changed PATH configuration on the default response'

    printf 'y\n' | HOME="$path_prompt_home" SHELL=/bin/bash PATH="$PATH" \
        script -qfec "sh -eu -c \"$prompt_command\"" /dev/null > "$WORK/path-yes.out" 2>&1 ||
        fail 'POSIX installer PATH opt-in failed'
    grep -F "$path_prompt_root/bin" "$path_prompt_home/.bashrc" >/dev/null ||
        fail 'POSIX installer did not persist the accepted Bash interactive PATH entry'
    grep -F "$path_prompt_root/bin" "$path_prompt_home/.bash_profile" >/dev/null ||
        fail 'POSIX installer did not persist the accepted Bash login PATH entry'
    first_profile_hash=$(cat "$path_prompt_home/.bashrc" "$path_prompt_home/.bash_profile" | sha256sum | awk '{print $1}')

    printf 'yes\n' | HOME="$path_prompt_home" SHELL=/bin/bash PATH="$PATH" \
        script -qfec "sh -eu -c \"$prompt_command\"" /dev/null >/dev/null 2>&1 ||
        fail 'POSIX installer repeated PATH opt-in failed'
    second_profile_hash=$(cat "$path_prompt_home/.bashrc" "$path_prompt_home/.bash_profile" | sha256sum | awk '{print $1}')
    [ "$first_profile_hash" = "$second_profile_hash" ] ||
        fail 'POSIX installer duplicated an already persisted PATH entry'
fi

mkdir -p "$WORK/mock-bin"
cat > "$WORK/mock-bin/curl" <<'MOCK_CURL'
#!/usr/bin/env sh
set -eu
output= url= proto= proto_redir= max_redirs= connect_timeout= max_time= speed_time= speed_limit= max_filesize=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) output=$2; shift 2 ;;
        --proto) proto=$2; shift 2 ;;
        --proto-redir) proto_redir=$2; shift 2 ;;
        --max-redirs) max_redirs=$2; shift 2 ;;
        --connect-timeout) connect_timeout=$2; shift 2 ;;
        --max-time) max_time=$2; shift 2 ;;
        --speed-time) speed_time=$2; shift 2 ;;
        --speed-limit) speed_limit=$2; shift 2 ;;
        --max-filesize) max_filesize=$2; shift 2 ;;
        http://*|https://*) url=$1; shift ;;
        *) shift ;;
    esac
done
[ -n "$output" ] && [ -n "$url" ]
case "$url" in
    https://*) expected_proto='=https'; expected_max_redirs=10 ;;
    http://*) expected_proto='=http'; expected_max_redirs=0 ;;
    *) exit 91 ;;
esac
[ "$proto" = "$expected_proto" ] && [ "$proto_redir" = "$expected_proto" ]
[ "$max_redirs" = "$expected_max_redirs" ]
[ "$connect_timeout" = 15 ] && [ "$max_time" = 180 ]
[ "$speed_time" = 30 ] && [ "$speed_limit" = 1024 ]
[ -n "$max_filesize" ]
printf '%s\n' "${url##*/}" >> "$CUP_DOWNLOAD_TRACE"
"$CUP_TEST_CP" "$CUP_FIXTURE/${url##*/}" "$output"
MOCK_CURL
chmod 0755 "$WORK/mock-bin/curl"

mkdir -p "$WORK/windows-bin"
cp "$WORK/mock-bin/curl" "$WORK/windows-bin/curl"
cat > "$WORK/windows-bin/uname" <<'EOF_UNAME'
#!/usr/bin/env sh
case "${1:-}" in -s) printf 'MINGW64_NT-10.0\n' ;; -m) printf 'x86_64\n' ;; *) exit 2 ;; esac
EOF_UNAME
cat > "$WORK/windows-bin/cygpath" <<'EOF_CYGPATH'
#!/usr/bin/env sh
[ "$#" -eq 2 ] && [ "$1" = -w ]
printf '%s\n' "$2"
EOF_CYGPATH
cat > "$WORK/windows-bin/powershell.exe" <<'EOF_PS'
#!/usr/bin/env sh
set -eu
printf '%s\n' "$*" > "$CUP_POWERSHELL_TRACE"
EOF_PS
chmod 0755 "$WORK/windows-bin/uname" "$WORK/windows-bin/cygpath" "$WORK/windows-bin/powershell.exe"

hash_file() { sha256sum "$1" | awk '{print $1}'; }

write_manifest() {
    fixture=$1
    shift
    names=$(printf '%s\n' "$@" | LC_ALL=C sort)
    count=$(printf '%s\n' "$names" | wc -l | tr -d ' ')
    {
        printf 'format=2\nversion=%s\ncommit=%s\nroot_layout=2\ncatalog_format=1\nasset_count=%s\n' "$VERSION" "$SHA" "$count"
        i=0
        while IFS= read -r name; do
            printf 'asset.%s.name=%s\n' "$i" "$name"
            printf 'asset.%s.sha256=%s\n' "$i" "$(hash_file "$fixture/$name")"
            i=$((i + 1))
        done <<EOF_NAMES
$names
EOF_NAMES
    } > "$fixture/release.txt"
}

prepare_fixture() {
    fixture=$1
    mkdir -p "$fixture"
    cat > "$fixture/cup-linux-x64" <<'FAKE_CUP'
#!/usr/bin/env sh
set -eu
select_root() {
    base=$1; primary=$base/.cup; fallback=$base/.coffee-cup
    if [ ! -e "$primary" ] && [ ! -L "$primary" ]; then printf '%s\n' "$primary"; return 0; fi
    if [ -f "$primary/root.txt" ] && grep -Fx 'product=coffee-clang/cup' "$primary/root.txt" >/dev/null 2>&1; then printf '%s\n' "$primary"; return 0; fi
    if [ ! -e "$fallback" ] && [ ! -L "$fallback" ]; then printf '%s\n' "$fallback"; return 0; fi
    if [ -f "$fallback/root.txt" ] && grep -Fx 'product=coffee-clang/cup' "$fallback/root.txt" >/dev/null 2>&1; then printf '%s\n' "$fallback"; return 0; fi
    return 1
}
if [ "${1:-}" = --version ]; then printf 'cup %s\n' "${CUP_TEST_RELEASE_VERSION:?}"; exit 0; fi
if [ "${1:-}" = --internal-select-root ]; then [ "$#" -eq 2 ] || exit 79; select_root "$2"; exit $?; fi
if [ "${1:-}" = --internal-root-probe ]; then
    [ "$#" -eq 2 ] || exit 79; root=$2
    [ -f "$root/root.txt" ] && [ ! -L "$root/root.txt" ] || exit 1
    grep -Fx 'format=2' "$root/root.txt" >/dev/null || exit 1
    grep -Fx 'product=coffee-clang/cup' "$root/root.txt" >/dev/null || exit 1
    grep -Fx 'layout=2' "$root/root.txt" >/dev/null || exit 1
    grep -Fx 'host=linux-x64' "$root/root.txt" >/dev/null || exit 1
    [ -f "$root/bin/cup" ] && [ ! -L "$root/bin/cup" ] || exit 1
    cmp -s "$0" "$root/bin/cup" || exit 1
    exit 0
fi
if [ "${1:-}" = list ] && [ "${2:-}" = package-manager ]; then
    case "${CUP_COFFEE_MODE:-unavailable}" in
        poststate) printf 'package-manager: coffee@1.0.0\n' ;;
    esac
    exit 0
fi
if [ "${1:-}" = install ] && [ "${2:-}" = coffee ]; then
    printf 'coffee\n' >> "${CUP_COFFEE_TRACE:?}"
    case "${CUP_COFFEE_MODE:-unavailable}" in
        success) exit 0 ;;
        poststate)
            printf 'Error: wrapper reconciliation failed.\n' >&2
            exit 6
            ;;
        transaction)
            root=$(select_root "$HOME") || exit 84
            printf 'format=2\noperation=install\n' > "$root/transaction.txt"
            printf 'Error: package installation was interrupted.\n' >&2
            exit 6
            ;;
        *)
            printf 'Error: package coffee is unavailable.\n' >&2
            exit 6
            ;;
    esac
fi
[ "$#" -eq 3 ] && [ "$1" = --internal-bootstrap ] || exit 80
source_directory=$2; base=$3
case "$source_directory:$base" in /*:/*) ;; *) exit 81 ;; esac
[ -d "$source_directory" ] && [ ! -L "$source_directory" ] || exit 82
count=0
for entry in "$source_directory"/*; do [ -f "$entry" ] && [ ! -L "$entry" ] || exit 82; count=$((count + 1)); done
[ "$count" -eq 5 ] || exit 83
for required in release.txt cup-linux-x64 LICENSE THIRD_PARTY_NOTICES.txt catalog.cfg; do [ -f "$source_directory/$required" ] || exit 83; done
printf '%s\n' "$source_directory" > "$CUP_BOOTSTRAP_TRACE"
root=$(select_root "$base") || exit 84
"$CUP_TEST_MKDIR" -p "$root/bin" "$root/staging" "$root/config" "$root/components" "$root/tmp"
printf 'format=2\nproduct=coffee-clang/cup\nlayout=2\nhost=linux-x64\n' > "$root/root.txt"
: > "$root/cup.lock"
"$CUP_TEST_CP" "$source_directory/cup-linux-x64" "$root/bin/cup"; chmod 0700 "$root/bin/cup"
"$CUP_TEST_CP" "$source_directory/release.txt" "$root/release.txt"
"$CUP_TEST_CP" "$source_directory/LICENSE" "$root/LICENSE"
"$CUP_TEST_CP" "$source_directory/THIRD_PARTY_NOTICES.txt" "$root/THIRD_PARTY_NOTICES.txt"
"$CUP_TEST_CP" "$source_directory/catalog.cfg" "$root/config/catalog.cfg"
printf 'format=2\n' > "$root/state.txt"
printf 'CUP_BOOTSTRAP_ROOT=%s\n' "$root"
printf 'cup core installed and verified.\n'
FAKE_CUP
    chmod 0755 "$fixture/cup-linux-x64"
    printf 'license fixture\n' > "$fixture/LICENSE"
    printf 'notices fixture\n' > "$fixture/THIRD_PARTY_NOTICES.txt"
    printf 'format=1\nrevision=0\nupdate_url=https://example.invalid/catalog.cfg\n' > "$fixture/catalog.cfg"
    cp "$WORK/install.sh" "$fixture/install.sh"
    cp "$WORK/install.ps1" "$fixture/install.ps1"
    write_manifest "$fixture" LICENSE THIRD_PARTY_NOTICES.txt catalog.cfg cup-linux-x64 install.ps1 install.sh
}

run_success() {
    shell_label=$1; shift
    fixture=$WORK/fixture-$shell_label; home=$WORK/home-$shell_label
    trace=$WORK/bootstrap-$shell_label.trace; downloads=$WORK/downloads-$shell_label.trace; coffee=$WORK/coffee-$shell_label.trace
    prepare_fixture "$fixture"; mkdir -m 0700 "$home"; : > "$downloads"; : > "$coffee"
    output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$downloads" \
        CUP_BOOTSTRAP_TRACE="$trace" CUP_COFFEE_TRACE="$coffee" CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 "$@" "$WORK/install.sh" 2>&1)
    printf '%s\n' "$output" | grep -F "cup $VERSION installed successfully." >/dev/null || fail "$shell_label install did not complete"
    printf '%s\n' "$output" | grep -F 'Warning: Coffee was not installed' >/dev/null || fail "$shell_label did not report optional Coffee outcome"
    [ "$(wc -l < "$coffee" | tr -d ' ')" -eq 1 ] || fail "$shell_label did not attempt Coffee exactly once on fresh install"
    expected='release.txt
cup-linux-x64
LICENSE
THIRD_PARTY_NOTICES.txt
catalog.cfg'
    [ "$(cat "$downloads")" = "$expected" ] || { cat "$downloads" >&2; fail "$shell_label downloaded an unexpected asset set"; }
    [ "$(cat "$trace")" != "" ] || fail "$shell_label did not invoke native bootstrap"
    [ -f "$home/.cup/release.txt" ] && [ -f "$home/.cup/LICENSE" ] && [ -f "$home/.cup/THIRD_PARTY_NOTICES.txt" ] || fail "$shell_label did not install complete generation"

    # Reinstalling the existing root is synchronous and must not re-bootstrap optional Coffee.
    : > "$downloads"; : > "$coffee"
    HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$downloads" \
        CUP_BOOTSTRAP_TRACE="$trace" CUP_COFFEE_TRACE="$coffee" CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 "$@" "$WORK/install.sh" >/dev/null 2>&1
    [ ! -s "$coffee" ] || fail "$shell_label reinstalled Coffee after an existing-root reinstall"
}

run_success sh sh
command -v dash >/dev/null 2>&1 && run_success dash dash
command -v busybox >/dev/null 2>&1 && run_success busybox busybox sh

# Coffee bootstrap reports outcomes from persistent package state.
fixture=$WORK/coffee-poststate-fixture; home=$WORK/coffee-poststate-home
prepare_fixture "$fixture"; mkdir -m 0700 "$home"; : > "$WORK/coffee-poststate-downloads"; : > "$WORK/coffee-poststate-trace"
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/coffee-poststate-downloads" CUP_BOOTSTRAP_TRACE="$WORK/coffee-poststate-bootstrap" \
    CUP_COFFEE_TRACE="$WORK/coffee-poststate-trace" CUP_COFFEE_MODE=poststate CUP_TEST_RELEASE_VERSION="$VERSION" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" 2>&1)
printf '%s\n' "$output" | grep -F 'Coffee was installed, but derived commands need repair' >/dev/null ||
    fail 'post-state Coffee failure was misreported as not installed'
printf '%s\n' "$output" | grep -F "cup $VERSION installed successfully." >/dev/null ||
    fail 'post-state Coffee failure incorrectly failed the core installation'
[ "$(wc -l < "$WORK/coffee-poststate-trace" | tr -d ' ')" -eq 1 ] ||
    fail 'post-state Coffee fixture did not execute one install attempt'

fixture=$WORK/coffee-transaction-fixture; home=$WORK/coffee-transaction-home
prepare_fixture "$fixture"; mkdir -m 0700 "$home"; : > "$WORK/coffee-transaction-downloads"; : > "$WORK/coffee-transaction-trace"
set +e
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/coffee-transaction-downloads" CUP_BOOTSTRAP_TRACE="$WORK/coffee-transaction-bootstrap" \
    CUP_COFFEE_TRACE="$WORK/coffee-transaction-trace" CUP_COFFEE_MODE=transaction CUP_TEST_RELEASE_VERSION="$VERSION" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" 2>&1); status=$?
set -e
[ "$status" -ne 0 ] || fail 'unresolved Coffee transaction was reported as a clean installation'
printf '%s\n' "$output" | grep -F 'optional Coffee installation left an unresolved cup transaction' >/dev/null ||
    fail 'unresolved Coffee transaction did not receive its specific diagnosis'
[ -f "$home/.cup/transaction.txt" ] || fail 'Coffee transaction evidence was not preserved'

# A foreign .cup collision must publish into .coffee-cup, never merge into foreign bytes.
fixture=$WORK/foreign-fixture; home=$WORK/foreign-home; prepare_fixture "$fixture"
mkdir -m 0700 "$home" "$home/.cup" "$home/.cup/bin"; printf 'foreign\n' > "$home/.cup/bin/cup"
: > "$WORK/foreign-downloads"; : > "$WORK/foreign-coffee"
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/foreign-downloads" \
    CUP_BOOTSTRAP_TRACE="$WORK/foreign-bootstrap" CUP_COFFEE_TRACE="$WORK/foreign-coffee" CUP_TEST_RELEASE_VERSION="$VERSION" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" 2>&1)
printf '%s\n' "$output" | grep -F "Binary: $home/.coffee-cup/bin/cup" >/dev/null || fail 'foreign primary root did not select fallback'
[ -f "$home/.cup/bin/cup" ] && grep -Fx foreign "$home/.cup/bin/cup" >/dev/null || fail 'foreign primary root was modified'

# Manifest-authenticated bytes must fail before bootstrap/root mutation.
fixture=$WORK/tampered-fixture; home=$WORK/tampered-home; prepare_fixture "$fixture"; printf 'tampered\n' >> "$fixture/catalog.cfg"; mkdir -m 0700 "$home"; : > "$WORK/tampered-downloads"; : > "$WORK/tampered-coffee"
set +e
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/tampered-downloads" \
    CUP_BOOTSTRAP_TRACE="$WORK/tampered-bootstrap" CUP_COFFEE_TRACE="$WORK/tampered-coffee" CUP_TEST_RELEASE_VERSION="$VERSION" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" 2>&1); status=$?
set -e
[ "$status" -ne 0 ] || fail 'tampered catalog unexpectedly succeeded'
printf '%s\n' "$output" | grep -F 'release manifest digest mismatch for catalog.cfg' >/dev/null || fail 'tampered catalog was not rejected by manifest digest'
[ ! -e "$home/.cup" ] || fail 'tampered release mutated managed root'

# Authenticated metadata identity must match the prepared installer.
fixture=$WORK/identity-fixture; home=$WORK/identity-home; prepare_fixture "$fixture"; sed -i "s/^version=.*/version=9.9.9/" "$fixture/release.txt"; mkdir -m 0700 "$home"; : > "$WORK/identity-downloads"; : > "$WORK/identity-coffee"
set +e
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/identity-downloads" \
    CUP_BOOTSTRAP_TRACE="$WORK/identity-bootstrap" CUP_COFFEE_TRACE="$WORK/identity-coffee" CUP_TEST_RELEASE_VERSION="$VERSION" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" 2>&1); status=$?
set -e
[ "$status" -ne 0 ] || fail 'wrong release identity unexpectedly succeeded'
printf '%s\n' "$output" | grep -F 'release metadata version does not match the installer' >/dev/null || fail 'wrong release identity was not explained'

# Windows shell handoff authenticates exactly release.txt + install.ps1 before PowerShell.
fixture=$WORK/windows-fixture; prepare_fixture "$fixture"; : > "$WORK/windows-downloads"; rm -f "$WORK/windows-powershell"
PATH="$WORK/windows-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/windows-downloads" CUP_POWERSHELL_TRACE="$WORK/windows-powershell" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" >/dev/null 2>&1
[ "$(cat "$WORK/windows-downloads")" = $'release.txt\ninstall.ps1' ] || fail 'Windows shell handoff downloaded an unexpected asset set'
grep -F -- '-NoProfile -ExecutionPolicy Bypass -File' "$WORK/windows-powershell" >/dev/null || fail 'Windows handoff did not invoke PowerShell safely'

fixture=$WORK/windows-tampered-fixture; prepare_fixture "$fixture"; printf '# tampered\n' >> "$fixture/install.ps1"; : > "$WORK/windows-tampered-downloads"; rm -f "$WORK/windows-tampered-powershell"
set +e
output=$(PATH="$WORK/windows-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/windows-tampered-downloads" CUP_POWERSHELL_TRACE="$WORK/windows-tampered-powershell" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 sh "$WORK/install.sh" 2>&1); status=$?
set -e
[ "$status" -ne 0 ] || fail 'tampered Windows installer unexpectedly succeeded'
printf '%s\n' "$output" | grep -F 'release manifest digest mismatch for install.ps1' >/dev/null || fail 'tampered Windows handoff was not rejected'
[ ! -e "$WORK/windows-tampered-powershell" ] || fail 'PowerShell ran before handoff authentication'

run_pretransport_failure() {
    name=$1; expected=$2; shift 2
    fixture=$WORK/pre-$name-fixture; home=$WORK/pre-$name-home; prepare_fixture "$fixture"; mkdir -m 0700 "$home"; : > "$WORK/pre-$name-downloads"; : > "$WORK/pre-$name-coffee"
    set +e
    output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/pre-$name-downloads" \
        CUP_BOOTSTRAP_TRACE="$WORK/pre-$name-bootstrap" CUP_COFFEE_TRACE="$WORK/pre-$name-coffee" CUP_TEST_RELEASE_VERSION="$VERSION" \
        "$@" sh "$WORK/install.sh" 2>&1); status=$?
    set -e
    [ "$status" -ne 0 ] || fail "pretransport failure unexpectedly succeeded: $name"
    printf '%s\n' "$output" | grep -F "$expected" >/dev/null || fail "pretransport failure did not explain $name"
    [ ! -s "$WORK/pre-$name-downloads" ] || fail "$name reached transport before rejection"
    [ ! -e "$home/.cup" ] || fail "$name mutated root before rejection"
}
run_pretransport_failure arbitrary-https 'release base URL override is test-only' env CUP_INSTALL_BASE_URL=https://example.invalid
run_pretransport_failure remote-http 'allowed loopback host and explicit port' env CUP_INSTALL_BASE_URL=http://example.invalid:18080 CUP_INSTALL_ALLOW_INSECURE=1
run_pretransport_failure missing-port 'allowed loopback host and explicit port' env CUP_INSTALL_BASE_URL=http://127.0.0.1 CUP_INSTALL_ALLOW_INSECURE=1
run_pretransport_failure bad-port 'invalid port' env CUP_INSTALL_BASE_URL=http://127.0.0.1:65536 CUP_INSTALL_ALLOW_INSECURE=1

# Interrupts stop transport immediately rather than resuming mutation.
mkdir -p "$WORK/interrupt-bin"
cat > "$WORK/interrupt-bin/curl" <<'EOF_INTERRUPT'
#!/usr/bin/env sh
kill -INT "$PPID"
sleep 1
exit 1
EOF_INTERRUPT
chmod 0755 "$WORK/interrupt-bin/curl"
set +e
HOME="$WORK/interrupt-home" PATH="$WORK/interrupt-bin:$PATH" CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    sh "$WORK/install.sh" >/dev/null 2>&1
status=$?
set -e
[ "$status" -eq 130 ] || fail "installer interrupt returned $status instead of 130"

printf 'Installer behavior repository tests passed.\n'
