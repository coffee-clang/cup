#!/usr/bin/env bash

ZLIB_VERSION="${ZLIB_VERSION:-1.3.2}"
XZ_VERSION="${XZ_VERSION:-5.8.3}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.7}"
CURL_VERSION="${CURL_VERSION:-8.20.0}"
LIBARCHIVE_VERSION="${LIBARCHIVE_VERSION:-3.8.7}"

ZLIB_SHA256="${ZLIB_SHA256:-bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16}"
XZ_SHA256="${XZ_SHA256:-fff1ffcf2b0da84d308a14de513a1aa23d4e9aa3464d17e64b9714bfdd0bbfb6}"
OPENSSL_SHA256="${OPENSSL_SHA256:-a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8}"
CURL_SHA256="${CURL_SHA256:-63fe2dc148ba0ceae89922ef838f7e5c946272c2e78b7c59fab4b79d3ce2b896}"
LIBARCHIVE_SHA256="${LIBARCHIVE_SHA256:-d3a8ba457ae25c27c84fd2830a2efdcc5b1d40bf585d4eb0d35f47e99e5d4774}"

ARGTABLE3_VERSION="${ARGTABLE3_VERSION:-3.3.1}"
UTHASH_VERSION="${UTHASH_VERSION:-2.3.0}"
UNITY_VERSION="${UNITY_VERSION:-2.6.1}"

ARGTABLE3_SHA256="${ARGTABLE3_SHA256:-8b28a4fb2cd621d8d16f34e30e1956aa488077f6a6b902e7fc9f07883e1519c1}"
UTHASH_SHA256="${UTHASH_SHA256:-e10382ab75518bad8319eb922ad04f907cb20cccb451a3aa980c9d005e661acc}"
UNITY_SHA256="${UNITY_SHA256:-b41a66d45a6b99758fb3202ace6178177014d52fc524bf1f72687d93e9867292}"

ARGTABLE3_URL="${ARGTABLE3_URL:-https://github.com/argtable/argtable3/archive/refs/tags/v${ARGTABLE3_VERSION}.tar.gz}"
UTHASH_URL="${UTHASH_URL:-https://github.com/troydhanson/uthash/archive/refs/tags/v${UTHASH_VERSION}.tar.gz}"
UNITY_URL="${UNITY_URL:-https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v${UNITY_VERSION}.tar.gz}"

ARGTABLE3_MIN_BYTES="${ARGTABLE3_MIN_BYTES:-100000}"
UTHASH_MIN_BYTES="${UTHASH_MIN_BYTES:-100000}"
UNITY_MIN_BYTES="${UNITY_MIN_BYTES:-100000}"

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

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: required tool '$1' was not found." >&2
        exit 1
    fi
}

require_sha256_tool() {
    if ! command -v sha256sum >/dev/null 2>&1 &&
        ! command -v shasum >/dev/null 2>&1; then
        echo "Error: neither sha256sum nor shasum is available." >&2
        exit 1
    fi
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
        *) return 1 ;;
    esac
}

file_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "Error: neither sha256sum nor shasum is available." >&2
        return 1
    fi
}

verify_source_checksum() {
    package="$1"
    file="$2"
    expected="$(sha256_for_package "$package")"
    actual="$(file_sha256 "$file")"
    if [ "$actual" != "$expected" ]; then
        echo "Error: SHA-256 verification failed for $(basename "$file")." >&2
        echo "Expected: $expected" >&2
        echo "Actual:   $actual" >&2
        return 1
    fi
}

dependency_signature() {
    platform="$1"
    toolchain="$2"
    use_openssl="${3:-1}"

    printf '%s\n' \
        "platform=$platform" \
        "toolchain=$toolchain" \
        "zlib=$ZLIB_VERSION:$ZLIB_SHA256" \
        "xz=$XZ_VERSION:$XZ_SHA256"
    if [ "$use_openssl" = 1 ]; then
        printf '%s\n' "openssl=$OPENSSL_VERSION:$OPENSSL_SHA256"
    fi
    printf '%s\n' \
        "curl=$CURL_VERSION:$CURL_SHA256" \
        "libarchive=$LIBARCHIVE_VERSION:$LIBARCHIVE_SHA256" \
        "argtable3=$ARGTABLE3_VERSION:$ARGTABLE3_SHA256" \
        "uthash=$UTHASH_VERSION:$UTHASH_SHA256" \
        "unity=$UNITY_VERSION:$UNITY_SHA256"
}

