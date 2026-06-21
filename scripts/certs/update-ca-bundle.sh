#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
CERT_DIR="$ROOT_DIR/certs"
CERT_FILE="$CERT_DIR/cacert.pem"
HEADER_FILE="$ROOT_DIR/include/ca_bundle.h"
SOURCE_FILE="$ROOT_DIR/src/ca_bundle.c"
CACERT_URL="${CUP_CACERT_URL:-https://curl.se/ca/cacert.pem}"
CC="${CC:-cc}"

WORK_DIR="$(mktemp -d "$ROOT_DIR/.ca-bundle.XXXXXX")"
PEM_TMP="$WORK_DIR/cacert.pem"
HEADER_TMP="$WORK_DIR/ca_bundle.h"
SOURCE_TMP="$WORK_DIR/ca_bundle.c"
GENERATOR_C="$WORK_DIR/generate.c"
GENERATOR_BIN="$WORK_DIR/generate"
OBJECT_TMP="$WORK_DIR/ca_bundle.o"

cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT HUP INT TERM

mkdir -p "$CERT_DIR" "$ROOT_DIR/include" "$ROOT_DIR/src"
curl -fsSL "$CACERT_URL" -o "$PEM_TMP"
[ -s "$PEM_TMP" ] || { echo "Error: downloaded CA bundle is empty." >&2; exit 1; }
grep -q '^-----BEGIN CERTIFICATE-----$' "$PEM_TMP" || {
    echo "Error: downloaded CA bundle contains no PEM certificates." >&2
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

cat > "$GENERATOR_C" <<'GENERATOR'
#include <stdio.h>

int main(int argc, char **argv) {
    FILE *input;
    FILE *output;
    int byte;
    size_t count = 0;

    if (argc != 3) {
        return 1;
    }
    input = fopen(argv[1], "rb");
    output = fopen(argv[2], "wb");
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return 1;
    }

    fputs("#include \"ca_bundle.h\"\n\n", output);
    fputs("const unsigned char cup_ca_bundle[] = {\n    ", output);
    while ((byte = fgetc(input)) != EOF) {
        fprintf(output, "0x%02x,", (unsigned int)(unsigned char)byte);
        count++;
        if (count % 12 == 0) {
            fputs("\n    ", output);
        } else {
            fputc(' ', output);
        }
    }
    fputs("0x00\n};\n\n", output);
    fprintf(output, "const size_t cup_ca_bundle_len = %zu;\n", count);

    {
        int read_failed = ferror(input);
        int input_close_failed = fclose(input) != 0;
        int output_close_failed = fclose(output) != 0;

        if (read_failed || input_close_failed || output_close_failed) {
            return 1;
        }
    }
    return count == 0 ? 1 : 0;
}
GENERATOR

"$CC" -std=c11 -Wall -Wextra -Werror "$GENERATOR_C" -o "$GENERATOR_BIN"
"$GENERATOR_BIN" "$PEM_TMP" "$SOURCE_TMP"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$WORK_DIR" -c "$SOURCE_TMP" -o "$OBJECT_TMP"

mv "$PEM_TMP" "$CERT_FILE"
mv "$HEADER_TMP" "$HEADER_FILE"
mv "$SOURCE_TMP" "$SOURCE_FILE"
printf 'Updated embedded CA bundle from %s\n' "$CACERT_URL"
