#!/bin/sh

# Declares the exact artifact set that may authorize one release.
set -eu

CI_TESTS_EVIDENCE_COUNT=11

ci_tests_evidence_names() {
    attempt=$1

    printf '%s\n' \
        "cup-dependency-evidence-linux-arm64-gcc-attempt-$attempt" \
        "cup-dependency-evidence-linux-x64-gcc-attempt-$attempt" \
        "cup-dependency-evidence-macos-arm64-apple-clang-attempt-$attempt" \
        "cup-dependency-evidence-macos-x64-apple-clang-attempt-$attempt" \
        "cup-dependency-evidence-windows-x64-clang64-attempt-$attempt" \
        "cup-dependency-evidence-windows-x64-ucrt64-attempt-$attempt" \
        "cup-source-evidence-linux-arm64-attempt-$attempt" \
        "cup-source-evidence-linux-x64-attempt-$attempt" \
        "cup-source-evidence-macos-arm64-attempt-$attempt" \
        "cup-source-evidence-macos-x64-attempt-$attempt" \
        "cup-source-evidence-windows-x64-attempt-$attempt"
}
