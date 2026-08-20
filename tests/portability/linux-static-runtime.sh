#!/usr/bin/env bash

# Validates Linux-specific static runtime properties through
# embedded-CA HTTPS and proxy fixtures.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
PLATFORM="${PLATFORM:-linux-x64}"
DEPS_PREFIX="${DEPS_PREFIX:-$HOME/deps/$PLATFORM/install}"
JOBS="${CUP_TEST_JOBS:-4}"

fail() {
    printf 'Linux static runtime test: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found"
}

case "$PLATFORM" in
    linux-x64|linux-arm64) ;;
    *)
        fail "unsupported platform '$PLATFORM'"
        ;;
esac
case "$JOBS" in
    ''|*[!0-9]*|0)
        fail "CUP_TEST_JOBS must be a positive integer"
        ;;
esac

for tool in awk cc curl date make openssl readlink sha256sum tar; do
    require_tool "$tool"
done
"$ROOT/scripts/dependencies/verify.sh" "$PLATFORM" "$DEPS_PREFIX" >/dev/null

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cup-linux-static-runtime.XXXXXX")"
SOURCE="$WORK/source"
SOURCE_BUILD_ROOT="$SOURCE/build"
SERVER_ROOT="$WORK/server"
PACKAGE_ROOT="$WORK/package"
TRUSTED="$WORK/trusted"
UNTRUSTED="$WORK/untrusted"
PROXY_LOG="$WORK/proxy.log"
PIDS=()

cleanup() {
    local pid
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    for pid in "${PIDS[@]:-}"; do
        wait "$pid" >/dev/null 2>&1 || true
    done
    if [ -d "$WORK" ] && [ ! -L "$WORK" ]; then
        rm -rf -- "$WORK"
    fi
}
exit_handler() {
    local status=$?
    trap - EXIT HUP INT TERM
    cleanup
    exit "$status"
}
signal_handler() {
    local status=$1
    trap - EXIT HUP INT TERM
    cleanup
    exit "$status"
}
trap exit_handler EXIT
trap 'signal_handler 129' HUP
trap 'signal_handler 130' INT
trap 'signal_handler 143' TERM

wait_for_tls() {
    local port=$1
    local ca_file=$2
    local attempt
    attempt=0
    while [ "$attempt" -lt 50 ]; do
        if curl --noproxy '*' --connect-timeout 1 --max-time 2 -sS \
            --cacert "$ca_file" \
            "https://localhost:$port/SHA256SUMS" >/dev/null 2>&1; then
            return
        fi
        attempt=$((attempt + 1))
        sleep 0.1
    done
    fail "HTTPS fixture did not start on port $port"
}

