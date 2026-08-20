# Loads the pinned third-party source lock and defines download data.
# Versions, SHA-256 values and the manual dependency build revision live in
# config/dependencies.lock. URLs and minimum download sizes are transport data
# and do not change a compatible prefix when the verified bytes are identical.

if [ -z "${CUP_DEPENDENCIES_DIR:-}" ]; then
    CUP_DEPENDENCIES_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
fi
DEPENDENCY_LOCK_DEFAULT=$CUP_DEPENDENCIES_DIR/../../config/dependencies.lock
DEPENDENCY_LOCK_FILE=${CUP_DEPENDENCY_LOCK_FILE:-$DEPENDENCY_LOCK_DEFAULT}

validate_dependency_lock_pair() {
    local package="$1"
    local version="$2"
    local checksum="$3"

    case "$version" in
        ''|*[!A-Za-z0-9._-]*)
            echo "Error: invalid ${package}.version in dependency lock." >&2
            return 1
            ;;
    esac
    case "$checksum" in
        *[!0-9a-f]*|'')
            echo "Error: invalid ${package}.sha256 in dependency lock." >&2
            return 1
            ;;
    esac
    [ "${#checksum}" -eq 64 ] || {
        echo "Error: invalid ${package}.sha256 length in dependency lock." >&2
        return 1
    }
}

