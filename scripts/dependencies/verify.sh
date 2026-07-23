#!/usr/bin/env bash

# Purpose: Validates one pinned dependency prefix or prints its stable cache key.
set -euo pipefail

PLATFORM=${1:?platform is required}
MODE=${2:?dependency prefix, --print-cache-key or --print-profile is required}
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
require_sha256_tool

profile=$(dependency_profile "$PLATFORM")
use_openssl=$(dependency_uses_openssl "$PLATFORM")

case "$MODE" in
    --print-profile)
        printf '%s\n' "$profile"
        exit 0
        ;;
    --print-cache-key)
        dependency_cache_key "$PLATFORM" "$profile"
        exit 0
        ;;
esac

DEPS_PREFIX=$MODE
metadata=$(dependency_metadata "$PLATFORM" "$profile")
if ! dependency_prefix_matches "$DEPS_PREFIX" "$metadata" "$use_openssl"; then
    echo "Pinned dependency prefix is missing, incomplete or incompatible: $DEPS_PREFIX" >&2
    echo "Expected platform/profile: $PLATFORM/$profile" >&2
    echo "Expected recipe: $(dependency_recipe_version)" >&2
    echo "Expected lock SHA-256: $(dependency_lock_sha256)" >&2
    exit 1
fi
printf 'Dependency prefix is compatible: %s (%s/%s)\n' \
    "$DEPS_PREFIX" "$PLATFORM" "$profile"
