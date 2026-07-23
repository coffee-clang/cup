#!/bin/sh

# Purpose: Enforces assertion quality and complete test registration.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Assertion quality test failed: %s\n' "$*" >&2
    exit 1
}

audit_root=$(mktemp -d "${TMPDIR:-/tmp}/cup-assertions.XXXXXX") ||
    fail 'could not create assertion audit workspace'
trap 'rm -rf "$audit_root"' EXIT HUP INT TERM

placeholders=$(grep -RInE 'TEST_ASSERT_TRUE\(0\)|TEST_ASSERT_FALSE\(1\)' \
    tests/unit 2>/dev/null || :)
[ -z "$placeholders" ] || fail "opaque placeholder assertions remain:\n$placeholders"

direct_unity_fail=$(grep -RInE '\bUnityFail[[:space:]]*\(' \
    tests/unit --include='*.c' 2>/dev/null || :)
[ -z "$direct_unity_fail" ] ||
    fail "direct UnityFail calls remain; use public Unity assertion macros:\n$direct_unity_fail"

bare_posix=$(grep -RInF 'run_cup doctor >/dev/null' \
    tests/integration/posix 2>/dev/null || :)
[ -z "$bare_posix" ] || fail "POSIX health checks discard doctor output:\n$bare_posix"

bare_windows=$(grep -RInF 'Invoke-Cup -CommandArgs @("doctor") | Out-Null' \
    tests/integration/windows 2>/dev/null || :)
[ -z "$bare_windows" ] || fail "Windows health checks discard doctor output:\n$bare_windows"

audit_unit_file() {
    file=$1
    declared=$audit_root/declared
    registered=$audit_root/registered
    sed -nE \
        's/.*static[[:space:]]+void[[:space:]]+(test_[A-Za-z0-9_]+)[[:space:]]*\(.*/\1/p' \
        "$file" | sort > "$declared"
    sed -nE \
        's/.*RUN_TEST[[:space:]]*\([[:space:]]*(test_[A-Za-z0-9_]+)[[:space:]]*\).*/\1/p' \
        "$file" | sort > "$registered"
    names=$(cat "$declared" "$registered" | sort -u)
    for name in $names; do
        declaration_count=$(grep -Fxc "$name" "$declared" || :)
        registration_count=$(grep -Fxc "$name" "$registered" || :)
        [ "$declaration_count" -eq 1 ] ||
            fail "$file: $name declared $declaration_count times"
        [ "$registration_count" -eq 1 ] ||
            fail "$file: $name registered $registration_count times"
    done

    awk -v file="$file" '
        function brace_delta(line,    copy, opens, closes) {
            copy = line; opens = gsub(/\{/, "{", copy)
            copy = line; closes = gsub(/\}/, "}", copy)
            return opens - closes
        }
        $0 ~ /static[[:space:]]+void[[:space:]]+test_[A-Za-z0-9_]+/ {
            name = $0
            sub(/^.*static[[:space:]]+void[[:space:]]+/, "", name)
            sub(/[[:space:]]*\(.*/, "", name)
            active = 1; depth = brace_delta($0); asserted = 0
            next
        }
        active {
            if ($0 ~ /(TEST_ASSERT_[A-Z0-9_]+|assert_[A-Za-z0-9_]+)[[:space:]]*\(/) {
                asserted = 1
            }
            depth += brace_delta($0)
            if (depth <= 0) {
                if (!asserted) {
                    printf "%s: %s has no observable assertion\n", file, name > "/dev/stderr"
                    errors++
                }
                active = 0
            }
        }
        END { exit errors != 0 }
    ' "$file" || exit 1
    rm -f "$declared" "$registered"
}

for unit_file in tests/unit/test_*.c; do
    audit_unit_file "$unit_file"
done

for integration_file in tests/integration/posix/*.sh; do
    names=$(sed -nE \
        's/^(test_[A-Za-z0-9_]+)\(\)[[:space:]]*\{.*/\1/p' \
        "$integration_file")
    for name in $names; do
        calls=$(grep -Ec "^[[:space:]]*$name[[:space:]]*$" "$integration_file" || :)
        [ "$calls" -eq 1 ] || fail "$integration_file: $name invoked $calls times"
    done
    fixtures=$(sed -nE \
        's/.*run_cup_expect_failure[[:space:]]+"(\$TMP_ROOT\/[A-Za-z0-9_.-]+)".*/\1/p' \
        "$integration_file")
    for fixture in $fixtures; do
        uses=$(grep -Fc "$fixture" "$integration_file" || :)
        [ "$uses" -ge 2 ] || fail "$integration_file: failure output $fixture is never asserted"
    done
done
printf 'Test assertions and registration are coherent.\n'