load_dependency_lock() {
    [ -f "$DEPENDENCY_LOCK_FILE" ] && [ ! -L "$DEPENDENCY_LOCK_FILE" ] || {
        echo "Error: dependency lock file is missing or is a symlink: $DEPENDENCY_LOCK_FILE" >&2
        return 1
    }

    local line key value
    local seen=' '
    local lock_format=
    local lock_build_revision=
    local lock_zlib_version= lock_zlib_sha256=
    local lock_xz_version= lock_xz_sha256=
    local lock_openssl_version= lock_openssl_sha256=
    local lock_cares_version= lock_cares_sha256=
    local lock_curl_version= lock_curl_sha256=
    local lock_libarchive_version= lock_libarchive_sha256=
    local lock_argtable3_version= lock_argtable3_sha256=
    local lock_uthash_version= lock_uthash_sha256=
    local lock_unity_version= lock_unity_sha256=
    local lock_libevent_version= lock_libevent_sha256=

    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
            *=*) key=${line%%=*}; value=${line#*=} ;;
            *)
                echo "Error: invalid dependency lock line: $line" >&2
                return 1
                ;;
        esac
        [ -n "$key" ] && [ -n "$value" ] || {
            echo "Error: dependency lock keys and values must be non-empty." >&2
            return 1
        }
        case "$key$value" in
            *[[:space:]]*)
                echo "Error: dependency lock keys and values must not contain whitespace." >&2
                return 1
                ;;
        esac
        case "$seen" in
            *" $key "*)
                echo "Error: duplicate dependency lock key: $key" >&2
                return 1
                ;;
        esac
        seen="$seen$key "
        case "$key" in
            format) lock_format=$value ;;
            build_revision) lock_build_revision=$value ;;
            zlib.version) lock_zlib_version=$value ;;
            zlib.sha256) lock_zlib_sha256=$value ;;
            xz.version) lock_xz_version=$value ;;
            xz.sha256) lock_xz_sha256=$value ;;
            openssl.version) lock_openssl_version=$value ;;
            openssl.sha256) lock_openssl_sha256=$value ;;
            cares.version) lock_cares_version=$value ;;
            cares.sha256) lock_cares_sha256=$value ;;
            curl.version) lock_curl_version=$value ;;
            curl.sha256) lock_curl_sha256=$value ;;
            libarchive.version) lock_libarchive_version=$value ;;
            libarchive.sha256) lock_libarchive_sha256=$value ;;
            argtable3.version) lock_argtable3_version=$value ;;
            argtable3.sha256) lock_argtable3_sha256=$value ;;
            uthash.version) lock_uthash_version=$value ;;
            uthash.sha256) lock_uthash_sha256=$value ;;
            unity.version) lock_unity_version=$value ;;
            unity.sha256) lock_unity_sha256=$value ;;
            libevent.version) lock_libevent_version=$value ;;
            libevent.sha256) lock_libevent_sha256=$value ;;
            *)
                echo "Error: unknown dependency lock key: $key" >&2
                return 1
                ;;
        esac
    done < "$DEPENDENCY_LOCK_FILE"

    [ "$lock_format" = 2 ] || {
        echo "Error: unsupported dependency lock format: ${lock_format:-missing}" >&2
        return 1
    }
    case "$lock_build_revision" in
        ''|*[!0-9]*|0)
            echo "Error: build_revision must be a positive integer." >&2
            return 1
            ;;
    esac

    validate_dependency_lock_pair zlib "$lock_zlib_version" "$lock_zlib_sha256" || return 1
    validate_dependency_lock_pair xz "$lock_xz_version" "$lock_xz_sha256" || return 1
    validate_dependency_lock_pair openssl "$lock_openssl_version" "$lock_openssl_sha256" || return 1
    validate_dependency_lock_pair cares "$lock_cares_version" "$lock_cares_sha256" || return 1
    validate_dependency_lock_pair curl "$lock_curl_version" "$lock_curl_sha256" || return 1
    validate_dependency_lock_pair libarchive "$lock_libarchive_version" "$lock_libarchive_sha256" || return 1
    validate_dependency_lock_pair argtable3 "$lock_argtable3_version" "$lock_argtable3_sha256" || return 1
    validate_dependency_lock_pair uthash "$lock_uthash_version" "$lock_uthash_sha256" || return 1
    validate_dependency_lock_pair unity "$lock_unity_version" "$lock_unity_sha256" || return 1
    validate_dependency_lock_pair libevent "$lock_libevent_version" "$lock_libevent_sha256" || return 1

    # Commit the parsed state only after the complete file has been validated.
    DEPENDENCY_LOCK_FORMAT=$lock_format
    DEPENDENCY_BUILD_REVISION=$lock_build_revision
    ZLIB_VERSION=$lock_zlib_version
    ZLIB_SHA256=$lock_zlib_sha256
    XZ_VERSION=$lock_xz_version
    XZ_SHA256=$lock_xz_sha256
    OPENSSL_VERSION=$lock_openssl_version
    OPENSSL_SHA256=$lock_openssl_sha256
    CARES_VERSION=$lock_cares_version
    CARES_SHA256=$lock_cares_sha256
    CURL_VERSION=$lock_curl_version
    CURL_SHA256=$lock_curl_sha256
    LIBARCHIVE_VERSION=$lock_libarchive_version
    LIBARCHIVE_SHA256=$lock_libarchive_sha256
    ARGTABLE3_VERSION=$lock_argtable3_version
    ARGTABLE3_SHA256=$lock_argtable3_sha256
    UTHASH_VERSION=$lock_uthash_version
    UTHASH_SHA256=$lock_uthash_sha256
    UNITY_VERSION=$lock_unity_version
    UNITY_SHA256=$lock_unity_sha256
    LIBEVENT_VERSION=$lock_libevent_version
    LIBEVENT_SHA256=$lock_libevent_sha256
}

load_dependency_lock || exit 1

ZLIB_URL="https://github.com/madler/zlib/releases/download"
ZLIB_URL="${ZLIB_URL}/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
XZ_URL="https://github.com/tukaani-project/xz/releases/download"
XZ_URL="${XZ_URL}/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.xz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download"
OPENSSL_URL="${OPENSSL_URL}/openssl-${OPENSSL_VERSION}"
OPENSSL_URL="${OPENSSL_URL}/openssl-${OPENSSL_VERSION}.tar.gz"
CARES_URL="https://github.com/c-ares/c-ares/releases/download"
CARES_URL="${CARES_URL}/v${CARES_VERSION}/c-ares-${CARES_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.xz"
LIBARCHIVE_URL="https://github.com/libarchive/libarchive/releases/download"
LIBARCHIVE_URL="${LIBARCHIVE_URL}/v${LIBARCHIVE_VERSION}"
LIBARCHIVE_URL="${LIBARCHIVE_URL}/libarchive-${LIBARCHIVE_VERSION}.tar.xz"
ARGTABLE3_URL="https://github.com/argtable/argtable3/archive/refs/tags/v${ARGTABLE3_VERSION}.tar.gz"
UTHASH_URL="https://github.com/troydhanson/uthash/archive/refs/tags/v${UTHASH_VERSION}.tar.gz"
UNITY_URL="https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v${UNITY_VERSION}.tar.gz"
LIBEVENT_URL="https://github.com/libevent/libevent/releases/download"
LIBEVENT_URL="${LIBEVENT_URL}/release-${LIBEVENT_VERSION}"
LIBEVENT_URL="${LIBEVENT_URL}/libevent-${LIBEVENT_VERSION}.tar.gz"

