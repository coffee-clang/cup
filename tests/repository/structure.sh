#!/bin/sh

# Verifies checkout properties required for portable build execution.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

fail() {
    printf 'Repository structure test failed: %s\n' "$*" >&2
    exit 1
}

repository_shell_scripts() {
    find scripts tests -type f -name '*.sh' -print
}

# Dependency metadata uses one portable checkout representation.
carriage_return=$(printf '\r')
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    attribute=$(git check-attr eol -- config/dependencies.lock)
    [ "$attribute" = 'config/dependencies.lock: eol: lf' ] ||
        fail 'config/dependencies.lock must be checked out with LF line endings'
fi
if LC_ALL=C grep -q "$carriage_return" config/dependencies.lock; then
    fail 'config/dependencies.lock contains carriage-return characters'
fi

# Script entry points and sourced libraries use their required Git modes.
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

repository_shell_scripts | while IFS= read -r script; do
    first=$(sed -n '1p' "$script")
    case "$first" in
        '#!'*) [ -x "$script" ] || fail "entry point is not executable: $script" ;;
        *) [ ! -x "$script" ] || fail "sourced library is executable: $script" ;;
    esac
done

printf '%s\n' 'Repository structure tests passed.'
