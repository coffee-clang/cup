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
