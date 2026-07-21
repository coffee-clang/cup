#!/usr/bin/env bash

# Purpose: Exercises the standalone Linux executable through the network path
# CUP actually uses: DNS, HTTPS certificate validation, proxy tunnelling,
# checksum verification, package download, extraction and wrapper creation.
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
PLATFORM="${PLATFORM:-linux-x64}"
DEPS_PREFIX="${DEPS_PREFIX:-$HOME/deps/$PLATFORM/install}"
JOBS="${CUP_TEST_JOBS:-4}"

fail() {
    printf 'Linux portability test: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' was not found"
}

case "$PLATFORM" in
    linux-x64|linux-arm64) ;;
    *) fail "unsupported platform '$PLATFORM'" ;;
esac
case "$JOBS" in
    ''|*[!0-9]*|0) fail "CUP_TEST_JOBS must be a positive integer" ;;
esac

for tool in cc curl make openssl sha256sum tar; do
    require_tool "$tool"
done
"$ROOT/scripts/dependencies/verify.sh" "$PLATFORM" "$DEPS_PREFIX" >/dev/null

WORK="$(mktemp -d "${TMPDIR:-/tmp}/cup-linux-portability.XXXXXX")"
SOURCE="$WORK/source"
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
    rm -rf -- "$WORK"
}
trap cleanup EXIT HUP INT TERM

port_cursor=$((20000 + ($$ % 20000)))
choose_port() {
    local candidate
    while :; do
        candidate=$port_cursor
        port_cursor=$((port_cursor + 1))
        if ! (exec 3<>"/dev/tcp/127.0.0.1/$candidate") 2>/dev/null; then
            selected_port=$candidate
            return
        fi
    done
}

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

wait_for_proxy() {
    local port=$1
    local attempt=0
    while [ "$attempt" -lt 50 ]; do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&- 3<&-
            return
        fi
        attempt=$((attempt + 1))
        sleep 0.1
    done
    fail "proxy fixture did not start on port $port"
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
    local port=$2
    local log_file=$3
    (
        cd "$SERVER_ROOT"
        exec openssl s_server -quiet -WWW \
            -accept "127.0.0.1:$port" \
            -cert "$directory/server.pem" -key "$directory/server.key"
    ) >"$log_file" 2>&1 &
    PIDS+=("$!")
    wait_for_tls "$port" "$directory/ca.pem"
}

write_manifest() {
    local port=$1
    local package_path='{version}-{host_platform}-{target_platform}'
    local package_name='clang-{version}-{host_platform}-{target_platform}.{format}'
    local release_url="https://localhost:$port/$package_path"
    cat >"$SOURCE/config/packages.cfg" <<MANIFEST
compiler.clang.$PLATFORM.$PLATFORM.stable_version=99.0.0
compiler.clang.$PLATFORM.$PLATFORM.available_versions=99.0.0
compiler.clang.$PLATFORM.$PLATFORM.default_format=tar.gz
compiler.clang.$PLATFORM.$PLATFORM.formats=tar.gz
compiler.clang.$PLATFORM.$PLATFORM.url_template=$release_url/$package_name
compiler.clang.$PLATFORM.$PLATFORM.checksum_url_template=$release_url/SHA256SUMS
MANIFEST
}

run_cup_without_proxy() {
    local home=$1
    shift
    env -u ALL_PROXY -u all_proxy -u HTTPS_PROXY -u https_proxy \
        -u HTTP_PROXY -u http_proxy -u NO_PROXY -u no_proxy \
        HOME="$home" "$@"
}

mkdir -p "$SOURCE" "$SERVER_ROOT" "$PACKAGE_ROOT"
(
    cd "$ROOT"
    tar --exclude='./.git' --exclude='./build' -cf - .
) | tar -xf - -C "$SOURCE"

generate_ca_and_server "$TRUSTED" 'CUP portability trusted CA'
generate_ca_and_server "$UNTRUSTED" 'CUP portability untrusted CA'
cp "$TRUSTED/ca.pem" "$SOURCE/certs/cacert.pem"

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

