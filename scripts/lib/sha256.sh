
# Owns SHA-256 digest grammar and repository-tool hashing with portable host tools.

cup_sha256_valid() (
    value=${1-}
    [ "${#value}" -eq 64 ] || return 1
    case "$value" in
        *[!0-9a-f]*|'') return 1 ;;
    esac
)

cup_sha256_file() (
    file=${1-}
    [ -n "$file" ] || return 1
    output=
    value=

    if command -v sha256sum >/dev/null 2>&1; then
        output=$(sha256sum "$file" 2>/dev/null) || output=
        value=${output%%[[:space:]]*}
        if cup_sha256_valid "$value"; then
            printf '%s\n' "$value"
            return 0
        fi
    fi
    if command -v shasum >/dev/null 2>&1; then
        output=$(shasum -a 256 "$file" 2>/dev/null) || output=
        value=${output%%[[:space:]]*}
        if cup_sha256_valid "$value"; then
            printf '%s\n' "$value"
            return 0
        fi
    fi
    return 1
)

# Computes the digest of stdin with the same portable host-tool policy.
cup_sha256_stream() (
    output=
    value=

    if command -v sha256sum >/dev/null 2>&1; then
        output=$(sha256sum 2>/dev/null) || output=
        value=${output%%[[:space:]]*}
        if cup_sha256_valid "$value"; then
            printf '%s\n' "$value"
            return 0
        fi
    fi
    if command -v shasum >/dev/null 2>&1; then
        output=$(shasum -a 256 2>/dev/null) || output=
        value=${output%%[[:space:]]*}
        if cup_sha256_valid "$value"; then
            printf '%s\n' "$value"
            return 0
        fi
    fi
    return 1
)