ZLIB_MIN_BYTES=100000
XZ_MIN_BYTES=500000
OPENSSL_MIN_BYTES=1000000
CARES_MIN_BYTES=500000
CURL_MIN_BYTES=1000000
LIBEVENT_MIN_BYTES=1000000
LIBARCHIVE_MIN_BYTES=1000000
ARGTABLE3_MIN_BYTES=100000
UTHASH_MIN_BYTES=100000
UNITY_MIN_BYTES=100000

all_source_packages() {
    printf '%s\n' zlib xz openssl cares curl libarchive argtable3 uthash unity libevent
}

source_url_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_URL" ;;
        xz) printf '%s\n' "$XZ_URL" ;;
        openssl) printf '%s\n' "$OPENSSL_URL" ;;
        cares) printf '%s\n' "$CARES_URL" ;;
        curl) printf '%s\n' "$CURL_URL" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_URL" ;;
        argtable3) printf '%s\n' "$ARGTABLE3_URL" ;;
        uthash) printf '%s\n' "$UTHASH_URL" ;;
        unity) printf '%s\n' "$UNITY_URL" ;;
        libevent) printf '%s\n' "$LIBEVENT_URL" ;;
        *) echo "Error: unknown source package '$1'." >&2; return 1 ;;
    esac
}

minimum_bytes_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_MIN_BYTES" ;;
        xz) printf '%s\n' "$XZ_MIN_BYTES" ;;
        openssl) printf '%s\n' "$OPENSSL_MIN_BYTES" ;;
        cares) printf '%s\n' "$CARES_MIN_BYTES" ;;
        curl) printf '%s\n' "$CURL_MIN_BYTES" ;;
        libevent) printf '%s\n' "$LIBEVENT_MIN_BYTES" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_MIN_BYTES" ;;
        argtable3) printf '%s\n' "$ARGTABLE3_MIN_BYTES" ;;
        uthash) printf '%s\n' "$UTHASH_MIN_BYTES" ;;
        unity) printf '%s\n' "$UNITY_MIN_BYTES" ;;
        *) echo "Error: unknown source package '$1'." >&2; return 1 ;;
    esac
}

version_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_VERSION" ;;
        xz) printf '%s\n' "$XZ_VERSION" ;;
        openssl) printf '%s\n' "$OPENSSL_VERSION" ;;
        cares) printf '%s\n' "$CARES_VERSION" ;;
        curl) printf '%s\n' "$CURL_VERSION" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_VERSION" ;;
        argtable3) printf '%s\n' "$ARGTABLE3_VERSION" ;;
        uthash) printf '%s\n' "$UTHASH_VERSION" ;;
        unity) printf '%s\n' "$UNITY_VERSION" ;;
        libevent) printf '%s\n' "$LIBEVENT_VERSION" ;;
        *) return 1 ;;
    esac
}

sha256_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_SHA256" ;;
        xz) printf '%s\n' "$XZ_SHA256" ;;
        openssl) printf '%s\n' "$OPENSSL_SHA256" ;;
        cares) printf '%s\n' "$CARES_SHA256" ;;
        curl) printf '%s\n' "$CURL_SHA256" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_SHA256" ;;
        argtable3) printf '%s\n' "$ARGTABLE3_SHA256" ;;
        uthash) printf '%s\n' "$UTHASH_SHA256" ;;
        unity) printf '%s\n' "$UNITY_SHA256" ;;
        libevent) printf '%s\n' "$LIBEVENT_SHA256" ;;
        *) return 1 ;;
    esac
}
