#!/usr/bin/env sh

# Purpose: Validates a newly downloaded CA bundle before replacing the versioned PEM.
# The candidate is generated and compiled in a temporary directory before commit.
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
CERT_FILE="$ROOT_DIR/certs/cacert.pem"
CACERT_URL="${CUP_CACERT_URL:-https://curl.se/ca/cacert.pem}"
CURL="${CURL:-curl}"
HOSTCC="${HOSTCC:-cc}"

WORK_DIR="$(mktemp -d "$ROOT_DIR/.ca-bundle-update.XXXXXX")"
PEM_TMP="$WORK_DIR/cacert.pem"
GENERATED_DIR="$WORK_DIR/generated"
OBJECT_TMP="$WORK_DIR/ca_bundle.o"

cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT HUP INT TERM

# Download into private staging over authenticated transport, then reject
# empty or non-PEM input before compiling the generated representation.
case "$CACERT_URL" in
    https://*) ;;
    *)
        printf 'Error: CA bundle URL must use HTTPS: %s\n' "$CACERT_URL" >&2
        exit 1
        ;;
esac
"$CURL" -fsSL --proto '=https' --proto-redir '=https' \
    "$CACERT_URL" -o "$PEM_TMP"
[ -s "$PEM_TMP" ] || {
    printf 'Error: downloaded CA bundle is empty.\n' >&2
    exit 1
}
grep -q '^-----BEGIN CERTIFICATE-----$' "$PEM_TMP" || {
    printf 'Error: downloaded CA bundle contains no PEM certificates.\n' >&2
    exit 1
}

# Compile the generated C representation before replacing the versioned bundle.
"$ROOT_DIR/scripts/certs/generate-ca-bundle.sh" "$PEM_TMP" "$GENERATED_DIR"
"$HOSTCC" -std=c11 -Wall -Wextra -Werror -I"$GENERATED_DIR" \
    -c "$GENERATED_DIR/ca_bundle.c" -o "$OBJECT_TMP"

if [ -f "$CERT_FILE" ] && cmp -s "$PEM_TMP" "$CERT_FILE"; then
    printf 'Embedded CA bundle is already up to date.\n'
    exit 0
fi

mkdir -p "$(dirname -- "$CERT_FILE")"
mv "$PEM_TMP" "$CERT_FILE"
printf 'Updated embedded CA bundle from %s\n' "$CACERT_URL"
