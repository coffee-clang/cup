#!/usr/bin/env bash

# Validates one pinned dependency prefix, prints its stable cache key,
# or safely removes one owned dependency root.
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
require_sha256_tool

if [ "${1:-}" = --clean-root ]; then
    [ "$#" -eq 2 ] || {
        echo "Usage: $0 --clean-root <dependency-root>" >&2
        exit 2
    }
    dependency_clean_root "$2"
    exit 0
fi

PLATFORM=${1:?platform is required}
MODE=${2:?dependency prefix, --print-cache-key or --print-profile is required}
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
    dependency_prefix_diagnostic "$DEPS_PREFIX" "$metadata" "$use_openssl"
    echo "Pinned dependency prefix is missing, incomplete or incompatible: $DEPS_PREFIX" >&2
    echo "Expected platform/profile: $PLATFORM/$profile" >&2
    echo "Expected prefix format: $CUP_DEPENDENCY_PREFIX_FORMAT" >&2
    echo "Expected build revision: $DEPENDENCY_BUILD_REVISION" >&2
    echo "Expected source lock SHA-256: $(dependency_lock_sha256)" >&2
    echo "Expected toolchain SHA-256: $(dependency_toolchain_sha256 "$PLATFORM" "$profile")" >&2
    exit 1
fi
printf 'Dependency prefix is compatible: %s (%s/%s)\n' \
    "$DEPS_PREFIX" "$PLATFORM" "$profile"
