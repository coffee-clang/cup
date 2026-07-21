#!/usr/bin/env sh

# Purpose: Generates deterministic C sources from the versioned PEM CA bundle.
# Output: ca_bundle.h and ca_bundle.c in the caller-selected generated directory.
set -eu

if [ "$#" -ne 2 ]; then
    printf 'Usage: %s <cacert.pem> <output-directory>\n' "$0" >&2
    exit 1
fi

PEM_FILE=$1
OUTPUT_DIR=$2

[ -s "$PEM_FILE" ] || {
    printf "Error: CA bundle '%s' is missing or empty.\n" "$PEM_FILE" >&2
    exit 1
}
grep -q '^-----BEGIN CERTIFICATE-----$' "$PEM_FILE" || {
    printf "Error: CA bundle '%s' contains no PEM certificates.\n" "$PEM_FILE" >&2
    exit 1
}

mkdir -p "$OUTPUT_DIR"
WORK_DIR=$(mktemp -d "$OUTPUT_DIR/.ca-bundle.XXXXXX")
HEADER_TMP="$WORK_DIR/ca_bundle.h"
SOURCE_TMP="$WORK_DIR/ca_bundle.c"

cleanup() {
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT HUP INT TERM

cat > "$HEADER_TMP" <<'HEADER'
#ifndef CUP_CA_BUNDLE_H
#define CUP_CA_BUNDLE_H

#include <stddef.h>

extern const unsigned char cup_ca_bundle[];
extern const size_t cup_ca_bundle_len;

#endif /* CUP_CA_BUNDLE_H */
HEADER

{
    printf '%s\n\n' '#include "ca_bundle.h"'
    printf '%s\n' 'const unsigned char cup_ca_bundle[] = {'
    od -An -v -t x1 "$PEM_FILE" | awk '
        BEGIN { count = 0; printf "    " }
        {
            for (i = 1; i <= NF; ++i) {
                printf "0x%s,", $i
                count++
                if (count % 12 == 0) {
                    printf "\n    "
                } else {
                    printf " "
                }
            }
        }
        END {
            if (count == 0) {
                exit 1
            }
            printf "0x00\n};\n\n"
            printf "const size_t cup_ca_bundle_len = %d;\n", count
        }
    '
} > "$SOURCE_TMP"

mv "$HEADER_TMP" "$OUTPUT_DIR/ca_bundle.h"
mv "$SOURCE_TMP" "$OUTPUT_DIR/ca_bundle.c"
