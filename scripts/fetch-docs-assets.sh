#!/usr/bin/env bash

# Purpose: Fetches optional remote mdBook theme assets for an offline
# documentation build. It does not alter application or release artifacts.
set -euo pipefail

THEME_DIR="docs/theme"
BASE_URL="https://raw.githubusercontent.com/coffee-clang/coffee-clang.github.io/main"
DESTINATION="$THEME_DIR/index.hbs"
TEMPORARY="$DESTINATION.tmp.$$"

cleanup() {
    rm -f "$TEMPORARY"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$THEME_DIR"
curl -fsSL --proto '=https' --proto-redir '=https' \
    "$BASE_URL/theme/index.hbs" -o "$TEMPORARY"
[ -s "$TEMPORARY" ] || {
    printf 'Downloaded mdBook theme asset is empty.\n' >&2
    exit 1
}
mv -f "$TEMPORARY" "$DESTINATION"
trap - EXIT HUP INT TERM

printf 'Docs assets fetched to %s/\n' "$THEME_DIR"
