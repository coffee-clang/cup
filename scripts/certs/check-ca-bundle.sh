#!/bin/sh

# Validates the checked-in CA bundle, its metadata and release freshness.
set -eu

LC_ALL=C
LANG=C
TZ=UTC
export LC_ALL LANG TZ

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
PROJECT_ROOT=$ROOT
# shellcheck source=../lib/path-safety.sh
. "$ROOT/scripts/lib/path-safety.sh"
PEM=${CUP_CA_CERT_FILE:-$ROOT/certs/cacert.pem}
META=${CUP_CA_META_FILE:-$ROOT/certs/cacert.meta}
NOW_EPOCH=${CUP_CA_CURRENT_EPOCH:-$(date +%s)}
MODE=release

case "${1:-}" in
    '') ;;
    --integrity) MODE=integrity ;;
    *)
        printf 'Usage: %s [--integrity]\n' "$0" >&2
        exit 2
        ;;
esac
[ "$#" -le 1 ] || {
    printf 'Usage: %s [--integrity]\n' "$0" >&2
    exit 2
}

fail() {
    printf 'CA bundle validation: %s\n' "$*" >&2
    exit 1
}

reject_nul_or_cr() {
    file=$1
    label=$2
    if od -An -v -t x1 "$file" | awk '{ for (i = 1; i <= NF; ++i) if ($i == "00" || $i == "0d") exit 1 }'; then
        return 0
    fi
    fail "$label contains a NUL or carriage-return byte"
}

