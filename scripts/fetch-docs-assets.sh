#!/usr/bin/env bash

# Purpose: Fetches optional remote mdBook theme assets for an offline documentation build.
# It does not alter application or release artifacts.
set -euo pipefail

# Fetches remote theme assets from coffee-clang.github.io for offline docs builds.
# Usage: ./scripts/fetch-docs-assets.sh

THEME_DIR="docs/theme"
BASE_URL="https://raw.githubusercontent.com/coffee-clang/coffee-clang.github.io/main"

mkdir -p "$THEME_DIR"

curl -sS -o "$THEME_DIR/index.hbs" "$BASE_URL/theme/index.hbs"

echo "Docs assets fetched to $THEME_DIR/"
