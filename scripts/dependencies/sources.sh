#!/usr/bin/env sh

# Purpose: Canonical third-party source lock for CUP dependencies.
# This file is sourced by the dependency builders; values are repository policy
# and cannot be replaced by ambient environment variables.

ZLIB_VERSION=1.3.2
XZ_VERSION=5.8.3
OPENSSL_VERSION=3.5.7
CURL_VERSION=8.21.0
LIBARCHIVE_VERSION=3.8.8
ARGTABLE3_VERSION=3.3.1
UTHASH_VERSION=2.3.0
UNITY_VERSION=2.6.1
LIBEVENT_VERSION=2.1.13-stable

ZLIB_SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
XZ_SHA256=fff1ffcf2b0da84d308a14de513a1aa23d4e9aa3464d17e64b9714bfdd0bbfb6
OPENSSL_SHA256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
CURL_SHA256=aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6
LIBARCHIVE_SHA256=3873a88801da067d0528a989af06877710529d50ee8fe6f3970cbb4302efb918
ARGTABLE3_SHA256=8b28a4fb2cd621d8d16f34e30e1956aa488077f6a6b902e7fc9f07883e1519c1
UTHASH_SHA256=e10382ab75518bad8319eb922ad04f907cb20cccb451a3aa980c9d005e661acc
UNITY_SHA256=b41a66d45a6b99758fb3202ace6178177014d52fc524bf1f72687d93e9867292
LIBEVENT_SHA256=f7e9383b8c0baa81b687e5b5eecc01beefaf1b19b64151d95ed61647fe7a315c

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

# One canonical source graph is shared by application and test builds. The
# scope records link ownership without duplicating download/build machinery.
all_source_packages() {
    printf '%s\n' \
        zlib xz openssl curl libarchive argtable3 uthash unity libevent
}

dependency_scope_for_package() {
    case "$1" in
        zlib|xz|openssl|curl|libarchive|argtable3|uthash)
            printf '%s\n' runtime
            ;;
        unity|libevent)
            printf '%s\n' test
            ;;
        *)
            echo "Error: unknown source package '$1'." >&2
            return 1
            ;;
    esac
}

dependency_usage_for_package() {
    case "$1" in
        zlib|xz|openssl|curl|libarchive|argtable3)
            printf '%s\n' static-library
            ;;
        uthash)
            printf '%s\n' header-only
            ;;
        unity)
            printf '%s\n' unit-test-library
            ;;
        libevent)
            printf '%s\n' network-test-library
            ;;
        *)
            echo "Error: unknown source package '$1'." >&2
            return 1
            ;;
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
