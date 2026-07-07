#!/usr/bin/env sh
set -eu

. "$(dirname "$0")/common.sh"

: "${SHOULD_RELEASE:?SHOULD_RELEASE is required}"
validate_release_inputs

case "$SHOULD_RELEASE" in
    0|1) ;;
    *) fail "invalid SHOULD_RELEASE value: $SHOULD_RELEASE" ;;
esac

mkdir -p dist/decision
cat > dist/decision/release-decision.env <<EOF_DECISION
SHOULD_RELEASE=$SHOULD_RELEASE
VERSION=$VERSION
TAG=$TAG
SHA=$SHA
EOF_DECISION
