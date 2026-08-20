#!/bin/sh

# Generates deterministic C sources from a validated PEM CA bundle.
# It writes ca_bundle.h and ca_bundle.c in the caller-selected generated directory.
set -eu

LC_ALL=C
LANG=C
export LC_ALL LANG
umask 022

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd -P)
PROJECT_ROOT=$ROOT_DIR
# shellcheck source=../lib/path-safety.sh
. "$ROOT_DIR/scripts/lib/path-safety.sh"

if [ "$#" -ne 2 ]; then
    printf 'Usage: %s <cacert.pem> <output-directory>\n' "$0" >&2
    exit 1
fi

PEM_FILE=$1
OUTPUT_DIR=$2
case "$PEM_FILE" in /*|[A-Za-z]:/*) ;; *) PEM_FILE=$(pwd -P)/$PEM_FILE ;; esac
META_FILE=${CUP_CA_META_FILE:-$(dirname "$PEM_FILE")/cacert.meta}
case "$META_FILE" in /*|[A-Za-z]:/*) ;; *) META_FILE=$(pwd -P)/$META_FILE ;; esac
case "$OUTPUT_DIR" in /*|[A-Za-z]:/*) ;; *) OUTPUT_DIR=$(pwd -P)/$OUTPUT_DIR ;; esac

cup_path_require_regular_file "$PEM_FILE" 'CA bundle' || exit 1
cup_path_require_regular_file "$META_FILE" 'CA metadata' || exit 1

if [ -n "${CUP_BUILD_ROOT:-}" ]; then
    cup_path_require_build_root "$CUP_BUILD_ROOT" || exit 1
    cup_path_prepare_child_directory "$CUP_BUILD_ROOT" "$OUTPUT_DIR" 'generated CA directory' || exit 1
else
    cup_path_prepare_directory_chain "$OUTPUT_DIR" 'generated CA directory' || exit 1
fi
for destination in "$OUTPUT_DIR/ca_bundle.h" "$OUTPUT_DIR/ca_bundle.c"; do
    if [ -n "${CUP_BUILD_ROOT:-}" ]; then
        cup_path_prepare_child_file "$CUP_BUILD_ROOT" "$destination" 'generated CA output' || exit 1
    else
        cup_path_prepare_file_target "$destination" 'generated CA output' || exit 1
    fi
done

TEMP_BASE=${TMPDIR:-/tmp}
case "$TEMP_BASE" in /*) ;; *) TEMP_BASE=$(pwd -P)/$TEMP_BASE ;; esac
cup_path_check_directory_chain "$TEMP_BASE" 0 'CA generator temporary parent' || exit 1
WORK_DIR=$(cup_path_create_unique_directory \
    "$TEMP_BASE/cup-ca-generate.XXXXXX" 'CA generator work directory' 0700) || exit 1
HEADER_TMP=$WORK_DIR/ca_bundle.h
SOURCE_TMP=$WORK_DIR/ca_bundle.c
PKCS7_TMP=$WORK_DIR/cacert.p7b
SNAPSHOT_PEM=$WORK_DIR/cacert.pem
SNAPSHOT_META=$WORK_DIR/cacert.meta

cleanup() {
    if [ -n "${WORK_DIR:-}" ] && [ -e "$WORK_DIR" ]; then
        cup_path_check_directory_chain "$WORK_DIR" 0 'CA generator work directory' >/dev/null 2>&1 || return 0
        cup_path_remove_directory_tree "$WORK_DIR" 'CA generator work directory' >/dev/null 2>&1 || return 0
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

cup_path_copy_file "$PEM_FILE" "$SNAPSHOT_PEM" 0644 replace || exit 1
cup_path_copy_file "$META_FILE" "$SNAPSHOT_META" 0644 replace || exit 1
CUP_CA_CERT_FILE="$SNAPSHOT_PEM" CUP_CA_META_FILE="$SNAPSHOT_META" \
    "$ROOT_DIR/scripts/certs/check-ca-bundle.sh" --integrity >/dev/null
PEM_FILE=$SNAPSHOT_PEM

openssl_command=${CUP_OPENSSL:-openssl}
command -v "$openssl_command" >/dev/null 2>&1 || {
    printf 'Error: OpenSSL is required to generate the CA bundle: %s\n' "$openssl_command" >&2
    exit 1
}

"$openssl_command" crl2pkcs7 -nocrl -certfile "$PEM_FILE" -out "$PKCS7_TMP" >/dev/null 2>&1 || {
    printf "Error: CA bundle '%s' contains invalid X.509 certificate data.\n" "$PEM_FILE" >&2
    exit 1
}
"$openssl_command" pkcs7 -print_certs -noout -in "$PKCS7_TMP" >/dev/null 2>&1 || {
    printf "Error: CA bundle '%s' contains invalid X.509 certificate data.\n" "$PEM_FILE" >&2
    exit 1
}

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
            if (count == 0) exit 1
            printf "0x00\n};\n\n"
            printf "const size_t cup_ca_bundle_len = %d;\n", count
        }
    '
} > "$SOURCE_TMP"

for generated in ca_bundle.h ca_bundle.c; do
    temporary=$WORK_DIR/$generated
    destination=$OUTPUT_DIR/$generated
    cup_path_copy_file "$temporary" "$destination" 0644 if-different ||
        exit 1
done
