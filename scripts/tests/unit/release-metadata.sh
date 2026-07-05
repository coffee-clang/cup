#!/bin/sh
set -eu

TEST_SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$TEST_SCRIPT_DIR/../support/common.sh"

test_begin release-metadata
functions=$TMP_ROOT/install-functions.sh
sed '$d' "$PROJECT_ROOT/scripts/install/install-cup.sh" > "$functions"

validate() {
    file=$1
    sh -c '. "$1"; validate_release_metadata "$2"' sh "$functions" "$file"
}

expect_invalid() {
    file=$1
    if validate "$file" >/dev/null 2>&1; then
        fail "invalid release metadata unexpectedly succeeded: $file"
    fi
}

cat > "$TMP_ROOT/valid" <<'EOF'
format=1
version=0.2.0
commit=abcdef0
EOF
validate "$TMP_ROOT/valid"

cat > "$TMP_ROOT/leading-zero" <<'EOF'
format=1
version=00.2.0
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/leading-zero"

cat > "$TMP_ROOT/non-ascii-digit" <<'EOF'
format=1
version=٠.2.0
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/non-ascii-digit"

cat > "$TMP_ROOT/too-large" <<'EOF'
format=1
version=1000000.2.0
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/too-large"

printf '%s\n' '0.2.0' > "$TMP_ROOT/plain"
expect_invalid "$TMP_ROOT/plain"

cat > "$TMP_ROOT/duplicate" <<'EOF'
format=1
version=0.2.0
version=0.2.1
commit=abcdef0
EOF
expect_invalid "$TMP_ROOT/duplicate"

cat > "$TMP_ROOT/extra" <<'EOF'
format=1
version=0.2.0
commit=abcdef0
unknown=value
EOF
expect_invalid "$TMP_ROOT/extra"

cat > "$TMP_ROOT/bad-commit" <<'EOF'
format=1
version=0.2.0
commit=not-a-commit
EOF
expect_invalid "$TMP_ROOT/bad-commit"

printf '%s\n' 'Release metadata tests passed.'
