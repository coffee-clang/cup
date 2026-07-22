#!/bin/sh

# Purpose: Verifies deterministic CA source generation and safe update behavior.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/posix-cli.sh"

test_begin certificates
require_test_binary

generated=$TMP_ROOT/generated
"$PROJECT_ROOT/scripts/certs/generate-ca-bundle.sh" \
    "$PROJECT_ROOT/certs/cacert.pem" "$generated"

assert_file "$generated/ca_bundle.h"
assert_file "$generated/ca_bundle.c"
cmp "$generated/ca_bundle.h" \
    "$PROJECT_ROOT/build/$TEST_PLATFORM/development/generated/ca_bundle.h" >/dev/null ||
    fail 'generated CA bundle header is not deterministic'
cmp "$generated/ca_bundle.c" \
    "$PROJECT_ROOT/build/$TEST_PLATFORM/development/generated/ca_bundle.c" >/dev/null ||
    fail 'generated CA bundle source is not deterministic'

printf '%s\n' 'CA bundle generation tests passed.'

checker="$PROJECT_ROOT/scripts/certs/check-ca-bundle.sh"
meta_copy="$TMP_ROOT/cacert.meta"
pem_copy="$TMP_ROOT/cacert.pem"
cp "$PROJECT_ROOT/certs/cacert.meta" "$meta_copy"
cp "$PROJECT_ROOT/certs/cacert.pem" "$pem_copy"
source_date=$(sed -n 's/^source_date=//p' "$meta_copy")
[ -n "$source_date" ] || fail 'CA metadata is missing source_date'
source_epoch=$(perl -MTime::Piece -e '
    print Time::Piece->strptime($ARGV[0], "%Y-%m-%d")->epoch, "\n";
' "$source_date")
CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$meta_copy" \
    CUP_CA_CURRENT_EPOCH=$((source_epoch + 30 * 86400)) "$checker" >/dev/null

sed 's/^sha256=.*/sha256=bad/' "$PROJECT_ROOT/certs/cacert.meta" > "$meta_copy"
if CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$meta_copy" \
        CUP_CA_CURRENT_EPOCH=$((source_epoch + 30 * 86400)) "$checker" \
        >"$TMP_ROOT/bad-hash.out" 2>&1; then
    fail 'CA checker accepted mismatched metadata'
fi
assert_contains "$(cat "$TMP_ROOT/bad-hash.out")" 'SHA-256 does not match'

cp "$PROJECT_ROOT/certs/cacert.meta" "$meta_copy"
if CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$meta_copy" \
        CUP_CA_CURRENT_EPOCH=$((source_epoch - 86400)) "$checker" \
        >"$TMP_ROOT/future.out" 2>&1; then
    fail 'CA checker accepted a future source date'
fi
assert_contains "$(cat "$TMP_ROOT/future.out")" 'source date is in the future'

if CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$meta_copy" \
        CUP_CA_CURRENT_EPOCH=$((source_epoch + 121 * 86400)) "$checker" \
        >"$TMP_ROOT/stale.out" 2>&1; then
    fail 'CA checker accepted a stale bundle'
fi
assert_contains "$(cat "$TMP_ROOT/stale.out")" 'bundle is 121 days old'

printf '%s\n' 'CA metadata and freshness tests passed.'
