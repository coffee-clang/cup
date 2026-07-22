#!/usr/bin/env sh
# Purpose: Validates embedded CA metadata and rejects stale release inputs.
set -eu
LC_ALL=C
TZ=UTC
export LC_ALL TZ

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PEM=${CUP_CA_CERT_FILE:-$ROOT/certs/cacert.pem}
META=${CUP_CA_META_FILE:-$ROOT/certs/cacert.meta}
NOW_EPOCH=${CUP_CA_CURRENT_EPOCH:-$(date +%s)}

fail() {
    echo "CA bundle validation: $*" >&2
    exit 1
}

[ -s "$PEM" ] && [ -s "$META" ] || fail 'bundle or metadata is missing'
value() {
    sed -n "s/^$1=//p" "$META"
}
[ "$(value format)" = 1 ] || fail 'unsupported metadata format'
source_url=$(value source)
case "$source_url" in
    https://*)
        ;;
    *)
        fail 'metadata source must use HTTPS'
        ;;
esac

if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$PEM" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$PEM" | awk '{print $1}')
else
    fail 'neither sha256sum nor shasum is available'
fi
[ "$actual" = "$(value sha256)" ] || fail 'SHA-256 does not match metadata'

count=$(grep -c '^-----BEGIN CERTIFICATE-----$' "$PEM")
[ "$count" = "$(value certificate_count)" ] || fail 'certificate count does not match metadata'
[ "$count" -ge 100 ] || fail "certificate count is suspiciously low: $count"

source_date=$(value source_date)
max_age=$(value max_age_days)
case "$max_age" in
    ''|*[!0-9]*|0)
        fail 'invalid max_age_days'
        ;;
esac
[ "$max_age" -le 365 ] || fail 'max_age_days exceeds the repository safety ceiling'
case "$NOW_EPOCH" in
    ''|*[!0-9]*)
        fail 'invalid current epoch'
        ;;
esac

source_epoch=$(perl -MTime::Piece -e '
    my ($date)=@ARGV;
    my $then=Time::Piece->strptime($date, "%Y-%m-%d");
    print $then->epoch, "\n";
' "$source_date") || fail 'invalid source date'
age=$(( (NOW_EPOCH - source_epoch) / 86400 ))
[ "$age" -ge 0 ] || fail "source date is in the future: $source_date"
[ "$age" -le "$max_age" ] ||
    fail "bundle is $age days old (limit: $max_age); run make update-ca-bundle"

printf 'CA bundle verified: %s certificates, source date %s, age %s days.\n' \
    "$count" "$source_date" "$age"
