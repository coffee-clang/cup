#!/usr/bin/env bash

# Verifies that generated installers are transport-only, fail closed, and hand one exact
# verified generation to the hidden canonical bootstrap entry point.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/cup-installer-behavior.XXXXXX")"
VERSION=$(tr -d '\n' < "$ROOT/VERSION")
TAG=v$VERSION
SHA=0123456789abcdef0123456789abcdef01234567
CUP_TEST_CP=$(command -v cp) || exit 1
CUP_TEST_MKDIR=$(command -v mkdir) || exit 1
export CUP_TEST_CP CUP_TEST_MKDIR

cleanup() {
    [ ! -e "$WORK" ] || rm -rf -- "$WORK"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

check_shell_syntax() {
    label=$1
    shift
    "$@" -n "$ROOT/scripts/install/install.sh" ||
        fail "$label rejected the POSIX installer"
}
check_shell_syntax sh sh
if command -v dash >/dev/null 2>&1; then
    check_shell_syntax dash dash
fi
if command -v busybox >/dev/null 2>&1; then
    check_shell_syntax 'BusyBox sh' busybox sh
fi

# The transporters must not contain a second managed transaction implementation.
for forbidden in '.bootstrap' 'binary.old' 'platform-checksums.old' \
        'File.Replace' 'Move-Item'; do
    ! grep -F "$forbidden" "$ROOT/scripts/install/install.sh" \
        "$ROOT/scripts/install/install.ps1" >/dev/null ||
        fail "installer still contains managed transaction state: $forbidden"
done
grep -F -- '--internal-bootstrap' "$ROOT/scripts/install/install.sh" >/dev/null
grep -F -- '--internal-bootstrap' "$ROOT/scripts/install/install.ps1" >/dev/null
grep -F -- '$MaxRedirects = if ($BaseUrlOverridden) { 0 } else { 10 }' "$ROOT/scripts/install/install.ps1" >/dev/null

# Windows private transport ACLs use SID objects directly and are applied when
# the directory is created, avoiding account-name translation and a
# create-then-protect window.
! grep -F -- '$identity.User.Value' "$ROOT/scripts/install/install.ps1" >/dev/null ||
    fail 'Windows installer translates the current user SID through an account-name string'
! grep -F -- "'NT AUTHORITY\SYSTEM'" "$ROOT/scripts/install/install.ps1" >/dev/null ||
    fail 'Windows installer depends on the localized SYSTEM account name'
! grep -F -- "'BUILTIN\Administrators'" "$ROOT/scripts/install/install.ps1" >/dev/null ||
    fail 'Windows installer depends on the localized Administrators account name'
grep -F -- '[Security.Principal.WellKnownSidType]::LocalSystemSid' \
    "$ROOT/scripts/install/install.ps1" >/dev/null
grep -F -- '[Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid' \
    "$ROOT/scripts/install/install.ps1" >/dev/null
grep -F -- '[IO.Directory]::CreateDirectory($path, $security)' \
    "$ROOT/scripts/install/install.ps1" >/dev/null

# Windows PowerShell adapts Nullable<T> properties to their scalar value/null.
# Do not require Nullable<T> members such as HasValue/Value on Content-Length.
! grep -F -- 'ContentLength.HasValue' "$ROOT/scripts/install/install.ps1" >/dev/null ||
    fail 'Windows installer assumes Nullable<T>.HasValue survives PowerShell adaptation'
! grep -F -- 'ContentLength.Value' "$ROOT/scripts/install/install.ps1" >/dev/null ||
    fail 'Windows installer assumes Nullable<T>.Value survives PowerShell adaptation'
grep -F -- '$contentLength = $response.Content.Headers.ContentLength' \
    "$ROOT/scripts/install/install.ps1" >/dev/null
grep -F -- '$null -ne $contentLength -and $contentLength -gt $maximum' \
    "$ROOT/scripts/install/install.ps1" >/dev/null

SCRIPT_DIR="$ROOT/scripts/release"
# shellcheck source=scripts/release/common.sh
. "$SCRIPT_DIR/common.sh"
prepare_installer "$ROOT/scripts/install/install.sh" "$WORK/install.sh" 0755
chmod 0755 "$WORK/install.sh"

mkdir -p "$WORK/mock-bin"
cat > "$WORK/mock-bin/curl" <<'MOCK_CURL'
#!/usr/bin/env sh
set -eu
output=
url=
proto=
proto_redir=
max_redirs=
connect_timeout=
max_time=
speed_time=
speed_limit=
max_filesize=
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
cat > "$WORK/windows-bin/uname" <<'MOCK_UNAME'
#!/usr/bin/env sh
case "${1:-}" in
    -s) printf 'MINGW64_NT-10.0\n' ;;
    -m) printf 'x86_64\n' ;;
    *) exit 2 ;;
