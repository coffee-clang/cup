#!/bin/sh

# Exercises development-catalog fallback and the concrete artifact schema through the real CLI.
set -eu

TESTS_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$TESTS_ROOT/support/posix/cli.sh"

test_begin catalog
prepare_command_environment
catalog=$DEV_ROOT/config/catalog.cfg

# Source-only discovery uses the local development catalog without creating a runtime root.
run_cup search compiler >"$TMP_ROOT/seed-search.out"
assert_missing "$TEST_HOME/.cup"

cat > "$catalog" <<CATALOG
format=1
revision=1
update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg
package.0.component=compiler
package.0.tool=clang
package.0.host=$TEST_PLATFORM
package.0.target=$TEST_PLATFORM
package.0.version=98.0.1
package.0.stable=true
package.0.artifact.0.format=tar.gz
package.0.artifact.0.url=https://example.invalid/clang-98.0.1-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz
package.0.artifact.0.sha256=0000000000000000000000000000000000000000000000000000000000000000
CATALOG
run_cup search compiler >"$TMP_ROOT/concrete-search.out"
assert_contains "$(cat "$TMP_ROOT/concrete-search.out")" '98.0.1'
assert_missing "$TEST_HOME/.cup"

# A current-schema artifact without its digest is malformed, not merely unavailable.
awk '!/\.artifact\.0\.sha256=/' "$catalog" > "$TMP_ROOT/missing-sha.cfg"
cp "$TMP_ROOT/missing-sha.cfg" "$catalog"
run_cup_expect_failure "$TMP_ROOT/missing-sha.out" search compiler

# Production catalog artifact URLs require HTTPS; loopback HTTP is reserved for explicit tests.
cat > "$catalog" <<CATALOG
format=1
revision=1
update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg
package.0.component=compiler
package.0.tool=clang
package.0.host=$TEST_PLATFORM
package.0.target=$TEST_PLATFORM
package.0.version=98.0.1
package.0.stable=true
package.0.artifact.0.format=tar.gz
package.0.artifact.0.url=http://example.invalid/clang-98.0.1-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz
package.0.artifact.0.sha256=0000000000000000000000000000000000000000000000000000000000000000
CATALOG
run_cup_expect_failure "$TMP_ROOT/insecure-artifact.out" search compiler

# A structurally safe future version is tolerated but is not made operational by guessing.
cat > "$catalog" <<CATALOG
format=1
revision=2
update_url=https://github.com/coffee-clang/cup-components/releases/download/catalog/catalog.cfg
package.0.component=compiler
package.0.tool=clang
package.0.host=$TEST_PLATFORM
package.0.target=$TEST_PLATFORM
package.0.version=future-1
package.0.stable=true
package.0.artifact.0.format=tar.gz
package.0.artifact.0.url=https://example.invalid/clang-future-1-$TEST_PLATFORM-$TEST_PLATFORM.tar.gz
package.0.artifact.0.sha256=0000000000000000000000000000000000000000000000000000000000000000
CATALOG
run_cup search compiler >"$TMP_ROOT/future-search.out"
assert_not_contains "$(cat "$TMP_ROOT/future-search.out")" 'future-1'
assert_missing "$TEST_HOME/.cup"

printf '%s\n' 'Package catalog runtime schema tests passed.'
