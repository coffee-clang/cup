#!/usr/bin/env sh

# Purpose: Validates a newly downloaded CA bundle before replacing the versioned PEM.
# The candidate is generated and compiled in a temporary directory before commit.
set -eu
LC_ALL=C
TZ=UTC
export LC_ALL TZ

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
CERT_FILE="$ROOT_DIR/certs/cacert.pem"
META_FILE="$ROOT_DIR/certs/cacert.meta"
CACERT_URL="${CUP_CACERT_URL:-https://curl.se/ca/cacert.pem}"
CURL="${CURL:-curl}"
HOSTCC="${HOSTCC:-cc}"

WORK_DIR="$(mktemp -d "$ROOT_DIR/.ca-bundle-update.XXXXXX")"
PEM_TMP="$WORK_DIR/cacert.pem"
GENERATED_DIR="$WORK_DIR/generated"
OBJECT_TMP="$WORK_DIR/ca_bundle.o"
OLD_CERT="$WORK_DIR/old-cacert.pem"
OLD_META="$WORK_DIR/old-cacert.meta"
NEW_CERT="$ROOT_DIR/certs/.cacert.pem.new.$$"
NEW_META="$ROOT_DIR/certs/.cacert.meta.new.$$"
replacement_started=0
replacement_complete=0
had_cert=0
had_meta=0

cleanup() {
    if [ "$replacement_started" = 1 ] && [ "$replacement_complete" != 1 ]; then
        if [ "$had_cert" = 1 ]; then
            cp "$OLD_CERT" "$CERT_FILE"
        else
            rm -f "$CERT_FILE"
        fi
        if [ "$had_meta" = 1 ]; then
            cp "$OLD_META" "$META_FILE"
        else
            rm -f "$META_FILE"
        fi
    fi
    rm -f "$NEW_CERT" "$NEW_META"
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT HUP INT TERM

case "$CACERT_URL" in
    https://*)
        ;;
    *)
        printf 'Error: CA bundle URL must use HTTPS: %s\n' "$CACERT_URL" >&2
        exit 1
        ;;
esac
"$CURL" -fsSL --proto '=https' --proto-redir '=https' \
    "$CACERT_URL" -o "$PEM_TMP"
if [ ! -s "$PEM_TMP" ]; then
    printf 'Error: downloaded CA bundle is empty.\n' >&2
    exit 1
fi
grep -q '^-----BEGIN CERTIFICATE-----$' "$PEM_TMP" || {
    printf 'Error: downloaded CA bundle contains no PEM certificates.\n' >&2
    exit 1
}

"$ROOT_DIR/scripts/certs/generate-ca-bundle.sh" "$PEM_TMP" "$GENERATED_DIR"
"$HOSTCC" -std=c11 -Wall -Wextra -Werror -I"$GENERATED_DIR" \
    -c "$GENERATED_DIR/ca_bundle.c" -o "$OBJECT_TMP"

source_date=$(perl -MTime::Piece -ne '
    if (/Certificate data from Mozilla as of:\s+\w+\s+(\w+)\s+(\d+)\s+\S+\s+(\d{4})\s+GMT/) {
        print Time::Piece->strptime("$1 $2 $3", "%b %d %Y")->ymd;
        exit;
    }
' "$PEM_TMP")
if [ -z "$source_date" ]; then
    echo 'Error: CA bundle source date is missing.' >&2
    exit 1
fi
source_epoch=$(perl -MTime::Piece -e '
    my ($date)=@ARGV; print Time::Piece->strptime($date, "%Y-%m-%d")->epoch, "\n";
' "$source_date")
now_epoch=$(date +%s)
if [ "$source_epoch" -gt "$now_epoch" ]; then
    echo "Error: CA bundle source date is in the future: $source_date" >&2
    exit 1
fi
if [ -f "$META_FILE" ]; then
    previous_date=$(sed -n 's/^source_date=//p' "$META_FILE")
    [ -z "$previous_date" ] || [ "$source_date" \> "$previous_date" ] || \
        [ "$source_date" = "$previous_date" ] || {
            echo "Error: refusing to replace CA bundle $previous_date with older $source_date." >&2
            exit 1
        }
fi

if command -v sha256sum >/dev/null 2>&1; then
    digest=$(sha256sum "$PEM_TMP" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    digest=$(shasum -a 256 "$PEM_TMP" | awk '{print $1}')
else
    echo 'Error: neither sha256sum nor shasum is available.' >&2
    exit 1
fi
count=$(grep -c '^-----BEGIN CERTIFICATE-----$' "$PEM_TMP")
[ "$count" -ge 100 ] || {
    echo "Error: downloaded CA bundle has a suspicious certificate count: $count" >&2
    exit 1
}
max_age=120
if [ -f "$META_FILE" ]; then
    configured_age=$(sed -n 's/^max_age_days=//p' "$META_FILE")
    case "$configured_age" in
        ''|*[!0-9]*|0)
            ;;
        *)
            max_age=$configured_age
            ;;
    esac
fi
meta_tmp="$WORK_DIR/cacert.meta"
{
    echo 'format=1'
    printf 'source=%s\n' "$CACERT_URL"
    printf 'source_date=%s\n' "$source_date"
    printf 'sha256=%s\n' "$digest"
    printf 'certificate_count=%s\n' "$count"
    printf 'max_age_days=%s\n' "$max_age"
} > "$meta_tmp"

if [ -f "$CERT_FILE" ] && cmp -s "$PEM_TMP" "$CERT_FILE" && \
   [ -f "$META_FILE" ] && cmp -s "$meta_tmp" "$META_FILE"; then
    printf 'Embedded CA bundle is already up to date.\n'
    exit 0
fi

mkdir -p "$(dirname -- "$CERT_FILE")"
if [ -f "$CERT_FILE" ]; then
    cp "$CERT_FILE" "$OLD_CERT"
    had_cert=1
fi
if [ -f "$META_FILE" ]; then
    cp "$META_FILE" "$OLD_META"
    had_meta=1
fi
cp "$PEM_TMP" "$NEW_CERT"
cp "$meta_tmp" "$NEW_META"
chmod 0644 "$NEW_CERT" "$NEW_META"
replacement_started=1
mv "$NEW_CERT" "$CERT_FILE"
mv "$NEW_META" "$META_FILE"
replacement_complete=1
"$ROOT_DIR/scripts/certs/check-ca-bundle.sh"
printf 'Updated embedded CA bundle from %s\n' "$CACERT_URL"
