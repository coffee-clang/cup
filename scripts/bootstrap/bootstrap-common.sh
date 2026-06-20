#!/usr/bin/env bash

ZLIB_VERSION="${ZLIB_VERSION:-1.3.2}"
XZ_VERSION="${XZ_VERSION:-5.8.3}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.6}"
CURL_VERSION="${CURL_VERSION:-8.20.0}"
LIBARCHIVE_VERSION="${LIBARCHIVE_VERSION:-3.8.7}"

ZLIB_URL="${ZLIB_URL:-https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz}"
XZ_URL="${XZ_URL:-https://github.com/tukaani-project/xz/releases/download/v${XZ_VERSION}/xz-${XZ_VERSION}.tar.xz}"
OPENSSL_URL="${OPENSSL_URL:-https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz}"
CURL_URL="${CURL_URL:-https://curl.se/download/curl-${CURL_VERSION}.tar.xz}"
LIBARCHIVE_URL="${LIBARCHIVE_URL:-https://github.com/libarchive/libarchive/releases/download/v${LIBARCHIVE_VERSION}/libarchive-${LIBARCHIVE_VERSION}.tar.xz}"

ZLIB_MIN_BYTES="${ZLIB_MIN_BYTES:-100000}"
XZ_MIN_BYTES="${XZ_MIN_BYTES:-500000}"
OPENSSL_MIN_BYTES="${OPENSSL_MIN_BYTES:-1000000}"
CURL_MIN_BYTES="${CURL_MIN_BYTES:-1000000}"
LIBARCHIVE_MIN_BYTES="${LIBARCHIVE_MIN_BYTES:-1000000}"

source_url_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_URL" ;;
        xz) printf '%s\n' "$XZ_URL" ;;
        openssl) printf '%s\n' "$OPENSSL_URL" ;;
        curl) printf '%s\n' "$CURL_URL" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_URL" ;;
        *)
            echo "Error: unknown source package '$1'." >&2
            return 1
            ;;
    esac
}

minimum_bytes_for_package() {
    case "$1" in
        zlib) printf '%s\n' "$ZLIB_MIN_BYTES" ;;
        xz) printf '%s\n' "$XZ_MIN_BYTES" ;;
        openssl) printf '%s\n' "$OPENSSL_MIN_BYTES" ;;
        curl) printf '%s\n' "$CURL_MIN_BYTES" ;;
        libarchive) printf '%s\n' "$LIBARCHIVE_MIN_BYTES" ;;
        *) printf '%s\n' 0 ;;
    esac
}

download_source() {
    package="$1"
    output="$2"
    url="$(source_url_for_package "$package")"
    min_bytes="$(minimum_bytes_for_package "$package")"
    tmp_output="$output.tmp"
    size=""

    if [ -f "$output" ]; then
        size="$(wc -c < "$output" | tr -d '[:space:]')"
        if [ "$size" -ge "$min_bytes" ]; then
            echo "==> Using cached $(basename "$output")"
            return 0
        fi

        echo "==> Removing suspicious cached $(basename "$output") (${size} bytes)"
        rm -f "$output"
    fi

    echo "==> Downloading $url"
    rm -f "$tmp_output"
    curl -fL --retry 3 --retry-delay 5 --retry-all-errors "$url" -o "$tmp_output"

    size="$(wc -c < "$tmp_output" | tr -d '[:space:]')"
    if [ "$size" -lt "$min_bytes" ]; then
        echo "Error: downloaded $package archive is unexpectedly small: ${size} bytes." >&2
        echo "URL: $url" >&2
        echo "File: $tmp_output" >&2
        if command -v file >/dev/null 2>&1; then
            file "$tmp_output" >&2 || true
        fi
        echo "First bytes:" >&2
        head -c 300 "$tmp_output" >&2 || true
        echo >&2
        exit 1
    fi

    mv "$tmp_output" "$output"
}

extract_archive() {
    archive="$1"
    destination="$2"

    rm -rf "$destination"
    mkdir -p "$destination"

    case "$archive" in
        *.tar.gz|*.tgz)
            tar -xzf "$archive" -C "$destination" --strip-components=1
            ;;
        *.tar.xz)
            tar -xJf "$archive" -C "$destination" --strip-components=1
            ;;
        *)
            echo "Error: unsupported archive format '$archive'." >&2
            exit 1
            ;;
    esac
}
