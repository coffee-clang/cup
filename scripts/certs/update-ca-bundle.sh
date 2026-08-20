#!/bin/sh

# Downloads and validates a CA bundle before replacing versioned inputs.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ
umask 022

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
PROJECT_ROOT=$ROOT_DIR
# shellcheck source=../lib/path-safety.sh
. "$ROOT_DIR/scripts/lib/path-safety.sh"
CERT_DIR=$ROOT_DIR/certs
CERT_FILE=$CERT_DIR/cacert.pem
META_FILE=$CERT_DIR/cacert.meta
CACERT_URL=${CUP_CACERT_URL:-https://curl.se/ca/cacert.pem}
CURL=${CURL:-curl}
HOSTCC=${HOSTCC:-cc}
MAX_DOWNLOAD_BYTES=${CUP_CACERT_MAX_BYTES:-5242880}

fail() {
    printf 'CA bundle update: %s\n' "$*" >&2
    exit 1
}

cup_path_check_directory_chain "$CERT_DIR" 0 'certificate directory' || exit 1
for existing in "$CERT_FILE" "$META_FILE"; do
    if [ -e "$existing" ] || [ -L "$existing" ]; then
        cup_path_require_regular_file "$existing" 'repository CA input' || exit 1
    fi
done
command -v "$CURL" >/dev/null 2>&1 || fail "curl command is unavailable: $CURL"
command -v "$HOSTCC" >/dev/null 2>&1 || fail "host C compiler is unavailable: $HOSTCC"
command -v perl >/dev/null 2>&1 || fail 'Perl is required for source-date validation'
perl -MTime::Piece -e 1 >/dev/null 2>&1 || fail 'Perl Time::Piece is required for source-date validation'
case "$MAX_DOWNLOAD_BYTES" in
    ''|*[!0-9]*|0) fail 'CUP_CACERT_MAX_BYTES must be a positive integer' ;;
esac
case "$CACERT_URL" in
    https://*) ;;
    *) fail "CA bundle URL must use HTTPS: $CACERT_URL" ;;
esac

TEMP_BASE=$(cup_path_resolve_host_temporary_directory \
    'CA update temporary parent') || exit 1
WORK_DIR=$(cup_path_create_unique_directory \
    "$TEMP_BASE/cup-ca-update.XXXXXX" 'CA update work directory' 0700) || exit 1
PEM_TMP=$WORK_DIR/cacert.pem
META_TMP=$WORK_DIR/cacert.meta
GENERATED_DIR=$WORK_DIR/generated
OBJECT_TMP=$WORK_DIR/ca_bundle.o
OLD_CERT=$WORK_DIR/old-cacert.pem
OLD_META=$WORK_DIR/old-cacert.meta
replacement_started=0
replacement_complete=0
had_cert=0
had_meta=0

