#!/bin/sh

# Verifies deterministic CA source generation and safe update behavior.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin certificates
require_test_binary
command -v perl >/dev/null 2>&1 || fail 'certificate tests require Perl'
perl -MTime::Piece -e 1 >/dev/null 2>&1 ||
    fail 'certificate tests require the core Perl Time::Piece module'

test_build_root=${CUP_TEST_BUILD_ROOT:-$PROJECT_ROOT/build}

invalid_pem=$TMP_ROOT/invalid-balanced.pem
: > "$invalid_pem"
invalid_index=0
while [ "$invalid_index" -lt 100 ]; do
    cat >> "$invalid_pem" <<'INVALID_PEM'
-----BEGIN CERTIFICATE-----
Y3VwLW5vdC1hbi14NTA5LWNlcnRpZmljYXRl
-----END CERTIFICATE-----
INVALID_PEM
    invalid_index=$((invalid_index + 1))
done
invalid_meta=$TMP_ROOT/invalid-balanced.meta
if command -v sha256sum >/dev/null 2>&1; then
    invalid_sha=$(sha256sum "$invalid_pem" | awk '{print $1}')
else
    invalid_sha=$(shasum -a 256 "$invalid_pem" | awk '{print $1}')
fi
cat > "$invalid_meta" <<EOF_INVALID_META
format=1
source=https://example.invalid/cacert.pem
source_date=2026-01-01
sha256=$invalid_sha
certificate_count=100
max_age_days=365
EOF_INVALID_META
if CUP_CA_META_FILE="$invalid_meta" \
        "$PROJECT_ROOT/scripts/certs/generate-ca-bundle.sh" \
        "$invalid_pem" "$TMP_ROOT/invalid-generated" \
        >"$TMP_ROOT/invalid-generator.out" 2>&1; then
    fail 'CA generator accepted balanced non-X.509 PEM data'
fi
assert_contains "$(cat "$TMP_ROOT/invalid-generator.out")" 'invalid X.509 certificate data'

generated=$TMP_ROOT/generated
"$PROJECT_ROOT/scripts/certs/generate-ca-bundle.sh" \
    "$PROJECT_ROOT/certs/cacert.pem" "$generated"

assert_file "$generated/ca_bundle.h"
assert_file "$generated/ca_bundle.c"
cmp "$generated/ca_bundle.h" \
    "$test_build_root/$TEST_PLATFORM/development/generated/ca_bundle.h" >/dev/null ||
    fail 'generated CA bundle header is not deterministic'
cmp "$generated/ca_bundle.c" \
    "$test_build_root/$TEST_PLATFORM/development/generated/ca_bundle.c" >/dev/null ||
    fail 'generated CA bundle source is not deterministic'

printf '%s\n' 'CA bundle generation tests passed.'

# macOS supplies TMPDIR with a trailing slash and may expose it through a
# host-managed alias such as /var -> /private/var. The shared resolver must
# canonicalize that outer host representation before strict path validation.
temp_real=$TMP_ROOT/ca-temp-real
temp_alias=$TMP_ROOT/ca-temp-alias
mkdir "$temp_real"
ln -s "$temp_real" "$temp_alias"
alias_generated=$TMP_ROOT/generated-through-temp-alias
TMPDIR="$temp_alias/" "$PROJECT_ROOT/scripts/certs/generate-ca-bundle.sh" \
    "$PROJECT_ROOT/certs/cacert.pem" "$alias_generated"
cmp "$generated/ca_bundle.h" "$alias_generated/ca_bundle.h" >/dev/null ||
    fail 'CA generation changed through a canonicalized host temporary alias'
cmp "$generated/ca_bundle.c" "$alias_generated/ca_bundle.c" >/dev/null ||
    fail 'CA generation changed through a canonicalized host temporary alias'

