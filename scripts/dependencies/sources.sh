# Purpose: Loads the pinned third-party source lock and defines download data.
# Versions and SHA-256 values live in config/dependencies.lock. URLs and minimum
# download sizes are transport details and do not invalidate a compatible
# dependency prefix.

DEPENDENCY_LOCK_FILE=${CUP_DEPENDENCY_LOCK_FILE:-$CUP_DEPENDENCIES_DIR/../../config/dependencies.lock}

load_dependency_lock() {
    [ -f "$DEPENDENCY_LOCK_FILE" ] || {
        echo "Error: dependency lock file is missing: $DEPENDENCY_LOCK_FILE" >&2
        return 1
    }

    local line
    local key
    local value
    local format=
    local seen=' '

    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*)
                continue
                ;;
            *=*)
                key=${line%%=*}
                value=${line#*=}
                ;;
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
            format) format=$value ;;
            zlib.version) ZLIB_VERSION=$value ;;
            zlib.sha256) ZLIB_SHA256=$value ;;
            xz.version) XZ_VERSION=$value ;;
            xz.sha256) XZ_SHA256=$value ;;
            openssl.version) OPENSSL_VERSION=$value ;;
            openssl.sha256) OPENSSL_SHA256=$value ;;
            curl.version) CURL_VERSION=$value ;;
            curl.sha256) CURL_SHA256=$value ;;
            libarchive.version) LIBARCHIVE_VERSION=$value ;;
            libarchive.sha256) LIBARCHIVE_SHA256=$value ;;
            argtable3.version) ARGTABLE3_VERSION=$value ;;
            argtable3.sha256) ARGTABLE3_SHA256=$value ;;
            uthash.version) UTHASH_VERSION=$value ;;
            uthash.sha256) UTHASH_SHA256=$value ;;
            unity.version) UNITY_VERSION=$value ;;
            unity.sha256) UNITY_SHA256=$value ;;
            libevent.version) LIBEVENT_VERSION=$value ;;
            libevent.sha256) LIBEVENT_SHA256=$value ;;
            *)
                echo "Error: unknown dependency lock key: $key" >&2
                return 1
                ;;
        esac
    done < "$DEPENDENCY_LOCK_FILE"

    [ "$format" = 1 ] || {
        echo "Error: unsupported dependency lock format: ${format:-missing}" >&2
        return 1
    }

    for package in ZLIB XZ OPENSSL CURL LIBARCHIVE ARGTABLE3 UTHASH UNITY LIBEVENT; do
        eval "version=\${${package}_VERSION:-}"
        eval "checksum=\${${package}_SHA256:-}"
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
    done
}

load_dependency_lock

ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz"
XZ_URL="https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.xz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.xz"
LIBARCHIVE_URL="https://github.com/libarchive/libarchive/releases/download/v${LIBARCHIVE_VERSION}/libarchive-${LIBARCHIVE_VERSION}.tar.xz"
ARGTABLE3_URL="https://github.com/argtable/argtable3/archive/refs/tags/v${ARGTABLE3_VERSION}.tar.gz"
UTHASH_URL="https://github.com/troydhanson/uthash/archive/refs/tags/v${UTHASH_VERSION}.tar.gz"
UNITY_URL="https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v${UNITY_VERSION}.tar.gz"
LIBEVENT_URL="https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}/libevent-${LIBEVENT_VERSION}.tar.gz"

ZLIB_MIN_BYTES=100000
XZ_MIN_BYTES=500000
OPENSSL_MIN_BYTES=1000000
CURL_MIN_BYTES=1000000
LIBEVENT_MIN_BYTES=1000000
LIBARCHIVE_MIN_BYTES=1000000
ARGTABLE3_MIN_BYTES=100000
UTHASH_MIN_BYTES=100000
UNITY_MIN_BYTES=100000

all_source_packages() {
    printf '%s\n' zlib xz openssl curl libarchive argtable3 uthash unity libevent
}

dependency_scope_for_package() {
    case "$1" in
        zlib|xz|openssl|curl|libarchive|argtable3|uthash) printf '%s\n' runtime ;;
        unity|libevent) printf '%s\n' test ;;
        *) echo "Error: unknown source package '$1'." >&2; return 1 ;;
    esac
}

dependency_usage_for_package() {
    case "$1" in
        zlib|xz|openssl|curl|libarchive|argtable3) printf '%s\n' static-library ;;
        uthash) printf '%s\n' header-only ;;
        unity) printf '%s\n' unit-test-library ;;
        libevent) printf '%s\n' network-test-library ;;
        *) echo "Error: unknown source package '$1'." >&2; return 1 ;;
    esac
}

source_url_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_URL" ;;
        xz) printf '%s\n' "$XZ_URL" ;;
        openssl) printf '%s\n' "$OPENSSL_URL" ;;
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
        curl) printf '%s\n' "$CURL_MIN_BYTES" ;;
        libevent) printf '%s\n' "$LIBEVENT_MIN_BYTES" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_MIN_BYTES" ;;
        argtable3) printf '%s\n' "$ARGTABLE3_MIN_BYTES" ;;
        uthash) printf '%s\n' "$UTHASH_MIN_BYTES" ;;
        unity) printf '%s\n' "$UNITY_MIN_BYTES" ;;
        *) printf '%s\n' 0 ;;
    esac
}

sha256_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_SHA256" ;;
        xz) printf '%s\n' "$XZ_SHA256" ;;
        openssl) printf '%s\n' "$OPENSSL_SHA256" ;;
        curl) printf '%s\n' "$CURL_SHA256" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_SHA256" ;;
        argtable3) printf '%s\n' "$ARGTABLE3_SHA256" ;;
        uthash) printf '%s\n' "$UTHASH_SHA256" ;;
        unity) printf '%s\n' "$UNITY_SHA256" ;;
        libevent) printf '%s\n' "$LIBEVENT_SHA256" ;;
        *) return 1 ;;
    esac
}
