#!/usr/bin/env sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
CERT_DIR="$ROOT_DIR/certs"
CERT_FILE="$CERT_DIR/cacert.pem"
HEADER_FILE="$ROOT_DIR/include/ca_bundle.h"
SOURCE_FILE="$ROOT_DIR/src/ca_bundle.c"
CACERT_URL="${CUP_CACERT_URL:-https://curl.se/ca/cacert.pem}"
CC="${CC:-cc}"

mkdir -p "$CERT_DIR" "$ROOT_DIR/include" "$ROOT_DIR/src"

curl -fsSL "$CACERT_URL" -o "$CERT_FILE.tmp"
mv "$CERT_FILE.tmp" "$CERT_FILE"

cat > "$HEADER_FILE" <<'HEADER'
#ifndef CUP_CA_BUNDLE_H
#define CUP_CA_BUNDLE_H

#include <stddef.h>

extern const unsigned char cup_ca_bundle[];
extern const size_t cup_ca_bundle_len;

#endif /* CUP_CA_BUNDLE_H */
HEADER

GENERATOR_DIR="$(mktemp -d "${TMPDIR:-/tmp}/cup-ca-bundle-generator.XXXXXX")"
GENERATOR_C="$GENERATOR_DIR/generate-ca-bundle.c"
GENERATOR_BIN="$GENERATOR_DIR/generate-ca-bundle"

cleanup() {
    rm -rf "$GENERATOR_DIR"
}

trap cleanup EXIT HUP INT TERM

cat > "$GENERATOR_C" <<'GENERATOR'
#include <stdio.h>
#include <stdlib.h>

static void write_escaped_byte(FILE *out, int ch, int *column) {
    const char *escaped = NULL;
    char octal[5];

    switch (ch) {
        case '\\':
            escaped = "\\\\";
            break;
        case '"':
            escaped = "\\\"";
            break;
        case '\n':
            fputs("\\n\"\n    \"", out);
            *column = 0;
            return;
        case '\r':
            escaped = "\\r";
            break;
        case '\t':
            escaped = "\\t";
            break;
        default:
            if (ch >= 32 && ch <= 126) {
                fputc(ch, out);
                *column += 1;
                return;
            }

            snprintf(octal, sizeof(octal), "\\%03o", (unsigned int)(unsigned char)ch);
            escaped = octal;
            break;
    }

    fputs(escaped, out);
    *column += 1;
}

int main(int argc, char **argv) {
    FILE *in;
    FILE *out;
    int ch;
    int column = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <cacert.pem> <ca_bundle.c>\n", argv[0]);
        return 1;
    }

    in = fopen(argv[1], "rb");
    if (in == NULL) {
        perror(argv[1]);
        return 1;
    }

    out = fopen(argv[2], "wb");
    if (out == NULL) {
        perror(argv[2]);
        fclose(in);
        return 1;
    }

    fputs("#include \"ca_bundle.h\"\n\n", out);
    fputs("const unsigned char cup_ca_bundle[] =\n", out);
    fputs("    \"", out);

    while ((ch = fgetc(in)) != EOF) {
        if (column >= 96 && ch != '\n') {
            fputs("\"\n    \"", out);
            column = 0;
        }

        write_escaped_byte(out, ch, &column);
    }

    fputs("\";\n\n", out);
    fputs("const size_t cup_ca_bundle_len = sizeof(cup_ca_bundle) - 1;\n", out);

    if (ferror(in)) {
        perror(argv[1]);
        fclose(in);
        fclose(out);
        return 1;
    }

    if (fclose(in) != 0) {
        perror(argv[1]);
        fclose(out);
        return 1;
    }

    if (fclose(out) != 0) {
        perror(argv[2]);
        return 1;
    }

    return 0;
}
GENERATOR

"$CC" -std=c11 -Wall -Wextra -Werror "$GENERATOR_C" -o "$GENERATOR_BIN"
"$GENERATOR_BIN" "$CERT_FILE" "$SOURCE_FILE"

printf 'Updated embedded CA bundle from %s\n' "$CACERT_URL"
