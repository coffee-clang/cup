#!/bin/sh

# Purpose: Enforces assertion quality, test registration and integration ownership.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Assertion quality test failed: %s\n' "$*" >&2
    exit 1
}

placeholders=$(grep -RInE 'TEST_ASSERT_TRUE\(0\)|TEST_ASSERT_FALSE\(1\)' \
    tests/unit 2>/dev/null || :)
[ -z "$placeholders" ] || fail "opaque placeholder assertions remain:\n$placeholders"

direct_unity_fail=$(grep -RInE '\bUnityFail[[:space:]]*\(' \
    tests/unit --include='*.c' 2>/dev/null || :)
[ -z "$direct_unity_fail" ] ||
    fail "direct UnityFail calls remain; use public Unity assertion macros:\n$direct_unity_fail"

legacy_state_entry=$(grep -RInE '\bStateEntry\b' \
    include src tests/unit --include='*.c' --include='*.h' 2>/dev/null || :)
[ -z "$legacy_state_entry" ] || fail "legacy string-based StateEntry records remain:\n$legacy_state_entry"

legacy_domain_files=$(find include src tests/unit -maxdepth 1 -type f \
    \( -name 'entry.[ch]' -o -name 'info.[ch]' -o -name 'manifest.[ch]' \
       -o -name 'bootstrap.[ch]' -o -name 'test_core.c' \
       -o -name 'test_info.c' -o -name 'test_manifest.c' \
       -o -name 'test_bootstrap.c' \) -print 2>/dev/null || :)
[ -z "$legacy_domain_files" ] || fail "legacy domain module filenames remain:\n$legacy_domain_files"

retired_types='Manifest|ManifestPackage|ManifestSource|PackageInfo|PackageInfoField'
retired_types="$retired_types|BootstrapInspection|BootstrapAssetStatus|BootstrapSource"
retired_types="$retired_types|EntryRequest|EntryPointSpec|EntryPointPlan"
retired_constants='MAX_NAME_LEN|MAX_ENTRY_LEN|MAX_ENTRYPOINT_NAME_LEN'
retired_constants="$retired_constants|CUP_MANIFEST_FILENAME|CUP_ERR_MANIFEST"
retired_constants="$retired_constants|CUP_ERR_DEFAULT_FULL"
retired_helpers='made_default|removed_default|prepare_default_change|save_default_change'
retired_helpers="$retired_helpers|remove_stale_defaults|wrapper_plan_build_default"
retired_prefixes='manifest_(init|free|load|resolve|build|get|has)'
retired_prefixes="$retired_prefixes|info_(init|load|get|validate)"
retired_prefixes="$retired_prefixes|bootstrap_(inspect|has|installed|development|legacy"
retired_prefixes="$retired_prefixes|find|uninstall|binary|platform|verify)"
retired_prefixes="$retired_prefixes|entry_request_(parse|resolve|print)"
retired_prefixes="$retired_prefixes|entrypoint_plan_[A-Za-z0-9_]*"
legacy_domain_pattern="\\b($retired_types|$retired_constants|$retired_helpers)\\b"
legacy_domain_pattern="$legacy_domain_pattern|\\b($retired_prefixes)\\b"

legacy_domain_symbols=$(grep -RInE "$legacy_domain_pattern" \
    include src tests/unit --include='*.c' --include='*.h' 2>/dev/null || :)
[ -z "$legacy_domain_symbols" ] || fail "retired internal domain symbols remain:\n$legacy_domain_symbols"

entry_boundary_leaks=$(grep -RInF '"entry."' src include \
    --include='*.c' --include='*.h' 2>/dev/null \
    | grep -v '^src/package_metadata.c:' || :)
[ -z "$entry_boundary_leaks" ] ||
    fail "external entry.* keys escaped the package metadata boundary:\n$entry_boundary_leaks"

bare_posix=$(grep -RInF 'run_cup doctor >/dev/null' \
    tests/integration/posix 2>/dev/null || :)
[ -z "$bare_posix" ] || fail "POSIX health checks discard doctor output:\n$bare_posix"

bare_windows=$(grep -RInF 'Invoke-Cup -CommandArgs @("doctor") | Out-Null' \
    tests/integration/windows 2>/dev/null || :)
[ -z "$bare_windows" ] || fail "Windows health checks discard doctor output:\n$bare_windows"

# State suites own persisted records and corruption handling. Public list/info/default
# formatting belongs to the package lifecycle suites.
if grep -nE '\brun_cup (list|info|default)\b' \
        tests/integration/posix/state.sh >/dev/null 2>&1; then
    fail 'POSIX state suite duplicates public catalog/default workflow assertions'
fi
if grep -nE 'Invoke-Cup -CommandArgs @\("(list|info|default)"' \
        tests/integration/windows/state.ps1 >/dev/null 2>&1; then
    fail 'Windows state suite duplicates public catalog/default workflow assertions'
fi

audit_unit_file() {
    file=$1
    declared=${TMPDIR:-/tmp}/cup-assert-declared.$$
    registered=${TMPDIR:-/tmp}/cup-assert-registered.$$
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
printf 'Test assertions and ownership are coherent.\n'
