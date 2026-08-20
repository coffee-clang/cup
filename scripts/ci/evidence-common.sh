#!/bin/sh

# Provides shared canonical and atomic contracts for CI evidence.

: "${PROJECT_ROOT:?PROJECT_ROOT is required before sourcing evidence-common.sh}"
# shellcheck source=../lib/path-safety.sh
. "$PROJECT_ROOT/scripts/lib/path-safety.sh"

ci_evidence_fail() {
    printf 'CI evidence: %s\n' "$*" >&2
    return 1
}

ci_evidence_require_canonical_text() {
    ci_file=$1
    ci_maximum=${2:-4194304}
    cup_path_require_regular_file "$ci_file" 'CI evidence file' ||
        ci_evidence_fail "unsafe evidence file: $ci_file" || return 1
    ci_size=$(wc -c < "$ci_file" | tr -d '[:space:]')
    case "$ci_size" in
        ''|*[!0-9]*)
            ci_evidence_fail "could not determine evidence size: $ci_file"
            return 1
            ;;
    esac

    if [ "$ci_size" -le 0 ] || [ "$ci_size" -gt "$ci_maximum" ]; then
        ci_evidence_fail "evidence size is outside the allowed range: $ci_file"
        return 1
    fi

    ci_last=$(tail -c 1 "$ci_file" | od -An -tu1 | tr -d '[:space:]')
    if [ "$ci_last" != 10 ]; then
        ci_evidence_fail "evidence is not LF-terminated: $ci_file"
        return 1
    fi

    if ! od -An -tu1 -v "$ci_file" | awk '
        {
            for (i = 1; i <= NF; ++i) {
                if ($i != 10 && ($i < 32 || $i > 126)) {
                    exit 1
                }
            }
        }
    '; then
        ci_evidence_fail "evidence contains non-canonical bytes: $ci_file"
        return 1
    fi
}


ci_evidence_resolve_path() {
    ci_path_base=$1
    ci_path_value=$2

    case "$ci_path_value" in
        /*|[A-Za-z]:/*)
            printf '%s\n' "$ci_path_value"
            ;;
        *)
            printf '%s/%s\n' "$ci_path_base" "$ci_path_value"
            ;;
    esac
}

ci_evidence_validate_repository() {
    printf '%s\n' "$1" | grep -Eq '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' ||
        ci_evidence_fail "invalid repository: $1"
}

ci_evidence_validate_sha() {
    printf '%s\n' "$1" | grep -Eq '^[0-9a-f]{40}$' ||
        ci_evidence_fail "invalid commit SHA: $1"
}

ci_evidence_validate_number() {
    printf '%s\n' "$1" | grep -Eq '^[1-9][0-9]*$' ||
        ci_evidence_fail "invalid positive number: $1"
}

ci_evidence_validate_slug() {
    ci_slug_value=$1
    ci_slug_label=$2

    printf '%s\n' "$ci_slug_value" | grep -Eq '^[a-z0-9][a-z0-9-]*$' ||
        ci_evidence_fail "invalid $ci_slug_label: $ci_slug_value"
}

ci_evidence_validate_artifact_name() {
    printf '%s\n' "$1" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9_.-]{0,199}$' ||
        ci_evidence_fail "invalid artifact name: $1"
}

ci_evidence_sha256_file() {
    ci_hash_input=$1
    ci_hash_output=
    if command -v sha256sum >/dev/null 2>&1; then
        ci_hash_output=$(sha256sum "$ci_hash_input" 2>/dev/null) || ci_hash_output=
        ci_hash_output=${ci_hash_output%%[[:space:]]*}
        if printf '%s\n' "$ci_hash_output" | grep -Eq '^[0-9a-f]{64}$'; then
            printf '%s\n' "$ci_hash_output"
            return 0
        fi
    fi
    if command -v shasum >/dev/null 2>&1; then
        ci_hash_output=$(shasum -a 256 "$ci_hash_input" 2>/dev/null) || ci_hash_output=
        ci_hash_output=${ci_hash_output%%[[:space:]]*}
        if printf '%s\n' "$ci_hash_output" | grep -Eq '^[0-9a-f]{64}$'; then
            printf '%s\n' "$ci_hash_output"
            return 0
        fi
    fi
    ci_evidence_fail 'neither sha256sum nor shasum produced a valid SHA-256 digest'
}

ci_evidence_prepare_directory() {
    CI_EVIDENCE_DESTINATION=$1
    CI_EVIDENCE_LABEL=${2:-evidence directory}
    cup_path_validate_absolute_clean \
        "$CI_EVIDENCE_DESTINATION" "$CI_EVIDENCE_LABEL" || return 1

    if [ -e "$CI_EVIDENCE_DESTINATION" ] || [ -L "$CI_EVIDENCE_DESTINATION" ]; then
        ci_evidence_fail \
            "$CI_EVIDENCE_LABEL already exists: $CI_EVIDENCE_DESTINATION"
        return 1
    fi

    CI_EVIDENCE_PARENT=$(dirname -- "$CI_EVIDENCE_DESTINATION")
    cup_path_prepare_directory_chain \
        "$CI_EVIDENCE_PARENT" "$CI_EVIDENCE_LABEL parent" || return 1
    CI_EVIDENCE_STAGING=$(cup_path_create_unique_directory \
        "$CI_EVIDENCE_PARENT/.cup-evidence.XXXXXX" "$CI_EVIDENCE_LABEL staging" 0700) || return 1
}

ci_evidence_commit_directory() {
    if [ -z "${CI_EVIDENCE_STAGING:-}" ]; then
        ci_evidence_fail 'evidence staging was not prepared'
        return 1
    fi

    if [ -e "$CI_EVIDENCE_DESTINATION" ] || [ -L "$CI_EVIDENCE_DESTINATION" ]; then
        ci_evidence_fail \
            "$CI_EVIDENCE_LABEL appeared before commit: $CI_EVIDENCE_DESTINATION"
        return 1
    fi

    if ! cup_path_move_entry "$CI_EVIDENCE_STAGING" "$CI_EVIDENCE_DESTINATION"; then
        ci_evidence_fail "could not commit $CI_EVIDENCE_LABEL"
        return 1
    fi
    CI_EVIDENCE_STAGING=
}