esac
MOCK_UNAME
cat > "$WORK/windows-bin/cygpath" <<'MOCK_CYGPATH'
#!/usr/bin/env sh
[ "$#" -eq 2 ] && [ "$1" = -w ]
printf '%s\n' "$2"
MOCK_CYGPATH
cat > "$WORK/windows-bin/powershell.exe" <<'MOCK_POWERSHELL'
#!/usr/bin/env sh
set -eu
printf '%s\n' "$*" > "$CUP_POWERSHELL_TRACE"
MOCK_POWERSHELL
chmod 0755 "$WORK/windows-bin/uname" "$WORK/windows-bin/cygpath" \
    "$WORK/windows-bin/powershell.exe"

hash_file() {
    sha256sum "$1" | awk '{print $1}'
}

prepare_fixture() {
    fixture=$1
    mkdir -p "$fixture"
    cat > "$fixture/cup-linux-x64" <<'FAKE_CUP'
#!/usr/bin/env sh
set -eu
if [ "${1:-}" = --version ]; then
    printf 'cup %s\n' "${CUP_TEST_RELEASE_VERSION:?}"
    exit 0
fi
if [ "${1:-}" = --internal-runtime-ready ]; then
    if [ -n "${CUP_TEST_RUNTIME_READY_RETRY_FILE:-}" ] &&
        [ ! -e "$CUP_TEST_RUNTIME_READY_RETRY_FILE" ]; then
        : > "$CUP_TEST_RUNTIME_READY_RETRY_FILE"
        exit 1
    fi
    printf 'Doctor found no issues.\n'
    exit 0
fi
[ "$#" -eq 2 ] && [ "$1" = --internal-bootstrap ]
source_directory=$2
case "$source_directory" in /*) ;; *) exit 81 ;; esac
[ -d "$source_directory" ] && [ ! -L "$source_directory" ]
count=0
for entry in "$source_directory"/*; do
    [ -f "$entry" ] && [ ! -L "$entry" ] || exit 82
    count=$((count + 1))
done
[ "$count" -eq 8 ] || exit 83
printf '%s\n' "$source_directory" > "$CUP_BOOTSTRAP_TRACE"
primary=$HOME/.cup
fallback=$HOME/.coffee-cup
if [ ! -e "$primary" ] && [ ! -L "$primary" ]; then
    root=$primary
elif [ ! -e "$fallback" ] && [ ! -L "$fallback" ]; then
    root=$fallback
else
    exit 84
fi
"$CUP_TEST_MKDIR" -p "$root/bin" "$root/staging"
printf 'format=1\nproduct=coffee-clang/cup\nlayout=1\n' > "$root/root.txt"
"$CUP_TEST_CP" "$0" "$root/bin/cup"
chmod 0700 "$root/bin/cup"
printf 'CUP_BOOTSTRAP_ROOT=%s\n' "$root"
printf 'Verified cup installation scheduled.\n'
FAKE_CUP
    chmod 0755 "$fixture/cup-linux-x64"
    cat > "$fixture/release.txt" <<EOF_METADATA
format=1
version=$VERSION
commit=$SHA
EOF_METADATA
    printf 'format=1\n' > "$fixture/packages.cfg"
    printf 'format=1\n' > "$fixture/install.cfg"
    cp "$WORK/install.sh" "$fixture/install.sh"
    printf '# generated Windows transport fixture\n' > "$fixture/install.ps1"
    {
        for asset in packages.cfg install.cfg install.sh install.ps1; do
            printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
        done
    } > "$fixture/SHA256SUMS.common"
    {
        for asset in cup-linux-x64 release.txt SHA256SUMS.common; do
            printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
        done
    } > "$fixture/SHA256SUMS.linux-x64"
}

prepare_windows_handoff_fixture() {
    fixture=$1
    prepare_fixture "$fixture"
    prepare_installer "$ROOT/scripts/install/install.ps1" "$fixture/install.ps1" 0644
    printf 'fake Windows binary\n' > "$fixture/cup-windows-x64.exe"
    {
        for asset in packages.cfg install.cfg install.sh install.ps1; do
            printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
        done
    } > "$fixture/SHA256SUMS.common"
    {
        for asset in cup-windows-x64.exe release.txt SHA256SUMS.common; do
            printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
        done
    } > "$fixture/SHA256SUMS.windows-x64"
}

run_success() {
    shell_label=$1
    shift
    fixture=$WORK/fixture-$shell_label
    home=$WORK/home-$shell_label
    trace=$WORK/bootstrap-$shell_label.trace
    downloads=$WORK/downloads-$shell_label.trace
    prepare_fixture "$fixture"
    mkdir -m 0700 "$home"
    : > "$downloads"

    output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" \
        CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$downloads" \
        CUP_BOOTSTRAP_TRACE="$trace" CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
        CUP_INSTALL_WAIT_ATTEMPTS=2 "$@" "$WORK/install.sh" 2>&1)
    printf '%s\n' "$output" | grep -F "cup $VERSION installed successfully." >/dev/null || {
        printf '%s\n' "$output" >&2
        fail "valid transport did not complete under $shell_label"
    }
    [ "$(wc -l < "$downloads")" -eq 8 ] || fail "$shell_label did not download exactly eight assets"
    [ -s "$trace" ] || fail "$shell_label did not invoke the hidden bootstrap"
    source_directory=$(tr -d '\n' < "$trace")
    [ ! -e "$source_directory" ] || fail "$shell_label did not remove the private transport directory"
    [ -x "$home/.cup/bin/cup" ] || fail "$shell_label did not observe the canonical commit"
    [ ! -e "$home/.cup/.bootstrap" ] || fail "$shell_label created unexpected bootstrap state"
}
run_success sh sh
if command -v dash >/dev/null 2>&1; then run_success dash dash; fi
if command -v busybox >/dev/null 2>&1; then run_success busybox busybox sh; fi

# A normal POSIX installation must not depend on undeclared host utilities.
# The fixture itself uses absolute test-only helpers, while PATH exposes only
# the shell plus the commands require_commands() deliberately accepts.
portable_bin=$WORK/portable-bin
portable_fixture=$WORK/fixture-portable-path
portable_home=$WORK/home-portable-path
portable_trace=$WORK/bootstrap-portable-path.trace
portable_downloads=$WORK/downloads-portable-path.trace
mkdir -p "$portable_bin"
for tool in chmod cmp mktemp rm sha256sum sh sleep uname wc; do
    tool_path=$(command -v "$tool") || fail "test prerequisite is unavailable: $tool"
    ln -s "$tool_path" "$portable_bin/$tool"
done
cp "$WORK/mock-bin/curl" "$portable_bin/curl"
chmod 0755 "$portable_bin/curl"
prepare_fixture "$portable_fixture"
mkdir -m 0700 "$portable_home"
: > "$portable_downloads"
portable_output=$(
    HOME="$portable_home" PATH="$portable_bin" \
        CUP_FIXTURE="$portable_fixture" CUP_DOWNLOAD_TRACE="$portable_downloads" \
        CUP_BOOTSTRAP_TRACE="$portable_trace" CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
        CUP_INSTALL_WAIT_ATTEMPTS=2 /bin/sh "$WORK/install.sh" 2>&1
)
printf '%s\n' "$portable_output" | grep -F "cup $VERSION installed successfully." >/dev/null || {
    printf '%s\n' "$portable_output" >&2
    fail 'POSIX installer depends on an undeclared external command'
}

# BSD/macOS wc may pad a single count with leading spaces. The installer must
# normalize that presentation without weakening the numeric size check.
mkdir -p "$WORK/bsd-wc-bin"
real_wc=$(command -v wc) || fail 'wc is unavailable for the BSD-style fixture'
cat > "$WORK/bsd-wc-bin/wc" <<'MOCK_BSD_WC'
#!/usr/bin/env sh
set -eu
result=$("${CUP_REAL_WC:?}" "$@") || exit $?
case "${1:-}" in
    -c) printf '    %s\n' "$result" ;;
    *) printf '%s\n' "$result" ;;
esac
MOCK_BSD_WC
chmod 0755 "$WORK/bsd-wc-bin/wc"
bsd_wc_fixture=$WORK/fixture-bsd-wc
bsd_wc_home=$WORK/home-bsd-wc
bsd_wc_trace=$WORK/bootstrap-bsd-wc.trace
bsd_wc_downloads=$WORK/downloads-bsd-wc.trace
prepare_fixture "$bsd_wc_fixture"
mkdir -m 0700 "$bsd_wc_home"
: > "$bsd_wc_downloads"
bsd_wc_output=$(
    HOME="$bsd_wc_home" PATH="$WORK/bsd-wc-bin:$WORK/mock-bin:$PATH" \
        CUP_REAL_WC="$real_wc" CUP_FIXTURE="$bsd_wc_fixture" \
        CUP_DOWNLOAD_TRACE="$bsd_wc_downloads" CUP_BOOTSTRAP_TRACE="$bsd_wc_trace" \
        CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
        CUP_INSTALL_WAIT_ATTEMPTS=2 sh "$WORK/install.sh" 2>&1
)
printf '%s\n' "$bsd_wc_output" | grep -F "cup $VERSION installed successfully." >/dev/null || {
    printf '%s\n' "$bsd_wc_output" >&2
    fail 'installer rejected BSD-style padded wc output'
}

# macOS temporary directories may be reached through aliases such as /var or
# /tmp. The installer must pass the physical private directory to the bootstrap
# so the bootstrap's no-follow directory-chain validation remains meaningful.
physical_tmp=$WORK/physical-tmp
alias_tmp=$WORK/alias-tmp
mkdir -m 0700 "$physical_tmp"
ln -s "$physical_tmp" "$alias_tmp"
physical_fixture=$WORK/fixture-physical-tmp
physical_home=$WORK/home-physical-tmp
physical_trace=$WORK/bootstrap-physical-tmp.trace
physical_downloads=$WORK/downloads-physical-tmp.trace
prepare_fixture "$physical_fixture"
mkdir -m 0700 "$physical_home"
: > "$physical_downloads"
physical_output=$(
    HOME="$physical_home" TMPDIR="$alias_tmp" PATH="$WORK/mock-bin:$PATH" \
        CUP_FIXTURE="$physical_fixture" CUP_DOWNLOAD_TRACE="$physical_downloads" \
        CUP_BOOTSTRAP_TRACE="$physical_trace" CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
        CUP_INSTALL_WAIT_ATTEMPTS=2 sh "$WORK/install.sh" 2>&1
)
printf '%s\n' "$physical_output" | grep -F "cup $VERSION installed successfully." >/dev/null || {
    printf '%s\n' "$physical_output" >&2
    fail 'installer rejected a private transport directory reached through a symlink alias'
}
physical_source=$(tr -d '\n' < "$physical_trace")
case "$physical_source" in
    "$physical_tmp"/cup-install.*) ;;
    *) fail "installer did not canonicalize the private transport directory: $physical_source" ;;
esac
[ ! -e "$physical_source" ] || fail 'canonical private transport directory was not removed'

# The installer must not report success until the installed binary can acquire
# its runtime snapshot. The fixture makes the first readiness probe fail and the
# second succeed, modelling the detached helper releasing its lock.
ready_fixture=$WORK/fixture-runtime-ready
ready_home=$WORK/home-runtime-ready
ready_trace=$WORK/bootstrap-runtime-ready.trace
ready_downloads=$WORK/downloads-runtime-ready.trace
ready_probe=$WORK/runtime-ready.probe
prepare_fixture "$ready_fixture"
mkdir -m 0700 "$ready_home"
: > "$ready_downloads"
ready_output=$(
    HOME="$ready_home" PATH="$WORK/mock-bin:$PATH" \
        CUP_FIXTURE="$ready_fixture" \
        CUP_DOWNLOAD_TRACE="$ready_downloads" \
        CUP_BOOTSTRAP_TRACE="$ready_trace" \
        CUP_TEST_RELEASE_VERSION="$VERSION" \
        CUP_TEST_RUNTIME_READY_RETRY_FILE="$ready_probe" \
        CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 \
        CUP_INSTALL_ALLOW_INSECURE=1 CUP_INSTALL_WAIT_ATTEMPTS=3 \
        sh "$WORK/install.sh" 2>&1
)
[ -f "$ready_probe" ] || fail 'installer did not probe installed runtime readiness'
printf '%s\n' "$ready_output" | grep -F "cup $VERSION installed successfully." >/dev/null ||
    fail 'installer did not retry until the installed runtime became ready'

# The POSIX entrypoint may hand off to PowerShell only after authenticating the
# Windows installer through the existing platform-to-common checksum chain.
fixture=$WORK/windows-handoff-fixture
prepare_windows_handoff_fixture "$fixture"
: > "$WORK/windows-handoff-downloads"
rm -f -- "$WORK/windows-handoff-powershell"
HOME="$WORK/windows-handoff-home" PATH="$WORK/windows-bin:$PATH" \
    CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/windows-handoff-downloads" \
    CUP_POWERSHELL_TRACE="$WORK/windows-handoff-powershell" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    sh "$WORK/install.sh"
[ -s "$WORK/windows-handoff-powershell" ] || fail 'verified Windows handoff did not invoke PowerShell'
[ "$(wc -l < "$WORK/windows-handoff-downloads")" -eq 7 ] ||
    fail 'Windows handoff did not download the exact verification asset set'

fixture=$WORK/windows-handoff-tampered-fixture
prepare_windows_handoff_fixture "$fixture"
printf '# tampered body with unchanged release identity\n' >> "$fixture/install.ps1"
grep -F "\$ReleaseVersion = \"$VERSION\"" "$fixture/install.ps1" >/dev/null ||
    fail 'tamper fixture lost the valid release version literal'
grep -F "\$ReleaseTag = \"$TAG\"" "$fixture/install.ps1" >/dev/null ||
    fail 'tamper fixture lost the valid release tag literal'
grep -F "\$ReleaseCommit = \"$SHA\"" "$fixture/install.ps1" >/dev/null ||
    fail 'tamper fixture lost the valid release commit literal'
: > "$WORK/windows-handoff-tampered-downloads"
rm -f -- "$WORK/windows-handoff-tampered-powershell"
set +e
output=$(HOME="$WORK/windows-handoff-tampered-home" PATH="$WORK/windows-bin:$PATH" \
    CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/windows-handoff-tampered-downloads" \
    CUP_POWERSHELL_TRACE="$WORK/windows-handoff-tampered-powershell" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    sh "$WORK/install.sh" 2>&1)
status=$?
set -e
[ "$status" -ne 0 ] || fail 'tampered Windows installer body unexpectedly executed'
printf '%s\n' "$output" | grep -F 'checksum mismatch for install.ps1' >/dev/null ||
    fail 'tampered Windows installer body was not rejected by its published digest'
[ ! -e "$WORK/windows-handoff-tampered-powershell" ] ||
    fail 'PowerShell was invoked for a tampered Windows installer body'

# Re-signing the tampered body only in an arbitrary common checksum document is
# insufficient: the platform checksum must still bind that common document to the release.
fixture=$WORK/windows-handoff-unbound-common-fixture
prepare_windows_handoff_fixture "$fixture"
printf '# tampered body with locally rewritten common checksum\n' >> "$fixture/install.ps1"
{
    for asset in packages.cfg install.cfg install.sh install.ps1; do
        printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
    done
} > "$fixture/SHA256SUMS.common"
: > "$WORK/windows-handoff-unbound-downloads"
rm -f -- "$WORK/windows-handoff-unbound-powershell"
set +e
output=$(HOME="$WORK/windows-handoff-unbound-home" PATH="$WORK/windows-bin:$PATH" \
    CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$WORK/windows-handoff-unbound-downloads" \
    CUP_POWERSHELL_TRACE="$WORK/windows-handoff-unbound-powershell" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    sh "$WORK/install.sh" 2>&1)
status=$?
set -e
[ "$status" -ne 0 ] || fail 'unbound common checksum unexpectedly authorized Windows installer'
printf '%s\n' "$output" | grep -F 'checksum mismatch for SHA256SUMS.common' >/dev/null ||
    fail 'unbound common checksum was not rejected by the platform binding'
[ ! -e "$WORK/windows-handoff-unbound-powershell" ] ||
    fail 'PowerShell was invoked with an unbound common checksum document'

run_failure() {
    name=$1
    expected=$2
    fixture=$WORK/failure-$name-fixture
    home=$WORK/failure-$name-home
    downloads=$WORK/failure-$name-downloads
    shift 2
    prepare_fixture "$fixture"
    mkdir -m 0700 "$home"
    : > "$downloads"
    set +e
    output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" \
        CUP_FIXTURE="$fixture" CUP_DOWNLOAD_TRACE="$downloads" \
        CUP_BOOTSTRAP_TRACE="$WORK/failure-$name-bootstrap" \
        CUP_TEST_RELEASE_VERSION="$VERSION" CUP_INSTALL_WAIT_ATTEMPTS=1 "$@" sh "$WORK/install.sh" 2>&1)
    status=$?
    set -e
    [ "$status" -ne 0 ] || fail "failure case unexpectedly succeeded: $name"
    printf '%s\n' "$output" | grep -F "$expected" >/dev/null || {
        printf '%s\n' "$output" >&2
        fail "failure case did not explain $name"
    }
    [ ! -s "$downloads" ] || fail "failure case reached transport before rejection: $name"
    [ ! -e "$WORK/failure-$name-bootstrap" ] ||
        fail "failure case reached bootstrap before rejection: $name"
    [ ! -e "$home/.cup" ] || fail "failure case mutated the managed root: $name"
}

run_failure arbitrary-https \
    'release base URL override is test-only and requires CUP_INSTALL_ALLOW_INSECURE=1' \
    env CUP_INSTALL_BASE_URL=https://example.invalid
run_failure arbitrary-https-opted-in \
    'installer release base URL override must use loopback HTTP' \
    env CUP_INSTALL_BASE_URL=https://example.invalid CUP_INSTALL_ALLOW_INSECURE=1
run_failure invalid-scheme \
    'installer release base URL override must use loopback HTTP' \
    env CUP_INSTALL_BASE_URL=ftp://example.invalid CUP_INSTALL_ALLOW_INSECURE=1
run_failure explicit-userinfo 'installer test release base URL is invalid' \
    env CUP_INSTALL_BASE_URL=http://user@127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1
run_failure remote-http \
    'installer release base URL override must use an allowed loopback host and explicit port' \
    env CUP_INSTALL_BASE_URL=http://example.invalid:18080 CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-without-opt-in \
    'release base URL override is test-only and requires CUP_INSTALL_ALLOW_INSECURE=1' \
    env CUP_INSTALL_BASE_URL=http://127.0.0.1:1234
run_failure loopback-missing-port \
    'installer release base URL override must use an allowed loopback host and explicit port' \
    env CUP_INSTALL_BASE_URL=http://127.0.0.1 CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-port-zero \
    'installer release base URL override has an invalid port' \
    env CUP_INSTALL_BASE_URL=http://127.0.0.1:0 CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-port-too-large \
    'installer release base URL override has an invalid port' \
    env CUP_INSTALL_BASE_URL=http://127.0.0.1:65536 CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-port-nonnumeric \
    'installer release base URL override has an invalid port' \
    env CUP_INSTALL_BASE_URL=http://127.0.0.1:nope CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-query 'installer test release base URL is invalid' \
    env 'CUP_INSTALL_BASE_URL=http://127.0.0.1:18080/path?query' CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-fragment 'installer test release base URL is invalid' \
    env 'CUP_INSTALL_BASE_URL=http://127.0.0.1:18080/path#fragment' CUP_INSTALL_ALLOW_INSECURE=1
run_failure loopback-backslash 'installer test release base URL is invalid' \
    env 'CUP_INSTALL_BASE_URL=http://127.0.0.1:18080/path\evil' CUP_INSTALL_ALLOW_INSECURE=1

fixture=$WORK/tampered-fixture
prepare_fixture "$fixture"
printf 'tampered\n' >> "$fixture/packages.cfg"
home=$WORK/tampered-home
mkdir -m 0700 "$home"
: > "$WORK/tampered-downloads"
set +e
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/tampered-downloads" CUP_BOOTSTRAP_TRACE="$WORK/tampered-bootstrap" \
    CUP_TEST_RELEASE_VERSION="$VERSION" CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 \
    CUP_INSTALL_ALLOW_INSECURE=1 CUP_INSTALL_WAIT_ATTEMPTS=1 \
    sh "$WORK/install.sh" 2>&1)
status=$?
set -e
[ "$status" -ne 0 ] || fail 'tampered common asset unexpectedly succeeded'
printf '%s\n' "$output" | grep -F 'checksum mismatch for packages.cfg' >/dev/null ||
    fail 'tampered common asset failure was not explained'
[ ! -e "$home/.cup" ] || fail 'tampered common asset mutated the managed root'

fixture=$WORK/metadata-fixture
prepare_fixture "$fixture"
printf 'format=1\nversion=9.9.9\ncommit=%s\n' "$SHA" > "$fixture/release.txt"
# Re-authenticate the intentionally wrong metadata so the identity check, not SHA, rejects it.
{
    for asset in cup-linux-x64 release.txt SHA256SUMS.common; do
        printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
    done
} > "$fixture/SHA256SUMS.linux-x64"
home=$WORK/metadata-home
mkdir -m 0700 "$home"
: > "$WORK/metadata-downloads"
set +e
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/metadata-downloads" CUP_BOOTSTRAP_TRACE="$WORK/metadata-bootstrap" \
    CUP_TEST_RELEASE_VERSION="$VERSION" CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 \
    CUP_INSTALL_ALLOW_INSECURE=1 CUP_INSTALL_WAIT_ATTEMPTS=1 \
    sh "$WORK/install.sh" 2>&1)
status=$?
set -e
[ "$status" -ne 0 ] || fail 'wrong authenticated release identity unexpectedly succeeded'
printf '%s\n' "$output" | grep -F 'release metadata version does not match the installer' >/dev/null ||
    fail 'wrong authenticated release identity was not explained'
[ ! -e "$home/.cup" ] || fail 'wrong release identity mutated the managed root'


# A foreign primary root must not be mistaken for the generation installed in the fallback root.
fixture=$WORK/foreign-root-fixture
home=$WORK/foreign-root-home
prepare_fixture "$fixture"
mkdir -m 0700 "$home" "$home/.cup" "$home/.cup/bin"
printf '#!/usr/bin/env sh\nprintf "unrelated program\\n"\n' > "$home/.cup/bin/cup"
chmod 0755 "$home/.cup/bin/cup"
: > "$WORK/foreign-root-downloads"
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/foreign-root-downloads" \
    CUP_BOOTSTRAP_TRACE="$WORK/foreign-root-bootstrap" \
    CUP_TEST_RELEASE_VERSION="$VERSION" CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    CUP_INSTALL_WAIT_ATTEMPTS=2 sh "$WORK/install.sh" 2>&1)
printf '%s\n' "$output" | grep -F "Binary: $home/.coffee-cup/bin/cup" >/dev/null || {
    printf '%s\n' "$output" >&2
    fail 'installer reported the foreign primary root instead of the committed fallback root'
}

# Canonical checksum documents must end after the expected LF-terminated records.
fixture=$WORK/trailing-checksum-fixture
home=$WORK/trailing-checksum-home
prepare_fixture "$fixture"
printf 'evil' >> "$fixture/SHA256SUMS.common"
{
    for asset in cup-linux-x64 release.txt SHA256SUMS.common; do
        printf '%s  %s\n' "$(hash_file "$fixture/$asset")" "$asset"
    done
} > "$fixture/SHA256SUMS.linux-x64"
mkdir -m 0700 "$home"
: > "$WORK/trailing-checksum-downloads"
set +e
output=$(HOME="$home" PATH="$WORK/mock-bin:$PATH" CUP_FIXTURE="$fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/trailing-checksum-downloads" \
    CUP_BOOTSTRAP_TRACE="$WORK/trailing-checksum-bootstrap" \
    CUP_TEST_RELEASE_VERSION="$VERSION" CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    CUP_INSTALL_WAIT_ATTEMPTS=1 sh "$WORK/install.sh" 2>&1)
status=$?
set -e
[ "$status" -ne 0 ] || fail 'checksum document with unterminated trailing bytes succeeded'
printf '%s\n' "$output" | grep -F 'checksum document has unexpected entries' >/dev/null ||
    fail 'unterminated checksum bytes were not explained'

# The generated POSIX installer must reject non-canonical release versions before transport.
invalid_installer=$WORK/install-invalid-version.sh
sed \
    -e 's/CUP_RELEASE_VERSION="[^"]*"/CUP_RELEASE_VERSION="1"/' \
    -e 's/CUP_RELEASE_TAG="[^"]*"/CUP_RELEASE_TAG="v1"/' \
    "$WORK/install.sh" > "$invalid_installer"
chmod 0755 "$invalid_installer"
set +e
output=$(HOME="$WORK/invalid-version-home" PATH="$WORK/mock-bin:$PATH" \
    CUP_TEST_RELEASE_VERSION=1 CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    sh "$invalid_installer" 2>&1)
status=$?
set -e
[ "$status" -ne 0 ] || fail 'non-canonical installer release version succeeded'
printf '%s\n' "$output" | grep -F 'installer has an invalid release version' >/dev/null ||
    fail 'non-canonical installer release version was not explained'

# An interrupt must stop the installer rather than resume after cleanup.
mkdir -p "$WORK/interrupt-bin"
cat > "$WORK/interrupt-bin/curl" <<'INTERRUPT_CURL'
#!/usr/bin/env sh
set -eu
kill -INT "$PPID"
sleep 1
exit 1
INTERRUPT_CURL
chmod 0755 "$WORK/interrupt-bin/curl"
signal_home=$WORK/signal-home
mkdir -m 0700 "$signal_home"
set +e
HOME="$signal_home" PATH="$WORK/interrupt-bin:$PATH" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 CUP_TEST_RELEASE_VERSION="$VERSION" \
    sh "$WORK/install.sh" >"$WORK/signal.out" 2>&1
status=$?
set -e
[ "$status" -eq 130 ] || {
    cat "$WORK/signal.out" >&2
    fail "installer interrupt returned $status instead of 130"
}
! grep -F 'installed successfully' "$WORK/signal.out" >/dev/null ||
    fail 'installer continued to success after SIGINT'

# The public installer requires curl so every transport uses one bounded policy.
curl_required_bin=$WORK/curl-required-bin
mkdir -p "$curl_required_bin"
for tool in chmod cmp mkdir mktemp rm sha256sum sh sleep uname wc; do
    tool_path=$(command -v "$tool") || fail "test prerequisite is unavailable: $tool"
    ln -s "$tool_path" "$curl_required_bin/$tool"
done
curl_required_home=$WORK/curl-required-home
mkdir -m 0700 "$curl_required_home"
set +e
curl_required_output=$(HOME="$curl_required_home" PATH="$curl_required_bin" \
    CUP_TEST_RELEASE_VERSION="$VERSION" CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    sh "$WORK/install.sh" 2>&1)
curl_required_status=$?
set -e
[ "$curl_required_status" -ne 0 ] || fail 'installer succeeded without curl'
printf '%s\n' "$curl_required_output" | grep -F \
    'required command is unavailable: curl' >/dev/null ||
    fail 'missing curl was not explained'

# The post-download size check remains authoritative when a transporter ignores its own limit.
oversize_installer=$WORK/install-oversize-limit.sh
sed 's/MAX_TEXT_BYTES=16777216/MAX_TEXT_BYTES=8/' "$WORK/install.sh" > "$oversize_installer"
chmod 0755 "$oversize_installer"
oversize_fixture=$WORK/oversize-fixture
oversize_home=$WORK/oversize-home
prepare_fixture "$oversize_fixture"
mkdir -m 0700 "$oversize_home"
: > "$WORK/oversize-downloads"
set +e
oversize_output=$(
    HOME="$oversize_home" \
    PATH="$WORK/mock-bin:$PATH" \
    CUP_FIXTURE="$oversize_fixture" \
    CUP_DOWNLOAD_TRACE="$WORK/oversize-downloads" \
    CUP_BOOTSTRAP_TRACE="$WORK/oversize-bootstrap" \
    CUP_TEST_RELEASE_VERSION="$VERSION" \
    CUP_INSTALL_BASE_URL=http://127.0.0.1:18080 CUP_INSTALL_ALLOW_INSECURE=1 \
    CUP_INSTALL_WAIT_ATTEMPTS=1 \
    sh "$oversize_installer" 2>&1
)
oversize_status=$?
set -e
[ "$oversize_status" -ne 0 ] || fail 'oversized text asset unexpectedly succeeded'
printf '%s\n' "$oversize_output" | grep -F 'downloaded asset is too large' >/dev/null ||
    fail 'oversized text asset failure was not explained'

printf 'Installer transport behavior checks passed.\n'