discover_listen_port() {
    local pid=$1
    local attempt fd target inode endpoint port_hex

    for ((attempt = 0; attempt < 100; attempt++)); do
        [ -d "/proc/$pid/fd" ] || return 1
        for fd in "/proc/$pid/fd"/*; do
            target=$(readlink "$fd" 2>/dev/null || true)
            case "$target" in
                'socket:['*']')
                    inode=${target#socket:[}
                    inode=${inode%]}
                    endpoint=$(awk -v inode="$inode" \
                        '$4 == "0A" && $10 == inode { print $2; exit }' \
                        /proc/net/tcp)
                    [ -n "$endpoint" ] || continue
                    port_hex=${endpoint##*:}
                    printf -v selected_port '%d' "0x$port_hex"
                    [ "$selected_port" -gt 0 ] && return 0
                    ;;
            esac
        done
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.05
    done
    return 1
}

wait_for_ready_port() {
    local ready_file=$1
    local pid=$2
    local attempt port_text

    for ((attempt = 0; attempt < 100; attempt++)); do
        if [ -s "$ready_file" ]; then
            port_text=$(cat "$ready_file")
            case "$port_text" in
                ''|*[!0-9]*) fail "fixture reported invalid port: $port_text" ;;
            esac
            [ "$port_text" -ge 1 ] && [ "$port_text" -le 65535 ] ||
                fail "fixture reported out-of-range port: $port_text"
            selected_port=$port_text
            return
        fi
        kill -0 "$pid" 2>/dev/null || fail 'fixture exited before becoming ready'
        sleep 0.05
    done
    fail 'fixture did not publish its listening port'
}

generate_ca_and_server() {
    local directory=$1
    local common_name=$2
    mkdir -p "$directory"
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -subj "/CN=$common_name" \
        -addext 'basicConstraints=critical,CA:TRUE' \
        -addext 'keyUsage=critical,keyCertSign,cRLSign' \
        -keyout "$directory/ca.key" -out "$directory/ca.pem" \
        >/dev/null 2>&1
    openssl req -new -newkey rsa:2048 -nodes -subj '/CN=localhost' \
        -keyout "$directory/server.key" -out "$directory/server.csr" \
        >/dev/null 2>&1
    cat >"$directory/server.ext" <<'EXT'
subjectAltName=DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth
keyUsage=digitalSignature,keyEncipherment
EXT
    openssl x509 -req -days 1 -in "$directory/server.csr" \
        -CA "$directory/ca.pem" -CAkey "$directory/ca.key" \
        -CAcreateserial -extfile "$directory/server.ext" \
        -out "$directory/server.pem" >/dev/null 2>&1
}

start_https_server() {
    local directory=$1
    local log_file=$2
    local pid

    (
        cd "$SERVER_ROOT"
        exec openssl s_server -quiet -WWW \
            -accept '127.0.0.1:0' \
            -cert "$directory/server.pem" -key "$directory/server.key"
    ) >"$log_file" 2>&1 &
    pid=$!
    PIDS+=("$pid")
    discover_listen_port "$pid" || {
        cat "$log_file" >&2 || true
        fail 'could not discover the HTTPS fixture port'
    }
    wait_for_tls "$selected_port" "$directory/ca.pem"
}

write_package_catalog() {
    local port=$1
    local package_path='{version}-{host_platform}-{target_platform}'
    local package_name='clang-{version}-{host_platform}-{target_platform}.{format}'
    local release_url="https://localhost:$port/$package_path"
    cat >"$SOURCE/config/packages.cfg" <<PACKAGE_CATALOG
format=1
compiler.clang.$PLATFORM.$PLATFORM.stable_version=99.0.0
compiler.clang.$PLATFORM.$PLATFORM.available_versions=99.0.0
compiler.clang.$PLATFORM.$PLATFORM.default_format=tar.gz
compiler.clang.$PLATFORM.$PLATFORM.formats=tar.gz
compiler.clang.$PLATFORM.$PLATFORM.url_template=$release_url/$package_name
compiler.clang.$PLATFORM.$PLATFORM.checksum_url_template=$release_url/SHA256SUMS
PACKAGE_CATALOG
}

run_cup_without_proxy() {
    local home=$1
    shift
    env -u ALL_PROXY -u all_proxy -u HTTPS_PROXY -u https_proxy \
        -u HTTP_PROXY -u http_proxy -u NO_PROXY -u no_proxy \
        HOME="$home" "$@"
}

verify_successful_install() {
    local home=$1
    local list_output wrapper_output staging

    list_output=$(HOME="$home" "$CUP" list compiler)
    grep -F 'compiler:clang@99.0.0' <<<"$list_output" >/dev/null ||
        fail 'installed package is missing from cup list'
    HOME="$home" "$CUP" doctor >/dev/null ||
        fail 'cup doctor rejected the completed installation'
    [ ! -e "$home/.cup/transaction.txt" ] ||
        fail 'completed installation left a transaction journal'
    staging="$home/.cup/staging"
    if [ -d "$staging" ] && find "$staging" -mindepth 1 -print -quit | grep -q .; then
        fail 'completed installation left staging entries'
    fi
    [ -x "$home/.cup/bin/clang" ] || fail 'managed clang wrapper is missing'
    wrapper_output=$(HOME="$home" "$home/.cup/bin/clang")
    [ "$wrapper_output" = portable-clang ] ||
        fail "managed clang wrapper returned: $wrapper_output"
}

mkdir -p "$SOURCE" "$SERVER_ROOT" "$PACKAGE_ROOT"
source_snapshot="$WORK/source.tar"
tar -C "$ROOT" --exclude='./.git' --exclude='./.vscode' --exclude='./build' \
    -cf "$source_snapshot" .
tar -xf "$source_snapshot" -C "$SOURCE"
rm -f -- "$source_snapshot"

generate_ca_and_server "$TRUSTED" 'cup portability trusted CA'
generate_ca_and_server "$UNTRUSTED" 'cup portability untrusted CA'
# The release validator intentionally rejects implausibly small trust stores. Repeat the
# isolated CA into a bounded 100-certificate fixture and authenticate that exact snapshot.
: > "$SOURCE/certs/cacert.pem"
for certificate_index in $(awk 'BEGIN { for (i = 1; i <= 100; ++i) print i }'); do
    cat "$TRUSTED/ca.pem" >> "$SOURCE/certs/cacert.pem"
done
cat > "$SOURCE/certs/cacert.meta" <<CA_METADATA
format=1
source=https://example.invalid/cup-portability-ca.pem
source_date=$(date -u +%F)
sha256=$(sha256sum "$SOURCE/certs/cacert.pem" | awk '{print $1}')
certificate_count=100
max_age_days=1
CA_METADATA

package_name="clang-99.0.0-$PLATFORM-$PLATFORM"
package_directory="$PACKAGE_ROOT/$package_name"
release_directory="$SERVER_ROOT/99.0.0-$PLATFORM-$PLATFORM"
mkdir -p "$package_directory/bin" "$release_directory"
cat >"$package_directory/info.txt" <<METADATA
package.component=compiler
package.tool=clang
package.version=99.0.0
platform.host=$PLATFORM
platform.target=$PLATFORM
entry.clang=bin/clang
METADATA
cat >"$package_directory/bin/clang" <<'PROGRAM'
#!/bin/sh
printf '%s\n' portable-clang
PROGRAM
chmod +x "$package_directory/bin/clang"
tar -czf "$release_directory/$package_name.tar.gz" \
    -C "$PACKAGE_ROOT" "$package_name"
(
    cd "$release_directory"
    sha256sum "$package_name.tar.gz" >SHA256SUMS
)

start_https_server "$TRUSTED" "$WORK/trusted-server.log"
trusted_port=$selected_port
start_https_server "$UNTRUSTED" "$WORK/untrusted-server.log"
untrusted_port=$selected_port

printf '==> Building a static Linux test release with an isolated CA bundle...\n'
write_package_catalog "$trusted_port"
make -C "$SOURCE" -j"$JOBS" PLATFORM="$PLATFORM" \
    BUILD_DIR="$SOURCE_BUILD_ROOT" DEPS_PREFIX="$DEPS_PREFIX" \
    CUP_INTERNAL_DEPS_TARGET=deps-check release >/dev/null
make -C "$SOURCE" PLATFORM="$PLATFORM" BUILD_DIR="$SOURCE_BUILD_ROOT" \
    DEPS_PREFIX="$DEPS_PREFIX" CUP_INTERNAL_DEPS_TARGET=deps-check \
    CUP_BUILD_CONFIGURATION=release check-binary >/dev/null
make -C "$SOURCE" PLATFORM="$PLATFORM" BUILD_DIR="$SOURCE_BUILD_ROOT" \
    DEPS_PREFIX="$DEPS_PREFIX" CUP_INTERNAL_DEPS_TARGET=deps-check \
    CUP_TEST_CONFIGURATION=release test-helpers >/dev/null
CUP="$SOURCE_BUILD_ROOT/$PLATFORM/release/bin/cup"
[ -x "$CUP" ] || fail "release executable was not produced: $CUP"

printf '==> Rejecting a server outside the embedded trust bundle...\n'
write_package_catalog "$untrusted_port"
mkdir -p "$WORK/home-untrusted"
if (
    cd "$SOURCE"
    run_cup_without_proxy "$WORK/home-untrusted" \
        "$CUP" install compiler clang@stable
) >"$WORK/untrusted.out" 2>&1; then
    fail 'an untrusted HTTPS server was accepted'
fi
grep -Eiq 'certificate|SSL|TLS' "$WORK/untrusted.out" || {
    cat "$WORK/untrusted.out" >&2
    fail 'the untrusted-server failure did not report TLS validation'
}
[ ! -e "$WORK/home-untrusted/.cup/transaction.txt" ] ||
    fail 'failed TLS installation left a transaction journal'
HOME="$WORK/home-untrusted" "$CUP" doctor >/dev/null ||
    fail 'cup doctor rejected state after the failed TLS installation'

printf '==> Exercising the static runtime through direct HTTPS...\n'
write_package_catalog "$trusted_port"
mkdir -p "$WORK/home-direct"
(
    cd "$SOURCE"
    run_cup_without_proxy "$WORK/home-direct" \
        "$CUP" install compiler clang@stable
) >"$WORK/direct.out" 2>&1
verify_successful_install "$WORK/home-direct"

printf '==> Exercising the static runtime through an HTTP CONNECT proxy...\n'
proxy_helper="$SOURCE_BUILD_ROOT/$PLATFORM/release/tests/helpers/network-helper"
[ -x "$proxy_helper" ] || fail "proxy helper was not built: $proxy_helper"
proxy_ready="$WORK/proxy.ready"
"$proxy_helper" connect-proxy 0 "$PROXY_LOG" "$proxy_ready" \
    >"$WORK/proxy-server.out" 2>&1 &
proxy_pid=$!
PIDS+=("$proxy_pid")
wait_for_ready_port "$proxy_ready" "$proxy_pid"
proxy_port=$selected_port
mkdir -p "$WORK/home-proxy"
(
    cd "$SOURCE"
    env -u ALL_PROXY -u all_proxy -u NO_PROXY -u no_proxy \
        HTTPS_PROXY="http://127.0.0.1:$proxy_port" \
        https_proxy="http://127.0.0.1:$proxy_port" \
        HOME="$WORK/home-proxy" \
        "$CUP" install compiler clang@stable
) >"$WORK/proxy.out" 2>&1
verify_successful_install "$WORK/home-proxy"
connect_count=$(grep -Fc "CONNECT localhost:$trusted_port" "$PROXY_LOG" || true)
[ "$connect_count" -ge 2 ] ||
    fail "expected checksum and package downloads through the proxy, got $connect_count"

printf '%s\n' \
    'Linux static runtime test passed:' \
    '  unknown CA rejected' \
    '  embedded test CA accepted through direct HTTPS' \
    '  HTTPS downloads traversed the HTTP CONNECT proxy'