cleanup() {
    if [ "$replacement_started" = 1 ] && [ "$replacement_complete" != 1 ]; then
        if [ "$had_cert" = 1 ]; then
            cup_path_copy_file "$OLD_CERT" "$CERT_FILE" 0644 replace >/dev/null 2>&1 || true
        else
            cup_path_remove_file "$CERT_FILE" 'canonical CA bundle' >/dev/null 2>&1 || true
        fi
        if [ "$had_meta" = 1 ]; then
            cup_path_copy_file "$OLD_META" "$META_FILE" 0644 replace >/dev/null 2>&1 || true
        else
            cup_path_remove_file "$META_FILE" 'canonical CA metadata' >/dev/null 2>&1 || true
        fi
    fi
    if [ -n "${WORK_DIR:-}" ] && [ -e "$WORK_DIR" ]; then
        cup_path_remove_directory_tree "$WORK_DIR" 'CA update work directory' >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"$CURL" -fsSL --proto '=https' --proto-redir '=https' \
    --connect-timeout 20 --max-time 120 --speed-time 30 --speed-limit 1024 \
    --max-filesize "$MAX_DOWNLOAD_BYTES" \
    "$CACERT_URL" -o "$PEM_TMP"
[ -s "$PEM_TMP" ] || fail 'downloaded CA bundle is empty'
actual_size=$(wc -c < "$PEM_TMP" | tr -d '[:space:]')
[ "$actual_size" -le "$MAX_DOWNLOAD_BYTES" ] || fail 'downloaded CA bundle exceeds the configured size limit'

source_date=$(perl -MTime::Piece -ne '
    if (/Certificate data from Mozilla as of:\s+\w+\s+(\w+)\s+(\d+)\s+\S+\s+(\d{4})\s+GMT/) {
        print Time::Piece->strptime("$1 $2 $3", "%b %d %Y")->ymd;
        exit;
    }
' "$PEM_TMP")
[ -n "$source_date" ] || fail 'CA bundle source date is missing'
source_epoch=$(perl -MTime::Piece -e '
    my ($date)=@ARGV; print Time::Piece->strptime($date, "%Y-%m-%d")->epoch, "\n";
' "$source_date")
now_epoch=$(date +%s)
[ "$source_epoch" -le "$now_epoch" ] || fail "CA bundle source date is in the future: $source_date"

max_age=120
if [ -f "$META_FILE" ]; then
    CUP_CA_CERT_FILE=$CERT_FILE CUP_CA_META_FILE=$META_FILE \
        "$ROOT_DIR/scripts/certs/check-ca-bundle.sh" --integrity >/dev/null
    previous_date=$(sed -n 's/^source_date=//p' "$META_FILE")
    [ "$source_date" \> "$previous_date" ] || [ "$source_date" = "$previous_date" ] ||
        fail "refusing to replace CA bundle $previous_date with older $source_date"
    max_age=$(sed -n 's/^max_age_days=//p' "$META_FILE")
fi

if command -v sha256sum >/dev/null 2>&1; then
    digest=$(sha256sum "$PEM_TMP" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    digest=$(shasum -a 256 "$PEM_TMP" | awk '{print $1}')
else
    fail 'neither sha256sum nor shasum is available'
fi
count=$(grep -c '^-----BEGIN CERTIFICATE-----$' "$PEM_TMP" || true)
[ "$count" -ge 100 ] || fail "downloaded CA bundle has a suspicious certificate count: $count"

cat > "$META_TMP" <<META
format=1
source=$CACERT_URL
source_date=$source_date
sha256=$digest
certificate_count=$count
max_age_days=$max_age
META
chmod 0644 "$PEM_TMP" "$META_TMP"

CUP_CA_CERT_FILE=$PEM_TMP CUP_CA_META_FILE=$META_TMP CUP_CA_CURRENT_EPOCH=$now_epoch \
    "$ROOT_DIR/scripts/certs/check-ca-bundle.sh" >/dev/null

CUP_CA_META_FILE=$META_TMP \
    "$ROOT_DIR/scripts/certs/generate-ca-bundle.sh" "$PEM_TMP" "$GENERATED_DIR"
"$HOSTCC" -std=c11 -Wall -Wextra -Werror -I"$GENERATED_DIR" \
    -c "$GENERATED_DIR/ca_bundle.c" -o "$OBJECT_TMP"

if [ -f "$CERT_FILE" ] && cmp -s "$PEM_TMP" "$CERT_FILE" &&
   [ -f "$META_FILE" ] && cmp -s "$META_TMP" "$META_FILE"; then
    printf 'Embedded CA bundle is already up to date.\n'
    exit 0
fi

if [ -f "$CERT_FILE" ]; then
    cup_path_copy_file "$CERT_FILE" "$OLD_CERT" 0644 replace
    had_cert=1
fi
if [ -f "$META_FILE" ]; then
    cup_path_copy_file "$META_FILE" "$OLD_META" 0644 replace
    had_meta=1
fi
cup_path_prepare_child_file "$CERT_DIR" "$CERT_FILE" 'canonical CA bundle' || exit 1
cup_path_prepare_child_file "$CERT_DIR" "$META_FILE" 'canonical CA metadata' || exit 1
replacement_started=1
cup_path_copy_file "$PEM_TMP" "$CERT_FILE" 0644 replace ||
    fail 'could not commit canonical CA bundle'
cup_path_copy_file "$META_TMP" "$META_FILE" 0644 replace ||
    fail 'could not commit canonical CA metadata'
"$ROOT_DIR/scripts/certs/check-ca-bundle.sh" >/dev/null
replacement_complete=1
printf 'Updated embedded CA bundle from %s\n' "$CACERT_URL"