# Build-time CA generation owns scratch space below the managed build root and
# therefore must not depend on an ambient TMPDIR representation.
build_generated=$test_build_root/$TEST_PLATFORM/development/generated-ca-temp-regression
CUP_BUILD_ROOT=$test_build_root TMPDIR="$TMP_ROOT/nonexistent/../bad/" \
    "$PROJECT_ROOT/scripts/certs/generate-ca-bundle.sh" \
    "$PROJECT_ROOT/certs/cacert.pem" "$build_generated"
cmp "$generated/ca_bundle.h" "$build_generated/ca_bundle.h" >/dev/null ||
    fail 'managed build-root CA generation changed with an unusable ambient TMPDIR'
cmp "$generated/ca_bundle.c" "$build_generated/ca_bundle.c" >/dev/null ||
    fail 'managed build-root CA generation changed with an unusable ambient TMPDIR'

printf '%s\n' 'CA temporary-path regression tests passed.'

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
assert_contains "$(cat "$TMP_ROOT/bad-hash.out")" 'metadata SHA-256'

cp "$PROJECT_ROOT/certs/cacert.meta" "$meta_copy"
head -c -1 "$meta_copy" > "$TMP_ROOT/cacert-nul.meta"
printf '\0\n' >> "$TMP_ROOT/cacert-nul.meta"
if CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$TMP_ROOT/cacert-nul.meta" \
        CUP_CA_CURRENT_EPOCH=$((source_epoch + 30 * 86400)) "$checker" \
        >"$TMP_ROOT/nul-meta.out" 2>&1; then
    fail 'CA checker accepted a hidden NUL byte in metadata'
fi
assert_contains "$(cat "$TMP_ROOT/nul-meta.out")" 'NUL or carriage-return byte'

head -c -1 "$PROJECT_ROOT/certs/cacert.meta" > "$TMP_ROOT/cacert-no-lf.meta"
if CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$TMP_ROOT/cacert-no-lf.meta" \
        CUP_CA_CURRENT_EPOCH=$((source_epoch + 30 * 86400)) "$checker" \
        >"$TMP_ROOT/no-lf-meta.out" 2>&1; then
    fail 'CA checker accepted metadata without a final LF'
fi
assert_contains "$(cat "$TMP_ROOT/no-lf-meta.out")" 'metadata is not canonical'

sed 's/$/\r/' "$PROJECT_ROOT/certs/cacert.meta" > "$TMP_ROOT/cacert-crlf.meta"
if CUP_CA_CERT_FILE="$pem_copy" CUP_CA_META_FILE="$TMP_ROOT/cacert-crlf.meta" \
        CUP_CA_CURRENT_EPOCH=$((source_epoch + 30 * 86400)) "$checker" \
        >"$TMP_ROOT/crlf-meta.out" 2>&1; then
    fail 'CA checker accepted CRLF metadata'
fi
assert_contains "$(cat "$TMP_ROOT/crlf-meta.out")" 'NUL or carriage-return byte'

cp "$PROJECT_ROOT/certs/cacert.pem" "$TMP_ROOT/cacert-nul.pem"
printf '\0' >> "$TMP_ROOT/cacert-nul.pem"
if CUP_CA_CERT_FILE="$TMP_ROOT/cacert-nul.pem" CUP_CA_META_FILE="$meta_copy" \
        "$checker" --integrity >"$TMP_ROOT/nul-pem.out" 2>&1; then
    fail 'CA checker accepted a NUL byte in the PEM bundle'
fi
assert_contains "$(cat "$TMP_ROOT/nul-pem.out")" 'NUL or carriage-return byte'

sed 's/$/\r/' "$PROJECT_ROOT/certs/cacert.pem" > "$TMP_ROOT/cacert-cr.pem"
if CUP_CA_CERT_FILE="$TMP_ROOT/cacert-cr.pem" CUP_CA_META_FILE="$meta_copy" \
        "$checker" --integrity >"$TMP_ROOT/cr-pem.out" 2>&1; then
    fail 'CA checker accepted carriage returns in the PEM bundle'
fi
assert_contains "$(cat "$TMP_ROOT/cr-pem.out")" 'NUL or carriage-return byte'

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