case "$PEM" in /*|[A-Za-z]:/*) ;; *) PEM=$(pwd -P)/$PEM ;; esac
case "$META" in /*|[A-Za-z]:/*) ;; *) META=$(pwd -P)/$META ;; esac
cup_path_require_regular_file "$PEM" 'CA bundle' || exit 1
cup_path_require_regular_file "$META" 'CA metadata' || exit 1
[ -s "$PEM" ] || fail "bundle is empty: $PEM"
[ -s "$META" ] || fail "metadata is empty: $META"
reject_nul_or_cr "$PEM" 'CA bundle'
reject_nul_or_cr "$META" 'CA metadata'
TEMP_BASE=$(cup_path_resolve_host_temporary_directory \
    'CA validation temporary parent') || exit 1
command -v perl >/dev/null 2>&1 || fail 'Perl is required for date validation'
perl -MTime::Piece -e 1 >/dev/null 2>&1 || fail 'Perl Time::Piece is required for date validation'

metadata_value() {
    metadata_key=$1
    metadata_count=$(grep -c "^${metadata_key}=" "$META" || true)
    [ "$metadata_count" -eq 1 ] ||
        fail "metadata key '$metadata_key' must appear exactly once"
    sed -n "s/^${metadata_key}=//p" "$META"
}

# The metadata format is deliberately exact so unknown fields cannot silently
# acquire authority.
awk '
    BEGIN {
        allowed["format"] = 1
        allowed["source"] = 1
        allowed["source_date"] = 1
        allowed["sha256"] = 1
        allowed["certificate_count"] = 1
        allowed["max_age_days"] = 1
    }
    /^[[:space:]]*$/ { exit 10 }
    index($0, "=") == 0 { exit 11 }
    {
        key = substr($0, 1, index($0, "=") - 1)
        value = substr($0, index($0, "=") + 1)
        if (!(key in allowed) || value == "" || seen[key]++) exit 12
    }
    END {
        if (NR != 6) exit 13
        for (key in allowed) if (!seen[key]) exit 14
    }
' "$META" || fail 'metadata schema is invalid, incomplete or contains unknown fields'

[ "$(metadata_value format)" = 1 ] || fail 'unsupported metadata format'
source_url=$(metadata_value source)
case "$source_url" in
    https://*) ;;
    *) fail 'metadata source must use HTTPS' ;;
esac

expected_sha=$(metadata_value sha256)
case "$expected_sha" in
    *[!0-9a-f]*|'') fail 'metadata SHA-256 is not lowercase hexadecimal' ;;
esac
[ "${#expected_sha}" -eq 64 ] || fail 'metadata SHA-256 must contain 64 characters'

if command -v sha256sum >/dev/null 2>&1; then
    actual_sha=$(sha256sum "$PEM" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
    actual_sha=$(shasum -a 256 "$PEM" | awk '{print $1}')
else
    fail 'neither sha256sum nor shasum is available'
fi
[ "$actual_sha" = "$expected_sha" ] || fail 'SHA-256 does not match metadata'

begin_count=$(grep -c '^-----BEGIN CERTIFICATE-----$' "$PEM" || true)
end_count=$(grep -c '^-----END CERTIFICATE-----$' "$PEM" || true)
[ "$begin_count" -eq "$end_count" ] || fail 'PEM certificate boundaries are unbalanced'
[ "$begin_count" -ge 100 ] || fail "certificate count is suspiciously low: $begin_count"

expected_count=$(metadata_value certificate_count)
case "$expected_count" in
    ''|*[!0-9]*) fail 'invalid certificate_count' ;;
esac
[ "$begin_count" -eq "$expected_count" ] || fail 'certificate count does not match metadata'

source_date=$(metadata_value source_date)
case "$source_date" in
    [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
    *) fail 'source_date must use YYYY-MM-DD' ;;
esac
source_epoch=$(perl -MTime::Piece -e '
    my ($date)=@ARGV;
    my $then=eval { Time::Piece->strptime($date, "%Y-%m-%d") };
    die "invalid\n" unless $then && $then->ymd eq $date;
    print $then->epoch, "\n";
' "$source_date" 2>/dev/null) || fail 'invalid source date'

max_age=$(metadata_value max_age_days)
case "$max_age" in
    ''|*[!0-9]*|0) fail 'invalid max_age_days' ;;
esac
[ "$max_age" -le 365 ] || fail 'max_age_days exceeds the repository safety ceiling'

canonical_meta=$(mktemp "$TEMP_BASE/cup-ca-meta.XXXXXX") ||
    fail 'could not create metadata comparison file'
{
    printf 'format=1\n'
    printf 'source=%s\n' "$source_url"
    printf 'source_date=%s\n' "$source_date"
    printf 'sha256=%s\n' "$expected_sha"
    printf 'certificate_count=%s\n' "$expected_count"
    printf 'max_age_days=%s\n' "$max_age"
} > "$canonical_meta" || {
    rm -f -- "$canonical_meta"
    fail 'could not prepare canonical metadata comparison'
}
if ! cmp -s "$canonical_meta" "$META"; then
    rm -f -- "$canonical_meta"
    fail 'metadata is not canonical LF-terminated ASCII in the required key order'
fi
rm -f -- "$canonical_meta"

openssl_command=${CUP_OPENSSL:-openssl}
command -v "$openssl_command" >/dev/null 2>&1 ||
    fail "OpenSSL command is required for X.509 validation: $openssl_command"
validation_dir=$(mktemp -d "$TEMP_BASE/cup-ca-check.XXXXXX")
validation_pkcs7=$validation_dir/cacert.p7b
cleanup_validation() {
    if [ -d "$validation_dir" ] && [ ! -L "$validation_dir" ]; then
        rm -rf -- "$validation_dir"
    fi
}
trap cleanup_validation EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
"$openssl_command" crl2pkcs7 -nocrl -certfile "$PEM" -out "$validation_pkcs7" >/dev/null 2>&1 ||
    fail 'bundle contains invalid X.509 certificate data'
"$openssl_command" pkcs7 -print_certs -noout -in "$validation_pkcs7" >/dev/null 2>&1 ||
    fail 'bundle contains invalid X.509 certificate data'

if [ "$MODE" = release ]; then
    case "$NOW_EPOCH" in
        ''|*[!0-9]*) fail 'invalid current epoch' ;;
    esac
    age=$(( (NOW_EPOCH - source_epoch) / 86400 ))
    [ "$age" -ge 0 ] || fail "source date is in the future: $source_date"
    [ "$age" -le "$max_age" ] ||
        fail "bundle is $age days old (limit: $max_age); run make update-ca-bundle"
    printf 'CA bundle verified: %s certificates, source date %s, age %s days.\n' \
        "$begin_count" "$source_date" "$age"
else
    printf 'CA bundle integrity verified: %s certificates, source date %s.\n' \
        "$begin_count" "$source_date"
fi
