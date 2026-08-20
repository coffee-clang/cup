#!/bin/sh

# Verifies atomic run-attempt evidence index creation and exact lookup.
set -eu
TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$TESTS_ROOT/support/common.sh"
test_begin ci-evidence

WRITER=$PROJECT_ROOT/scripts/ci/write-tests-evidence-index.sh
VERIFIER=$PROJECT_ROOT/scripts/ci/verify-tests-evidence-index.sh
NAMES=$PROJECT_ROOT/scripts/ci/tests-evidence-artifacts.sh
REPOSITORY=example/cup
COMMIT=0123456789abcdef0123456789abcdef01234567
RUN_ID=31
ATTEMPT=2
JSON=$TMP_ROOT/artifacts.json
OUTPUT=$TMP_ROOT/index

write_json() {
    missing=${1:-none}
    duplicate=${2:-none}
    {
        printf '{"artifacts":['
        first=1
        id=100
        for name in $(. "$NAMES"; ci_tests_evidence_names "$ATTEMPT"); do
            [ "$name" = "$missing" ] && { id=$((id + 1)); continue; }
            [ "$first" -eq 1 ] || printf ','
            first=0
            printf '{"id":%s,"name":"%s","digest":"sha256:%064x","expired":false,' \
                "$id" "$name" "$id"
            printf '"workflow_run":{"id":%s,"head_sha":"%s"}}' "$RUN_ID" "$COMMIT"
            if [ "$name" = "$duplicate" ]; then
                printf ',{"id":%s,"name":"%s","digest":"sha256:%064x","expired":false,' \
                    "$((id + 1000))" "$name" "$((id + 1000))"
                printf '"workflow_run":{"id":%s,"head_sha":"%s"}}' "$RUN_ID" "$COMMIT"
            fi
            id=$((id + 1))
        done
        printf ']}\n'
    } > "$JSON"
}

write_json
"$WRITER" "$OUTPUT" "$JSON" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" >/dev/null
"$VERIFIER" "$OUTPUT/index.txt" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT"
[ "$("$VERIFIER" "$OUTPUT/index.txt" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
    --artifact-id cup-source-evidence-linux-x64-attempt-2)" = 107 ] ||
    fail 'artifact ID lookup returned the wrong identity'

if "$WRITER" "$OUTPUT" "$JSON" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
        >"$TMP_ROOT/existing.out" 2>&1; then
    fail 'evidence writer replaced an existing destination'
fi
grep -Fq 'already exists' "$TMP_ROOT/existing.out"

rm -rf "$OUTPUT"
missing=cup-source-evidence-macos-arm64-attempt-2
write_json "$missing"
if "$WRITER" "$OUTPUT" "$JSON" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
        >"$TMP_ROOT/missing.out" 2>&1; then
    fail 'evidence writer accepted a missing artifact'
fi
[ ! -e "$OUTPUT" ] || fail 'failed evidence generation exposed a partial destination'

write_json none cup-source-evidence-windows-x64-attempt-2
if "$WRITER" "$OUTPUT" "$JSON" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
        >"$TMP_ROOT/duplicate.out" 2>&1; then
    fail 'evidence writer accepted a duplicate artifact name'
fi
[ ! -e "$OUTPUT" ] || fail 'duplicate evidence generation exposed a partial destination'

write_json
sed -i '0,/"id":31,"head_sha"/s//"id":32,"head_sha"/' "$JSON"
if "$WRITER" "$OUTPUT" "$JSON" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
        >"$TMP_ROOT/wrong-run.out" 2>&1; then
    fail 'evidence writer accepted an artifact from a different workflow run'
fi
[ ! -e "$OUTPUT" ] || fail 'wrong-run evidence generation exposed a partial destination'
grep -Fq 'does not belong to run 31' "$TMP_ROOT/wrong-run.out"

write_json
"$WRITER" "$OUTPUT" "$JSON" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" >/dev/null
printf '\r' >> "$OUTPUT/index.txt"
if "$VERIFIER" "$OUTPUT/index.txt" "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
        >"$TMP_ROOT/cr.out" 2>&1; then
    fail 'evidence verifier accepted CR bytes'
fi
grep -Eq 'not LF-terminated|non-canonical bytes' "$TMP_ROOT/cr.out"


DEPENDENCY_WRITER=$PROJECT_ROOT/scripts/ci/write-dependency-evidence.sh
PREFIX=$TMP_ROOT/dependency-prefix
DEPENDENCY_OUTPUT=$TMP_ROOT/dependency-evidence
mkdir -p "$PREFIX"
cat > "$PREFIX/.cup-dependencies" <<'EOF_DEPENDENCY_METADATA'
prefix_format=5
product=coffee-clang/cup
kind=dependency-prefix
platform=linux-x64
profile=gcc
build_revision=4
source_lock_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
toolchain_sha256=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789
EOF_DEPENDENCY_METADATA
"$DEPENDENCY_WRITER" "$DEPENDENCY_OUTPUT" linux-x64 linux-x64 gcc "$PREFIX" \
    cache-key "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
    cup-dependency-evidence-linux-x64-gcc-attempt-2 >/dev/null
dependency_before=$(sha256sum "$DEPENDENCY_OUTPUT/evidence.txt" | awk '{print $1}')
if "$DEPENDENCY_WRITER" "$DEPENDENCY_OUTPUT" linux-x64 linux-x64 gcc "$PREFIX" \
        changed-key "$REPOSITORY" "$COMMIT" "$RUN_ID" "$ATTEMPT" \
        cup-dependency-evidence-linux-x64-gcc-attempt-2 \
        >"$TMP_ROOT/dependency-existing.out" 2>&1; then
    fail 'dependency evidence writer replaced an existing destination'
fi
[ "$(sha256sum "$DEPENDENCY_OUTPUT/evidence.txt" | awk '{print $1}')" = "$dependency_before" ] ||
    fail 'failed dependency evidence rewrite changed committed evidence'
grep -Fq 'already exists' "$TMP_ROOT/dependency-existing.out"

printf 'CI evidence index and dependency-writer contract tests passed.\n'
