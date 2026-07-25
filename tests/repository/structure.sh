#!/bin/sh

# Purpose: Verifies tracked-file hygiene and executable bits.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Repository structure test failed: %s\n' "$*" >&2
    exit 1
}

tracked_shell_scripts() {
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files -- '*.sh' | while IFS= read -r script; do
            [ -f "$script" ] && printf '%s\n' "$script"
        done
    else
        find scripts tests -type f -name '*.sh' -print
    fi
}

# Dependency metadata is parsed identically on every runner. Pin its checkout
# representation to LF and reject carriage returns in the tracked files.
carriage_return=$(printf '\r')
for file in config/dependencies.lock config/dependencies.recipe; do
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        attribute=$(git check-attr eol -- "$file")
        [ "$attribute" = "$file: eol: lf" ] ||
            fail "$file must be checked out with LF line endings"
    fi
    if LC_ALL=C grep -q "$carriage_return" "$file"; then
        fail "$file contains carriage-return characters"
    fi
done

unversioned_actions=$(find .github/workflows -type f -name '*.yml' \
    -exec grep -HnE 'uses:[[:space:]]+[^.]' {} + \
    | grep -Ev '@v[0-9]+([[:space:]]|$)' || :)
[ -z "$unversioned_actions" ] ||
    fail "external actions must use numeric major tags:\n$unversioned_actions"

# The first line defines the script role: standalone shell programs have a shebang
# and mode 100755; sourced libraries have no shebang and mode 100644.
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git ls-files -s -- '*.sh' | while read -r mode object stage path; do
        [ -f "$path" ] || continue
        first=$(sed -n '1p' "$path")
        case "$first" in
            '#!'*) expected=100755 ;;
            *) expected=100644 ;;
        esac
        [ "$mode" = "$expected" ] ||
            fail "$path has Git mode $mode; expected $expected"
    done
fi

tracked_shell_scripts | while IFS= read -r script; do
    first=$(sed -n '1p' "$script")
    case "$first" in
        '#!'*) [ -x "$script" ] || fail "entry point is not executable: $script" ;;
        *) [ ! -x "$script" ] || fail "sourced library is executable: $script" ;;
    esac
done

tracked_outputs=$(git ls-files 2>/dev/null | grep -E '^(build|dist|book)/' || :)
[ -z "$tracked_outputs" ] || fail "generated output is tracked:\n$tracked_outputs"

printf '%s\n' 'Repository structure tests passed.'