prepare_dependency_prefix() {
    prefix="$1"
    signature="$2"
    config="$prefix/.cup-deps-config"
    building="$prefix/.cup-deps-building"

    if [ -f "$building" ]; then
        echo "Error: an interrupted dependency build exists in $prefix." >&2
        echo "Remove the prefix and rebuild it cleanly." >&2
        exit 1
    fi
    if [ -f "$config" ] && [ "$(cat "$config")" != "$signature" ]; then
        echo "Error: dependency prefix configuration does not match this build." >&2
        echo "Remove $prefix and rebuild it cleanly." >&2
        exit 1
    fi
    if [ ! -f "$config" ] && [ -d "$prefix" ] &&
        [ -n "$(ls -A "$prefix" 2>/dev/null)" ]; then
        echo "Error: dependency prefix is not empty and has no cup configuration stamp." >&2
        echo "Remove $prefix and rebuild it cleanly." >&2
        exit 1
    fi

    mkdir -p "$prefix"
    printf '%s\n' "$signature" > "$building"
}

finish_dependency_prefix() {
    prefix="$1"
    mv "$prefix/.cup-deps-building" "$prefix/.cup-deps-config"
}

download_source() {
    package="$1"
    output="$2"
    url="$(source_url_for_package "$package")"
    min_bytes="$(minimum_bytes_for_package "$package")"
    size=""

    if [ -f "$output" ]; then
        size="$(wc -c < "$output" | tr -d '[:space:]')"
        if [ "$size" -ge "$min_bytes" ] &&
            verify_source_checksum "$package" "$output"; then
            echo "==> Using cached $(basename "$output")"
            return 0
        fi

        echo "==> Removing suspicious cached $(basename "$output") (${size} bytes)"
        rm -f "$output"
    fi

    tmp_output="$(mktemp "${output}.tmp.XXXXXX")"
    echo "==> Downloading $url"
    if ! curl -fL --retry 3 --retry-delay 5 --retry-all-errors \
        "$url" -o "$tmp_output"; then
        rm -f "$tmp_output"
        return 1
    fi

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
        rm -f "$tmp_output"
        return 1
    fi

    if ! verify_source_checksum "$package" "$tmp_output"; then
        rm -f "$tmp_output"
        return 1
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


build_argtable3_uthash_unity() {
    prefix="$1"
    src_dir="$2"
    build_dir="$3"
    compiler="$4"
    archiver="$5"
    ranlib_tool="$6"
    archive=""
    source=""
    object_dir="$build_dir/cup-small-deps"

    mkdir -p "$prefix/include" "$prefix/lib" "$object_dir"

    archive="$src_dir/argtable3-${ARGTABLE3_VERSION}.tar.gz"
    source="$build_dir/argtable3-${ARGTABLE3_VERSION}"
    download_source argtable3 "$archive"
    extract_archive "$archive" "$source"
    rm -f "$object_dir"/argtable3-*.o "$prefix/lib/libargtable3.a"
    for file in "$source"/src/*.c; do
        object="$object_dir/argtable3-$(basename "${file%.c}").o"
        "$compiler" -O2 -I"$source/src" -c "$file" -o "$object"
    done
    "$archiver" rcs "$prefix/lib/libargtable3.a" "$object_dir"/argtable3-*.o
    "$ranlib_tool" "$prefix/lib/libargtable3.a"
    cp "$source/src/argtable3.h" "$prefix/include/argtable3.h"

    archive="$src_dir/uthash-${UTHASH_VERSION}.tar.gz"
    source="$build_dir/uthash-${UTHASH_VERSION}"
    download_source uthash "$archive"
    extract_archive "$archive" "$source"
    cp "$source/src/uthash.h" "$prefix/include/uthash.h"

    archive="$src_dir/unity-${UNITY_VERSION}.tar.gz"
    source="$build_dir/unity-${UNITY_VERSION}"
    download_source unity "$archive"
    extract_archive "$archive" "$source"
    "$compiler" -O2 -I"$source/src" -c "$source/src/unity.c" \
        -o "$object_dir/unity.o"
    "$archiver" rcs "$prefix/lib/libunity.a" "$object_dir/unity.o"
    "$ranlib_tool" "$prefix/lib/libunity.a"
    cp "$source/src/unity.h" "$source/src/unity_internals.h" "$prefix/include/"
}