choose_port
trusted_port=$selected_port
choose_port
untrusted_port=$selected_port
choose_port
proxy_port=$selected_port
start_https_server "$TRUSTED" "$trusted_port" "$WORK/trusted-server.log"
start_https_server "$UNTRUSTED" "$untrusted_port" "$WORK/untrusted-server.log"

printf '==> Building a static Linux test release with an isolated CA bundle...\n'
write_manifest "$trusted_port"
make -C "$SOURCE" -j"$JOBS" PLATFORM="$PLATFORM" \
    DEPS_PREFIX="$DEPS_PREFIX" release >/dev/null
make -C "$SOURCE" PLATFORM="$PLATFORM" DEPS_PREFIX="$DEPS_PREFIX" \
    CUP_BUILD_CONFIGURATION=release check-binary >/dev/null
make -C "$SOURCE" PLATFORM="$PLATFORM" DEPS_PREFIX="$DEPS_PREFIX" \
    CUP_TEST_CONFIGURATION=release test-helpers >/dev/null
CUP="$SOURCE/build/$PLATFORM/release/bin/cup"
[ -x "$CUP" ] || fail "release executable was not produced: $CUP"

printf '==> Rejecting a server outside the embedded trust bundle...\n'
write_manifest "$untrusted_port"
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

printf '==> Downloading and extracting through direct HTTPS and DNS...\n'
write_manifest "$trusted_port"
mkdir -p "$WORK/home-direct"
(
    cd "$SOURCE"
    run_cup_without_proxy "$WORK/home-direct" \
        "$CUP" install compiler clang@stable
) >"$WORK/direct.out" 2>&1
[ "$(HOME="$WORK/home-direct" "$WORK/home-direct/.cup/bin/clang")" = portable-clang ] ||
    fail 'the directly downloaded package wrapper did not execute'
(
    cd "$SOURCE"
    run_cup_without_proxy "$WORK/home-direct" "$CUP" doctor
) >"$WORK/direct-doctor.out" 2>&1
grep -F 'Doctor found no issues.' "$WORK/direct-doctor.out" >/dev/null ||
    fail 'doctor rejected the directly installed package'

printf '==> Downloading and extracting through an HTTP CONNECT proxy...\n'
proxy_helper="$SOURCE/build/$PLATFORM/release/tests/helpers/connect-proxy"
[ -x "$proxy_helper" ] || fail "proxy helper was not built: $proxy_helper"
"$proxy_helper" "$proxy_port" "$PROXY_LOG" \
    >"$WORK/proxy-server.out" 2>&1 &
PIDS+=("$!")
wait_for_proxy "$proxy_port"
mkdir -p "$WORK/home-proxy"
(
    cd "$SOURCE"
    env -u ALL_PROXY -u all_proxy -u NO_PROXY -u no_proxy \
        HTTPS_PROXY="http://127.0.0.1:$proxy_port" \
        https_proxy="http://127.0.0.1:$proxy_port" \
        HOME="$WORK/home-proxy" \
        "$CUP" install compiler clang@stable
) >"$WORK/proxy.out" 2>&1
[ "$(HOME="$WORK/home-proxy" "$WORK/home-proxy/.cup/bin/clang")" = portable-clang ] ||
    fail 'the proxied package wrapper did not execute'
connect_count=$(grep -Fc "CONNECT localhost:$trusted_port" "$PROXY_LOG" || true)
[ "$connect_count" -ge 2 ] ||
    fail "expected checksum and package downloads through the proxy, got $connect_count"

printf '%s\n' \
    'Linux portability test passed:' \
    '  unknown CA rejected' \
    '  localhost DNS and HTTPS accepted with the embedded test CA' \
    '  checksum and package downloaded directly' \
    '  checksum and package downloaded through an HTTP CONNECT proxy' \
    '  archive extracted, wrapper executed and doctor passed'
